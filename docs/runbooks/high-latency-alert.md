# Runbook: High Latency Alert

> Applies to alerts fired by PudimNetMon rules such as `high-tcp-latency`,
> `high-http-latency`, `high-packet-loss`, and `dns-failure`.

## Alert Summary

| Field | Example |
|---|---|
| Rule | `high-tcp-latency` — High TCP Connect Latency |
| Severity | `warning` / `critical` |
| Trigger | `latency_ms > 500` (per check type & target) |
| Repeat interval | 300 s (re-notifies while still firing) |
| Notifications | Collector structured JSON log + webhook (if configured) |

## Severity Guide

| Severity | Meaning | Response |
|---|---|---|
| `info` | Informational | Monitor; no action needed yet |
| `warning` | SLO at risk but not breached | Investigate within 30 min |
| `critical` | Service-impacting (probe failures, packet loss) | Investigate immediately |

## Step 1 — Verify the alert is real

1. Check the dashboard **Active Alerts** pane for the alert card (agent, target, value, threshold).
2. Pull the agent's recent metrics for the affected check:
   ```bash
   curl 'http://<collector>:8080/api/metrics?agent_id=<AGENT_ID>&check_type=tcp_connect&window_seconds=3600'
   ```
3. Confirm the spike is sustained (not a single-sample outlier). A single sample over the
   threshold may be network jitter; a sustained breach for 2+ cycles is actionable.

## Step 2 — Localize the failure

| Symptom | Likely cause | Commands |
|---|---|---|
| High latency to one target from all agents | Target/upstream issue | `mtr <target>`, check target provider status |
| High latency from one agent to all targets | Agent host network issue | `mtr <collector>`, `ss -i` on the agent, check CPU/queue |
| DNS failures (`dns-failure`) | Resolver outage / bad hostname | `dig +trace <target>`, `cat /etc/resolv.conf`, check `systemd-resolved` |
| Packet loss on ICMP | Firewall/filtering or path congestion | `ping -c 100 <target>`, compare TCP-based RTT |

## Step 3 — Remedy

### Single noisy target
```bash
# From the agent host, verify connectivity
curl -o /dev/null -s -w 'dns=%{time_namelookup} connect=%{time_connect} total=%{time_total}\n' https://<target>
# If the target is deprioritised, remove it from the agent's probe targets
# and reload the agent config (Phase 5 adds SIGHUP reload).
```

### Agent host problem
- Check load: `uptime`, `vmstat 1 5`, `top`
- Check NIC errors/drops: `ip -s link`
- Check TCP retransmits: `netstat -s | grep -i retrans`
- If the host is saturated, scale the fleet or move the agent to a quieter host.

### Network path problem
- `mtr -rw <target>` to find the lossy/high-latency hop.
- Coordinate with the network provider; capture pcaps if needed (see
  `docs/networking-deep-dive.md` in Phase 4).

## Step 4 — Acknowledge & monitor

- Confirm the alert resolves on its own within the expected repeat interval.
- Watch the dashboard **Alert History** for the `resolved` event.
- If the condition is chronic, tune the rule (raise threshold, lengthen repeat
  interval) in `collector/config/alert_rules.json` and restart the collector
  (`--alert-rules-path`).

## Escalation

- **2 repeat notifications** without improvement → escalate to network team.
- **critical severity** alert (probe failures / packet loss) → on-call immediately.
- Post-incident: file a blameless postmortem if the incident exceeded the error
  budget (see `docs/slo.md`).
