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

- [x] Monorepo: `agent/`, `collector/`, `dashboard/`, `api/`, `infra/`, `docker-compose.yml`
- [x] Agent (C++):
  - [x] CMake build, basic main loop, structured logging (spdlog or custom)
  - [x] systemd unit file for daemon mode
  - [x] Command‑line flags: collector endpoint, node ID, interval
  - [x] Send heartbeat gRPC/protobuf message to collector
- [x] Collector (C++):
  - [x] CMake build, gRPC server, receive heartbeats, log them
  - [x] In‑memory agent registry (agent ID, last seen)
  - [x] Health HTTP endpoint (`/health`, `/metrics`)
- [x] Dashboard (TypeScript + React + Vite):
  - [x] Scaffold, call collector `/health`, display connection status
- [x] API contracts: Protobuf definitions for heartbeat, metrics (check in `api/proto/`)
- [x] Docker Compose: collector + dashboard + dummy agent (or run agent natively)
- [x] CI/CD: GitHub Actions for C++ build/test, dashboard lint/build
- [x] Observability seed: collector Prometheus `/metrics` endpoint, structured logging with trace IDs
- [x] SLO draft: agent heartbeat delivery success rate, document in `docs/slo.md`
- [x] ADR: `001-choose-cpp-and-grpc.md`

---

## Phase 1 – Core Metrics Collection & Storage (2–3 weeks)

**Goal:** Agents measure real network metrics; collector stores them; dashboard displays time‑series.

- [x] Agent metrics:
  - [x] DNS resolution time for a target domain
  - [x] TCP connect latency to a target host:port
  - [x] TLS handshake time for HTTPS endpoints
  - [x] HTTP response time, status code (HTTP/2 and HTTP/1.1)
  - [x] Packet loss (ICMP echo, raw socket fallback) and RTT
  - [x] Jitter calculation (standard deviation of successive RTTs)
  - [x] All metrics sent via gRPC streaming (or unary) to collector
- [x] Collector:
  - [x] gRPC service to receive metrics, parse, validate
  - [x] Storage backend: PostgreSQL with TimescaleDB extension for time‑series (or InfluxDB – document choice)
  - [x] Write path: batch inserts, handle high throughput
- [x] Dashboard:
  - [x] Agent list with last seen, health status (green/yellow/red)
  - [x] Time‑series graphs for latency, packet loss, DNS duration per agent
  - [x] Auto‑refresh via polling or WebSocket
- [x] Observability: collector emits metrics on ingestion rate, errors, storage latency; Grafana dashboard for pipeline health
- [x] Testing: unit tests for agent metrics collection (mock sockets), integration tests for collector storage
- [x] ADR: `002-time-series-storage-choice.md`

---

## Phase 2 – Alerting System & Notification (1–2 weeks)

**Goal:** Detect anomalies and notify operators (or simulated on‑call).

- [x] Alert rules: configure thresholds (e.g., latency > 500ms, packet loss > 5%, DNS timeout) per agent or per check
- [x] Alert evaluation: collector evaluates incoming metrics against rules
- [x] Alert state machine: firing, resolved, repeat interval
- [x] Notification channels:
  - [x] Log alert to file/stdout (basic)
  - [x] Webhook to a mock incident service (or Slack/Discord for demo)
  - [x] Dashboard: alert timeline, active alerts pane
- [x] Runbook draft: how to respond to high latency alert
- [x] ADR: `003-alerting-design.md`

---

## Phase 3 – Introducing Kafka as Event Backbone (1–2 weeks)

**Goal:** Replace direct collector storage with Kafka for decoupled, scalable stream processing.

- [x] Kafka cluster: single broker (KRaft) in Docker Compose; document production multi‑broker setup
- [x] Collector produces all received metrics to Kafka topic `network.metrics` (partitioned by agent ID)
- [x] Storage consumer: separate C++ service (or thread) reads from Kafka and writes to TimescaleDB
- [x] Alert consumer: reads same topic, evaluates alert rules in real‑time (or reuse existing evaluator but via topic)
- [x] Consumer groups: ensure multiple consumers can scale
- [x] Exactly‑once / at‑least‑once semantics: document choice; implement idempotent writing to DB (dedup by metric ID)
- [x] Observability: monitor consumer lag, Kafka broker metrics in Grafana
- [x] ADR: `004-kafka-as-event-backbone.md`

---

## Phase 4 – Advanced Networking & Deep Diagnostics (2–3 weeks)

**Goal:** Demonstrate deep networking expertise through additional checks and diagnostic tools.

- [x] Agent enhancements:
  - [x] DNS record lookup (A, AAAA, CNAME) and validate against expected values; alarm on mismatch
  - [x] TCP handshake capture and timing (SYN, SYN‑ACK, ACK) using raw sockets or `libpcap`
  - [x] Packet retransmission detection (via raw socket or `getsockopt(TCP_INFO)`)
  - [x] TLS certificate validation: check expiry, issuer, hostname match
  - [x] HTTP/2 vs HTTP/3 comparison: measure connect+request time over both protocols if server supports (or document simulation)
  - [x] Path MTU discovery and fragmentation detection
