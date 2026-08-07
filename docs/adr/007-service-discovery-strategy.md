# ADR 007: Service Discovery Strategy

**Status:** Accepted
**Date:** 2026-08-07

## Context

Agents need to reach the collector reliably. The collector endpoint may change
(failover, rebalancing, new region), and the fleet may span Kubernetes and bare
Linux hosts. We need a discovery strategy that is simple, DNS-native, and
provides failover without a service mesh.

## Decision

1. **DNS-based discovery (primary).** The collector endpoint is expressed as a
   hostname:port string. gRPC's channel name resolver performs DNS resolution
   lazily on each connection attempt, so a hostname that changes A/AAAA records
   (e.g. an ALB, a `collector.example.com` CNAME, or a Kubernetes Service)
   re-resolves automatically. No polling code needed.

2. **Prioritized endpoint list with failover.** The agent accepts
   `--collector-endpoints=host1:50051,host2:50051` (comma-separated). On 3
   consecutive send failures (heartbeat or metrics), the agent rotates to the
   next endpoint, recreating its gRPC channels, and logs the failover as a
   structured JSON line. Success on any endpoint resets the strike count.

3. **Kubernetes discovery.** Inside a cluster the agent is pointed at
   `collector-service.namespace.svc.cluster.local:50051`. CoreDNS resolves this
   to the Service ClusterIP, and gRPC re-resolves it when the service's
   endpoints change — so a rebuilt collector pod is reached without agent
   restart. Optionally, a `StatefulSet`/`headless` Service can be used for
   stable per-pod addresses.

4. **Retry/backoff.** The agent's main loop sleeps `interval_ms` (default 5s)
   between cycles and only rotates after 3 failures, giving a natural ~15s
   primary→secondary cutover. No exponential backoff loop is needed in this
   phase; a future phase may add jittered backoff for high-failure scenarios.

5. **Multi-region (Phase 7).** Failover integrates by listing region-local
   collector endpoints first, then the secondary region's endpoint. Success is
   recorded in logs and the (future) agent Prometheus metrics.

## Rationale

- **DNS-native:** no ZooKeeper/etcd client, no service mesh dependency, works on
  bare metal and in K8s with the same code path.
- **Prioritized list:** simple, deterministic, and testable — rotation order is
  explicit and recovery prefers the primary once reachable (list wraps around).
- **gRPC lazy re-resolution:** we get DNS re-discovery for free from the
  transport, so the agent code stays thin.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Kubernetes API client in the agent | Heavy dependency; only helps in-cluster; DNS covers it |
| Service mesh (Istio/Linkerd) | Operational overhead out of scope; not present on bare metal |
| etcd/consul agent | New failure domain, more moving parts than DNS |
| Single hardcoded endpoint | No failover; the 3-strike list is a small, high-value addition |

## Consequences

- Agent CLI gains `--collector-endpoints` (falls back to `--collector-endpoint`).
- `FailoverClient` (3-strike rotation) added under `agent/src/metrics/`.
- Failover events are structured log lines; a `pudim_agent_failovers_total`
  counter is tracked for future agent Prometheus export.
- Documentation: this ADR plus `docs/networking-deep-dive.md` §7 (diagnostics)
  reference the discovery model.

## Compliance

- ADR format per DEV_PLAN.md; failover covered by agent tests; K8s deployment
  documented for Phase 6/7 (DaemonSet, Service, headless Service).
