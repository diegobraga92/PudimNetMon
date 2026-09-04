# 🍮 PudimNetMon — Network Monitoring Platform

> Distributed network monitoring platform: C++ agents (Linux daemons and Windows services), C++ central collector, and a TypeScript/React web dashboard.

## Architecture Overview

```
          gRPC (heartbeat + metrics)       produce (protobuf, keyed by agent)
  ┌──────────────┐      ┌──────────────┐     ┌──────────────────────┐
  │  Agent(s)     │─────▶│  Collector   │────▶│   Kafka broker       │
  │  (C++ daemon) │      │  (C++ server)│     │   network.metrics     │
  └──────────────┘      └──────────────┘     └───────────┬──────────┘
     Port 50051 gRPC           HTTP :8080               │ consume (groups:
     /health /agents /api/metrics                       │  storage, alert)
                                                         ▼
                                           ┌──────────────────────────────────┐
                                           │ pudim-consumer-storage ──▶ TimescaleDB │
                                           │ pudim-consumer-alert   ──▶ AlertManager│
                                           └──────────────────────────────────┘
                                             Prometheus :9091 (storage) / :9093 (alert)

  ┌──────────────┐     HTTP     ┌──────────────┐
  │  Dashboard    │◀────────────│  Collector   │  :3000
  │  (React/TS)  │   :3000      │  :8080       │
  └──────────────┘              └──────────────┘
```

## Quick Start

### Prerequisites

- Docker & Docker Compose
- Or: CMake 3.20+, gRPC, Protobuf, Node.js 22+

### Using Docker Compose (Recommended)

