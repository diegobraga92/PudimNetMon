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

## Current Phase: Phase 3 — Kafka Event Backbone

**Phase 0 (Skeleton) ✅ — Phase 1 (Metrics & Storage) ✅ — Phase 2 (Alerting) ✅ — Phase 3 (Kafka) ✅**

- ✅ Protobuf API contracts (`heartbeat.proto`, `metrics.proto`)
- ✅ C++ agent: gRPC client (unary + streaming), JSON logging, CLI flags, systemd unit
- ✅ C++ collector: gRPC server, agent registry, HTTP endpoints, TimescaleDB storage
- ✅ Network probes: DNS, TCP connect, TLS handshake, HTTP, ICMP (packet loss + RTT + jitter)
- ✅ Alerting engine: JSON rules, state machine, Log + Webhook notifiers
- ✅ **Kafka backbone**: KRaft broker (Docker Compose), collector produces `network.metrics`
  (keyed by agent ID), separate `pudim-consumer-storage` + `pudim-consumer-alert` consumers,
  at-least-once with idempotent DB writes (`ON CONFLICT DO NOTHING`), consumer-lag metrics
- ✅ React dashboard: health, agent list, time-series graphs, active alerts pane, alert history
- ✅ GitHub Actions CI: C++ build/test (Debug), TypeScript lint/build
- ✅ ADRs 001–004, SLO draft, runbooks

### Architecture (Phase 3)

```
Agent ──gRPC──▶ Collector ──produce──▶ Kafka ──consume──▶ pudim-consumer-storage ──▶ TimescaleDB
                       (network.metrics)                 └──▶ pudim-consumer-alert ──▶ AlertManager
```

### Running the full stack

```bash
docker compose up --build
# dashboard:     http://localhost:3000
# collector:     http://localhost:8080 (health/metrics)
# storage cons.: http://localhost:9091/metrics (Prometheus)
# alert cons.:   http://localhost:9092/metrics (Prometheus)
```

The collector runs in **Kafka mode** (via `--kafka-brokers=kafka:9092`) and no longer
writes to TimescaleDB or evaluates alerts in-process — the two consumers own those
concerns. Without `--kafka-brokers` the collector falls back to the direct path.

### Alerting

Alert rules live in `collector/config/alert_rules.json`, evaluated by the **alert
consumer** in Kafka mode. Each rule declares a check type, metric field, operator,
threshold, repeat interval and severity:

```json
{
  "webhook_url": "http://localhost:9000/hooks/pudim",
  "rules": [
    { "id": "high-tcp-latency", "check_type": "tcp_connect", "metric": "latency_ms",
      "op": ">", "threshold": 500, "repeat_interval_sec": 300, "severity": "warning" },
    { "id": "dns-failure", "check_type": "dns_resolution", "on_failure": true,
      "severity": "critical" }
  ]
}
```

## License

MIT