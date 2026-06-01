# Service Level Objectives (SLO) — Phase 0 Draft

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