```bash
# Clone the repository
git clone https://github.com/diegobraga92/PudimNetMon.git
cd PudimNetMon

# Build and start all services
docker compose up --build

# The dashboard is available at:
#   http://localhost:3000
#
# Collector HTTP endpoints:
#   http://localhost:8080/health
#   http://localhost:8080/agents
#   http://localhost:8080/metrics
```
> **All host-side ports are configurable** via a `.env` file — see
> [Running on a LAN Server](#running-on-a-lan-server) below. Defaults are
> `3000` (dashboard), `3100` (Grafana), `8080`/`50051` (collector),
> `5432` (TimescaleDB), `9092` (Kafka), `9091`/`9093` (Prometheus endpoints).

## Running on a LAN Server

PudimNetMon publishes several ports on the host. If the LAN server already
runs many Docker containers (web servers, databases, monitoring stacks…),
the defaults below may collide. Every port is overridable — **nothing needs
to be recompiled**: only the Docker host-side mapping changes.

### Step 1 — Pick free ports

Create `.env` from the template and edit the ports you need:

```bash
cp .env.example .env
$EDITOR .env
```

Only override the ports that conflict on your server. The template documents
every variable; the defaults are:

| Service | Env var | Default port |
|---|---|---|
| Dashboard (web UI) | `PUDIM_DASHBOARD_PORT` | `3000` |
| Grafana | `PUDIM_GRAFANA_PORT` | `3100` |
| Collector HTTP (REST API, health) | `PUDIM_COLLECTOR_HTTP_PORT` | `8080` |
| Collector gRPC (agents) | `PUDIM_COLLECTOR_GRPC_PORT` | `50051` |
| Collector-secondary HTTP | `PUDIM_COLLECTOR_SECONDARY_HTTP_PORT` | `8081` |
| Collector-secondary gRPC | `PUDIM_COLLECTOR_SECONDARY_GRPC_PORT` | `50052` |
| TimescaleDB (PostgreSQL) | `PUDIM_TIMESCALEDB_PORT` | `5432` |
| Kafka broker | `PUDIM_KAFKA_PORT` | `9092` |
| Consumer-storage Prometheus | `PUDIM_CONSUMER_STORAGE_PROMETHEUS_PORT` | `9091` |
| Consumer-alert Prometheus | `PUDIM_CONSUMER_ALERT_PROMETHEUS_PORT` | `9093` |

Example `.env` when `3000`, `8080` and `5432` are already taken:

```bash
PUDIM_DASHBOARD_PORT=3300
PUDIM_COLLECTOR_HTTP_PORT=8800
PUDIM_TIMESCALEDB_PORT=55432
```

### Step 2 — Start the stack

```bash
docker compose up --build
```

Docker Compose reads `.env` automatically. Access the dashboard from any
machine on the LAN at `http://<server-ip>:<PUDIM_DASHBOARD_PORT>` (e.g.
`http://192.168.1.50:3300`). A firewall may need to allow the chosen ports.

### Troubleshooting

- **`port is already allocated`** (or `Bind for 0.0.0.0:<port> failed`):
  another container or process owns that port. Find the offender with
  `docker ps --format 'table {{.Names}}\t{{.Ports}}'` or `ss -tlnp | grep <port>`,
  then set a different value for the corresponding `PUDIM_*_PORT` in `.env`
  and `docker compose up -d` again.
- **Grafana shows no data / blank panels**: the Prometheus endpoints of the
  consumers are still reachable inside the Compose network; you only need to
  touch `PUDIM_CONSUMER_*_PROMETHEUS_PORT` if you want to scrape them from an
  **external** Prometheus on the host.
- **Agents can't reach the collector**: agent → collector traffic stays on the
  internal Compose network (`collector:50051`), so remapping host ports never
  breaks the in-stack agent. Only **external** agents pointing at
  `<server-ip>:<PUDIM_COLLECTOR_GRPC_PORT>` are affected — update their
  `--collector-endpoints` accordingly.


### Building from Source (Linux)

**Agent:**
```bash
cd agent
cmake -B build -S .
cmake --build build -j$(nproc)
./build/pudim-agent --help
```

**Collector:**
```bash
cd collector
cmake -B build -S .
cmake --build build -j$(nproc)
./build/pudim-collector --help
```

**Dashboard:**
```bash
cd dashboard
npm install
npm run dev
```

## Deploying the Agent & Other Components

The sections above run **everything in Docker on one server**. But the agent
is designed to run on the actual hosts you monitor, and the dashboard /
consumers can also run outside Compose. Full instructions in
[`docs/deployment.md`](docs/deployment.md); quick version below.

### Agent on target hosts

The agent is a C++17 daemon with **layered configuration**:
built-in defaults < config file < CLI flags. Settings live in a flat
`key=value` config file whose keys are the long CLI flag names (see
[`agent/config/agent.conf.example`](agent/config/agent.conf.example)), read
from `/etc/pudim/agent.conf` on Linux (or `<state-dir>\agent.conf` on Windows)
when present. CLI flags still win, for host-specific values and one-off
troubleshooting. The shipped systemd unit installs as a hardened service:

```bash
# 1. Build (from the repo root; Ubuntu 24.04 deps)
sudo apt-get install -y cmake protobuf-compiler libprotobuf-dev libgrpc++-dev \
    libgrpc-dev protobuf-compiler-grpc build-essential pkg-config \
    libcurl4-openssl-dev libssl-dev libpcap-dev libsystemd-dev libsqlite3-dev
cmake -S agent -B build && cmake --build build -j$(nproc)
sudo cmake --install build                      # /usr/local/bin/pudim-agent

# 2. Install the shipped, pre-hardened systemd unit
sudo cp agent/systemd/pudim-agent.service /etc/systemd/system/

# 3. Point it at your collector and define probe targets in the config file
sudo mkdir -p /etc/pudim
sudo cp agent/config/agent.conf.example /etc/pudim/agent.conf
sudoedit /etc/pudim/agent.conf   # collector-endpoints, node-id, probes, mTLS...

sudo systemctl daemon-reload && sudo systemctl enable --now pudim-agent
```

Unknown keys in the config file are rejected at startup (typos fail fast, so a
misspelled setting can't silently fall back to defaults). Use
`--config-file=<path>` to load from another location; the agent logs which
file it loaded (if any) at startup. Key settings: `collector-endpoints`
(comma-separated failover list), `node-id`, `interval`, `dns-targets` /
`tcp-targets` / `tls-targets` / `http-targets` / `ping-targets`,
`diagnostic-address` (enables dashboard diagnostics), and
`tls-ca` / `tls-cert` / `tls-key` for [mTLS](#enable-mtls). The unit runs
unprivileged with `CAP_NET_RAW`/`CAP_NET_ADMIN` for ICMP + pcap probes. Run
one per host, all pointing at the same collector(s) — verify with
`curl <server>:8080/agents`.

### Agent on Windows

The agent also builds and runs on **Windows 10/11** (MSVC + vcpkg) as a console
application or a native Windows service (`PudimNetMonAgent`). Windows hosts
report into the same collector stack over gRPC. Feature parity is near-total —
ICMP uses the Windows ICMP API (`iphlpapi`), TCP retransmit uses
`SIO_TCP_INFO`, NTP offset uses a built-in SNTP client, and traceroute maps to
`tracert`. Packet capture (libpcap) degrades to an explicit "unsupported"
metric.

**Recommended install:** the CI job "C++ Agent (Windows build)" packages a
self-contained install wizard (`PudimNetMon-Agent-Setup-<version>.exe`,
uploaded as the `pudimnetmon-agent-windows-setup` artifact). The wizard asks
for the node ID, collector endpoint(s) and polling interval, installs the
binary under `Program Files`, writes `%ProgramData%\PudimNetMon\agent.conf`
(node-id stays on the service command line; all other settings come from the
file — the same split as the Linux systemd unit), registers and starts the
`PudimNetMonAgent` auto-start service, and removes it again on uninstall.
Silent mode:

```powershell
PudimNetMon-Agent-Setup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

Manual builds are still fully supported (this is what CI validates). To run
the freshly built binary as the auto-start service instead of in the
foreground, register it with the Service Control Manager from an elevated
PowerShell (the agent binary itself has no `--install-service` flag — that is
what the install wizard does internally with `sc.exe`). Only the node ID is
baked into the service command line; the mutable settings live in
`%ProgramData%\PudimNetMon\agent.conf`.

```powershell
cmake -S agent -B build-agent-windows `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build-agent-windows --config Release -j
$exe = "$PWD\build-agent-windows\Release\pudim-agent.exe"
$dir = Join-Path $env:ProgramData 'PudimNetMon'
New-Item -ItemType Directory -Force $dir | Out-Null
'collector-endpoints=collector.lan:50051', 'interval=10000' |
  Set-Content (Join-Path $dir 'agent.conf')
# New-Service stores the ImagePath verbatim (no shell re-parsing)
$binPath = '"' + $exe + '" --node-id=win-01'
New-Service -Name PudimNetMonAgent -DisplayName 'PudimNetMon Agent' `
  -BinaryPathName $binPath -StartupType Automatic
Start-Service PudimNetMonAgent
```

See [`docs/windows.md`](docs/windows.md) for details, the feature matrix, the
installer workflow, and the CI job that produces the wizard.

### Dashboard & consumers outside Compose

- **Dashboard**: `cd dashboard && npm ci && npm run build`, then serve the
  static `dist/` from any web server and reverse-proxy `/api/*` to the
  collector HTTP port (`8080` by default). Example nginx block in
  [`docs/deployment.md`](docs/deployment.md#2-dashboard-standalone).
- **Consumers** (`pudim-consumer-storage`, `pudim-consumer-alert`): built from
  the collector CMake project (`cmake --build build --target pudim-consumer-storage pudim-consumer-alert`);
  they only need Kafka (+ TimescaleDB for storage). systemd unit examples in
  [`docs/deployment.md`](docs/deployment.md#3-kafka-consumers-storage--alert).


## Project Structure

```
├── agent/                  # C++ agent daemon
│   ├── CMakeLists.txt
│   ├── src/main.cpp
│   └── systemd/           # systemd unit file
├── collector/              # C++ collector server
│   ├── CMakeLists.txt
│   └── src/main.cpp
├── dashboard/              # TypeScript/React dashboard
│   ├── src/
│   ├── vite.config.ts
│   └── Dockerfile
├── api/
│   └── proto/             # Protobuf API definitions
├── infra/
│   └── docker/            # Dockerfiles
├── docs/
│   ├── adr/               # Architecture Decision Records
│   ├── DEV_PLAN.md         # Full development plan
│   └── slo.md             # Service Level Objectives
├── .github/workflows/     # CI/CD
├── .env.example          # Host port configuration template
└── docker-compose.yml     # Local development environment
```

## Current Phase: Phase 8 — Dashboard Polish, API & Portfolio

**Phases 0–7 ✅ · mTLS ✅ · Phase 8 ✅** (network topology map deferred — optional)

- ✅ **Interactive dashboard**: zoom/pan (recharts `Brush`), alert acknowledge, and an **agent configuration panel** that reads (`GetConfig`) and applies (`Reconfigure`) probe targets at runtime — no agent restart
- ✅ **UX/UI overhaul (v0.3)**: full component architecture (React Query data layer, Radix UI primitives, Tailwind CSS v4 design tokens), dark/light mode, responsive sidebar navigation, interactive chart legends, alert severity filtering + bulk ack, agent sparklines + search, validated config form, skeleton/empty/error states, toast notifications, **metrics explorer** (sortable raw table + CSV export), and **PWA support** (manifest, app icons, offline app-shell service worker); tested with vitest + Testing Library
- ✅ **Self-hosted agent download (v0.4)**: the dashboard's **Deploy Agent** page downloads prebuilt `pudim-agent` binaries straight from the collector (`/api/agent/versions` + `/api/agent/download`) and shows copy-paste install commands for Linux, Windows and Docker; the docker-compose collector image ships a Linux x86_64 agent
- ✅ **REST API + OpenAPI spec**: all dashboard endpoints under `/api/*`
  (`/api/health`, `/api/agents`, `/api/metrics`, `/api/alerts` + `/ack`,
  `/api/alert-history`, `/api/alert-rules`, `/api/diagnostic`,
  `/api/agents/config`); documented in [`docs/openapi.yaml`](docs/openapi.yaml)
- ✅ **Bundle optimization**: app chunk 578 kB → 21 kB (recharts split); audit runbook in [`docs/performance.md`](docs/performance.md)
- ✅ **Portfolio docs**: [C4 architecture](docs/architecture.md), [tradeoffs / ADR index](docs/tradeoffs.md), [demo walkthrough](docs/demo.md), [mTLS + cert rotation](docs/certificate-rotation.md)

### Enable mTLS

```bash
./scripts/gen-certs.sh certs                        # ca.crt + collector/agent certs
./build-collector/pudim-collector \
    --tls-ca certs/ca.crt --tls-cert certs/collector.crt --tls-key certs/collector.key ...
./build-agent/pudim-agent \
    --tls-ca certs/ca.crt --tls-cert certs/agent.crt --tls-key certs/agent.key ...
```

Without `--tls-*` both fall back to insecure gRPC (local dev / CI convenience);
both log the effective transport at startup.

### Architecture

```
Agent ──gRPC──▶ Collector(primary :50051) ──produce──▶ Kafka ──consume──▶ storage/alert consumers ──▶ TimescaleDB
       │  3-strike failover (--collector-endpoints)
       └──▶ Collector-secondary (:50052)   (DR stand-in for region B)
       │  on outage: in-memory FIFO → overflow → SQLite disk buffer → drain on reconnect
       └── diagnostic gRPC (:50052) ◀── POST /diagnostic ── Collector :8080
```

### DR drill (Phase 7)

```bash
docker compose up --build
docker compose kill collector                      # region-A outage
docker compose logs agent | grep 'failing over'    # → collector-secondary
docker compose up -d collector                     # restore
docker compose logs agent | grep 'Drained'         # disk buffer drained
```

### Overload handling (Phase 6)

```bash
./build-agent/pudim-agent --max-buffer-size=200 --disk-buffer-path=/var/lib/pudim/pending.db ...
./build-collector/pudim-collector --backpressure-threshold-ms=1000 ...
./scripts/overload-collector.sh 10 500
./scripts/overload-kafka.sh 30
```

## Documentation Index

| Area | Docs |
|---|---|
| Architecture | [`docs/architecture.md`](docs/architecture.md) — C4 context/container/component (Mermaid) |
| API | [`docs/openapi.yaml`](docs/openapi.yaml) — OpenAPI 3.0 spec for the collector REST API |
| ADRs | [`docs/adr/001`…`010`](docs/adr/) — every architecture decision (indexed in [`docs/tradeoffs.md`](docs/tradeoffs.md)) |
| Security | [`docs/certificate-rotation.md`](docs/certificate-rotation.md) — mTLS cert lifecycle |
| Runbooks | [`docs/runbooks/incident-response.md`](docs/runbooks/incident-response.md), [`docs/runbooks/high-latency-alert.md`](docs/runbooks/high-latency-alert.md) |
| Postmortems | [`docs/postmortems/001-collector-overload-oom.md`](docs/postmortems/001-collector-overload-oom.md), [`docs/postmortems/002-clock-skew-alert-storm.md`](docs/postmortems/002-clock-skew-alert-storm.md) |
| Chaos & DR | [`docs/chaos-experiments.md`](docs/chaos-experiments.md), [`docs/dr-test.md`](docs/dr-test.md) |
| Deployment | [`docs/deployment.md`](docs/deployment.md) — agents on target hosts (systemd), dashboard standalone, consumers as services |
| Windows agent | [`docs/windows.md`](docs/windows.md) — build with MSVC + vcpkg, run as console/service, feature matrix |
| Local Windows CI | [`docs/running-windows-ci-locally.md`](docs/running-windows-ci-locally.md) — reproduce the `cpp-agent-windows` CI job locally (MSVC + vcpkg + Inno Setup) |
| Performance | [`docs/performance.md`](docs/performance.md) — bundle optimization + Lighthouse runbook |
| Deep dives | [`docs/networking-deep-dive.md`](docs/networking-deep-dive.md), [`docs/kernel-tuning.md`](docs/kernel-tuning.md), [`docs/cost-analysis.md`](docs/cost-analysis.md) |
| Demo | [`docs/demo.md`](docs/demo.md) — recorded portfolio walkthrough |

## License

MIT