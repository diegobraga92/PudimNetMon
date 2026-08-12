# PudimNetMon Agent on Windows

The agent (`pudim-agent.exe`) builds and runs on Windows 10/11 as either a
**console application** or a native **Windows service**. The collector,
consumers, TimescaleDB and Kafka are unaffected — they keep running on Linux;
Windows hosts simply run an agent that reports into the same stack over gRPC.

## Feature matrix

| Capability | Windows support |
|---|---|
| Heartbeat, gRPC, mTLS, failover, disk buffer (SQLite) | ✅ |
| DNS / TCP connect / TLS handshake / TLS certificate / HTTP (1.1, 2, 3) | ✅ |
| ICMP ping (loss / RTT / jitter) | ✅ via `iphlpapi` (no admin needed) |
| TCP retransmit | ✅ via `WSAIoctl(SIO_TCP_INFO)` (Windows 10+) |
| NTP offset | ✅ via a built-in SNTP client (`--ntp-server`, default `pool.ntp.org`) |
| TCP handshake capture (libpcap) | ⚠️ degraded — returns a `tcp_handshake` failure metric |
| On-demand diagnostic: traceroute | ✅ via `tracert` |
| On-demand diagnostic: packet capture | ⚠️ degraded — returns "not supported on this platform" |
| systemd notify / watchdog | ➡️ replaced by Windows Service status reporting |
| `CAP_NET_RAW` raw sockets | ➡️ not needed (ICMP API used instead) |

## Prerequisites

- Windows 10 (build 1809+) or Windows 11, x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.20+
- Git
- [vcpkg](https://github.com/microsoft/vcpkg) (for gRPC, Protobuf, OpenSSL, libcurl, SQLite)

## Build

```powershell
# 1. Bootstrap vcpkg (once)
git clone https://github.com/microsoft/vcpkg C:\vcpkg
& C:\vcpkg\bootstrap-vcpkg.bat

# 2. Configure and build using the agent manifest (agent/vcpkg.json)
cmake -S agent -B build-agent-windows `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build-agent-windows --config Release -j

# 3. Run the probe unit tests
ctest --test-dir build-agent-windows -C Release --output-on-failure
```

The binary is produced at `build-agent-windows\Release\pudim-agent.exe`.
`agent/vcpkg.json` declares the dependencies, so `vcpkg install` (or the CMake
toolchain) builds/installs them automatically. The first build downloads and
compiles gRPC/Protobuf/OpenSSL, which takes a while; vcpkg caches the result.

## Run as a console application

```powershell
build-agent-windows\Release\pudim-agent.exe `
  --node-id=win-01 --interval=10000 `
  --collector-endpoints=collector.lan:50051 `
  --dns-targets=example.com --tcp-targets=example.com:443 `
  --tls-targets=example.com:443 --http-targets=https://example.com `
  --ping-targets=1.1.1.1
```

## Install as a Windows service

From an **elevated** PowerShell prompt:

```powershell
# Auto-start service named "PudimNetMonAgent" (LocalSystem)
.\scripts\install-agent-windows.ps1 `
  -ServiceArgs "--node-id=win-01", "--interval=10000", `
               "--collector-endpoints=collector.lan:50051"

# Stop/remove it
.\scripts\install-agent-windows.ps1 -Uninstall
```

The script calls the binary's built-in `--install-service` / `--uninstall-service`
handlers (which create/delete the service via the SCM) and then starts it. You
can also drive it manually:

```powershell
# install:  pudim-agent.exe --install-service --node-id=win-01 --interval=10000 ...
# uninstall: pudim-agent.exe --uninstall-service
sc.exe query PudimNetMonAgent
```

Graceful shutdown works like systemd: `Stop-Service` / `sc stop` raises
`SERVICE_CONTROL_STOP`, which sets the agent's stop flag so it drains its disk
buffer and exits cleanly.

## Notes & limitations

- **Default state path**: the SQLite disk buffer lives in
  `%ProgramData%\PudimNetMon\pending.db` (created on first start; override with
  `--disk-buffer-path`).
- **Diagnostics**: `traceroute` maps to `tracert`; packet capture (`tcpdump`)
  has no Windows equivalent, so the diagnostic endpoint reports it as
  unsupported. Installing [Npcap](https://npcap.com/) would allow wiring the
  `tcp_handshake` probe and packet capture in a follow-up.
- **TCP_INFO**: the `tcp_retransmit` probe requires Windows 10+ (older Windows
  reports a failure metric).
- **CI**: `.github/workflows/ci.yml` includes a `cpp-agent-windows` job that
  builds with MSVC + vcpkg and runs `ctest`.
