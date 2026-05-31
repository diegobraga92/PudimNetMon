# Network Tracker – Development Plan (DEV_PLAN.md)

> Distributed network monitoring platform: C++ agents (Linux daemons), C++ central collector, Kafka event backbone, TypeScript/React web dashboard.
> Flagship project for Systems Engineering, Infrastructure, and SRE roles.
> Deep networking, kernel tuning, overload handling, time synchronisation, service discovery, chaos engineering, and operational excellence.

---

## Cross‑Cutting Engineering Practices (applied throughout)

- **Architecture Decision Records (ADRs):** Every significant choice (Kafka, protocol design, daemon model, time‑series storage, overload handling, time sync, service discovery) documented in `docs/adr/`
- **Design Documents (RFCs):** Pre‑implementation for agent‑collector protocol, Kafka topic design, failure modes, backpressure
- **Tradeoff Documents:** `docs/tradeoffs.md` summary linking to ADRs
- **Testing:** Unit, integration, chaos/load tests; CI quality gates
- **Observability:** OpenTelemetry traces, Prometheus metrics, structured JSON logs; RED dashboards for collector and agent health
- **SLOs & Error Budgets:** Defined for data delivery (e.g., 99.9% of metrics delivered within 10s), alerting on burn rate
- **Incident Runbooks:** For agent disconnection, collector overload, Kafka slowdown, DNS resolution failures, clock skew anomalies
- **Blameless Postmortems:** At least two simulated incidents (e.g., collector overload-induced OOM, alert storm from clock drift)
- **CI/CD & GitOps:** GitHub Actions, Docker builds, Kubernetes deployments (DaemonSet for agents), ArgoCD (optional)
- **IaC:** Terraform for cloud instances, Kubernetes cluster, managed Kafka (or self‑hosted)
- **Capacity Planning:** Throughput per agent, Kafka partition sizing, storage retention, cost projection
- **Stakeholder Communication:** README explains the system to SRE teams and management

---

## Security Requirements (Implemented Throughout)

- **Mutual TLS (mTLS):** Agent‑to‑collector and collector‑to‑Kafka communication encrypted and authenticated
- **Certificate Management:** Short‑lived certificates, rotation via Vault or cert‑manager; documented rotation procedure
- **Network Policies:** Kubernetes network policies restricting agent/collector traffic
- **Secrets Management:** Credentials never in code; environment variables / Vault
- **Audit Logs:** Collector logs connection attempts, certificate expirations, configuration changes
- **Vulnerability Scanning:** Static analysis for C++ (clang‑tidy, cppcheck), container scanning (Trivy) in CI
- **Least Privilege:** Agents run as non‑root (capabilities for raw sockets only), collector minimal IAM roles

---

## Phase 0 – Skeleton, Build System & Local Environment (2–3 days)

**Goal:** A single agent, collector, and dashboard that compile, run, and exchange a heartbeat.

- [ ] Monorepo: `agent/`, `collector/`, `dashboard/`, `api/`, `infra/`, `docker-compose.yml`
- [ ] Agent (C++):
  - [ ] CMake build, basic main loop, structured logging (spdlog or custom)
  - [ ] systemd unit file for daemon mode
  - [ ] Command‑line flags: collector endpoint, node ID, interval
  - [ ] Send heartbeat gRPC/protobuf message to collector
- [ ] Collector (C++):
  - [ ] CMake build, gRPC server, receive heartbeats, log them
  - [ ] In‑memory agent registry (agent ID, last seen)
  - [ ] Health HTTP endpoint (`/health`, `/metrics`)
- [ ] Dashboard (TypeScript + React + Vite):
  - [ ] Scaffold, call collector `/health`, display connection status
- [ ] API contracts: Protobuf definitions for heartbeat, metrics (check in `api/proto/`)
- [ ] Docker Compose: collector + dashboard + dummy agent (or run agent natively)
- [ ] CI/CD: GitHub Actions for C++ build/test, dashboard lint/build
- [ ] Observability seed: collector Prometheus `/metrics` endpoint, structured logging with trace IDs
- [ ] SLO draft: agent heartbeat delivery success rate, document in `docs/slo.md`
- [ ] ADR: `001-choose-cpp-and-grpc.md`

---

## Phase 1 – Core Metrics Collection & Storage (2–3 weeks)

**Goal:** Agents measure real network metrics; collector stores them; dashboard displays time‑series.

