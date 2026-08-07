# ADR 002: Time-Series Storage Choice — PostgreSQL + TimescaleDB

**Status:** Accepted  
**Date:** 2026-01-01

## Context

PudimNetMon agents will emit network metrics at regular intervals (DNS resolution time, TCP connect latency, TLS handshake time, HTTP response time, packet loss, RTT, jitter). The collector must store these metrics durably and serve them to the dashboard for time-series visualization.

Key requirements:

- High-throughput writes (hundreds of agents × multiple metrics every few seconds)
- Time-series query patterns (range queries, downsampling, retention policies)
- Relational joins when needed (metrics ↔ agents ↔ alert rules)
- Operational simplicity (single database to run and back up)
- SQL query ability for ad-hoc analysis and future alerting rules
- Dashboard read path must be fast (p95 < 100ms for typical dashboard queries)

## Decision

We will use **PostgreSQL with the TimescaleDB extension** as the primary time-series storage backend.

## Rationale

### Why TimescaleDB over plain PostgreSQL?

- **Hypertables:** Transparent chunking by time interval. Writes and queries are partitioned automatically without application changes.
- **Compression:** TimescaleDB's native compression reduces storage footprint for older chunks (configurable retention/compression policies).
- **Continuous aggregates:** Built-in downsampling (e.g., `1m`, `5m`, `1h` averages) for dashboard graphs, dramatically reducing query load on raw data.
- **Retention policies:** Automated `drop_chunks` policies align with the SLO/cost analysis goals from DEV_PLAN Phase 7.
- **Drop-in SQL:** All standard PostgreSQL features remain available (JOINs, window functions, indexes, extensions).
- **Operational familiarity:** The team already knows PostgreSQL administration, tooling (pg_dump, psql), and ecosystem (connection pools, monitoring).

### Why TimescaleDB over InfluxDB?

| Criterion | TimescaleDB | InfluxDB |
|---|---|---|
| Query language | SQL (universally known) | Flux/InfluxQL (proprietary) |
| JOINs with relational data | Native | Limited |
| Operational model | Single PostgreSQL instance | Separate InfluxDB daemon, custom backup tooling |
| Ecosystem | Huge PostgreSQL ecosystem | Smaller, specialised |
| Compression | Chunk-based, configurable | Built-in, less control |
| License | TimescaleDB Community Edition (TSL) | MIT (InfluxDB OSS) |

### Why TimescaleDB over ClickHouse?

| Criterion | TimescaleDB | ClickHouse |
|---|---|---|
| SQL | Full SQL | SQL-like (subset) |
| Write throughput | Very high (batches) | Extremely high |
| Operational complexity | Low (PG) | High (distributed setup, ZK, sharding) |
| Point updates/deletes | Supported | Limited |
| Use case fit | This scale (100–1000 agents) | Petabyte-scale analytics |
| Dashboard read latency | Excellent with aggregates | Excellent |

### Why TimescaleDB over a hand-rolled ring buffer / SQLite?

- SQLite lacks network access, concurrent writers, and is a bad fit for a multi-agent collector.
- A custom ring buffer would sacrifice durability and make dashboard queries unnecessarily complex.

## Implementation Notes

- Use `libpq` (or `libpqxx` for RAII wrappers) from the C++ collector.
- Schema:
  - `agents` table (agent metadata, static per fleet)
  - `network_metrics` hypertable with columns: `time`, `agent_id`, `check_type`, `target`, `value_ms` (or `value_double`), plus tags/metadata as JSONB
  - Continuous aggregate views for `1m`, `5m`, `1h` downsampling
- Write path: batch inserts in transactions (e.g., 1000 rows per tx). If throughput exceeds design limits (Phase 6 overload handling), a bounded in-memory queue + periodic flush.
- Indexes: primary on `(agent_id, check_type, time DESC)` for dashboard per-agent graphs.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| **InfluxDB** | Proprietary query language, weaker relational joins, additional operational burden |
| **ClickHouse** | Overkill for this scale, higher operational complexity |
| **Kafka-only (no DB)** | Kafka is for streaming/decoupling, not queryable storage; dashboard needs indexed historical queries |
| **MongoDB (time-series collections)** | Schema flexibility is not needed; SQL analytics and joins are stronger in PostgreSQL |

## Consequences

- Collector implementation will use `libpq`/`libpqxx`; CI must install PostgreSQL client dev packages.
- Docker Compose gains a `timescaledb` service (image `timescale/timescaledb:latest-pg16`).
- Migration scripts are plain SQL (`infra/db/migrations/0001_init.sql`), versioned in the repo.
- Dashboard queries will use continuous aggregates for large time ranges.
- Retention and compression policies will be defined in SQL and documented in `docs/capacity.md` (Phase 7 cost analysis).

## Compliance

- Cross-cutting: ADR format per DEV_PLAN.md; testing of storage layer via integration tests in CI (PostgreSQL service container or Docker).
- Observability: collector will emit `pudim_storage_insert_latency_ms`, `pudim_storage_batch_size`, `pudim_storage_errors_total` Prometheus metrics.
- SLOs: storage write latency contributes to the "metrics delivered within 10s" SLO.