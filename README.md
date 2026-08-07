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

## Current Phase: Phase 4 — Advanced Networking & Deep Diagnostics

**Phase 0 (Skeleton) ✅ — Phase 1 (Metrics & Storage) ✅ — Phase 2 (Alerting) ✅ — Phase 3 (Kafka) ✅ — Phase 4 (Deep Networking) ✅**

- ✅ Protobuf API contracts (`heartbeat.proto`, `metrics.proto`, `diagnostic.proto`)
- ✅ C++ agent: gRPC client (unary + streaming), diagnostic gRPC server, JSON logging, CLI flags, systemd unit
- ✅ C++ collector: gRPC server, agent registry, HTTP endpoints, TimescaleDB storage
- ✅ **Deep network probes**: TLS certificate validation (expiry/issuer/hostname), DNS record lookup + expected-value
  alarm, TCP handshake capture (libpcap, SYN/SYN-ACK/ACK timing), TCP retransmission (`TCP_INFO`), HTTP/1.1 vs HTTP/2 vs HTTP/3
- ✅ **Diagnostic mode**: collector-triggered `traceroute` + `tcpdump` on the agent, results returned via gRPC
- ✅ Alerting engine: JSON rules, state machine, Log + Webhook notifiers
- ✅ **Kafka backbone**: KRaft broker, `network.metrics` topic keyed by agent ID, storage + alert consumers,
  at-least-once + idempotent writes, consumer-lag metrics
- ✅ React dashboard: health, agent list, time-series graphs, **TLS expiry timeline**, **HTTP protocol comparison**,
  **per-agent diagnostic runner**, active alerts + history
- ✅ Metric attributes persisted as JSONB and exposed via `/api/metrics`
- ✅ GitHub Actions CI: C++ build/test (Debug, incl. libpcap), TypeScript lint/build
- ✅ ADRs 001–005, SLO draft, runbooks, `docs/networking-deep-dive.md`

### Architecture

```
Agent ──gRPC──▶ Collector ──produce──▶ Kafka ──consume──▶ pudim-consumer-storage ──▶ TimescaleDB
       │                  (network.metrics)             └──▶ pudim-consumer-alert ──▶ AlertManager
       └── diagnostic gRPC (:50052) ◀── POST /diagnostic ── Collector :8080
```

### Running the full stack

```bash
docker compose up --build
# dashboard:     http://localhost:3000
# collector:     http://localhost:8080 (health/metrics)
# storage cons.: http://localhost:9091/metrics (Prometheus)
# alert cons.:   http://localhost:9092/metrics (Prometheus)
```

### Deep diagnostics (Phase 4)

```bash
# Agent with deep probes enabled (default on)
./build-agent/pudim-agent \
  --tls-targets=example.com:443 \
  --http-targets=https://example.com --http-protocols=http1.1,http2 \
  --dns-targets=example.com \
  --dns-expected=example.com=A:93.184.216.34 \
  --diagnostic-address=agent.example.com:50052

# Trigger a diagnostic (traceroute + 5s of tcpdump on the agent)
curl -s -X POST 'http://localhost:8080/diagnostic' \
  --data-urlencode 'agent_id=agent-docker-001' \
  --data-urlencode 'trace_target=example.com' \
  --data-urlencode 'pcap_duration_s=5' \
  --data-urlencode 'pcap_filter=tcp port 443'
```

Requires `libpcap` (compile), plus `traceroute` and `tcpdump` on the agent host.
Without libpcap the TCP-handshake probe degrades gracefully (see ADR 005).

### Alerting

Alert rules live in `collector/config/alert_rules.json`, evaluated by the **alert
consumer** in Kafka mode:

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