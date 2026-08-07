# 🍮 PudimNetMon — Network Monitoring Platform

> Distributed network monitoring platform: C++ agents (Linux daemons), C++ central collector, and a TypeScript/React web dashboard.

## Architecture Overview

```
┌─────────────┐     gRPC      ┌─────────────┐     HTTP      ┌──────────────┐
│  Agent(s)    │──────────────▶│  Collector   │──────────────▶│  Dashboard    │
│  (C++ daemon)│   heartbeat   │  (C++ server)│  /health      │  (React/TS)  │
└─────────────┘               │              │               └──────────────┘
                              │  Port 50051  │
                              │  Port 8080   │
                              └─────────────┘
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

## Current Phase: Phase 2 — Alerting & Notification

**Phase 0 (Skeleton) ✅ complete — Phase 1 (Core Metrics & Time-Series) ✅ complete — Phase 2 (Alerting) ✅ complete**

- ✅ Protobuf API contracts (`heartbeat.proto`, `metrics.proto`)
- ✅ C++ agent: gRPC client (unary + streaming), JSON logging, CLI flags, systemd unit
- ✅ C++ collector: gRPC server, in-memory agent registry, health/metrics endpoints, TimescaleDB storage
- ✅ Network probes: DNS, TCP connect, TLS handshake, HTTP, ICMP (packet loss + RTT + jitter)
- ✅ Alerting engine: JSON rules, state machine (firing/repeat/resolved), Log + Webhook notifiers
- ✅ Collector endpoints: `/health`, `/agents`, `/metrics`, `/alerts`, `/alert-history`, `/alert-rules`, `/api/metrics`
- ✅ React dashboard: health, agent list, time-series graphs, active alerts pane, alert history
- ✅ Docker Compose: collector + agent + dashboard + TimescaleDB
- ✅ GitHub Actions CI: C++ build/test (Debug), TypeScript lint/build
- ✅ ADRs 001–003, SLO draft, high-latency runbook

### Alerting

Alert rules live in `collector/config/alert_rules.json` (JSON, loaded at startup via
`--alert-rules-path`). Each rule declares a check type, metric field, operator, threshold,
repeat interval and severity:

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

Alerts appear in the collector's structured JSON logs, are POSTed to the webhook (if set),
and are visible in the dashboard's **Active Alerts** and **Alert History** panes.

## License

MIT