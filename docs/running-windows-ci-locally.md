# Running the Windows CI build locally

CI runs a Windows job (`cpp-agent-windows` in `.github/workflows/ci.yml`) that
builds the agent with **MSVC + vcpkg**, runs CTest, compiles the Inno Setup
installer and smoke-tests it. Because the code is usually developed and built
on Linux, Windows-only regressions only show up on that CI job.

[`scripts/run-ci-windows.ps1`](../scripts/run-ci-windows.ps1) reproduces that
job step-for-step on a local Windows machine so you can find those failures
before pushing. It is **not** a shortcut: it uses the same tools, versions,
commands, environment variables and pinned baselines as CI.

## Host prerequisites (what the `windows-latest` runner has)

Install all of the following from an **elevated** PowerShell, then open a
**new** PowerShell and run the script:

```powershell
# 1. Visual Studio 2022 Build Tools - MSVC v143 x64/x86 + Windows 10/11 SDK
winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
  --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

# 2. CMake (>= 3.20) - the runner image ships it on PATH
winget install -e --id Kitware.CMake

# 3. Git for Windows (if not already installed)
winget install -e --id Git.Git

# 4. Chocolatey + Inno Setup 6 (step 7 runs the exact CI command
#    `choco install innosetup -y --no-progress`). Alternatively install Inno
#    Setup another way (winget install -e --id JRSoftware.InnoSetup) and pass
#    -SkipInnoSetupInstall to the script.
Set-ExecutionPolicy Bypass -Scope Process -Force
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
choco install innosetup -y --no-progress
```

vcpkg does **not** need to be installed: the script clones it at the exact
baseline CI pins (`.vcpkg` in the repo root) and downloads the pinned
vcpkg-tool. The agent dependencies (`grpc`, `protobuf`, `openssl`, `curl`,
`sqlite3` from `agent/vcpkg.json`) are compiled from source by vcpkg for the
`x64-windows-static-md-release` triplet on the first Configure - this is the
long part and matches a cold CI run.

## Running the script

```powershell
cd C:\dev\PudimNetMon

# Windows PowerShell 5.1 can block script execution; -ExecutionPolicy Bypass
# avoids that (this is the only reason the flag is needed):
powershell -ExecutionPolicy Bypass -File .\scripts\run-ci-windows.ps1 -Clean
```

To include step 10 (the installer smoke test that registers/upgrades/uninstalls
the real `PudimNetMonAgent` Windows service), run from an **elevated**
PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-ci-windows.ps1 -Clean -RunSmokeTest
```

> Re-running the job on the same machine after a smoke test no longer needs
> manual cleanup: `-RunSmokeTest` (and `-ResetServiceState`) first removes any
> leftover agent state from a previous run - the `PudimNetMonAgent` service,
> `C:\Program Files\PudimNetMon Agent`, `%ProgramData%\PudimNetMon` (the stale
> `agent.conf` would otherwise break `agent-config-tests`) and the Add/Remove
> entry - so every run starts from a CI-fresh machine.

### What it runs (same order as the CI job)

0. Reset leftover agent state (CI-fresh machine) - only when `-RunSmokeTest`/`-ResetServiceState`
1. Checkout vcpkg (pinned baseline)
2. Fetch vcpkg tool (pinned vcpkg-tool release)
3. Inspect restored vcpkg cache
4. Configure (MSVC): `cmake -S agent -B build\agent-win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<repo>\.vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release`
5. Build: `cmake --build build\agent-win --config Release -j 2`
6. Test: `ctest -C Release --output-on-failure`
7. Install Inno Setup (`choco install innosetup -y --no-progress`)
8. Stage installer payload (`pudim-agent.exe` + `vc_redist.x64.exe`)
9. Build installer (`ISCC.exe /DMyAppVersion=<version>`)
10. Smoke test installer (install -> config model -> upgrade -> uninstall)

Environment mirrors CI exactly: `VCPKG_ROOT=<repo>\.vcpkg`,
`VCPKG_DEFAULT_TRIPLET=x64-windows-static-md-release`,
`VCPKG_TOOL_VERSION=2026-07-27`,
`VCPKG_BINARY_SOURCES=clear;files,<repo>/.vcpkg-cache,readwrite`.

### Useful flags

| Flag | Meaning |
|---|---|
| `-Clean` | wipe `build\agent-win`, `dist\installer`, `installer\payload` first (CI uses a fresh checkout; vcpkg clones/caches are kept like the CI caches) |
| `-SkipInstaller` | stop after step 6 - good while iterating on a compile/test failure |
| `-SkipInnoSetupInstall` | skip `choco install innosetup` if you installed Inno Setup yourself |
| `-RunSmokeTest` | run step 10 - requires an elevated shell; implies `-ResetServiceState` |
| `-ResetServiceState` | remove leftover agent state (service, Program Files payload, `%ProgramData%\PudimNetMon`, Add/Remove entry) before the run - use when re-running after a smoke test |
| `-KeepGoing` | keep running the remaining steps after a failure (CI stops at the first failure) |
| `-Jobs N` | parallel build jobs (default `2`, same as CI) |

The preflight runs first and prints exactly which host tool is missing plus the
install command for it. The script exits non-zero when any step fails, like CI.

## Expected first-run timing

A cold run has to compile the vcpkg dependency graph (gRPC/protobuf/OpenSSL/
cURL/SQLite3, static triplet) from source - allow 1-3 h on a typical machine,
same as a cold CI run. Later runs reuse `.vcpkg-cache` (the files-based binary
cache) and are much faster.
