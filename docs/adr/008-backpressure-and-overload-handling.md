# ADR 008: Backpressure and Overload Handling

**Status:** Accepted
**Date:** 2026-08-07

## Context

Agents send metric batches to the collector (directly or via Kafka). Under
sustained load — many agents, short intervals, or a slow storage/Kafka path —
the collector or its consumers can saturate, causing memory growth, dropped
metrics, or OOM. We need a defined strategy for degrading gracefully rather
than failing hard.

## Decision

### 1. Agent-side bounded buffer

- The agent keeps an in-memory `std::deque<MetricsBatch>` capped at
  `--max-buffer-size` (default 200).
- Each cycle pushes the new batch, then **drops the oldest** while over the cap
  (FIFO). Each drop increments a structured-log counter
  (`pudim_agent_buffer_drops_total`) and logs a WARNING.
- Sends drain the buffer FIFO, so failed deliveries are retried (bounded) rather
  than silently lost; when the buffer is full, the oldest/lowest-value data is
  sacrificed first.

### 2. Collector backpressure signal (`x-overloaded`)

- The collector measures `IngestBatch` latency. If it exceeds
  `--backpressure-threshold-ms` (default 1000), it adds gRPC trailing metadata
  `x-overloaded: true` to the response.
- The agent's `MetricsClient` reads this metadata and exposes
  `BackpressureSignalled()`.

### 3. Agent adaptive interval

- While the collector signals overload, the agent **doubles its sleep interval**
  each cycle, capped at 10× the configured interval (i.e. 5s → 10s → 20s → …
  → 50s).
- The first successful cycle without backpressure restores the configured
  interval. This is a simple, effective global backoff — no per-metric
  prioritisation in this phase.

### 4. Memory safeguards

- systemd enforces `MemoryMax=256M` and `CPUQuota=50%` (Phase 5) as a backstop
  that kills a runaway agent before it degrades the host.
- The bounded buffer guarantees the agent's per-cycle memory footprint is
  independent of how long the collector is unreachable.

### 5. Metric dropping policy (documented)

| Tier | Contents | Behaviour when buffered data is dropped |
|---|---|---|
| 1 (always kept) | heartbeat, NTP offset, DNS/TCP/TLS/HTTP basic probes | never dropped first |
| 2 (drop-first) | HTTP protocol comparison, TCP handshake capture | dropped when the buffer is full |

Because batches are dropped **oldest-first**, tier-2 data (which is also the
bulk of each cycle) is effectively sacrificed before tier-1 when the buffer is
full. A future refinement can implement intra-batch prioritisation.

## Rationale

- **Oldest-first + bounded buffer** is the simplest correct strategy: it bounds
  memory, retries failed sends, and degrades to "keep recent data" under stress.
- **gRPC trailing metadata** is the natural channel for a server→client signal;
  no extra transport, and the client already inspects response metadata.
- **Adaptive interval** gives global backpressure without coordinating across
  agents — each agent reacts to its own collector response.
- **systemd limits** are the final backstop; application-level limits come first
  so the daemon never gets killed unnecessarily.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| HTTP 429 to agents | Our transport is gRPC; trailing metadata is the idiomatic signal |
| Per-metric priority inside a batch | More complex; drops are already weighted by batch composition |
| Unbounded agent buffer | Memory growth → OOM, exactly what we are avoiding |
| Kafka client internal backpressure only | Only helps the consumer hop; the agent→collector hop needs its own signal |

## Consequences

- `--max-buffer-size`, `--backpressure-threshold-ms` CLI flags.
- `pudim_agent_buffer_drops_total` (agent structured log) and
  `pudim_backpressure_signals_sent_total` (collector Prometheus).
- Overload simulation scripts: `scripts/overload-collector.sh`,
  `scripts/overload-kafka.sh`.
- Related to ADR 006 (skew) and Phase 7 DR (buffering for multi-region).

## Compliance

- ADR format per DEV_PLAN.md; overload handling exercised by chaos experiments
  (Phase 6) and documented in `docs/chaos-experiments.md`.
