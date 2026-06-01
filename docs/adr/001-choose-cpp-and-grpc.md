# ADR 001: Choose C++ and gRPC for Agent and Collector

**Status:** Accepted  
**Date:** 2026-01-01

## Context

The PudimNetMon platform requires a high-performance network monitoring solution with agents running on remote Linux hosts and a central collector that aggregates metrics. Key requirements:

- Agents must impose minimal resource overhead (CPU, memory)
- Collectors must handle hundreds to thousands of concurrent agent connections
- Strongly-typed, evolvable API contracts between components
- Cross-language compatibility (C++ agents/collector, TypeScript dashboard)
- Streaming capability for future real-time metric transmission

## Decision

We will use:

1. **C++17** for both agent and collector implementations
2. **gRPC** with **Protocol Buffers** (protobuf3) for all inter-component communication
3. **CMake** as the build system (minimum version 3.20)

## Rationale

### Why C++?

- **Performance:** Direct control over memory layout, zero-cost abstractions, no garbage collection pauses. Critical for agents that must not interfere with the workloads they monitor.
- **Resource footprint:** Static linking produces small binaries; minimal runtime dependencies.
- **System access:** Native access to raw sockets (`CAP_NET_RAW`), `libpcap`, kernel interfaces (`getsockopt(TCP_INFO)`), and NTP syscalls (`ntp_gettime()`). Rust or Go would require FFI or CGo for these operations.
- **Ecosystem maturity:** Decades of battle-tested tooling (CMake, sanitizers, perf, Valgrind).
- **Production daemon pattern:** systemd integration, signal handling, watchdog support are well-documented in C.

### Why gRPC + Protobuf?

- **Strong typing:** Protobuf schemas define the contract explicitly; breaking changes detected at compile time.
- **HTTP/2 transport:** Multiplexed streams, flow control, and deadline propagation for future real-time streaming metrics.
- **Code generation:** Single `.proto` file generates C++, TypeScript (via `protoc-gen-ts` or gRPC-Web), and Go stubs, ensuring consistency across the stack.
- **Streaming support:** Unary RPCs for heartbeats now; server-streaming or bidirectional streaming for metrics in Phase 1+.
- **Industry adoption:** gRPC is used extensively in observability (OpenTelemetry, Prometheus remote write) and infrastructure (etcd, Kubernetes).

### Why CMake?

- **Cross-platform:** Same build scripts work on Linux, macOS, and Windows (for development).
- **FetchContent:** Allows pulling dependencies (cpp-httplib) without system package manager.
- **Widely adopted:** All major IDEs support CMake natively (CLion, VS Code, Visual Studio).

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| **Rust** | Excellent for safety, but raw socket and kernel interface access requires `unsafe` blocks; smaller ecosystem for gRPC. Learning curve for the team. |
| **Go** | Great for network services, but GC latency profile unsuitable for high-frequency packet capture; CGo overhead for raw socket calls. |
| **ZeroMQ / raw TCP** | No schema enforcement, no built-in streaming, no deadline propagation; requires custom serialization. |
| **Thrift** | Less active ecosystem than Protobuf; no native HTTP/2 transport. |

## Consequences

- Initial development requires C++17 knowledge and CMake proficiency
- Build times are longer than Go or Rust equivalent (mitigated by ccache in CI)
- Memory safety relies on developer discipline (mitigated by static analysis, ASan, UBSan in CI)
- Protobuf wire format is opaque compared to JSON (mitigated by `protoc --decode` and `grpcurl` for debugging)

## Compliance

- Cross-cutting: Observability (structued JSON logs), Testing (unit tests via Google Test in later phases)
- Security: gRPC supports mTLS natively (Phase 3+); protobuf has no injection surface
- ADR compliance: This document follows the Architecture Decision Record format per DEV_PLAN.md
</content>
<write_to_file>
<path>docs/slo.md</path>
<content># Service Level Objectives (SLO) — Phase 0 Draft

> This document defines initial SLOs for the PudimNetMon core heartbeat delivery pipeline. SLOs will be refined and expanded in subsequent phases as more metrics and alerting are implemented.

## Core SLO: Agent Heartbeat Delivery

| Metric | Target | Window | Measurement |
|---|---|---|---|
| Heartbeat delivery success rate | ≥ 99.9% | 5 minutes | `heartbeats_received_total / expected_heartbeats` per agent |
| Heartbeat end-to-end latency (agent send → collector ACK) | ≤ 1 second (p99) | 5 minutes | Agent measures `collector_time_unix_ms - timestamp_unix_ms` |
| Collector uptime | ≥ 99.99% | 30 days | Prometheus `/metrics` target_up |

## Definitions

- **Heartbeat delivery success**: A heartbeat is considered delivered if the collector returns an ACK within 10 seconds (the gRPC deadline configured in the agent).
- **Expected heartbeats**: `(window_duration_ms / agent_interval_ms)` per agent. For a 5-minute window with a 5-second interval, expected = 60 heartbeats.
- **Error budget**: 0.1% failure rate per 5-minute window. Over 30 days (8,640 windows), this permits ~8.6 windows with > 0.1% failure.

## Measurement

- Collector exports `pudim_heartbeats_received_total` (counter) and `pudim_agents_active` (gauge) via the `/metrics` Prometheus endpoint.
- A simple burn-rate alert: if success rate drops below 99.9% over 5m, fire a warning. If below 99% over 5m, fire a page.
- Detailed latency measurement requires agent-side timestamp logging (agent already logs `collector_time_unix_ms - timestamp_unix_ms` in structured logs).

## Future SLOs (Phases 1+)

- Metrics ingestion latency (agent → collector → TimescaleDB)
- Alert evaluation latency
- Dashboard query response time (p95)
- Kafka consumer lag (if applicable)

## Budget Enforcement

During Phase 0, error budgets are informational only. In Phase 6, error budget alerts will trigger incident response according to documented runbooks.