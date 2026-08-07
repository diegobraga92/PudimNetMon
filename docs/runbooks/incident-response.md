# Incident Response Runbooks

> Short, actionable runbooks for the most common PudimNetMon incidents.

---

## R1. Agent reconnection

**Symptoms:** an agent's heartbeat success drops to 0; `pudim_agents_active` falls; the agent logs `Heartbeat failed`.

1. Confirm the agent process is alive: `systemctl status pudim-agent` or `docker compose ps agent`.
2. Check the agent log for the failure mode:
   ```bash
   journalctl -u pudim-agent -n 50 --no-pager
   # or
   docker compose logs agent --tail=50
   ```
3. **DNS resolution failure** → verify `collector.example.com` resolves; fix DNS, agent retries automatically.
4. **Connection refused / timeout** → check the collector is up: `curl -s localhost:8080/health`.
5. If the agent uses `--collector-endpoints=a,b`, it fails over automatically after 3 strikes (ADR 007). Confirm in logs: `failing over to <b>`.
6. Verify recovery: `pudim_agents_active` returns to expected; agent logs `Metrics batch accepted`.

---

## R2. Collector scale-up

**Symptoms:** `pudim_metrics_received_total` plateaus, insert latency climbs, `x-overloaded` signals appear, agents back off.

1. Confirm the bottleneck: `curl -s localhost:8080/metrics | grep -E 'pudim_storage_insert|pudim_metrics_received|pudim_backpressure'`.
2. Check TimescaleDB: `docker compose logs timescaledb` for slow queries / locks.
3. Short-term: raise the backpressure threshold so agents resume normal rate:
   `--backpressure-threshold-ms=2000` (restart collector).
4. Medium-term: increase `--batch-size` on the storage path (fewer, larger transactions).
5. Long-term (multi-agent): scale horizontally — Kafka consumer groups already split partitions; add a second storage consumer instance.

---

## R3. Kafka partition rebalancing

**Symptoms:** consumer lag spikes after a broker restart or consumer rebalance; `pudim_kafka_consumer_lag` climbs.

1. Confirm rebalance: `docker compose logs consumer-storage | grep -i rebalance`.
2. Check broker health: `docker compose ps kafka`; restart if unhealthy.
3. Monitor lag drain:
   ```bash
   curl -s localhost:9091/metrics | grep pudim_kafka_consumer_lag
   ```
4. If lag persists, the storage consumer may be behind on inserts — check `pudim_kafka_handler_errors_total` and TimescaleDB health.
5. Add a consumer if a single instance can't keep up (partitions split across group members).

---

## R4. Clock skew remediation

**Symptoms:** `pudim_clock_skew_warnings_total` climbing; `ntp_offset` alert firing; NTP chart shows a large spike.

1. Identify the agent: collector logs `clock skew detected` with `agent_id` and `skew_ms`.
2. On the affected host, check NTP:
   ```bash
   timedatectl status
   systemctl status systemd-timesyncd chronyd ntpd 2>/dev/null
   ntpq -p 2>/dev/null
   ```
3. Restart the NTP service: `systemctl restart systemd-timesyncd`.
4. Confirm the kernel clock re-disciplines: run the agent's own probe —
   `pudim-agent --interval=60000 --dns-targets=example.com` and watch the
   `ntp_offset` metric return to ~0.
5. If the skew was deliberate (VM pause), no action needed — collector timestamps
   (ADR 006) kept storage correct.

---

## Related

- `docs/runbooks/high-latency-alert.md` — latency/packet-loss/DNS-failure alerts
- `docs/postmortems/001-collector-overload-oom.md`
- `docs/postmortems/002-clock-skew-alert-storm.md`
