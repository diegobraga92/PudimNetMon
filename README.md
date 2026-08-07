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

## Current Phase: Phase 6 — Observability, Overload & Chaos

**Phases 0–5 ✅ · Phase 6 (Observability/Overload/Chaos/Incidents) ✅**

- ✅ **W3C Trace Context**: `traceparent` propagated agent → collector (gRPC metadata) → Kafka (headers) → consumers. Every hop logs the trace; a single measurement is traceable end-to-end
- ✅ **Prometheus + Grafana**: Grafana provisioned in Compose with a curated `network-monitor` dashboard (agents, throughput, Kafka lag, storage latency, skew, backpressure); SLO burn-rate alert rules in `infra/prometheus/alerts.yml`
- ✅ **Overload handling (ADR 008)**: agent bounded buffer (`--max-buffer-size`, oldest dropped), collector `x-overloaded` gRPC backpressure signal, agent adaptive interval (back off 2× up to 10×)
- ✅ **Chaos experiments** (`docs/chaos-experiments.md`): collector kill, network partition, Kafka restart, DNS failure, clock-skew injection — all verified with graceful degradation
- ✅ **Two postmortems** (`docs/postmortems/`): collector overload OOM, clock-skew alert storm
- ✅ **Runbooks**: `docs/runbooks/incident-response.md` (reconnection, scale-up, rebalancing, skew) + high-latency
- ✅ **Deep probes** (Phase 4): TLS cert, DNS records, TCP handshake (libpcap), retransmit, HTTP/1.1-vs-2-vs-3
- ✅ **Daemon/Time/Discovery** (Phase 5): `Type=notify`+watchdog, NTP offset, clock-skew detection, collector-endpoints failover
- ✅ **Kafka backbone** (Phase 3): KRaft broker, storage + alert consumers, at-least-once + idempotent writes
- ✅ ADRs 001–008, CI (C++ build/test + TS lint/build)

### Architecture

```
Agent ──gRPC──▶ Collector ──produce──▶ Kafka ──consume──▶ pudim-consumer-storage ──▶ TimescaleDB
       │        (traceparent)          (traceparent hdr) └──▶ pudim-consumer-alert ──▶ AlertManager
       │           ▲  x-overloaded (backpressure)
       └── diagnostic gRPC (:50052) ◀── POST /diagnostic ── Collector :8080

Collector :8080/metrics · storage :9091 · alert :9092  →  Grafana :3100
```

### Running the full stack

```bash
docker compose up --build
# dashboard:     http://localhost:3000
# collector:     http://localhost:8080 (health/metrics)
# Grafana:       http://localhost:3100 (provisioned dashboard, anonymous login)
# storage cons.: http://localhost:9091/metrics (Prometheus)
# alert cons.:   http://localhost:9092/metrics (Prometheus)
```

### Overload handling (Phase 6)

```bash
# Agent: bounded buffer + adaptive backoff (on x-overloaded)
./build-agent/pudim-agent --max-buffer-size=200 ...

# Collector: signal agents to back off when ingest exceeds 1s
./build-collector/pudim-collector --backpressure-threshold-ms=1000 ...

# Simulate overload
./scripts/overload-collector.sh 10 500      # 10 agents at 500 ms
./scripts/overload-kafka.sh 30              # pause/resume Kafka for 30 s
```

## License

MIT