# ADR 006: Time Handling Across Agents

**Status:** Accepted
**Date:** 2026-08-07

## Context

Distributed agents report timestamps in heartbeats and metric batches. Wall
clocks on individual agents drift (NTP skew, VM pause, manual adjustments), so
naively trusting agent timestamps corrupts time-series ordering and alert
semantics. We need a coherent time strategy.

## Decision

1. **Collector-assigned timestamps are the source of truth for storage.**
   When the collector ingests a `MetricsBatch`, it stamps the rows with its own
   `system_clock` at receipt (`MetricsServiceImpl::IngestBatch`), regardless of
   the agent's reported `timestamp_unix_ms`. The agent's clock is preserved
   only as metadata for debugging (e.g. `metric.monotonic_us`, and the batch
   timestamp in logs).

2. **Clock-skew detection at the collector.** On the unary `SendMetrics` path,
   the collector computes `skew = |collector_now - agent_timestamp_unix_ms|`.
   If `skew > --skew-threshold-ms` (default 5000), it logs a WARNING with the
   skew and increments the `pudim_clock_skew_warnings_total` Prometheus counter.

3. **NTP drift is measured on the agent with `ntp_adjtime()`** (not a network
   SNTP query). The kernel's disciplined clock exposes the current offset
   (`struct timex.offset` in µs), plus `maxerror`/`esterror`. This requires no
   network, no extra capability, and reflects what the kernel actually believes
   its clock error is. The agent emits a `CHECK_TYPE_NTP_OFFSET` metric every
   collection cycle.

4. **Synchronisation state is surfaced.** `ntp_synchronised` (from
   `STA_UNSYNC`) is reported as a metric attribute so dashboards/alerting can
   distinguish "clock not synced at all" from "synced with small offset".

## Rationale

- **Collector as authority:** the collector sees all agents and has the most
  controlled environment; its clock is the comparison baseline for cross-agent
  alerting. Storing collector time makes queries/joins consistent even if an
  agent's clock is wildly wrong.
- **`ntp_adjtime()` vs SNTP:** SNTP adds a network dependency and another source
  of failure; the kernel already maintains the best estimate from whichever
  discipline source is configured (chrony/systemd-timesyncd/ntpd), so reading it
  is cheaper and more accurate than a one-shot network sample.
- **Skew threshold as config:** the default 5s bounds false positives from
  legitimate small drift while still catching VM pause/large manual changes.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Trust agent timestamps end-to-end | Broken when an agent's clock is wrong; corrupts storage ordering |
| Network SNTP client in the agent | Extra dependency, needs UDP egress to NTP servers, no benefit over kernel state |
| OTel-style semantic conventions only | Overkill for our storage schema; collector-now is sufficient as source of truth |

## Consequences

- `CHECK_TYPE_NTP_OFFSET` (enum 11) added to `metrics.proto`; agent probes it
  each cycle; the dashboard gains an NTP offset chart per agent.
- Collector CLI gains `--skew-threshold-ms`; `/metrics` gains
  `pudim_clock_skew_warnings_total`.
- Storage timestamps are now collector-assigned on BOTH unary and streaming
  paths (previously the unary path used the agent's batch timestamp).
- Runbook/alerting: a sustained NTP offset beyond ±5s is actionable and now
  visible in the dashboard + a potential `ntp_offset` alert rule.

## Compliance

- ADR format per DEV_PLAN.md; implementation covered by agent probe tests and
  collector integration tests; dashboard section documents the NTP chart.
