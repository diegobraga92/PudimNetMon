# Networking Deep-Dive

> Annotated packet-flow analysis and troubleshooting for PudimNetMon's Phase 4
> deep diagnostics: TCP handshake, DNS resolution, TLS negotiation, HTTP/2 vs
> HTTP/3, and path MTU discovery.

---

## 1. TCP Three-Way Handshake

The agent's `CHECK_TYPE_TCP_HANDSHAKE` probe captures the handshake with
libpcap and reports `latency_ms` = SYN→SYN-ACK (one RTT plus server processing)
and `synack_ack_ms` = SYN-ACK→ACK (client kernel response, usually < 1 ms).

```
Client                          Server
  │  ─── SYN (seq=x) ─────────▶   │   t=0.000ms
  │                              │
  │  ◀── SYN+ACK (seq=y, ack=x+1)│   t=45.200ms   ← latency_ms = 45.2ms
  │                              │
  │  ─── ACK (ack=y+1) ────────▶ │   t=45.205ms   ← synack_ack_ms = 0.005ms
```

### See it yourself

```bash
# One-shot capture of the handshake to example.com:443
sudo tcpdump -i any -nn -c 3 'tcp port 443 and host example.com'

# With timestamps and verbose decode
sudo tcpdump -i any -nn -tttt -v 'tcp port 443 and host example.com'
```

Expected output (annotated):

```
1  10:12:33.000000 IP 192.168.1.20.48124 > 93.184.216.34.443: Flags [S], seq 1000   ← SYN
2  10:12:33.045200 IP 93.184.216.34.443 > 192.168.1.20.48124: Flags [S.], seq 2000, ack 1001  ← SYN-ACK
3  10:12:33.045205 IP 192.168.1.20.48124 > 93.184.216.34.443: Flags [.], ack 2001   ← ACK
```

**Troubleshooting:**

| Symptom | Meaning | Next step |
|---|---|---|
| SYN sent, no SYN-ACK | Server not listening or filtered | `nmap -p443 <host>`; check firewall/ACLs |
| SYN retransmitted (RTO backoff) | Packet loss on the path | `mtr -rw <host>`; compare with `ping` |
| SYN-ACK received, connect() fails | Local kernel rejects | check `ss -s`, SYN cookies (`net.ipv4.tcp_syncookies`) |

---

## 2. DNS Resolution

`CHECK_TYPE_DNS_RECORD` reports A/AAAA/CNAME records and validates expected
values (e.g. `--dns-expected example.com=A:93.184.216.34`).

```bash
# Full resolution chain
dig +trace example.com | head -20

# Canonical name (CNAME target)
dig example.com CNAME +noall +answer

# Compare with what the agent resolved
curl 'http://<collector>:8080/api/metrics?check_type=dns_record'
```

**Annotated A/AAAA records for example.com:**

```
; example.com.                3600  IN  A      93.184.216.34
; example.com.                3600  IN  A      93.184.216.35
; example.com.                3600  IN  AAAA   2606:2800:220:1:248:1893:25c8:1946
```

**Troubleshooting:**

| Symptom | Meaning | Next step |
|---|---|---|
| `dns_record` A is empty | No A record (AAAA-only host) | check `dig <host> A` |
| A/AAAA mismatch vs expected | DNS rebinding, split-horizon DNS, stale cache | `dig @8.8.8.8 <host>` vs local resolver |
| CNAME differs | Host is an alias | update `--dns-expected host=CNAME:target` |

---

## 3. TLS Handshake & Certificate Validation

`CHECK_TYPE_TLS_HANDSHAKE` measures handshake latency; `CHECK_TYPE_TLS_CERTIFICATE`
reports the peer certificate's subject, issuer, expiry (days), and hostname match.

```
Client                                   Server
  │  ClientHello ─────────────────────▶   │
  │  ◀── ServerHello, Certificate, ...    │   (one RTT, often 0-RTT with TLS 1.3)
  │  Finished ───────────────────────▶   │
  │  ◀── Finished                        │
  │  Application Data ────────────────▶  │
```

### Annotated OpenSSL certificate output

```
$ echo | openssl s_client -connect example.com:443 -servername example.com 2>/dev/null \
    | openssl x509 -noout -subject -issuer -dates -ext subjectAltName
subject=CN = example.com
issuer=CN = Cloudflare TLS Issuing ECC CA 3, O = SSL Corporation, C = US
notBefore=Jan  1 00:00:00 2026 GMT
notAfter=Jan  1 00:00:00 2027 GMT
X509v3 Subject Alternative Name:
    DNS:example.com, DNS:www.example.com
```

The agent's metric attributes mirror this:
`tls_cert_subject`, `tls_cert_issuer`, `tls_cert_expiry_days`, `tls_cert_hostname_match`.

