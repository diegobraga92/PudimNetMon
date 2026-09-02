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

Configuration is split so an upgrade/reinstall never leaves stale values
frozen into the service registration: **the node ID is baked into the service
command line** (like the Linux unit's `--node-id=%H`), and everything mutable
(collector endpoints, interval) lives in `%ProgramData%\PudimNetMon\agent.conf`
which the agent reads at startup. Precedence stays
`built-in defaults < agent.conf < service command line`.

The installer then:

1. installs `pudim-agent.exe` under `%ProgramFiles%\PudimNetMon Agent`,
2. writes `%ProgramData%\PudimNetMon\agent.conf` with the wizard's interval /
   collector endpoint(s) (before the service is registered or started),
3. registers the `PudimNetMonAgent` auto-start service via the agent's own
   `--install-service` support, baking in only `--node-id`,
4. starts the service,
5. bundles the Microsoft VC++ redistributable for machines that lack it.

Uninstalling via **Add/Remove Programs** stops and removes the service
(`--uninstall-service`) before deleting the files. State files under
`%ProgramData%\PudimNetMon` (`agent.conf`, `pending.db`) are kept so buffered
metrics survive an uninstall.

### Silent / unattended install

The installer accepts the standard Inno Setup command line, which is also what
CI uses for its own smoke test:

```powershell
# Install silently with defaults (hostname as node ID, 5s interval)
.\PudimNetMon-Agent-Setup-0.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

# Uninstall silently
"C:\Program Files\PudimNetMon Agent\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
```

After a silent install, all agent settings live in
`%ProgramData%\PudimNetMon\agent.conf` (the node ID is on the service command
line — see `sc.exe qc PudimNetMonAgent`). Edit the file and restart the
service to apply changes:

```powershell
Restart-Service PudimNetMonAgent
```

> **Note on signing:** CI-built installers are unsigned, so Windows SmartScreen
> will warn on first run (same as the PS1 approach). Code-signing certificates
> can be wired into the packaging step later.

## Build and install manually (MSVC + vcpkg)

CI validates this build; it is the fallback when you want to build locally
without the CI wizard. Run the binary in the foreground for a quick check, or
register it as the auto-start service yourself (elevated PowerShell):

```powershell
cmake -S agent -B build-agent-windows `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build-agent-windows --config Release -j
$exe = "$PWD\build-agent-windows\Release\pudim-agent.exe"

# Console mode (foreground; stops when the session closes)
& $exe --node-id=win-01 --interval=10000

# Run as the auto-start "PudimNetMonAgent" service instead.
# The binary does the registration; only the node ID is baked into the service
# command line — write the mutable settings to agent.conf first:
$confDir = Join-Path $env:ProgramData 'PudimNetMon'
New-Item -ItemType Directory -Force $confDir | Out-Null
'collector-endpoints=collector.lan:50051', 'interval=10000' |
  Set-Content (Join-Path $confDir 'agent.conf')
& $exe --install-service '--node-id=win-01'
Start-Service PudimNetMonAgent

# Uninstall the service (state files in the agent.conf folder are kept)
& $exe --uninstall-service
```

The installer script lives at `installer\installer-agent.iss` and is compiled
by CI with:

```powershell
ISCC.exe /DMyAppVersion=0.1.0 installer\installer-agent.iss
```

## Service management

```powershell
Get-Service PudimNetMonAgent
sc.exe query PudimNetMonAgent        # running state
sc.exe qc PudimNetMonAgent           # service config (binPath carries --node-id)
type "$env:ProgramData\PudimNetMon\agent.conf"   # all mutable settings
Restart-Service PudimNetMonAgent     # pick up agent.conf changes
```

The service runs as `NT AUTHORITY\LocalSystem` with automatic start. To change
the node ID, re-run the installer/PS1 script (or update the ImagePath with
`sc.exe config PudimNetMonAgent binPath= "..."`); every other setting is edited
in `agent.conf` and applied with a restart.

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
