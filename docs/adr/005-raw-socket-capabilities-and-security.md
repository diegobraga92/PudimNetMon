# ADR 005: Raw Socket Capabilities and Security

**Status:** Accepted
**Date:** 2026-08-07

## Context

Phase 4 adds deep network diagnostics to the agent:

- TCP handshake capture and per-segment timing (SYN/SYN-ACK/ACK) via **libpcap**
- Packet-retransmission counters via `getsockopt(TCP_INFO)`
- DNS record inspection (A/AAAA/CNAME)
- TLS certificate validation (chain, expiry, hostname)
- On-demand diagnostic mode (traceroute, tcpdump) invoked by the collector

These features exercise raw network primitives and require additional system
capabilities or host tooling, which conflicts with the "run as unprivileged"
security posture.

## Decision

1. **libpcap capture (TCP handshake probe) is optional** and compiled out when
   libpcap is unavailable (`HAVE_LIBPCAP` guard). The probe degrades to a
   `success=false` metric instead of crashing.
2. **Packet capture requires `CAP_NET_RAW`** (or root). The agent is granted
   only `CAP_NET_RAW` (via `cap_add: NET_RAW` in Docker, `AmbientCapabilities`
   or `CapabilityBoundingSet=+CAP_NET_RAW` in systemd) and runs as an
   unprivileged user (`nobody`/`nogroup`), so a compromise of the agent does not
   yield root.
3. **`getsockopt(TCP_INFO)`** needs no extra capability — it is a plain syscall
   on an existing connected socket.
4. **Diagnostic mode** shells out to host tools (`traceroute`, `tcpdump`), which
   are documented agent-host dependencies. The agent performs **no argument
   interpolation** of its own; the collector passes the target verbatim, and the
   diagnostic response is text-only (no file uploads) in this phase.
5. **TLS certificate validation uses the system CA bundle**
   (`SSL_CTX_set_default_verify_paths` + `SSL_VERIFY_PEER`) and reports expiry,
   issuer, subject, and hostname-match as metric attributes.
6. **Least privilege is documented** in the systemd unit and Dockerfile; no
   component ever runs with `CAP_SYS_ADMIN` unless explicitly required by a
   future feature.

## Rationale

- **libpcap vs raw sockets:** libpcap is portable, avoids bespoke raw-socket
  parser bugs, and gives timestamps on each packet — ideal for handshake timing.
  Raw sockets would require `CAP_NET_RAW` anyway and duplicate BPF filtering.
- **Optional compilation:** keeps the agent buildable on minimal hosts (e.g.
  CI without libpcap) while still shipping the full feature when available.
- **Capability scoping:** `CAP_NET_RAW` alone cannot read arbitrary files or
  become root; combined with `NoNewPrivileges`, `ProtectSystem=strict` and an
  unprivileged user, it satisfies least privilege.
- **System tools for diagnostics:** writing a full traceroute/tcpdump clone is
  out of scope; delegating to the well-tested host tools yields better output
  and less attack surface than embedding more native code.

## Alternatives Considered

| Option | Reason for Rejection |
|---|---|
| Raw sockets instead of libpcap | More code, more parser bugs, same `CAP_NET_RAW` requirement |
| Run the agent as root | Violates least privilege; root compromise = host compromise |
| Bundled `traceroute`/`tcpdump` binaries | Heavy, GPL-licensed, version-mismatch risks; host tools suffice |
| Upload pcap files to the collector | Storage/cost and security surface; text summaries are enough for Phase 4 |

## Consequences

- Agent build is conditional on libpcap; CI and the Dockerfile install
  `libpcap-dev` (and `traceroute`/`tcpdump` at runtime) for the full feature set.
- `CAP_NET_RAW` is the only elevated capability the agent may hold.
- Diagnostic endpoints accept text commands; the collector validates that a
  diagnostic endpoint was advertised by the agent before calling it.
- TLS certificate attributes flow through the existing metrics pipeline
  (now persisted as JSONB) and are surfaced in the dashboard timeline.

## Compliance

- Security: least privilege (non-root, `CAP_NET_RAW` only), no secrets, no
  unvalidated file uploads.
- Cross-cutting: ADR format per DEV_PLAN.md; tests for the new probes; docs in
  `docs/networking-deep-dive.md`.
