# PudimNetMon Architecture (C4 model)

PudimNetMon is a distributed network-monitoring platform: lightweight C++
agents run deep connectivity probes (DNS, TCP, TLS, HTTP, ICMP, pcap, NTP),
stream metrics to a collector, which persists to TimescaleDB, fans out to
Kafka, evaluates alert rules, and serves a React dashboard.

This document follows the [C4 model](https://c4model.com/): Context →
Container → Component.

---

## Level 1 — System Context

```mermaid
C4Context
  title System Context — PudimNetMon
  Person(admin, "Network Admin", "Monitors infrastructure health, responds to alerts, runs diagnostics")
  Person(agentHost, "Agent Host", "A Linux host/VPN endpoint running the agent")

  System(mon, "PudimNetMon", "End-to-end network monitoring: metrics, alerting, diagnostics, failover, DR")

  System_Ext(internet, "Internet / External services", "example.com, 1.1.1.1, SaaS endpoints, TLS/HTTP targets")
  System_Ext(tools, "Ops Tooling", "Prometheus, Grafana, PagerDuty/webhook")

  Rel(agentHost, mon, "streams metrics + heartbeats (gRPC, mTLS)")
  Rel(mon, internet, "probes: DNS/TCP/TLS/HTTP/ICMP")
  Rel(admin, mon, "uses dashboard (HTTP /api)")
  Rel(mon, tools, "exposes /metrics, fires webhooks")
  UpdateLayoutConfig($c4ShapeInRow="2", $c4BoundaryInRow="2")
```

---

## Level 2 — Containers

```mermaid
C4Container
  title Container diagram — PudimNetMon

  Person(admin, "Network Admin", "Dashboard user")
  System_Ext(tools, "Ops Tooling", "Prometheus / Grafana")

  Container_Boundary(platform, "PudimNetMon platform") {
    Container(agent, "Agent", "C++17, gRPC", "Deep probes, streaming metrics, disk buffer (SQLite), failover client, diagnostic server")
    Container(collector, "Collector", "C++17, gRPC/HTTP", "Heartbeat registry, metric ingest, REST API (/api), Prometheus /metrics, alert evaluation (in memory-only mode)")
    Container(kafka, "Kafka", "Apache Kafka + ZooKeeper", "Metrics event backbone; at-least-once, idempotent consumers")
    Container(storageConsumer, "Storage Consumer", "C++17 (rdkafka)", "Persists metrics to TimescaleDB")
    Container(alertConsumer, "Alert Consumer", "C++17 (rdkafka)", "Evaluates alert rules from the stream")
    Container(db, "TimescaleDB", "PostgreSQL + hypertables", "Metric time-series storage (JSONB attributes, compression)")
    Container(dashboard, "Dashboard", "React 18 + Recharts + Vite", "Health, charts, alerts, diagnostics, agent config, alert ack")
  }

  Rel(agent, collector, "heartbeat + metrics (gRPC, mTLS)", "50051")
  Rel(agent, collector, "diagnostic + reconfigure (gRPC, mTLS)", "50052")
  Rel(collector, kafka, "produce network.metrics")
  Rel(kafka, storageConsumer, "consume")
  Rel(kafka, alertConsumer, "consume")
  Rel(storageConsumer, db, "INSERT (batch)")
  Rel(alertConsumer, tools, "webhook notifications")
  Rel(collector, db, "direct write when Kafka disabled")
  Rel(dashboard, collector, "REST /api/* (JSON)")
  Rel(admin, dashboard, "uses")
  Rel(tools, collector, "scrapes /metrics")
  UpdateLayoutConfig($c4ShapeInRow="3", $c4BoundaryInRow="2")
```

---

## Level 3 — Collector (component view)

```mermaid
C4Component
  title Component diagram — Collector

  Container_Boundary(col, "Collector (pudim-collector)") {
    Component(grpc, "gRPC Services", "AgentService / MetricsService", "heartbeat registry, metric ingest, x-overloaded backpressure")
    Component(rest, "REST API", "cpp-httplib", "/api/health, /api/agents, /api/metrics, /api/alerts(+/ack), /api/alert-history, /api/alert-rules, /api/agents/config, /api/diagnostic")
    Component(prom, "Prometheus exporter", "text format", "pudim_* counters (heartbeats, alerts, lag)")
    Component(alert, "AlertManager", "state machine", "rules, firing/repeat/resolved, ack, webhook+log notifiers")
    Component(producer, "Kafka Producer", "librdkafka", "publishes metric batches")
    Component(diagclient, "Diagnostic Client", "gRPC", "forwards traceroute/pcap/reconfigure to agents")
    Component(storage, "TimescaleStorage", "libpq", "direct SQL when Kafka disabled")
  }

  Rel(grpc, alert, "evaluate()")
  Rel(grpc, producer, "produce()")
  Rel(grpc, storage, "insert()")
  Rel(rest, alert, "snapshots + Ack()")
  Rel(rest, diagclient, "RunDiagnostic/Reconfigure/GetConfig")
  Rel(alert, External("webhook", "Webhook endpoint"), "Notify()")
```

---

## Level 3 — Agent (component view)

```mermaid
C4Component
  title Component diagram — Agent

  Container_Boundary(ag, "Agent (pudim-agent)") {
    Component(probes, "Probes", "libpcap/openssl/curl/DNS", "DNS, DNS-record, TCP connect, TCP retransmit, TCP handshake, TLS, TLS cert, HTTP/1.1/2/3, ICMP loss/RTT/jitter, NTP offset")
    Component(stream, "Metrics Client", "gRPC", "unary or client-streaming, x-agent-id + W3C traceparent")
    Component(hb, "Heartbeat Client", "gRPC", "registers node, advertises diagnostic endpoint")
    Component(fail, "Failover Client", "multi-endpoint", "3-strike failover across collector list")
    Component(buf, "Buffering", "deque + SQLite", "in-memory FIFO, disk spill, FIFO drain on reconnect")
    Component(diag, "Diagnostic Service", "gRPC server", "traceroute/pcap + Reconfigure/GetConfig (Phase 8)")
    Component(ntfy, "systemd integration", "sd_notify", "Type=notify, watchdog")
  }

  Rel(probes, stream, "batches")
  Rel(hb, fail, "endpoint selection")
  Rel(stream, buf, "overflow spill")
  Rel(diag, probes, "runtime config (ProbeConfigStore)")
```

---

## Key flows

1. **Ingest** — agent probes each interval → `MetricsBatch` → collector gRPC →
   Kafka topic `network.metrics` → storage + alert consumers (or direct SQL +
   inline alerting when Kafka is disabled).
2. **Failover / DR** — agent monitors its collector list; on 3 failures it
   switches to the next endpoint. Overflowing batches spill to the SQLite disk
   buffer and drain oldest-first on reconnect (RPO = buffer contents).
3. **Backpressure** — when collector ingest latency exceeds the threshold, the
   gRPC response sets `x-overloaded`; the agent backs off its interval.
4. **Diagnostics (Phase 4/8)** — dashboard POSTs to the collector, which
   forwards to the agent's DiagnosticService (mTLS): traceroute, short pcap
   capture, and runtime probe reconfiguration.
5. **Alerting** — rules evaluated per metric batch; transitions logged to the
   bounded history; `POST /api/alerts/ack` marks a firing alert acknowledged.

## Deployment (docker-compose)

```mermaid
flowchart LR
  subgraph stack["docker-compose network"]
    ag1[agent :50052 diag] --> col[collector :50051/:8080]
    ag2[agent-2] --> col
    col --> kafka[Kafka :9092]
    kafka --> sc[storage-consumer] --> db[(TimescaleDB :5432)]
    kafka --> ac[alert-consumer]
    dash[dashboard :3000] -->|/api/*| col
    prom[prometheus] -->|/metrics| col
    graf[grafana :3100] --> prom
  end
```

Every agent↔collector RPC is mutually authenticated with `--tls-*` (see
[ADR 010](adr/010-mtls-agent-collector.md) and
[certificate-rotation.md](certificate-rotation.md)).

