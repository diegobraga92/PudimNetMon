# ADR 009: Multi-Region Disaster Recovery Strategy

**Status:** Accepted
**Date:** 2026-08-07

## Context

A single-region deployment is a single point of failure. PudimNetMon should
survive a regional outage of the collector and its storage/Kafka, and agents
must not lose metrics during the outage or the cutover.

## Decision

1. **Active/passive regions with agent-side failover.**
   Each region runs its own collector, Kafka, and TimescaleDB (active in one
   region, passive in the other). Agents hold a prioritized endpoint list
   (`--collector-endpoints=primary:50051,secondary:50052`); the Phase 5
   FailoverClient rotates to the next endpoint after 3 consecutive send
   failures. The passive region's collector can be pre-warmed (already connected
   to its own Kafka) to make the cutover a short client-side switch.

2. **Agent disk buffer (SQLite) is the RPO boundary.**
   When the collector is unreachable:
   - the in-memory FIFO buffer (default 200 batches) absorbs the first ~33 min
     (at 10 s interval);
   - overflow spills to a local SQLite database (`/var/lib/pudim/pending.db`,
     cap 100 MB, oldest deleted first);
   - on reconnect the agent drains the disk buffer oldest-first.
   This bounds memory, survives agent restarts, and gives an RPO of **hours**
   rather than minutes.

3. **Storage semantics remain collector-authoritative** (ADR 006): the
   recovering collector stamps received batches with its own clock, so a
   secondary-region ingest is time-consistent even if the agent's clock was
   wrong during the outage.

4. **Durability is additive:** Kafka provides the cross-region replay for
   consumers; the agent disk buffer provides the agent-side replay; `ON CONFLICT
   DO NOTHING` (ADR 004) makes any duplicate delivery harmless.

## Targets

| Metric | Target | Basis |
|---|---|---|
| **RTO** | ≈ 3 × agent interval (~15–30 s) | 3-strike heartbeat failover |
| **RPO** | 0 for disk-buffered data; ≤ in-memory cap for the rest | SQLite + bounded FIFO |

## Rationale

- **SQLite over memory-mapped ring buffer:** SQLite is crash-safe (WAL), already
  familiar, and gives bounded durable storage with a few lines — no bespoke
  mmap/CRC code.
- **Agent-side failover over DNS-only:** DNS re-resolution is passive; an
  explicit endpoint list gives a deterministic, testable cutover and prefers the
  primary on recovery (list wrap-around).
- **Active/passive over active/active:** half the Kafka/DB cost; the passive
  collector still needs a live Kafka for pre-warm, so the marginal cost of
  active/active is mostly the DB — acceptable to defer.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Memory-mapped ring buffer only | Not crash-safe; loses data on agent restart |
| SQLite for the whole history | DB grows unbounded; disk cap + FIFO is enough |
| Active/active with cross-region replication | Complex (DB + Kafka replication), costly; no benefit for this scale |
| Kafka replication across regions (MM2) | Adds a cross-region Kafka requirement before agents even fail over |

## Consequences

- Agent build optionally links SQLite3 (`HAVE_SQLITE3`); without it the agent
  degrades to in-memory buffering.
- New CLI: `--disk-buffer-path`, `--disk-buffer-max-mb`.
- Compose gains a `collector-secondary` service for the DR drill
  (`docs/dr-test.md`).
- `docs/cost-analysis.md` documents the fleet cost at 100/1000 agents.

## Compliance

- ADR format per DEV_PLAN.md; the DR drill (RTO/RPO measurement) is documented
  in `docs/dr-test.md`; cost model in `docs/cost-analysis.md`.
