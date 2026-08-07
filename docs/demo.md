# Portfolio Demo Walkthrough

This is the recorded walkthrough script for the PudimNetMon portfolio video.
Every block shows the command and the expected observable outcome. Run the
whole thing top-to-bottom for a complete demo (~15 min).

---

## 0. Build & prerequisites

```bash
git clone git@github.com:diegobraga92/PudimNetMon.git && cd PudimNetMon

# C++ components
cmake -S agent -B build-agent && cmake --build build-agent -j$(nproc)
cmake -S collector -B build-collector && cmake --build build-collector -j$(nproc)

# Dashboard
cd dashboard && npm install && npm run build && cd ..
```

**Expected:** `pudim-agent` and `pudim-collector` binaries; `dashboard/dist/`
builds with the app chunk ≈ 21 kB (see `docs/performance.md`).

## 1. Bring up the stack

```bash
docker compose up --build -d
docker compose ps
```

**Expected:** collector, Kafka, ZooKeeper, TimescaleDB, consumers, Prometheus,
Grafana (:3100), dashboard (:3000). Health check:

```bash
curl -s localhost:8080/api/health        # {"status":"ok",...}
```

## 2. Agents deploying and streaming metrics

```bash
./build-agent/pudim-agent -n edge-01 -c collector:50051 \
  -d example.com -t example.com:443 -w https://example.com -g 1.1.1.1 \
  -a edge-01:50052
```

**Expected:** agent registers (collector log: `New agent registered`), then
logs `Collected N metrics, sending to collector` each interval and
`Metrics batch accepted`. Dashboard shows `edge-01` alive with latency charts.

```bash
curl -s localhost:8080/api/agents | python3 -m json.tool   # alive:true
curl -s "localhost:8080/api/metrics?agent_id=edge-01&window_seconds=60" | head
```

## 3. Alerting end-to-end

Point the agent at a broken DNS target so `dns-failure` (on_failure) fires:

```bash
curl -s -X POST localhost:8080/api/agents/config -H 'Content-Type: application/json' -d '{
  "agent_id":"edge-01",
  "dns_targets":["nonexistent-host.invalid"],"tcp_targets":[],"tls_targets":[],
  "http_targets":[],"ping_targets":[],"ping_count":4,
  "tls_cert_check":false,"tcp_retransmit_check":false,
  "tcp_handshake_capture":false,"http_protocols":[]}'

curl -s localhost:8080/api/alerts          # critical dns-failure firing
curl -s -X POST localhost:8080/api/alerts/ack -H 'Content-Type: application/json' \
  -d '{"rule_id":"dns-failure","agent_id":"edge-01","target":"nonexistent-host.invalid"}'
                                          # {"acknowledged":true,...}
```

**Expected:** alert card appears in the dashboard with `Acknowledge` button;
clicking it marks it acknowledged. Restore the config to resolve it.

## 4. Overload handling (backpressure)

```bash
./scripts/overload-collector.sh 10 500    # simulates slow storage for 10s
```

**Expected:** collector sets `x-overloaded`; agent logs backoff and drops/spills
oldest buffered batches (`Buffer full; dropped oldest metric batch`). Counter
`pudim_metrics_dropped_total` in `/metrics` increases; the pipeline stays up.

## 5. Failover (chaos: kill the primary collector)

```bash
docker compose stop collector
```

**Expected (agent):**
- `Heartbeat failed; failing over to collector-secondary:50051`
- in-memory buffer fills → `Disk buffer unavailable/spilled` (with
  `HAVE_SQLITE3`: `Disk buffer ready at /var/lib/pudim/pending.db`)

```bash
docker compose start collector
```

**Expected:** agent reconnects to primary and **drains** buffered batches
(`Drained N batches from disk buffer`) — no metric gap beyond the buffer.

## 6. Deep diagnostic + mTLS (security)

```bash
./scripts/gen-certs.sh certs
docker compose stop agent-collector   # (see compose) or run binaries manually
./build-collector/pudim-collector --tls-ca certs/ca.crt \
  --tls-cert certs/collector.crt --tls-key certs/collector.key ...
./build-agent/pudim-agent --tls-ca certs/ca.crt --tls-cert certs/agent.crt \
  --tls-key certs/agent.key ...
curl -s -X POST "localhost:8080/api/diagnostic?agent_id=edge-01&trace_target=example.com"
```

**Expected:** collector log shows `gRPC transport: mTLS (...)`; the diagnostic
returns a traceroute report. Start a second agent **without** certs → collector
rejects it at the handshake and it never registers.

## 7. Dashboard interaction (Phase 8)

Open `http://localhost:3000`:

1. **Zoom/pan** — drag the brush under the time-series chart.
2. **Agent config panel** — select `edge-01` → Load → edit targets → Apply →
   the summary returns and the metric set changes within one interval.
3. **Alert ack** — fire an alert (step 3) and acknowledge it from the card.
4. **Diagnostics** — click `🔬 Run Diagnostic` on an agent card.

## 8. DR / chaos log

```bash
cat docs/chaos-experiments.md    # 5 experiments incl. DNS failure, clock skew
cat docs/dr-test.md              # RTO ≈ 3×interval, RPO = disk buffer
cat docs/postmortems/001-collector-overload-oom.md
cat docs/postmortems/002-clock-skew-alert-storm.md
```

Record the screen with `asciinema rec demo.cast` (or OBS) while running
sections 1–7 for the portfolio video.
