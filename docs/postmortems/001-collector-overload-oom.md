# Postmortem 001: Collector Overload-Induced OOM

**Date:** 2026-08-07
**Severity:** SEV-2 (degraded ingestion, no data loss beyond SLO window)
**Status:** Closed — action items tracked

## Summary

A sustained burst of high-frequency metrics (10 agents × 500 ms intervals,
large probe batches) saturated the collector's TimescaleDB write path. The
collector's memory footprint grew beyond the systemd `MemoryMax=256M` backstop,
systemd killed it, and the collector restarted. Agents buffered and failed over
(ADR 008), and Kafka retained the stream, so post-restart ingestion recovered
with no permanent loss beyond the agent buffer cap.

## Timeline (UTC)

| Time | Event |
|---|---|
| 10:00:00 | `overload-collector.sh 10 500` started |
| 10:00:45 | Collector RSS climbing; `pudim_metrics_received_total` ~2× baseline |
| 10:01:20 | Insert latency spikes (`pudim_storage_insert_latency_total_ms` rate ↑) |
| 10:01:30 | Collector killed by systemd (`MemoryMax` exceeded); `Restart=always` respawns it |
| 10:01:35 | Agents observe `SendMetrics` failure → fail over + buffer (drops logged) |
| 10:01:40 | Collector back up; Kafka consumer lag high |
| 10:03:00 | Lag drained to 0; agents restore configured interval |

## Root Cause

The collector had no application-level admission control: every batch was
accepted and synchronously written to TimescaleDB. Under sustained load the
write queue grew, allocations grew, and the systemd memory limit was the only
backstop. The `x-overloaded` backpressure signal (ADR 008) was not yet wired at
the time of the incident (this postmortem drove its implementation).

## Detection

- Systemd journal: `collector.service: Memory cgroup limit reached`.
- Prometheus: RSS gauge (if scraped) and `pudim_metrics_received_total` plateau.
- Agents: `Buffer full; dropped oldest metric batch` WARNING logs.

## Impact

- ~60 s of ingestion degradation; agent buffers (200 batches) absorbed most of
  it. No metrics lost beyond the buffer cap (oldest dropped).
- SLO: heartbeat delivery success remained above 99.9% per 5-min window (agents
  retried), so the error budget was not consumed.

## Recovery

- Automatic: systemd restarted the collector; agents reconnected and drained
  FIFO; Kafka replay let the storage consumer catch up.

## Lessons

1. **Backpressure must be proactive**, not just a memory-limit backstop.
2. **Agent buffering is the resilience boundary** — bounded buffers prevent
   agent-side OOM and provide the replay window.
3. **Kafka is the durable buffer** — a restarted consumer replays from committed
   offsets with zero loss.

## Action Items

- [x] Implement `x-overloaded` gRPC backpressure signal + agent adaptive
      interval (ADR 008).
- [x] Document overload simulation (`scripts/overload-collector.sh`).
- [x] Add `pudim_backpressure_signals_sent_total` to collector Prometheus.
- [ ] (Phase 6 Grafana) Panel for collector RSS + insert latency to alert
      before the memory limit is reached.
