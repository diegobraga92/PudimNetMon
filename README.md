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

## Current Phase: Phase 5 — Systems Engineering, Time Sync & Service Discovery

**Phase 0 (Skeleton) ✅ — Phase 1 (Metrics) ✅ — Phase 2 (Alerting) ✅ — Phase 3 (Kafka) ✅ — Phase 4 (Deep Networking) ✅ — Phase 5 (SysEng/Time/Discovery) ✅**

- ✅ Protobuf API contracts (`heartbeat.proto`, `metrics.proto`, `diagnostic.proto`)
- ✅ C++ agent: `Type=notify` + watchdog (`sd_notify`), SIGHUP handling, unprivileged user with `CAP_NET_RAW`+`CAP_NET_ADMIN`, journald logging, diagnostic gRPC server
- ✅ **NTP offset probe** (`ntp_adjtime()`, `CHECK_TYPE_NTP_OFFSET`), clock-skew detection at the collector (`--skew-threshold-ms`, `pudim_clock_skew_warnings_total`), collector-assigned timestamps as source of truth
- ✅ **Service discovery**: `--collector-endpoints` failover list with 3-strike rotation (`FailoverClient`), DNS/K8s discovery documented
- ✅ **Deep network probes**: TLS cert validation, DNS records, TCP handshake (libpcap), TCP retransmit, HTTP/1.1-vs-2-vs-3
- ✅ **Diagnostic mode**: collector-triggered `traceroute` + `tcpdump` on the agent
- ✅ Kafka backbone (KRaft broker, storage + alert consumers, at-least-once + idempotent writes)
- ✅ Dashboard: NTP offset chart, TLS expiry timeline, HTTP protocol comparison, diagnostic runner, alerts + history
- ✅ Docs: ADRs 001–007, `docs/networking-deep-dive.md`, `docs/kernel-tuning.md`, runbooks
- ✅ CI: C++ build/test (Debug), TypeScript lint/build

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

### Service discovery & clock hygiene (Phase 5)

```bash
# Failover: try collector-a first, then collector-b after 3 consecutive failures
./build-agent/pudim-agent --collector-endpoints=collector-a:50051,collector-b:50051 ...

# Skew threshold on the collector (default 5000 ms)
./build-collector/pudim-collector --skew-threshold-ms=5000 ...
```

### Deep diagnostics (Phase 4)

```bash
./build-agent/pudim-agent \
  --tls-targets=example.com:443 \
  --http-targets=https://example.com --http-protocols=http1.1,http2 \
  --dns-targets=example.com --dns-expected=example.com=A:93.184.216.34 \
  --diagnostic-address=agent.example.com:50052
```

## License

MIT