**Troubleshooting:**

| Symptom | Meaning | Next step |
|---|---|---|
| `tls_certificate` success=false, chain invalid | Untrusted CA or missing intermediate | verify with `openssl s_client -verify_return_error` |
| hostname match = false | Cert SAN does not cover the probed name | check `-ext subjectAltName` |
| `tls_cert_expiry_days` negative | Certificate is expired | rotate the certificate |

---

## 4. HTTP/1.1 vs HTTP/2 vs HTTP/3

The agent measures the same URL over each protocol
(`--http-protocols=http1.1,http2,http3`); the metric target embeds the protocol
(e.g. `https://example.com;http2`). Latency is curl's `TOTAL_TIME`.

| Protocol | Transport | Handshake | Notes |
|---|---|---|---|
| HTTP/1.1 | TCP + TLS | 2 RTT | one request per connection, HEAD/GET |
| HTTP/2 | TCP + TLS | 2 RTT + connection reuse | multiplexing; ALPN |
| HTTP/3 | UDP/QUIC + TLS 1.3 | 1 RTT (0-RTT resumption) | requires curl built with quiche/nghttp3 |

**Verifying protocol support of the installed curl:**

```bash
curl --version | grep -oE 'HTTP2|HTTP3|quiche|nghttp3'
# curl 8.x with nghttp2 → HTTP/2 only; HTTP/3 needs a QUIC-enabled build
```

If the build lacks HTTP/3, the `http3` metric is `success=false` with detail
`"http/3 not available in this curl build"` — the comparison chart simply shows
the two supported protocols.

**Troubleshooting:**

| Symptom | Meaning | Next step |
|---|---|---|
| `http2` fails on a cleartext URL | HTTP/2 requires TLS (h2c is non-standard) | use `https://` |
| `http3` unsupported | curl lacks QUIC | rebuild curl with quiche or document as N/A |
| Same latency across protocols | Server/ALPN forces negotiation | `curl -I -v` to see `ALPN, server accepted` |

---

## 5. Packet Retransmissions (`TCP_INFO`)

`CHECK_TYPE_TCP_RETRANSMIT` reads `tcpi_total_retrans` from
`getsockopt(fd, IPPROTO_TCP, TCP_INFO)` after connecting. Zero means a clean
connect; non-zero indicates lost SYN/data segments.

```bash
# Kernel-wide retransmission stats (not per-socket)
netstat -s | grep -i retrans
```

| Symptom | Meaning | Next step |
|---|---|---|
| retransmit > 0, high RTT | Lossy path or congestion | `mtr`, `ping -f` (careful), check `tcp_rmem/tcp_wmem` |
| retransmit grows over time | Sustained packet loss | capture with tcpdump; look for duplicate ACKs |

---

## 6. Path MTU Discovery (PMTUD)

The agent documents PMTUD rather than implementing raw probes; use the standard
tools on the agent host:

```bash
# Per-hop MTU along the path
tracepath -n example.com

# Force DF (don't fragment) to find the max usable payload
ping -M do -s 1472 example.com   # 1500 MTU → 1472 payload + 28 IP/ICMP bytes
ping -M do -s 1473 example.com   # fragments blocked → "Packet needs to be fragmented"
```

- `ping -M do -s 1472` succeeds and `-s 1473` fails → path MTU is 1500.
- If `1472` also fails, the path MTU is smaller (e.g. PPPoE 1492, VPN overlays).
- `tracepath` prints `pmtu 1500` at each hop and flags blackholes.

| Symptom | Meaning | Next step |
|---|---|---|
| TCP works, `ping -M do` fails | ICMP-based PMTUD blocked; TCP blackhole | check `net.ipv4.tcp_mtu_probing=1` (Linux) |
| Large HTTPS requests stall | Path MTU < 1500 | reduce MSS or enable `tcp_mtu_probing` |

---

## 7. Diagnostic Mode

The collector can trigger the agent's diagnostic gRPC service:

```bash
curl -s -X POST 'http://<collector>:8080/diagnostic' \
  --data-urlencode 'agent_id=agent-docker-001' \
  --data-urlencode 'trace_target=example.com' \
  --data-urlencode 'pcap_duration_s=5' \
  --data-urlencode 'pcap_filter=tcp port 443'
```

The response contains the `traceroute` output and a `tcpdump` summary (first 15
packets + total count). Agent hosts must have `traceroute` and `tcpdump`
installed; without them the result reports the missing tool.

```bash
sudo apt-get install -y traceroute tcpdump
```

> **Security note (ADR 005):** the diagnostic service is plaintext gRPC on the
> agent's diagnostic port; Phase 5 adds mTLS. The agent runs with
> `CAP_NET_RAW` only and never as root.
