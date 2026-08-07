# ADR 003: Alerting System Design

**Status:** Accepted
**Date:** 2026-08-06

## Context

PudimNetMon's collector ingests network metrics from agents (DNS resolution time, TCP connect latency, TLS handshake time, HTTP response time, packet loss, RTT, jitter). Phase 2 adds the ability to detect anomalies in real time and notify operators.

Key requirements:

- Operators must be notified when a metric violates a threshold (e.g., latency > 500 ms, packet loss > 5%)
- Rules must be configurable per agent or globally (all agents)
- Alerts must not spam: the same condition repeating must be throttled (repeat interval)
- Alerts must be visible in the dashboard (active alerts + history)
- The evaluation must add negligible latency to the ingestion path
- The design must remain viable when Kafka is introduced in Phase 3 (evaluation moves to a consumer)

## Decision

We will implement an **in-process alerting engine inside the collector**, co-located with the gRPC metrics ingestion:

1. **Rule configuration** lives in a JSON file (`alert_rules.json`), loaded at startup and passed via `--alert-rules-path`. Rules are parsed by `AlertManager` using `nlohmann/json`.
2. **Evaluation is synchronous** in the metrics ingestion path: after a batch is successfully stored, `AlertManager::Evaluate()` runs against the batch's metrics. For Phase 2 throughput, this is acceptable (metrics arrive every few seconds per agent; evaluation is O(metrics × rules)).
3. **State machine per (rule, agent, target)**: `OK` → `FIRING` (on violation) → repeat notifications on continued violation after `repeat_interval_sec` → `RESOLVED` (on return to normal).
4. **Notifications** are emitted through a `Notifier` interface:
   - `LogNotifier` — structured JSON log line (always on)
   - `WebhookNotifier` — HTTP POST of the alert JSON to a configurable URL (Slack/Discord/mock incident service), implemented with `httplib::Client` (already a collector dependency)
5. **Alert state and history** are kept in memory: `GetActiveAlerts()` and `GetAlertHistory()` are exposed via HTTP endpoints `/alerts` and `/alert-history` for the dashboard.
6. **Agent identity on the streaming path** (`StreamMetrics`) is carried in the gRPC metadata header `x-agent-id`.

## Rationale

### Why in-process evaluation (not a separate alerting service)?

- **Zero extra moving parts** in Phase 2; the collector already has all metrics in memory at evaluation time.
- **Latency**: evaluation happens in the same process that ACKs the agent, so alert latency ≈ ingestion latency.
- **Kafka path**: when Phase 3 introduces Kafka, the evaluation logic is extracted into the storage/alert consumer with the same `AlertManager` class; the interface stays identical.

### Why JSON config (not DB-backed rules)?

- Simple to version in the repo and diff in code review.
- No schema migration or admin UI needed for Phase 2.
- A future dashboard "configuration panel" (Phase 8) can write the same JSON via a REST endpoint.

### Why state machine with repeat intervals?

- Without repeat intervals, a sustained high-latency condition would fire one alert and never re-notify until resolved — operators would miss recovery milestones.
- With `repeat_interval_sec`, on-call gets a "still firing" reminder (e.g., every 5 minutes) until resolution. This is the Prometheus Alertmanager `repeat_interval` behavior.

### Why `nlohmann/json` as a dependency?

- Header-only, permissive MIT license, de-facto standard JSON library for C++17.
- Fetched via `FetchContent` (consistent with how the collector already pulls `cpp-httplib`).

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Alert evaluation in the dashboard (client-side) | Latency depends on dashboard polling; alerts stop if no dashboard is open; not operational-grade |
| A separate alerting microservice | Extra deployment unit with no benefit at current scale; evaluation data would need to be re-read from storage |
| Rules in a DB table | Admin UI/migration burden for Phase 2; config-as-code is simpler to review |
| Prometheus Alertmanager only | Our metrics are gRPC-delivered to the collector, not Prometheus scrape targets; building a Prometheus remote-write bridge is out of scope for Phase 2 |

## Consequences

- Collector gains three new source files: `alert_rule`, `alert_manager`, `notifier` (under `collector/src/alerting/`).
- Collector CLI gains `--alert-rules-path` (and `--webhook-url` override for the notifier).
- HTTP endpoints `/alerts` and `/alert-history` are added to the collector.
- Alert state is in-memory only → a collector restart clears active alerts (acceptable for Phase 2; Phase 6 may persist state).
- The synchronous evaluation adds a small, bounded latency to ingestion (documented; will be moved to the Kafka consumer in Phase 3).
- Dashboard gains an alerts pane consuming the two new endpoints.

## Compliance

- Cross-cutting: ADR format per DEV_PLAN.md; unit tests for `AlertManager` state transitions (firing → repeat → resolved); integration via the existing gRPC E2E test.
- Observability: collector Prometheus `/metrics` gains `pudim_alerts_firing`, `pudim_alerts_total`, `pudim_alert_notifications_total`.
- Runbooks: `docs/runbooks/high-latency-alert.md` documents operator response.