- [ ] Agent metrics:
  - [ ] DNS resolution time for a target domain
  - [ ] TCP connect latency to a target host:port
  - [ ] TLS handshake time for HTTPS endpoints
  - [ ] HTTP response time, status code (HTTP/2 and HTTP/1.1)
  - [ ] Packet loss (ICMP echo, raw socket fallback) and RTT
  - [ ] Jitter calculation (standard deviation of successive RTTs)
  - [ ] All metrics sent via gRPC streaming (or unary) to collector
- [ ] Collector:
  - [ ] gRPC service to receive metrics, parse, validate
  - [ ] Storage backend: PostgreSQL with TimescaleDB extension for time‑series (or InfluxDB – document choice)
  - [ ] Write path: batch inserts, handle high throughput
- [ ] Dashboard:
  - [ ] Agent list with last seen, health status (green/yellow/red)
  - [ ] Time‑series graphs for latency, packet loss, DNS duration per agent
  - [ ] Auto‑refresh via polling or WebSocket
- [ ] Observability: collector emits metrics on ingestion rate, errors, storage latency; Grafana dashboard for pipeline health
- [ ] Testing: unit tests for agent metrics collection (mock sockets), integration tests for collector storage
- [ ] ADR: `002-time-series-storage-choice.md`

---

## Phase 2 – Alerting System & Notification (1–2 weeks)

**Goal:** Detect anomalies and notify operators (or simulated on‑call).

- [ ] Alert rules: configure thresholds (e.g., latency > 500ms, packet loss > 5%, DNS timeout) per agent or per check
- [ ] Alert evaluation: collector evaluates incoming metrics against rules
- [ ] Alert state machine: firing, resolved, repeat interval
- [ ] Notification channels:
  - [ ] Log alert to file/stdout (basic)
  - [ ] Webhook to a mock incident service (or Slack/Discord for demo)
  - [ ] Dashboard: alert timeline, active alerts pane
- [ ] Runbook draft: how to respond to high latency alert
- [ ] ADR: `003-alerting-design.md`

---

## Phase 3 – Introducing Kafka as Event Backbone (1–2 weeks)

**Goal:** Replace direct collector storage with Kafka for decoupled, scalable stream processing.

- [ ] Kafka cluster: single broker (KRaft) in Docker Compose; document production multi‑broker setup
- [ ] Collector produces all received metrics to Kafka topic `network.metrics` (partitioned by agent ID)
- [ ] Storage consumer: separate C++ service (or thread) reads from Kafka and writes to TimescaleDB
- [ ] Alert consumer: reads same topic, evaluates alert rules in real‑time (or reuse existing evaluator but via topic)
- [ ] Consumer groups: ensure multiple consumers can scale
- [ ] Exactly‑once / at‑least‑once semantics: document choice; implement idempotent writing to DB (dedup by metric ID)
- [ ] Observability: monitor consumer lag, Kafka broker metrics in Grafana
- [ ] ADR: `004-kafka-as-event-backbone.md`

---

## Phase 4 – Advanced Networking & Deep Diagnostics (2–3 weeks)

**Goal:** Demonstrate deep networking expertise through additional checks and diagnostic tools.

- [ ] Agent enhancements:
  - [ ] DNS record lookup (A, AAAA, CNAME) and validate against expected values; alarm on mismatch
  - [ ] TCP handshake capture and timing (SYN, SYN‑ACK, ACK) using raw sockets or `libpcap`
  - [ ] Packet retransmission detection (via raw socket or `getsockopt(TCP_INFO)`)
  - [ ] TLS certificate validation: check expiry, issuer, hostname match
  - [ ] HTTP/2 vs HTTP/3 comparison: measure connect+request time over both protocols if server supports (or document simulation)
  - [ ] Path MTU discovery and fragmentation detection
- [ ] Diagnostic mode: agent can be triggered to run a one‑off detailed report (traceroute, extended pcap) and upload to collector
- [ ] Dashboard: detailed per‑agent network diagnostic page, TLS certificate expiry timeline, HTTP protocol comparison chart
- [ ] Documentation: `docs/networking-deep-dive.md` with tcpdump/Wireshark annotated examples of packet flows, handshake analysis, and troubleshooting steps
- [ ] ADR: `005-raw-socket-capabilities-and-security.md`

---

## Phase 5 – Systems Engineering, Time Sync & Service Discovery (2–3 weeks)

**Goal:** Show mastery of Linux internals, production hardening, reliable time handling, and dynamic agent discovery.

