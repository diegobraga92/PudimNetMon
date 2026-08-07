# Postmortem 002: Clock-Skew Alert Storm

**Date:** 2026-08-07
**Severity:** SEV-3 (alert noise, no data loss)
**Status:** Closed — action items tracked

## Summary

An agent host's clock jumped ~2 minutes forward (NTP daemon restart / manual
adjustment). The collector's Phase 5 clock-skew detection (ADR 006) correctly
fired a warning per batch; a broad `ntp_offset` alert rule converted those
warnings into a sustained alert storm in the alert consumer. No metrics were
lost — collector-assigned timestamps kept storage ordering correct — but
operators were paged repeatedly.

## Timeline (UTC)

| Time | Event |
|---|---|
| 09:30:00 | Agent clock +120 s (NTP service restart) |
| 09:30:05 | Collector logs `clock skew detected`, skew_ms≈120000 |
| 09:30:15 | `pudim_clock_skew_warnings_total` climbing; alert consumer fires `ntp_offset` rule |
| 09:30:20 | Repeat-interval (60 s) re-notifications begin → alert storm |
| 09:35:00 | Operator on-call investigates; NTP service found stopped |
| 09:36:00 | `systemctl restart systemd-timesyncd`; clock steps back |
| 09:36:30 | Skew warnings stop; alerts resolve |

## Root Cause

The alert rule `ntp_offset` used `op: ">", threshold: 5` with a short repeat
interval and **no cooldown/severity tiering**, so a single clock jump produced a
cascade of repeat alerts. The underlying detection behaved correctly; the
alerting configuration amplified one fault into many pages.

## Detection

- `pudim_clock_skew_warnings_total` climbing monotonically.
- NTP offset dashboard panel showing a +120 s spike.

## Impact

- Alert noise for ~6 minutes; on-call time consumed; no data loss or SLO impact
  (storage timestamps remained collector-authoritative).

## Recovery

- Restart the NTP daemon on the affected host; the kernel clock discipline
  (`ntp_adjtime`) recovered, and skew warnings stopped once offset < threshold.

## Lessons

1. **Detection without policy is noise.** Skew detection needs alert rules that
   require *sustained* skew (e.g. 3 consecutive samples over threshold) and
   longer repeat intervals.
2. **Alert rules need severity tiers** — a 5 ms drift is a dashboard widget, a
   120 s jump is an on-call page.
3. **Collector-authoritative timestamps (ADR 006) saved the data** — storage
   ordering never depended on the agent's broken clock.

## Action Items

- [x] Clock-skew detection + warning counter (ADR 006).
- [x] NTP offset chart (dashboard) to make drift visible before it becomes an
      alert.
- [ ] Add a "sustained skew" alert rule (N samples over threshold) in
      `collector/config/alert_rules.json`.
- [ ] Document skew remediation in the runbook (`docs/runbooks/high-latency-alert.md`
      references clock hygiene).
