# Kernel Tuning Experiments

> Practical Linux network-stack tuning experiments for PudimNetMon agents, and
> how the agent's own probes observe the impact. Apply with
> `sysctl -w` (temporary) or `/etc/sysctl.d/*.conf` (persistent).

---

## 1. TCP buffers (`tcp_rmem`, `tcp_wmem`)

Default auto-tuning is usually fine; the interesting cases are large-BDP links.

```bash
# Current values (three numbers: min, default, max) in bytes
cat /proc/sys/net/ipv4/tcp_rmem
cat /proc/sys/net/ipv4/tcp_wmem

# Example: widen buffers for a 1 Gbps/100ms path (BDP ≈ 12.5 MB)
sudo sysctl -w net.ipv4.tcp_rmem="4096 131072 16777216"
sudo sysctl -w net.ipv4.tcp_wmem="4096 16384 16777216"
```

**Agent observation:** the HTTP/2 probe (`--http-protocols=http1.1,http2`) total
time and the TCP retransmit probe (`tcp_retransmit`) show whether throughput is
buffer-limited (large RTT × small buffers → window stalls, more RTOs).

## 2. Congestion control: cubic vs bbr

```bash
# Current algorithm
cat /proc/sys/net/ipv4/tcp_congestion_control

# Available algorithms
cat /proc/sys/net/ipv4/tcp_available_congestion_control

# Switch to BBR (needs 4.9+ kernel and the module)
sudo modprobe tcp_bbr
sudo sysctl -w net.ipv4.tcp_congestion_control=bbr
```

**Agent observation:** compare `tcp_retransmit` and `tcp_handshake` latency
before/after. BBR usually reduces retransmissions on lossy high-BDP paths but
can look worse on very short transfers (it spends time probing).

| Algorithm | Best for | Agent probe signature |
|---|---|---|
| cubic | default, fair on typical paths | higher retransmit under loss |
| bbr | lossy high-BDP, streaming | lower retransmit, longer ramp-up on short flows |

## 3. TCP Fast Open (`tcp_fastopen`)

Allows the client to send data in the SYN packet — saving one RTT on new
connections.

```bash
# 1 = client enabled, 2 = server enabled, 3 = both
cat /proc/sys/net/ipv4/tcp_fastopen
sudo sysctl -w net.ipv4.tcp_fastopen=3
```

**Agent observation:** the `tcp_handshake` probe measures SYN→SYN-ACK (the RTT
that TFO avoids for data) — expect no change there, but HTTP/1.1 total time to
TFO-enabled servers drops by ~1 RTT. Note the agent must use `TCP_FASTOPEN_CONNECT`
socket option to benefit (not yet enabled in the probes).

## 4. TIME-WAIT reuse (`tcp_tw_reuse`)

Allows the kernel to reuse TIME-WAIT sockets for new outbound connections —
relevant when an agent opens many short-lived connections (e.g. HTTP/1.1 probes).

```bash
cat /proc/sys/net/ipv4/tcp_tw_reuse
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
```

**Caution:** safe only when the local port range won't recycle a connection
whose packets are still in flight; fine for outbound client roles like the
agent. Modern kernels default to `tcp_tw_reuse=2` (safe for loopback only) —
test before enabling globally.

**Agent observation:** with many probe targets, watch for `Cannot assign
requested address` (port exhaustion) vs. no change after enabling.

## 5. Network namespace isolation (optional)

Run the agent inside its own network namespace to prevent it from influencing
or being influenced by the host's routing table:

```bash
sudo ip netns add pudim-mon
sudo ip netns exec pudim-mon ip link set lo up
# Move a NIC (or use a veth pair) into the namespace, then run the agent there:
sudo ip netns exec pudim-mon /usr/local/bin/pudim-agent --node-id=ns-agent ...
```

**Agent observation:** the `dns_record`/`tcp_handshake` probes run against the
namespace's routing/MTU, isolating experiment noise from the host stack.

---

## Safety notes

- `sysctl -w` changes are **not persistent** — reboots revert them.
- Put persistent changes in `/etc/sysctl.d/99-pudimnetmon.conf`.
- Always A/B test with the agent's own metrics; never tune blindly.
