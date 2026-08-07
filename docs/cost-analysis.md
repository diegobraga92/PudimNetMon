# Cost Analysis & Capacity Planning

> Monthly cost projection for PudimNetMon on AWS (illustrative list pricing,
> us-east-1, 2026). Adjust for your region/commitments. Companion to
> `docs/adr/009-multi-region-dr-strategy.md`.

## Capacity model

Per agent per cycle (all deep probes enabled):

| Item | Value |
|---|---|
| Metrics per batch | ~10 (dns, dns_record, tcp, retransmit, handshake, tls, tls_cert, http×protocols, ntp) |
| Protobuf batch size | ~1 KB (5–10 KB with attributes) |
| Batch rate | 1 / interval (0.1/s at 10 s, 1/s at 1 s) |
| Bytes/s (10 s interval) | ≈ 100 B/s → ≈ 8.6 MB/day |

Scaling:

| Fleet | Metrics/s | KB/s | Storage/day (raw) | Kafka partitions |
|---|---|---|---|---|
| 100 agents | ~100 | ~10 | ~0.9 GB | 3 |
| 1,000 agents | ~1,000 | ~100 | ~9 GB | 6 |

TimescaleDB compression (~10:1 on network metrics) reduces raw storage ~10×.

## Monthly cost (single region)

| Component | Choice | 100 agents | 1,000 agents |
|---|---|---|---|
| Collector (2× for HA) | EC2 t3.small | $34 | $34 (vertical or +instances) |
| Agent compute | on existing hosts (t3.nano equivalent) | $0 (agents piggyback) | $0 |
| Kafka (managed, 3 brokers) | MSK kafka.t3.small | $159 | $318 (kafka.m5.large) |
| TimescaleDB | RDS db.r6g.large (2 vCPU/16GB, 100 GB gp3) | $258 | $516 (2×) |
| S3 (pcap/diagnostics, 100 GB) | standard + lifecycle | $2 | $10 |
| **Total** | | **≈ $453/month** | **≈ $878/month** |

Optimisation levers:

| Lever | Saving | Risk |
|---|---|---|
| Spot instances for collectors | ~60% of EC2 | interruption → rely on failover |
| Self-host Kafka on EC2 | ~50% vs MSK | ops burden |
| TimescaleDB retention (drop >30 d) | shrinks RDS storage | history loss beyond 30 d |
| TimescaleDB continuous aggregates (1 m/5 m) | faster queries, less raw storage | downsampling loses sub-minute precision |
| Kafka tiered storage (old segments → S3) | lower broker disk | slight query latency |
| Shared collectors per region (1 per 1000 agents) | fewer instances | single point of failure without HA pair |

## Capacity planning notes

- **Collector**: handles ~1000 metrics/s easily on t3.small; the real limiter is
  DB insert latency. Batch inserts (`--batch-size`) and the `x-overloaded`
  backpressure signal (ADR 008) keep it stable.
- **Kafka**: partition key = agent_id ⇒ per-agent ordering. 3 partitions handle
  ~100 agents each; scale to 6 at 1,000 agents. Retention = 7 days × ingress.
- **Storage**: 8.6 MB/day/agent raw → 1000 agents ≈ 9 GB/day → ~270 GB/mo → ~27 GB
  with compression. 30-day retention ≈ 810 GB raw / 81 GB compressed.
- **Dashboard**: use continuous aggregates for >1 h windows to keep p95 query
  time < 100 ms.

## References

- `docs/adr/009-multi-region-dr-strategy.md` — DR topology, RTO/RPO
- `docs/adr/002-timeseries-storage-choice.md` — TimescaleDB compression/aggregates
- `docs/slo.md` — delivery SLOs that drive retention decisions