### Daemon & Hardening

- [ ] Agent as a proper daemon:
  - [ ] systemd unit with `Type=notify`, watchdog support, `Restart=always`
  - [ ] Signal handling (SIGTERM graceful shutdown, SIGHUP config reload)
  - [ ] Process lifecycle management, PID file, logging to stdout/stderr
- [ ] Linux capabilities: drop all privileges, retain only `CAP_NET_RAW` and `CAP_SYS_ADMIN` (if needed)
- [ ] Resource limits: set `LimitNOFILE`, `MemoryMax`, `CPUQuota` in systemd unit
- [ ] Kernel tuning experiments:
  - [ ] TCP buffer sizes (`tcp_rmem`, `tcp_wmem`), `tcp_congestion_control` comparison (cubic vs bbr)
  - [ ] `tcp_fastopen`, `tcp_tw_reuse` – document impact on agent’s measurements
  - [ ] Network namespace isolation (optional)
- [ ] Log rotation: journald or logrotate configuration
- [ ] Security: run agent as unprivileged user; AppArmor/SELinux profile (optional but documented)

### Time Synchronization & Clock Hygiene

- [ ] NTP drift monitoring: agent periodically checks NTP offset via `ntp_gettime()` or external NTP query; includes `ntp.offset.ms` in health metrics
- [ ] Clock skew detection: collector compares agent timestamp to its own wall clock on receipt; logs warning if skew exceeds configurable threshold
- [ ] Timestamp normalization strategy: all metrics stored with collector‑assigned timestamp as source of truth; agent‑reported timestamp preserved as metadata for debugging
- [ ] Dashboard: NTP offset graph per agent, skew warnings
- [ ] ADR: `006-time-handling-across-agents.md`

### Service Discovery & Configuration

- [ ] DNS‑based discovery: agent resolves a configurable hostname (e.g., `collector.example.com`); repointable for failover
- [ ] Kubernetes service discovery: when running in‑cluster, agent uses `collector-service.namespace.svc.cluster.local` via CoreDNS
- [ ] Failover discovery process: agent holds a prioritized list of collector endpoints; on connection failure, tries next; success recorded in log and metric
- [ ] Document discovery logic, retry backoff, and how this integrates with multi‑region failover (Phase 7)
- [ ] ADR: `007-service-discovery-strategy.md`

---

## Phase 6 – Observability, Overload & Backpressure, Chaos & Incidents (2–3 weeks)

**Goal:** Full stack observability, graceful degradation under load, chaos experiments, and operational artifacts.

### Full Observability

- [ ] OpenTelemetry: trace propagation from agent to collector to Kafka to storage (W3C trace context in gRPC metadata and Kafka headers)
- [ ] Prometheus + Grafana:
  - [ ] Dashboards: agent health overview, per‑agent metrics, Kafka consumer lag, collector resource usage, NTP offset, discovery failures
  - [ ] Alertmanager rules based on SLO error budgets
- [ ] Structured logging: all components emit JSON logs with trace ID, agent ID

### Overload & Backpressure Deep‑Dive

- [ ] Simulate collector overload: inject high metric throughput until collector saturates; observe behaviour
- [ ] Simulate Kafka slowdown: throttle broker or consumer; observe backpressure propagation
- [ ] Backpressure strategy: collector signals overload via gRPC flow control (stream backpressure) or HTTP 429 to agents; agents reduce send rate or drop low‑priority metrics
- [ ] Metric dropping policy: define which metrics to drop first when buffers are full (e.g., keep health, drop diagnostics), document in ADR
- [ ] Agent‑side buffering limits: memory‑mapped ring buffer or bounded in‑memory queue; agent self‑monitors buffer usage and emits metric
- [ ] Memory growth safeguards: enforce hard limits; if memory exceeds threshold, agent logs, drops oldest metrics, optionally restarts (systemd `MemoryMax` as backstop)
- [ ] Tests: overload simulation scripts, verify graceful degradation, no data corruption
- [ ] ADR: `008-backpressure-and-overload-handling.md`

### Chaos Engineering

- [ ] Chaos experiments:
  - [ ] Kill collector pod under load; verify agent buffering and reconnection without data loss
  - [ ] Network partition: isolate an agent from collector using `iptables`; verify agent buffers and flushes on reconnect
  - [ ] Kafka broker restart; verify consumer recovery and no duplicate writes (idempotency)
  - [ ] DNS failure: point agent to non‑existent collector; verify graceful degradation and alert
  - [ ] Inject clock skew on agent; verify detection and normalization
