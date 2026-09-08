# PudimNetMon Network Monitoring Platform

Distributed network monitoring platform. C++ agents (Linux daemons and Windows
services), a C++ central collector, and a TypeScript/React web dashboard.

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

- Docker and Docker Compose
- Or CMake 3.20+, gRPC, Protobuf, Node.js 22+

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
All host-side ports are configurable through a `.env` file.

## Running on a LAN Server

```bash
cp .env.example .env
$EDITOR .env
```

Override ports that conflict on the server. Defaults are the following.

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

```bash
docker compose up --build
```

Access the dashboard from any machine on the LAN at `http://<server-ip>:<PUDIM_DASHBOARD_PORT>`.

### Building from Source (Linux)

**Agent.**
```bash
cd agent
cmake -B build -S .
cmake --build build -j$(nproc)
./build/pudim-agent --help
```

**Collector.**
```bash
cd collector
cmake -B build -S .
cmake --build build -j$(nproc)
./build/pudim-collector --help
```

**Dashboard.**
```bash
cd dashboard
npm install
npm run dev
```

## Agent on target hosts

The agent uses layered configuration. built-in defaults < config file < CLI flags. 
Settings live in a flat `key=value` config file (see [`agent/config/agent.conf.example`](agent/config/agent.conf.example)), 
read from `/etc/pudim/agent.conf` on Linux or `<state-dir>\agent.conf` on Windows.

### Linux

```bash
# 1. Build
sudo apt-get install -y cmake protobuf-compiler libprotobuf-dev libgrpc++-dev \
    libgrpc-dev protobuf-compiler-grpc build-essential pkg-config \
    libcurl4-openssl-dev libssl-dev libpcap-dev libsystemd-dev libsqlite3-dev
cmake -S agent -B build && cmake --build build -j$(nproc)
sudo cmake --install build                      # /usr/local/bin/pudim-agent

# 2. Idempotent installer. systemd unit + seed config (only if absent) + enable
sudo scripts/install-agent.sh

# 3. Point it at your collector and define probe targets in the config file
sudoedit /etc/pudim/agent.conf   # collector-endpoints, node-id, probes, mTLS...
sudo systemctl restart pudim-agent
```

### Windows

The CI job "C++ Agent (Windows build)" packages a self-contained install wizard 
`PudimNetMon-Agent-Setup-<version>.exe`, uploaded as the `pudimnetmon-agent-windows-setup` 
artifact.

```powershell
PudimNetMon-Agent-Setup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

Manual Build:

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

### Enable mTLS

```bash
./scripts/gen-certs.sh certs                        # ca.crt + collector/agent certs
./build-collector/pudim-collector \
    --tls-ca certs/ca.crt --tls-cert certs/collector.crt --tls-key certs/collector.key ...
./build-agent/pudim-agent \
    --tls-ca certs/ca.crt --tls-cert certs/agent.crt --tls-key certs/agent.key ...
```

Without `--tls-*` both fall back to insecure gRPC (local dev / CI convenience).
Both log the effective transport at startup.

## License

MIT