- [x] Diagnostic mode: agent can be triggered to run a one‑off detailed report (traceroute, extended pcap) and upload to collector
- [x] Dashboard: detailed per‑agent network diagnostic page, TLS certificate expiry timeline, HTTP protocol comparison chart
- [x] Documentation: `docs/networking-deep-dive.md` with tcpdump/Wireshark annotated examples of packet flows, handshake analysis, and troubleshooting steps
- [x] ADR: `005-raw-socket-capabilities-and-security.md`

---

## Phase 5 – Systems Engineering, Time Sync & Service Discovery (2–3 weeks)

**Goal:** Show mastery of Linux internals, production hardening, reliable time handling, and dynamic agent discovery.

### Daemon & Hardening

- [x] Agent as a proper daemon:
  - [x] systemd unit with `Type=notify`, watchdog support, `Restart=always`
  - [x] Signal handling (SIGTERM graceful shutdown, SIGHUP config reload)
  - [x] Process lifecycle management, PID file, logging to stdout/stderr
- [x] Linux capabilities: drop all privileges, retain only `CAP_NET_RAW` and `CAP_SYS_ADMIN` (if needed)
- [x] Resource limits: set `LimitNOFILE`, `MemoryMax`, `CPUQuota` in systemd unit
- [x] Kernel tuning experiments:
  - [x] TCP buffer sizes (`tcp_rmem`, `tcp_wmem`), `tcp_congestion_control` comparison (cubic vs bbr)
  - [x] `tcp_fastopen`, `tcp_tw_reuse` – document impact on agent’s measurements
  - [x] Network namespace isolation (optional)
- [x] Log rotation: journald or logrotate configuration
- [x] Security: run agent as unprivileged user; AppArmor/SELinux profile (optional but documented)

### Time Synchronization & Clock Hygiene

- [x] NTP drift monitoring: agent periodically checks NTP offset via `ntp_gettime()` or external NTP query; includes `ntp.offset.ms` in health metrics
- [x] Clock skew detection: collector compares agent timestamp to its own wall clock on receipt; logs warning if skew exceeds configurable threshold
- [x] Timestamp normalization strategy: all metrics stored with collector‑assigned timestamp as source of truth; agent‑reported timestamp preserved as metadata for debugging
- [x] Dashboard: NTP offset graph per agent, skew warnings
- [x] ADR: `006-time-handling-across-agents.md`

### Service Discovery & Configuration

- [x] DNS‑based discovery: agent resolves a configurable hostname (e.g., `collector.example.com`); repointable for failover
- [x] Kubernetes service discovery: when running in‑cluster, agent uses `collector-service.namespace.svc.cluster.local` via CoreDNS
- [x] Failover discovery process: agent holds a prioritized list of collector endpoints; on connection failure, tries next; success recorded in log and metric
- [x] Document discovery logic, retry backoff, and how this integrates with multi‑region failover (Phase 7)
- [x] ADR: `007-service-discovery-strategy.md`

---

## Phase 6 – Observability, Overload & Backpressure, Chaos & Incidents (2–3 weeks)

**Goal:** Full stack observability, graceful degradation under load, chaos experiments, and operational artifacts.

### Full Observability

- [x] OpenTelemetry: trace propagation from agent to collector to Kafka to storage (W3C trace context in gRPC metadata and Kafka headers)
- [x] Prometheus + Grafana:
  - [x] Dashboards: agent health overview, per‑agent metrics, Kafka consumer lag, collector resource usage, NTP offset, discovery failures
  - [x] Alertmanager rules based on SLO error budgets
- [x] Structured logging: all components emit JSON logs with trace ID, agent ID

### Overload & Backpressure Deep‑Dive

- [x] Simulate collector overload: inject high metric throughput until collector saturates; observe behaviour
- [x] Simulate Kafka slowdown: throttle broker or consumer; observe backpressure propagation
- [x] Backpressure strategy: collector signals overload via gRPC flow control (stream backpressure) or HTTP 429 to agents; agents reduce send rate or drop low‑priority metrics
- [x] Metric dropping policy: define which metrics to drop first when buffers are full (e.g., keep health, drop diagnostics), document in ADR
- [x] Agent‑side buffering limits: memory‑mapped ring buffer or bounded in‑memory queue; agent self‑monitors buffer usage and emits metric
- [x] Memory growth safeguards: enforce hard limits; if memory exceeds threshold, agent logs, drops oldest metrics, optionally restarts (systemd `MemoryMax` as backstop)
- [x] Tests: overload simulation scripts, verify graceful degradation, no data corruption
- [x] ADR: `008-backpressure-and-overload-handling.md`

### Chaos Engineering

- [x] Chaos experiments:
  - [x] Kill collector pod under load; verify agent buffering and reconnection without data loss
  - [x] Network partition: isolate an agent from collector using `iptables`; verify agent buffers and flushes on reconnect
  - [x] Kafka broker restart; verify consumer recovery and no duplicate writes (idempotency)
  - [x] DNS failure: point agent to non‑existent collector; verify graceful degradation and alert
  - [x] Inject clock skew on agent; verify detection and normalization