- [ ] Document results in `docs/chaos-experiments.md` with screenshots

### Incident Simulation & Postmortems

- [ ] Simulate a collector overload‑induced OOM; write postmortem `docs/postmortems/001-collector-overload-oom.md`
- [ ] Simulate an alert storm from NTP drift/clock skew; write postmortem `docs/postmortems/002-clock-skew-alert-storm.md`
- [ ] Runbooks: agent reconnection, collector scale‑up, Kafka partition rebalancing, clock skew remediation

---

## Phase 7 – Disaster Recovery, Multi‑Region & Cost Awareness (1–2 weeks)

**Goal:** Prove the system can survive regional failures and be operated cost‑effectively.

- [ ] Multi‑region deployment: deploy a secondary collector in a different AWS region, agents fail over if primary unreachable (using service discovery from Phase 5)
- [ ] Agent buffering: agents store metrics to local SQLite or memory‑mapped file when collector unreachable; flush on reconnect
- [ ] Disaster recovery drill:
  - [ ] Simulate primary region outage; verify agents switch to secondary collector
  - [ ] Measure data gap and recovery time, document in `docs/dr-test.md`
- [ ] Cost analysis:
  - [ ] Monthly cost estimate (EC2 instances, managed Kafka or self‑hosted brokers, TimescaleDB/RDS, S3 for pcap storage)
  - [ ] Scaling projection (100 agents, 1000 agents)
  - [ ] Optimisation: spot instances for collectors, Kafka tiered storage, data retention policies
- [ ] Capacity planning: metrics per second per agent, Kafka bandwidth, storage retention and downsampling strategies
- [ ] ADR: `009-multi-region-dr-strategy.md`

---

## Phase 8 – Dashboard Polish, API & Portfolio Artifacts (1–2 weeks)

**Goal:** A polished, demo‑ready product and complete documentation.

- [ ] Dashboard:
  - [ ] Interactive time‑series graphs with zoom/pan
  - [ ] Agent configuration panel (add/edit checks, thresholds)
  - [ ] Alert history and management (acknowledge, close)
  - [ ] Network topology map (optional, if agents discover each other)
- [ ] API: REST API for dashboard (or use gRPC‑Web); OpenAPI spec
- [ ] Performance: Lighthouse audit for dashboard, bundle optimization
- [ ] Final documentation:
  - [ ] Architecture diagram (C4 model) – agent, collector, Kafka, DB, dashboard
  - [ ] `README.md` with demo, setup, and runbooks index
  - [ ] All ADRs, postmortems, performance reports linked
  - [ ] `docs/tradeoffs.md` summary
- [ ] Portfolio demo: video showing agent deployment, metrics flow, alerting, overload handling, failover, chaos experiment recovery

---

## Completion Checklist – Network Tracker

- [ ] C++ agent daemon with systemd, capabilities, and logging
- [ ] gRPC streaming metrics to collector
- [ ] Time‑series storage (TimescaleDB) with efficient writes and reads
- [ ] Real‑time dashboard with agent health and metric graphs
- [ ] Alerting engine with notifications (webhook or log)
- [ ] Kafka integration as metrics backbone; consumer lag monitored
- [ ] Deep networking checks: DNS, TCP handshake, TLS cert, HTTP/2 vs HTTP/3, packet loss, jitter
- [ ] Network diagnostic mode (traceroute, pcap) with Wireshark/tcpdump annotated documentation
- [ ] Kernel tuning experiments and systemd hardening
- [ ] Mutual TLS between agent and collector, certificate rotation
- [ ] Time synchronization: NTP drift monitoring, clock skew detection, timestamp normalization; ADR
- [ ] Service discovery: DNS, Kubernetes, failover process; ADR
- [ ] Overload & backpressure: collector overload simulation, Kafka slowdown, backpressure strategy, metric dropping policy, buffering limits, memory safeguards; ADR
- [ ] Chaos experiments: collector kill, network partition, DNS failure, clock skew injection; documented
- [ ] Two simulated postmortems written (overload OOM, clock skew alert storm)
- [ ] Multi‑region failover with agent buffering; DR test and RTO/RPO measured
- [ ] Cost estimate, capacity plan, and cost optimization suggestions
- [ ] All ADRs (9 total), runbooks, and portfolio artifacts complete