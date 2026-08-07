# Tradeoff Documents

Every significant architecture decision in PudimNetMon is captured as an
[Architecture Decision Record (ADR)](adr/). This page is the index: one line
per ADR with the decision and the key tradeoff it resolved.

| ADR | Decision | Key tradeoff resolved |
|---|---|---|
| [001 — Choose C++ and gRPC for Agent and Collector](adr/001-choose-cpp-and-grpc.md) | C++17 agent + collector, protobuf/gRPC between them | Throughput & binary footprint of a streaming agent vs Go/Rust/Python. C++ chosen for zero-dependency probes and low idle cost; gRPC over REST for typed streaming RPCs. |
| [002 — Time-Series Storage: PostgreSQL + TimescaleDB](adr/002-timeseries-storage-choice.md) | PostgreSQL with the TimescaleDB extension | One SQL engine for relational + time-series data (no separate TSDB to operate); compression + hypertables for retention, vs InfluxDB/Prometheus-with-long-term-storage. |
| [003 — Alerting System Design](adr/003-alerting-design.md) | Collector-owned alert state machine with rule files, repeat intervals, webhook+log notifiers | Alerting where the data is (in-process, sub-second) vs a separate Alertmanager pipeline. Cheap and demo-friendly; re-evaluated in ADR 004 for the Kafka split. |
| [004 — Kafka as the Event Backbone](adr/004-kafka-as-event-backbone.md) | Kafka topic `network.metrics`; storage + alerting consumers own persistence | Decouples ingest from persistence so storage lag can't block probes; buys replay/idempotency and multi-consumer fan-out at the cost of a broker to operate. |
| [005 — Raw Socket Capabilities and Security](adr/005-raw-socket-capabilities-and-security.md) | libpcap for handshake capture; `CAP_NET_RAW` only; unprivileged user | Deep packet-level timing vs least privilege. Capability-scoped agent (non-root, `NoNewPrivileges`, bounding set) keeps a compromised probe from becoming root. |
| [006 — Time Handling Across Agents](adr/006-time-handling-across-agents.md) | NTP drift probe (`ntp_adjtime`), clock-skew warnings, collector-assigned timestamps | Correct cross-host time-series ordering vs trusting each agent's wall clock. Collector timestamps are authoritative; skew is measured and surfaced, not papered over. |
| [007 — Service Discovery Strategy](adr/007-service-discovery-strategy.md) | DNS-based discovery (SRV/round-robin) + Kubernetes service discovery; agent-side failover list | Zero-config endpoint discovery vs a registry to operate. Documented (not implemented) for K8s; agent `--collector-endpoints` provides the runtime failover path. |
| [008 — Backpressure and Overload Handling](adr/008-backpressure-and-overload-handling.md) | `x-overloaded` gRPC metadata, adaptive interval, bounded FIFO with drop-oldest | Collector overload vs agent data loss. Signalling + buffering keeps the pipeline stable; the bounded buffer caps memory (documented drops are preferred to OOM). |
| [009 — Multi-Region Disaster Recovery](adr/009-multi-region-dr-strategy.md) | Active/passive regions, agent disk buffer (SQLite) as the RPO boundary, DR drill | Availability vs cost of a hot replica. The agent's disk buffer spans collector outages, so RPO is bounded by buffer capacity rather than a replicated region. |
| [010 — Mutual TLS between Agent and Collector](adr/010-mtls-agent-collector.md) | mTLS on every agent↔collector gRPC hop; shared self-signed CA; graceful insecure fallback | Authenticated+encrypted data plane vs operational simplicity. TLS-native gRPC (no proxy/mesh); `--tls-*` flags, CA minted by `scripts/gen-certs.sh`, rotation documented. |

## Portfolio answers

- **"Why not a service mesh?"** — mTLS is implemented directly in gRPC (OpenSSL)
  with two flags and one script; a mesh (Istio/Linkerd) adds a control plane the
  project doesn't need. If the fleet grows to many teams, ADR 010's rotation doc
  points to Vault/cert-manager as the production PKI.
- **"Why SQL + Timescale instead of a TSDB?"** — one engine to operate, SQL for
  joins/aggregates, compression for retention. The 1000-agent capacity plan
  (≈ 8.6 MB/day/agent) fits comfortably.
- **"Why an in-process alert evaluator AND Kafka consumers?"** — ADR 004 keeps
  alerting independent of ingest; the collector still evaluates inline when
  Kafka is disabled so `docker compose up` works without a broker.
