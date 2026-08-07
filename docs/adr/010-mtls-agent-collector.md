# ADR 010: Mutual TLS between Agent and Collector

**Status:** Accepted
**Date:** 2026-08-07

## Context

Phase 8 (Security) closes the last open Completion Checklist item: the
agent↔collector gRPC path was previously **plaintext**. Attackers on the same
L2/L3 segment could:

- sniff metric/heartbeat payloads (metrics are not secret, but a compromised
  node id could impersonate an agent),
- inject forged heartbeats to poison the `/agents` registry and alerting,
- spoof a collector to harvest agent probes or issue fake diagnostic RPCs.

Requirement (from DEV_PLAN.md Security Requirements): *"Agent-to-collector
communication encrypted and authenticated."* The other hops (collector→Kafka,
storage) sit on a trusted docker-compose network and are out of scope for this
decision.

## Decision

1. **mTLS for every gRPC hop between agent and collector.** The collector's
   gRPC server uses `SslServerCredentials` with
   `GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY` — a client
   without a CA-signed certificate is rejected at the TLS handshake.
2. **Shared self-signed CA** minted by `scripts/gen-certs.sh` (CA + separate
   server/client certs, 1-year leaf validity).
3. **No hard dependency:** when `--tls-*` flags are omitted, both sides fall
   back to `InsecureChannelCredentials`/`InsecureServerCredentials` (local dev
   and CI convenience). The effective transport is logged at startup.
4. **Diagnostic hop secured too.** The agent's diagnostic gRPC server
   (collector-triggered traceroute/pcap) uses the same CA and the agent's
   cert/key; the collector's diagnostic client presents the collector's cert,
   so every RPC direction is mutually authenticated.
5. **Certificate rotation documented** in `docs/certificate-rotation.md`
   (roll agents first, then collector; CA stays offline).

## Rationale

- **gRPC-native TLS** (OpenSSL under the hood) avoids a separate sidecar/proxy
  and requires no service-mesh infrastructure, matching the project's lean
  footprint.
- **Require-and-verify** is the only mode that actually blocks impostors; the
  "request but don't verify" modes add latency without security.
- **Graceful fallback** keeps the bootstrap/portfolio story simple (`docker
  compose up` stays one command) while production can force TLS by simply not
  setting flags to insecure — the absence of a cert is a startup-logged
  property, not a compile-time fork.
- **Same cert for client and server roles** on the agent avoids a second PKI
  workflow; SANs cover the hostnames the collector uses.

## Alternatives Considered

| Option | Reason for Rejected |
|---|---|
| Plaintext (status quo) | Fails the stated security requirement; no authN/Z on the data plane |
| TLS without client auth | Encrypts but does not authenticate agents; any holder of the server cert's trust can connect |
| Vault/cert-manager PKI now | Correct long-term home, but heavier than this bootstrap warrants; documented as the production path in the rotation doc |
| WireGuard/overlay network | Encrypts the segment but does not provide per-service identity; overkill for a portfolio/lab |

## Consequences

- Agent and collector accept `--tls-ca/--tls-cert/--tls-key`; certs live in
  `certs/` (gitignored, never commit private keys).
- The collector log shows `gRPC transport: mTLS (...)` vs
  `insecure (no --tls-*)` at startup.
- Verified end-to-end locally: heartbeat + metrics ACK over mTLS, diagnostic
  round-trip over mTLS, and an unauthenticated agent rejected at handshake
  (UNAVAILABLE "Socket closed") without being registered.
- `ca.key` is a trust root that must be protected; CRL/OCSP revocation is
  explicitly out of scope for the bootstrap toolchain (see
  `docs/certificate-rotation.md`).

## Compliance

- Security: authenticated + encrypted agent↔collector transport; unauthenticated
  peers cannot register or drive diagnostics.
- Cross-cutting: ADR format per DEV_PLAN.md; live verification documented above.
