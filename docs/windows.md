# PudimNetMon Agent on Windows

The agent builds and runs on **Windows 10/11 (x64)** as a console application or
as a native auto-start Windows service (`PudimNetMonAgent`). Windows hosts
report into the same collector stack over gRPC.

## Install with the CI-built wizard (recommended)

The "C++ Agent (Windows build)" CI job compiles a self-contained setup EXE and
uploads it as the `pudimnetmon-agent-windows-setup` artifact:

- **Artifact name:** `pudimnetmon-agent-windows-setup`
- **File:** `PudimNetMon-Agent-Setup-<version>.exe`

Run it on the target host (an administrator is required — service
registration). The wizard collects:

| Field | Default | Notes |
|---|---|---|
| Node ID | machine hostname | unique per monitored host; used to identify this agent in the dashboard |
| Collector endpoint(s) | *(empty → `localhost:50051`)* | comma-separated failover list, e.g. `collector.lan:50051,backup.lan:50051` |
| Polling interval | `5000` ms | heartbeat + probe cadence |

The installer then:

1. installs `pudim-agent.exe` under `%ProgramFiles%\PudimNetMon Agent`,
2. registers the `PudimNetMonAgent` auto-start service via the agent's own
   `--install-service` support (same code path as the legacy PS1 wrapper),
3. starts the service,
4. writes `%ProgramData%\PudimNetMon\agent.conf` so the values are easy to
   review/adjust later,
5. bundles the Microsoft VC++ redistributable for machines that lack it.

Uninstalling via **Add/Remove Programs** stops and removes the service
(`--uninstall-service`) before deleting the files.

### Silent / unattended install

The installer accepts the standard Inno Setup command line, which is also what
CI uses for its own smoke test:

```powershell
# Install silently with defaults (hostname as node ID, 5s interval)
.\PudimNetMon-Agent-Setup-0.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

# Uninstall silently
"C:\Program Files\PudimNetMon Agent\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

After a silent install, tune the values in `%ProgramData%\PudimNetMon\agent.conf`
(agent precedence: built-in defaults < config file < service command line) and
restart the service:

```powershell
Restart-Service PudimNetMonAgent
```

> **Note on signing:** CI-built installers are unsigned, so Windows SmartScreen
> will warn on first run (same as the PS1 approach). Code-signing certificates
> can be wired into the packaging step later.

## Build and install manually (MSVC + vcpkg)

CI validates this flow; it is the fallback when you want to build locally:

```powershell
cmake -S agent -B build-agent-windows `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build-agent-windows --config Release -j
# Console mode (foreground)
.\build-agent-windows\Release\pudim-agent.exe --node-id=win-01 --interval=10000
# Or install as the auto-start service (requires an elevated PowerShell)
.\scripts\install-agent-windows.ps1 -ServiceArgs "--node-id=win-01", "--interval=10000"
# Uninstall the service
.\scripts\install-agent-windows.ps1 -Uninstall
```

The installer script lives at `installer\installer-agent.iss` and is compiled
by CI with:

```powershell
ISCC.exe /DMyAppVersion=0.1.0 installer\installer-agent.iss
```

## Service management

```powershell
Get-Service PudimNetMonAgent
sc.exe query PudimNetMonAgent        # details, incl. the baked-in args
sc.exe qc PudimNetMonAgent           # service configuration
Restart-Service PudimNetMonAgent     # pick up config changes
```

The service runs as `NT AUTHORITY\LocalSystem` with automatic start.

## Feature matrix

| Feature | Windows | Notes |
|---|---|---|
| ICMP / ping | ✅ | Windows ICMP API (`iphlpapi`) |
| TCP retransmit | ✅ | `SIO_TCP_INFO` |
| TCP handshake capture | ⚠️ unsupported | libpcap unavailable; metric reports "unsupported" |
| NTP offset | ✅ | built-in SNTP client |
| Traceroute | ✅ | maps to `tracert` via the diagnostic server |
| Disk buffer | ✅ | SQLite, `%ProgramData%\PudimNetMon\pending.db` |
| mTLS to collector | ✅ | PEM files via config (`tls-ca`, `tls-cert`, `tls-key`) |
