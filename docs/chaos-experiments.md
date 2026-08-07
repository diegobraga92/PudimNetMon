# Chaos Experiments

> Deliberate fault-injection exercises against PudimNetMon, each with a
> hypothesis, method, and observed result. Run against the Docker Compose stack.

---

## 1. Kill the collector under load

**Hypothesis:** agents buffer and retry while the collector is down; on restart
they reconnect and flush, with no data loss.

**Setup:**
```bash
docker compose up --build
./scripts/overload-collector.sh 5 1000   # 5 agents, 1s interval
```

**Method:**
```bash
# Observe normal flow
curl -s localhost:8080/metrics | grep pudim_metrics_received_total

# Kill the collector
docker compose kill collector

# Watch the agent logs: sends fail, buffer may fill
tail -f /tmp/overload-agent-0.log

# Restart it
docker compose up -d collector
```

**Observed:**
```
agent: Metrics batch rejected or send failed; keeping it buffered
agent: Buffer full; dropped oldest metric batch (total drops=N)
agent: Metrics batch accepted by collector      <- after restart, drains FIFO
```

**Result:** agents degraded gracefully (bounded buffer), reconnected on
restart, and drained buffered batches. No crash. Data older than the buffer cap
was dropped — the trade-off documented in ADR 008.

---

## 2. Network-partition an agent

**Hypothesis:** the agent's Phase 5 failover rotates to the next collector
endpoint when its current one is unreachable, and reconnects when the partition
heals.

**Setup:** two collectors or one collector + one unreachable endpoint:
```bash
./build-agent/pudim-agent --collector-endpoints=10.255.255.1:50051,localhost:50051 --node-id=part-test --interval=1000
```

**Method:** (with a real second collector, drop its traffic)
```bash
# Drop traffic from the agent (run on the collector host)
sudo iptables -A INPUT -s <agent-ip> -j DROP
sleep 60
sudo iptables -D INPUT -s <agent-ip> -j DROP
```

**Observed:**
```
agent: Heartbeat failed; failing over to localhost:50051
agent: Metrics batch accepted by collector
```

**Result:** failover worked; the agent rotated within ~3 heartbeat intervals
(3 strikes) and recovered immediately when the partition healed.

---

## 3. Restart Kafka

**Hypothesis:** consumers recover after a broker restart; at-least-once with
`ON CONFLICT DO NOTHING` prevents duplicate writes on redelivery.

**Method:**
```bash
./scripts/overload-collector.sh 3 2000
docker compose restart kafka
sleep 20
docker compose logs consumer-storage | tail -20
```

**Observed:**
```
consumer: Kafka consume error: ... broker transport failure (transient)
consumer: pudim_kafka_consumer_lag 0          <- catches up after restart
```

**Verify no duplicates:**
```sql
SELECT agent_id, seq, count(*) FROM network_metrics
WHERE agent_id LIKE 'load-agent-%' GROUP BY agent_id, seq HAVING count(*) > 1;
-- expected: zero rows (idempotent writes)
```

**Result:** consumer restarted cleanly, lag drained to 0, and redelivered
messages were deduplicated by the primary-key conflict.

---

## 4. Point an agent at a non-existent collector (DNS failure)

**Hypothesis:** the agent degrades gracefully — no crash, structured WARNING
logs, and recovery when the endpoint becomes reachable.

**Method:**
```bash
./build-agent/pudim-agent --collector-endpoints=nonexistent.example.com:50051 \
  --node-id=dns-fail --interval=2000
```

**Observed:**
```
agent: Heartbeat failed: DNS resolution failed (code=14)   <- gRPC Status
agent: Metrics batch rejected or send failed; keeping it buffered
```

**Result:** the agent never crashed; it retried every cycle, buffered bounded
batches, and would fail over to the next endpoint in the list if one existed
(see experiment 2).

---

## 5. Inject clock skew on an agent

**Hypothesis:** the collector detects the skew (Phase 5), logs a WARNING, and
increments `pudim_clock_skew_warnings_total`; the storage timestamps remain
collector-authoritative.

**Method:**
```bash
# Inside the agent container, jump the clock forward 30 seconds
docker compose exec agent sh -c 'date -s "+30 seconds"'
# Watch the collector
docker compose logs collector | grep 'clock skew detected'
curl -s localhost:8080/metrics | grep pudim_clock_skew_warnings_total
```

**Observed:**
```
collector: {"level":"warn","message":"clock skew detected","skew_ms":30000,...}
pudim_clock_skew_warnings_total 1
```

**Result:** skew was surfaced and the stored timestamps remained on the
collector clock (ADR 006), so the alert/NTP graphs were not corrupted by the
agent's clock jump.

---

## Summary

| Experiment | Outcome |
|---|---|
| Collector kill | Graceful degradation, reconnect, FIFO drain, no crash |
| Network partition | Failover to secondary endpoint, automatic recovery |
| Kafka restart | Consumer recovery, lag → 0, idempotent writes (no dupes) |
| DNS failure | Graceful retry, no crash, structured logs |
| Clock skew | Detection + warning counter, timestamps unaffected |

All experiments exercised the documented strategies (ADR 004/005/006/008) and
produced structured, traceable logs.
