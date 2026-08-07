# Disaster Recovery Drill — Multi-Region Failover

> Validates that agents fail over to a secondary collector when the primary is
> lost, and that no metrics are permanently lost thanks to the agent's disk
> buffer (SQLite, Phase 7). Companion to `docs/adr/009-multi-region-dr-strategy.md`.

## Topology

In Docker Compose, two collectors represent two regions:

| Service | gRPC | HTTP | Role |
|---|---|---|---|
| `collector` | :50051 | :8080 | primary |
| `collector-secondary` | :50052 | :8081 | secondary (stand-in for region B) |

Both share the local Kafka + TimescaleDB for the drill. **Real multi-region**
would run a full stack per region (see ADR 009); the drill measures the agent
behaviour, which is identical.

## Setup

```bash
docker compose up --build
# Agent already points at both collectors:
#   --collector-endpoints=collector:50051,collector-secondary:50052
docker compose logs agent | grep -E 'Connecting to collector endpoint'
```

Expected:
```
agent: Connecting to collector endpoint: collector:50051
```

## Drill: primary region outage

```bash
# 1. Confirm normal flow
curl -s localhost:8080/metrics | grep pudim_metrics_received_total

# 2. Kill the primary collector (region A failure)
docker compose kill collector

# 3. Watch the agent fail over
docker compose logs -f agent | grep -E 'failing over|accepted by collector'
```

Expected (failover within ~3 heartbeat intervals = RTO):
```
agent: Heartbeat failed; failing over to collector-secondary:50052
agent: Connecting to collector endpoint: collector-secondary:50052
agent: Metrics batch accepted by collector
```

If the agent's in-memory buffer overflows while switching, oldest batches spill
to the SQLite disk buffer (`/var/lib/pudim/pending.db`):

```
agent: Buffer full; spilled oldest batch to disk buffer (spilled=N, pending=M)
```

## Drill: restore the primary

```bash
docker compose up -d collector
```

The agent keeps using the secondary until the primary is next in the rotation
(wrap-around), then returns to it. On reconnect it drains any persisted batches:

```
agent: Drained N persisted batches from disk buffer (pending=0)
```

## Measuring RTO / RPO

| Metric | Value | Notes |
|---|---|---|
| **RTO** (recovery time objective) | ≈ 3 × agent interval | 3-strike failover: 3 × 10s = ~30s default; 3 × 5s = ~15s |
| **RPO** (recovery point objective) | ≤ in-memory buffer + disk buffer | in-memory 200 batches ≈ 33 min at 10s; disk buffer = hours (cap `--disk-buffer-max-mb`) |
| **Data gap** | 0 for disk-buffered batches | SQLite survives collector outage + agent restart |

**Verify no data gap** after the drill:
```bash
# Count metrics around the outage window on the secondary's storage
curl -s 'localhost:8081/api/metrics?agent_id=agent-docker-001&window_seconds=600' | \
  python3 -c "import json,sys; print('rows:', len(json.load(sys.stdin)))"
```

## What the drill proves

- Agents detect the primary loss via the 3-strike heartbeat failure and rotate
  to the secondary (ADR 007 discovery).
- Metrics are not lost: in-memory buffer absorbs the cutover, overflow persists
  to SQLite, and the drain replays them once a collector is reachable.
- The secondary collector is a fully functional ingest point (gRPC + Kafka
  produce), so storage/alerting continue via the shared consumers.

## Known limitations (see ADR 009)

- This drill shares one Kafka/TimescaleDB; a true region failure would also
  fail the Kafka/DB, exercising the secondary region's own Kafka + DB.
- The agent's `--collector-endpoints` list wraps around, so recovery prefers the
  primary only after a full rotation through the list.
