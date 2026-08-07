# mTLS Certificate Management & Rotation

## Overview

Agent↔collector gRPC traffic is protected with **mutual TLS (mTLS)**:

* The **collector** presents a server certificate (`CN=collector`, SANs for
  `localhost` / `collector` / `collector-1` / `collector-2` / `127.0.0.1`).
* The **agent** presents a client certificate (`CN=agent`, SANs for
  `localhost` / `agent` / `agent-1` / `agent-2` / `127.0.0.1`).
* Both sides verify the peer against a shared **CA** (`ca.crt`).
* The collector's gRPC server uses
  `GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY` — a client
  **without a CA-signed certificate is rejected at the handshake**.

The agent's diagnostic gRPC server (port 50052, collector-triggered
traceroute/pcap) uses the same CA and the agent's cert/key, so the collector
authenticates to the agent as well — every gRPC hop in the system is mutually
authenticated.

## Generating certificates

```bash
./scripts/gen-certs.sh certs
```

Produces `certs/{ca,collector,agent}.{crt,key}`. **`ca.key` is the trust root —
keep it offline** (air-gapped / HSM) and never commit private keys
(`certs/` and `*.key` are gitignored).

## Wiring

| Component | Flags |
|---|---|
| Collector (server) | `--tls-ca certs/ca.crt --tls-cert certs/collector.crt --tls-key certs/collector.key` |
| Agent (client) | `--tls-ca certs/ca.crt --tls-cert certs/agent.crt --tls-key certs/agent.key` |

If `--tls-*` is omitted the components fall back to **insecure** gRPC (local
dev / CI convenience). Log a line at startup:
`gRPC transport: insecure (no --tls-*)` vs `gRPC transport: mTLS (...)`.

## Rotation procedure

Leaf certificates have a 1-year validity; rotate them without changing the CA:

```bash
# 1. Create a fresh agent cert signed by the existing CA.
openssl genrsa -out certs/agent.key 2048
openssl req -new -key certs/agent.key -subj "/CN=agent/O=PudimNetMon" -out /tmp/agent.csr
openssl x509 -req -in /tmp/agent.csr -CA certs/ca.crt -CAkey <path-to-offline-ca.key> \
  -CAcreateserial -days 365 -out certs/agent.crt \
  -extfile <(printf "subjectAltName = DNS:localhost, DNS:agent, DNS:agent-1, DNS:agent-2, IP:127.0.0.1\n")

# 2. Deploy certs/agent.{crt,key} to the agent, then restart it.
#    Collectors are unaffected — they keep verifying against ca.crt.
```

Rollover order (zero-downtime):

1. **Deploy + restart one agent** with the new cert and verify heartbeats
   (`curl collector:8080/agents` → `alive:true`).
2. **Roll the remaining agents** one at a time.
3. **Rotate the collector's server cert** last (its client identity also
   authenticates to agent diagnostic servers, so restart collector + verify a
   diagnostic round-trip).

CA compromise / rotation is out of scope for this bootstrap toolchain; in
production use a proper PKI (Vault PKI, cert-manager, or an internal CA) that
supports CRL/OCSP and short-lived certs.

## Revocation

There is no CRL/OCSP in this bootstrap setup. To expel a compromised agent
immediately: rotate the CA (all certs) or block the agent's identity in the
collector's network policy / firewall. This is acceptable for a portfolio/
lab deployment; production should add CRL distribution points.
