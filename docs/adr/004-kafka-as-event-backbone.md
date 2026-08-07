# ADR 004: Kafka as the Event Backbone

**Status:** Accepted
**Date:** 2026-08-07

## Context

PudimNetMon's collector currently stores metrics directly into TimescaleDB and
evaluates alert rules in-process. As the fleet scales, we want to decouple the
ingestion path from storage and alerting so that:

- Storage writes and alert evaluation never slow down the agent-facing gRPC path
- Storage and alerting can scale independently (multiple consumers per concern)
- The pipeline survives collector restarts (durable buffer) and can be replayed
- Multiple downstream consumers (storage, alerting, future diagnostics) can read
  the same metrics stream

## Decision

Introduce **Apache Kafka** as the event backbone between the collector and the
downstream consumers, replacing the collector's direct DB write + in-process
alerting path when Kafka mode is enabled.

### Architecture

```
Agent ──gRPC──▶ Collector ──produce──▶ Kafka ──consume──▶ pudim-consumer-storage ──▶ TimescaleDB
                       (network.metrics)                 └──▶ pudim-consumer-alert ──▶ AlertManager
```

### Key decisions

1. **Broker topology:** single-node **KRaft** broker (no ZooKeeper) in Docker
   Compose for local dev; production multi-broker documented in Phase 7.
2. **Client library:** **librdkafka** (the de-facto C/C++ Kafka client; the C++
   wrapper `RdKafka::Producer`/`RdKafka::KafkaConsumer`). Installed as
   `librdkafka-dev`, found via CMake's `FindRdKafka`.
3. **Topic layout:** one topic `network.metrics`. Messages are **keyed by
   `agent_id`** so all metrics for one agent go to the same partition → per-agent
   ordering is preserved. Partition count configurable (default 3 for dev).
4. **Serialization:** `MetricsBatch` serialized as **protobuf binary** (not
   JSON) — compact, strongly typed, zero schema drift (consumers use the same
   `.proto`).
5. **Delivery semantics:** **at-least-once**.
   - Consumers run with `enable.auto.commit=false` and `enable.auto.offset.store=false`.
   - Offsets are committed **only after a successful handler invocation**.
   - A failed insert → no commit → message redelivered on restart.
   - Duplicate redelivery is made harmless by **idempotent DB writes**
     (`INSERT ... ON CONFLICT DO NOTHING` keyed on the existing PK
     `(time, agent_id, check_type, target, seq)`).
   - Unparseable messages (poison pills) are committed-but-logged to avoid an
     infinite redelivery loop.
6. **Consumer groups:** independent groups `storage` and `alert`, each
   horizontally scalable (partitions split across group members).
7. **Collector role in Kafka mode:** becomes a thin gRPC gateway — it validates,
   assigns the collector timestamp, and produces to Kafka. It **does not** write
   to TimescaleDB or evaluate alerts (the consumers own those). Enabled by
   `--kafka-brokers=...`; when unset the collector keeps the Phase 1–2 direct
   path (backward compatible).
8. **Observability:** consumers expose a Prometheus endpoint
   (`/metrics` on `--http-addr`) with processed/error counters and consumer lag
   (watermark − position).

## Rationale

- **Decoupling:** storage latency and alert evaluation can no longer stall the
  gRPC handshake with agents.
- **Durability & replay:** Kafka retains the stream, so a storage consumer can
  be recreated and replay history — enabling backfill and disaster recovery
  (Phase 7).
- **Extensibility:** adding a new downstream consumer (e.g. diagnostic uploader
  in Phase 4) is a new consumer group with no changes to agents or collector.
- **Scalability:** consumer groups let us scale readers independently of the
  single collector.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Keep direct DB writes (status quo) | Couples ingestion to storage; alerting blocks the gRPC path; no replay |
| RabbitMQ / NATS | Weaker log semantics, no per-partition ordering guarantees needed for time-series replay |
| Kafka via Java/Go consumers | Adds a second language to the stack; librdkafka C++ keeps the whole pipeline C++ |
| Redis Streams | No strong ordering/partitioning for this use case; less battle-tested for long-lived event logs |

## Consequences

- `librdkafka-dev` becomes a required build dependency (CI and Dockerfiles updated).
- New binaries `pudim-consumer-storage` and `pudim-consumer-alert`.
- `TimescaleStorage` INSERT gains `ON CONFLICT DO NOTHING` for idempotency.
- Kafka mode is opt-in (`--kafka-brokers`); the direct path remains the default
  until the stack is flipped in Docker Compose.
- Alert state moves from collector memory to the alert consumer memory; the
  collector's `/alerts` endpoint is therefore only populated in direct mode.

## Compliance

- Cross-cutting: ADR format per DEV_PLAN.md; unit tests for producer/consumer
  round-trip and idempotent storage; E2E test with a real Kafka broker.
- Observability: consumer Prometheus metrics (processed, errors, lag).
- SLOs: metrics-delivery latency now includes the Kafka hop; consumer lag is the
  key control, surfaced in Grafana (Phase 6).
