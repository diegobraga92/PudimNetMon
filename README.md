# 🍮 PudimNetMon — Network Monitoring Platform

> Distributed network monitoring platform: C++ agents (Linux daemons), C++ central collector, and a TypeScript/React web dashboard.

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
                                             Prometheus :9091 (storage) / :9092 (alert)

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
└── docker-compose.yml     # Local development environment
```

## Current Phase: Phase 8 — Dashboard Polish, API & Portfolio

**Phases 0–7 ✅ · mTLS ✅ · Phase 8 ✅** (network topology map deferred — optional)

- ✅ **Interactive dashboard**: zoom/pan (recharts `Brush`), alert acknowledge, and an **agent configuration panel** that reads (`GetConfig`) and applies (`Reconfigure`) probe targets at runtime — no agent restart
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
| Performance | [`docs/performance.md`](docs/performance.md) — bundle optimization + Lighthouse runbook |
| Deep dives | [`docs/networking-deep-dive.md`](docs/networking-deep-dive.md), [`docs/kernel-tuning.md`](docs/kernel-tuning.md), [`docs/cost-analysis.md`](docs/cost-analysis.md) |
| Demo | [`docs/demo.md`](docs/demo.md) — recorded portfolio walkthrough |

## License

MIT