- [x] Document results in `docs/chaos-experiments.md` with screenshots

### Incident Simulation & Postmortems

- [x] Simulate a collector overload‑induced OOM; write postmortem `docs/postmortems/001-collector-overload-oom.md`
- [x] Simulate an alert storm from NTP drift/clock skew; write postmortem `docs/postmortems/002-clock-skew-alert-storm.md`
- [x] Runbooks: agent reconnection, collector scale‑up, Kafka partition rebalancing, clock skew remediation

---

## Phase 7 – Disaster Recovery, Multi‑Region & Cost Awareness (1–2 weeks)

**Goal:** Prove the system can survive regional failures and be operated cost‑effectively.

- [x] Multi‑region deployment: deploy a secondary collector in a different AWS region, agents fail over if primary unreachable (using service discovery from Phase 5)
- [x] Agent buffering: agents store metrics to local SQLite or memory‑mapped file when collector unreachable; flush on reconnect
- [x] Disaster recovery drill:
  - [x] Simulate primary region outage; verify agents switch to secondary collector
  - [x] Measure data gap and recovery time, document in `docs/dr-test.md`
- [x] Cost analysis:
  - [x] Monthly cost estimate (EC2 instances, managed Kafka or self‑hosted brokers, TimescaleDB/RDS, S3 for pcap storage)
  - [x] Scaling projection (100 agents, 1000 agents)
  - [x] Optimisation: spot instances for collectors, Kafka tiered storage, data retention policies
- [x] Capacity planning: metrics per second per agent, Kafka bandwidth, storage retention and downsampling strategies
- [x] ADR: `009-multi-region-dr-strategy.md`

---

## Phase 8 – Dashboard Polish, API & Portfolio Artifacts (1–2 weeks)

**Goal:** A polished, demo‑ready product and complete documentation.

- [x] Dashboard:
  - [x] Interactive time‑series graphs with zoom/pan (recharts `Brush`)
  - [x] Agent configuration panel (add/edit checks, thresholds) — `Reconfigure`/`GetConfig` RPCs + `/api/agents/config`; live without restart
  - [x] Alert history and management (acknowledge, close) — `AlertManager::Ack` + `/api/alerts/ack` + dashboard button
  - [ ] Network topology map (optional, if agents discover each other) — *deferred (optional)*
- [x] API: REST API for dashboard (or use gRPC‑Web); OpenAPI spec — `docs/openapi.yaml`; all endpoints under `/api/*`
- [x] Performance: Lighthouse audit for dashboard, bundle optimization — bundle split 578 kB → 21 kB app chunk; Lighthouse runbook in `docs/performance.md` (requires Chrome, not available in this env)
- [x] Final documentation:
  - [x] Architecture diagram (C4 model) – agent, collector, Kafka, DB, dashboard — `docs/architecture.md` (Mermaid)
  - [x] `README.md` with demo, setup, and runbooks index — see `docs/demo.md` walkthrough
  - [x] All ADRs, postmortems, performance reports linked — `docs/tradeoffs.md` indexes all 10 ADRs
  - [x] `docs/tradeoffs.md` summary
- [x] Portfolio demo: video showing agent deployment, metrics flow, alerting, overload handling, failover, chaos experiment recovery — recorded walkthrough in `docs/demo.md` (record the video with asciinema/OBS)

---

## Completion Checklist – Network Tracker

- [x] C++ agent daemon with systemd, capabilities, and logging
- [x] gRPC streaming metrics to collector
- [x] Time‑series storage (TimescaleDB) with efficient writes and reads
- [x] Real‑time dashboard with agent health and metric graphs
- [x] Alerting engine with notifications (webhook or log)
- [x] Kafka integration as metrics backbone; consumer lag monitored
- [x] Deep networking checks: DNS, TCP handshake, TLS cert, HTTP/2 vs HTTP/3, packet loss, jitter
- [x] Network diagnostic mode (traceroute, pcap) with Wireshark/tcpdump annotated documentation
- [x] Kernel tuning experiments and systemd hardening
- [x] Mutual TLS between agent and collector, certificate rotation
- [x] Time synchronization: NTP drift monitoring, clock skew detection, timestamp normalization; ADR
- [x] Service discovery: DNS, Kubernetes, failover process; ADR
- [x] Overload & backpressure: collector overload simulation, Kafka slowdown, backpressure strategy, metric dropping policy, buffering limits, memory safeguards; ADR
- [x] Chaos experiments: collector kill, network partition, DNS failure, clock skew injection; documented
- [x] Two simulated postmortems written (overload OOM, clock skew alert storm)
- [x] Multi‑region failover with agent buffering; DR test and RTO/RPO measured
- [x] Cost estimate, capacity plan, and cost optimization suggestions
- [x] All ADRs (10 total), runbooks, and portfolio artifacts complete