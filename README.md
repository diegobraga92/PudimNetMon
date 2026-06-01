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

## Current Phase: Phase 0 — Skeleton

- ✅ Protobuf API contracts (`heartbeat.proto`)
- ✅ C++ agent: gRPC client, JSON logging, CLI flags, systemd unit
- ✅ C++ collector: gRPC server, in-memory agent registry, health/metrics endpoints
- ✅ React dashboard: collector health status, agent list with alive/dead status
- ✅ Docker Compose: collector + agent + dashboard
- ✅ GitHub Actions CI: C++ build, TypeScript lint/build
- ✅ ADR 001: C++ and gRPC rationale
- ✅ SLO draft: heartbeat delivery success rate

## License

MIT