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

## Current Phase: Phase 7 — Disaster Recovery, Multi-Region & Cost

**Phases 0–6 ✅ · Phase 7 (DR/Multi-region/Cost) ✅**

- ✅ **Agent disk buffer (SQLite)**: overflow from the in-memory FIFO spills to a local SQLite DB (`--disk-buffer-path`, cap `--disk-buffer-max-mb`); drained oldest-first on reconnect; survives agent restarts. `HAVE_SQLITE3`-guarded (graceful without libsqlite3)
- ✅ **Multi-region failover**: `collector-secondary` in Compose (gRPC :50052, HTTP :8081); agents use `--collector-endpoints=primary,secondary` (3-strike failover from Phase 5)
- ✅ **DR drill** (`docs/dr-test.md`): kill primary → agent fails over to secondary → restore → drain persisted batches. RTO ≈ 3× interval, RPO = in-memory + disk buffer
- ✅ **Cost model** (`docs/cost-analysis.md`): AWS monthly estimate at 100/1000 agents (≈$453 / ≈$878), optimisation levers (spot, self-host Kafka, retention, compression)
- ✅ **Capacity plan**: ~100 B/s/agent, storage ≈ 8.6 MB/day/agent, Kafka partition sizing, TimescaleDB compression/aggregates
- ✅ **ADR 009** — active/passive regions, agent disk buffer as RPO boundary
- ✅ All 9 ADRs, runbooks (high-latency + incident-response), postmortems, chaos log complete
- ⚠️ **Open item**: mTLS between agent and collector (tracked in the Completion Checklist)

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

## License

MIT