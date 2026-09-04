#!/usr/bin/env pwsh
# Requires Windows PowerShell 5.1 or later (also runs under PowerShell 7).
#requires -Version 5.1
<#
.SYNOPSIS
  Runs the PudimNetMon Windows CI job ("C++ Agent (Windows build)") locally.

.DESCRIPTION
  Step-for-step local reproduction of the job `cpp-agent-windows` in
  .github/workflows/ci.yml (the job that runs on `windows-latest` and is the
  Windows job failing in CI). No shortcuts: the same tools, versions, commands
  and environment variables CI uses are used here.

  Steps (names and order mirror the workflow):
    1. Checkout vcpkg (pinned baseline e90cc0982b7cfae62447f1f3bed1fbca0bc8f6be)
    2. Fetch vcpkg tool (vcpkg-tool release 2026-07-27, like CI)
    3. Inspect restored vcpkg cache
    4. Configure (MSVC)   cmake -S agent -B build/agent-win
                            -DCMAKE_BUILD_TYPE=Release
                            -DCMAKE_TOOLCHAIN_FILE=<repo>/.vcpkg/scripts/buildsystems/vcpkg.cmake
                            -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release
    5. Build              cmake --build build/agent-win --config Release -j 2
    6. Test (CTest)       ctest -C Release --output-on-failure
    7. Install Inno Setup (choco install innosetup -y --no-progress, like CI)
    8. Stage installer payload (pudim-agent.exe + vc_redist.x64.exe)
    9. Build installer (Inno Setup)  ISCC.exe /DMyAppVersion=<version>
   10. Smoke test installer (install -> config model -> upgrade -> uninstall)

  Environment variables set exactly like the CI job:
    VCPKG_ROOT             <repo>\.vcpkg
    VCPKG_DEFAULT_TRIPLET  x64-windows-static-md-release
    VCPKG_TOOL_VERSION     2026-07-27
    VCPKG_BINARY_SOURCES   clear;files,<repo>/.vcpkg-cache,readwrite

  The first Configure step makes vcpkg compile grpc/protobuf/openssl/curl/
  sqlite3 from source for the static triplet (agent/vcpkg.json). That is the
  long part and matches a cold CI run.

.PARAMETER Root
  Repository root. Defaults to the parent of this script.

.PARAMETER Clean
  Delete build/agent-win, dist/installer and installer/payload before running
  (CI uses a fresh checkout). .vcpkg sources and .vcpkg-cache are kept, like
  the CI cache layers.

.PARAMETER Jobs
  Parallel build jobs for `cmake --build`. Default 2 (the same bounded value
  the CI job uses to avoid OOM while linking gRPC).

.PARAMETER SkipInstaller
  Stop after Test (CTest): skip steps 7-10.

.PARAMETER SkipInnoSetupInstall
  Do not run `choco install innosetup`; use an Inno Setup 6 already installed.

.PARAMETER RunSmokeTest
  Run step 10. It registers the real auto-start PudimNetMonAgent service, so
  it requires an ELEVATED PowerShell (CI runs it on every Windows run; locally
  it is opt-in because it modifies this machine).

  This switch also implies -ResetServiceState: before the run, any leftover
  agent state from a previous smoke-test run (service, Program Files payload,
  %ProgramData%\PudimNetMon\agent.conf, Add/Remove entry) is removed so every
  run starts from a CI-fresh machine. Without that reset, the stale agent.conf
  left by the previous run's uninstall makes agent-config-tests fail when this
  script is re-run on the same machine.

.PARAMETER ResetServiceState
  Remove leftover PudimNetMon Agent state from a previous installer/smoke-test
  run before starting: the PudimNetMonAgent service, C:\Program Files\PudimNetMon
  Agent, %ProgramData%\PudimNetMon and the Add/Remove Programs entry. Requires
  an ELEVATED PowerShell. Use this when re-running the script after a smoke
  test (it is implied by -RunSmokeTest).

.PARAMETER KeepGoing
  Run every step even if an earlier one fails (CI stops at the first failure).

.EXAMPLE
  # Full CI parity from an admin PowerShell:
  powershell -ExecutionPolicy Bypass -File .\scripts\run-ci-windows.ps1 -Clean -RunSmokeTest

.EXAMPLE
  # Reproduce the failing configure/build/test part while iterating:
  .\scripts\run-ci-windows.ps1 -SkipInstaller
#>
[CmdletBinding()]
param(
    [string]$Root = '',
    [switch]$Clean,
    [switch]$KeepGoing,
    [switch]$SkipInstaller,
    [switch]$SkipInnoSetupInstall,
    [switch]$RunSmokeTest,
    [switch]$ResetServiceState,
    [int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'

# ---- constants copied from .github/workflows/ci.yml (job cpp-agent-windows) ----
$VcpkgPin = 'e90cc0982b7cfae62447f1f3bed1fbca0bc8f6be'
$VcpkgToolVersion = '2026-07-27'
$Triplet = 'x64-windows-static-md-release'

# ---- resolve repository root ---------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
} else {
    $Root = (Resolve-Path $Root).Path
}
$BuildDir  = Join-Path $Root 'build\agent-win'
$VcpkgDir  = Join-Path $Root '.vcpkg'
$VcpkgCacheDir = Join-Path $Root '.vcpkg-cache'
$DistDir   = Join-Path $Root 'dist\installer'
$PayloadDir = Join-Path $Root 'installer\payload'

$script:Failed = $false
$script:Results = New-Object System.Collections.Generic.List[object]

# ---- command/step infrastructure ----------------------------------------------
function Invoke-NativeCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = ''
    )
    $argLine = $Arguments -join ' '
    Write-Host ''
    Write-Host "   > $FilePath $argLine"
    if ($WorkingDirectory) { Push-Location $WorkingDirectory }
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "command failed with exit code ${LASTEXITCODE}: $FilePath $argLine"
        }
    } finally {
        if ($WorkingDirectory) { Pop-Location }
    }
}

function Invoke-Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Center "STEP: $Name"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
        $sw.Stop()
        $script:Results.Add([pscustomobject]@{
            Name = $Name
            Status = 'PASS'
            Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        })
        Write-Ok "$Name ($([math]::Round($sw.Elapsed.TotalSeconds, 1)) s)"
    } catch {
        $sw.Stop()
        $script:Results.Add([pscustomobject]@{
            Name = $Name
            Status = 'FAIL'
            Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        })
        $script:Failed = $true
        Write-Fail "$Name ($([math]::Round($sw.Elapsed.TotalSeconds, 1)) s)"
        Write-Host ('       ' + $_.Exception.Message) -ForegroundColor Red
        if (-not $KeepGoing) {
            throw 'Aborting on first failure (use -KeepGoing to run the rest).'
        }
    }
}

function Show-Summary {
    Write-Center 'SUMMARY'
    foreach ($r in $script:Results) {
        $color = if ($r.Status -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Host ('  {0,-4} {1,-58} {2,8} s' -f $r.Status, $r.Name, $r.Seconds) `
            -ForegroundColor $color
    }
    Write-Host ''
}

function Remove-DirIfExists([string]$Path) {
    if (Test-Path $Path) { Remove-Item -Recurse -Force $Path }
}

# The exact environment the CI job sets in its `env:` block.
function Set-CiEnvironment {
    $env:VCPKG_ROOT = $VcpkgDir
    $env:VCPKG_DEFAULT_TRIPLET = $Triplet
    $env:VCPKG_TOOL_VERSION = $VcpkgToolVersion
    $env:VCPKG_BINARY_SOURCES = "clear;files,$($VcpkgCacheDir.Replace('\', '/')),readwrite"
}


# ---- small helpers -------------------------------------------------------------
function Write-Center([string]$Text) {
    Write-Host ''
    Write-Host ('=' * 78)
    Write-Host $Text
    Write-Host ('=' * 78)
}
function Write-Ok([string]$Msg)   { Write-Host ('PASS  ' + $Msg) -ForegroundColor Green }
function Write-Warn([string]$Msg) { Write-Host ('WARN  ' + $Msg) -ForegroundColor Yellow }
function Write-Fail([string]$Msg) { Write-Host ('FAIL  ' + $Msg) -ForegroundColor Red }

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Session PATH augmentation for tools that exist but are not on this shell's PATH.
function Add-ToolDirToPath([string]$Dir) {
    if ($Dir -and (Test-Path $Dir) -and ($env:Path -notlike "*$Dir*")) {
        $env:Path = "$Dir;$env:Path"
        Write-Warn "adding $Dir to PATH for this session"
    }
}
Add-ToolDirToPath 'C:\Program Files\Git\cmd'
Add-ToolDirToPath 'C:\Program Files\CMake\bin'
Add-ToolDirToPath (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin')

# ---- tool discovery (same tools the windows-latest runner image provides) -------
$script:GitExe = $null
$script:CmakeExe = $null
$script:CtestExe = $null

function Find-Git {
    if ($script:GitExe) { return $script:GitExe }
    $cmd = Get-Command git -ErrorAction SilentlyContinue
    if ($cmd) { $script:GitExe = $cmd.Source }
    elseif (Test-Path 'C:\Program Files\Git\cmd\git.exe') {
        $script:GitExe = 'C:\Program Files\Git\cmd\git.exe'
    }
    return $script:GitExe
}

function Find-CMake {
    if ($script:CmakeExe) { return $script:CmakeExe }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { $script:CmakeExe = $cmd.Source }
    elseif (Test-Path 'C:\Program Files\CMake\bin\cmake.exe') {
        $script:CmakeExe = 'C:\Program Files\CMake\bin\cmake.exe'
    }
    elseif (Test-Path (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')) {
        $script:CmakeExe = Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe'
    }
    return $script:CmakeExe
}

function Find-CTest {
    if ($script:CtestExe) { return $script:CtestExe }
    $cmd = Get-Command ctest -ErrorAction SilentlyContinue
    if ($cmd) { $script:CtestExe = $cmd.Source }
    else {
        $dir = Split-Path (Find-CMake) -Parent
        if ($dir -and (Test-Path (Join-Path $dir 'ctest.exe'))) {
            $script:CtestExe = Join-Path $dir 'ctest.exe'
        }
    }
    return $script:CtestExe
}

# Locates MSVC the way CMake does: VS/Build Tools install with the VC.Tools
# component (queried via vswhere), falling back to `cl` on PATH (a VS developer
# prompt). Also reports whether the Windows 10/11 SDK headers exist - the agent
# needs them (SIO_TCP_INFO, _WIN32_WINNT=0x0A00).
function Find-Msvc {
    $r = @{ Found = $false; Cl = $null; VsPath = $null; Sdk = $false }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = (& $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1)
        if ($vsPath) {
            $r.VsPath = $vsPath
            $cl = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Recurse -Filter cl.exe `
                    -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match 'Hostx64\\x64' } |
                Select-Object -First 1
            if ($cl) { $r.Cl = $cl.FullName; $r.Found = $true }
        }
    }
    if (-not $r.Found) {
        $cmd = Get-Command cl -ErrorAction SilentlyContinue
        if ($cmd) { $r.Cl = $cmd.Source; $r.Found = $true }
    }
    $r.Sdk = Test-Path (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include')
    return $r
}

function Find-Iscc {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                     "$env:ProgramFiles\Inno Setup 6\ISCC.exe")) {
        if (Test-Path $p) { return $p }
    }
    return $null
}


# ---- preflight -----------------------------------------------------------------
function Assert-Prerequisites {
    Write-Center 'Preflight - host prerequisites for the Windows CI job'
    $problems = @()

    $git = Find-Git
    if ($git) {
        Write-Ok "git: $git"
    } else {
        $problems += 'git'
    }

    if (Test-Command 'curl.exe') {
        Write-Ok 'curl.exe: found (ships with Windows)'
    } else {
        $problems += 'curl.exe'
    }

    $cmake = Find-CMake
    if ($cmake) {
        Write-Ok "cmake: $((& $cmake --version | Select-Object -First 1)) ($cmake)"
    } else {
        $problems += 'cmake'
    }

    $msvc = Find-Msvc
    if ($msvc.Found) {
        Write-Ok "MSVC cl.exe: $($msvc.Cl)"
        if ($msvc.Sdk) {
            Write-Ok 'Windows SDK include dirs: found'
        } else {
            $problems += 'Windows SDK (needed: SIO_TCP_INFO / _WIN32_WINNT=0x0A00)'
        }
    } else {
        $problems += 'MSVC (Visual Studio C++ tools)'
    }

    if (Find-Iscc) {
        Write-Ok "Inno Setup ISCC.exe: $(Find-Iscc)"
    } else {
        Write-Warn 'Inno Setup not found yet - step 7 installs it with choco like CI (or use -SkipInnoSetupInstall after installing it yourself)'
    }

    if (-not (Test-Admin)) {
        Write-Warn 'shell is NOT elevated - step 10 (smoke test) and choco installs need an elevated shell'
    }
    Write-Host ''

    if ($problems.Count -gt 0) {
        throw @"

Missing required host tools: $($problems -join ', ')

To reproduce the Windows CI job locally you need the same host toolchain the
windows-latest runner image provides. Install everything below from an
ELEVATED PowerShell, then open a NEW PowerShell and rerun this script:

  1. Visual Studio 2022 Build Tools (MSVC v143 x64/x86 + Windows 10/11 SDK):
     winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"

  2. CMake (>= 3.20), same as the runner image PATH:
     winget install -e --id Kitware.CMake

  3. Git for Windows (already installed at C:\Program Files\Git here; the
     script adds it to PATH automatically):
     winget install -e --id Git.Git

  4. Chocolatey + Inno Setup 6 (step 7 runs `choco install innosetup -y
     --no-progress` exactly like CI):
     Set-ExecutionPolicy Bypass -Scope Process -Force
     iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
     choco install innosetup -y --no-progress

Nothing else is downloaded as a shortcut: vcpkg (pinned baseline) and the agent
dependencies (grpc/protobuf/openssl/curl/sqlite3 from agent/vcpkg.json) are
built from source by this script exactly as CI does.
"@
    }
    Write-Ok 'preflight passed'
}


# ---- step functions (names/order mirror ci.yml, job cpp-agent-windows) ----------
# Step 1 - Checkout vcpkg (pinned baseline)
function Step-CheckoutVcpkg {
    $git = Find-Git
    $dotGit = Join-Path $VcpkgDir '.git'
    if (-not (Test-Path $dotGit)) {
        if (Test-Path $VcpkgDir) { Remove-DirIfExists $VcpkgDir }
        Write-Host '   git clone --filter=blob:none https://github.com/microsoft/vcpkg.git (same as CI)'
        & $git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $VcpkgDir
        if ($LASTEXITCODE -ne 0) { throw 'git clone of vcpkg failed' }
    } else {
        Write-Host "   reusing existing vcpkg clone at $VcpkgDir"
    }

    & $git -C $VcpkgDir checkout --quiet $VcpkgPin 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "   pinned baseline $VcpkgPin not in the local clone; fetching it"
        & $git -C $VcpkgDir fetch --depth 1 origin $VcpkgPin
        if ($LASTEXITCODE -ne 0) {
            Write-Host '   fetch failed; re-cloning vcpkg from scratch'
            Remove-DirIfExists $VcpkgDir
            & $git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $VcpkgDir
            if ($LASTEXITCODE -ne 0) { throw 'git clone of vcpkg failed' }
        }
        & $git -C $VcpkgDir checkout --quiet $VcpkgPin
        if ($LASTEXITCODE -ne 0) { throw "vcpkg checkout of pinned baseline $VcpkgPin failed" }
    }
    $head = (& $git -C $VcpkgDir rev-parse HEAD).Trim()
    Write-Ok "vcpkg HEAD is the CI pin: $head"
}

# Step 2 - Fetch vcpkg tool (vcpkg-tool release pinned in the workflow env)
function Step-FetchVcpkgTool {
    $tool = Join-Path $VcpkgDir 'vcpkg.exe'
    Write-Host "   downloading vcpkg-tool $VcpkgToolVersion"
    Invoke-NativeCommand -FilePath 'curl.exe' -Arguments @(
        '-L', '--fail', '--retry', '5', '--retry-delay', '5', '--retry-connrefused',
        '-o', $tool,
        "https://github.com/microsoft/vcpkg-tool/releases/download/$VcpkgToolVersion/vcpkg.exe"
    )
    Invoke-NativeCommand -FilePath $tool -Arguments @('version', '--disable-metrics')
}

# Step 3 - Inspect restored vcpkg cache (CI's diagnostics step; makes cache
# behaviour visible instead of silently rebuilding every run)
function Step-InspectVcpkgCache {
    function Show-CacheDir([string]$Name, [string]$Dir) {
        if (Test-Path $Dir) {
            $files = Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue
            $sizeMB = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
            Write-Host "   $Name restored: $($files.Count) file(s), $sizeMB MB ($Dir)"
        } else {
            Write-Host "   $Name restored: MISSING ($Dir)"
        }
    }
    Show-CacheDir 'vcpkg downloads' (Join-Path $VcpkgDir 'downloads')
    Show-CacheDir 'binary cache' $VcpkgCacheDir
    Show-CacheDir 'installed tree' (Join-Path $BuildDir 'vcpkg_installed')
}

# Step 4 - Configure (MSVC)
function Step-Configure {
    $toolchain = ($VcpkgDir.Replace('\', '/') + '/scripts/buildsystems/vcpkg.cmake')
    Invoke-NativeCommand -FilePath (Find-CMake) -Arguments @(
        '-S', (Join-Path $Root 'agent'),
        '-B', $BuildDir,
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md-release'
    )
}

# Step 5 - Build (bounded parallelism, the -j 2 the CI job uses)
function Step-Build {
    Invoke-NativeCommand -FilePath (Find-CMake) -Arguments @(
        '--build', $BuildDir,
        '--config', 'Release',
        '-j', "$Jobs"
    )
}

# Step 6 - Test (CTest)
function Step-CTest {
    Invoke-NativeCommand -FilePath (Find-CTest) -Arguments @(
        '-C', 'Release', '--output-on-failure'
    ) -WorkingDirectory $BuildDir
}


# Step 7 - Install Inno Setup (same command the CI job runs)
function Step-InstallInnoSetup {
    if (Find-Iscc) {
        Write-Ok "Inno Setup already installed (ISCC.exe: $(Find-Iscc))"
        return
    }
    if ($SkipInnoSetupInstall) {
        throw 'ISCC.exe not found and -SkipInnoSetupInstall was used. Install Inno Setup 6 first (winget install -e --id JRSoftware.InnoSetup, or choco install innosetup -y --no-progress), then rerun.'
    }
    if (-not (Test-Command 'choco')) {
        throw @'
choco is not installed, so Inno Setup cannot be installed the way CI does
(`choco install innosetup -y --no-progress`). From an ELEVATED PowerShell run:

  Set-ExecutionPolicy Bypass -Scope Process -Force
  iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
  choco install innosetup -y --no-progress

then rerun this script (or install Inno Setup another way and use
-SkipInnoSetupInstall).
'@
    }
    if (-not (Test-Admin)) {
        throw 'Installing Inno Setup with choco needs an elevated shell. Reopen an elevated PowerShell and rerun, or install Inno Setup yourself and pass -SkipInnoSetupInstall.'
    }
    Write-Host '   choco install innosetup -y --no-progress (exact CI command)'
    Invoke-NativeCommand -FilePath 'choco' -Arguments @('install', 'innosetup', '-y', '--no-progress')
    if (-not (Find-Iscc)) {
        throw 'choco reported success but ISCC.exe was not found afterwards'
    }
    Write-Ok "Inno Setup installed (ISCC.exe: $(Find-Iscc))"
}

# Step 8 - Stage installer payload
function Step-StagePayload {
    New-Item -ItemType Directory -Force -Path $PayloadDir | Out-Null
    $agentExe = Join-Path $BuildDir 'Release\pudim-agent.exe'
    if (-not (Test-Path $agentExe)) {
        throw "expected agent binary not found: $agentExe (was the Release build produced?)"
    }
    Copy-Item $agentExe $PayloadDir -Force
    Write-Ok "staged $agentExe"

    Write-Host '   downloading VC++ runtime (bundled for hosts without the redistributable)'
    Invoke-NativeCommand -FilePath 'curl.exe' -Arguments @(
        '-L', '--fail', '--retry', '5', '--retry-delay', '5', '--retry-connrefused',
        '-o', (Join-Path $PayloadDir 'vc_redist.x64.exe'),
        'https://aka.ms/vs/17/release/vc_redist.x64.exe'
    )
}

# Step 9 - Build installer (Inno Setup)
function Get-AgentVersion {
    $match = Select-String -Path (Join-Path $Root 'agent\CMakeLists.txt') `
        -Pattern 'project\(pudim-agent VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
    if ($match) { return $match.Matches[0].Groups[1].Value }
    return '0.1.0'
}

function Step-BuildInstaller {
    $iscc = Find-Iscc
    if (-not $iscc) {
        throw 'ISCC.exe not found (step 7 should have installed Inno Setup)'
    }
    $version = Get-AgentVersion
    Write-Host "   compiling installer with MyAppVersion=$version"
    Invoke-NativeCommand -FilePath $iscc -Arguments @("/DMyAppVersion=$version", 'installer\installer-agent.iss') `
        -WorkingDirectory $Root
    $setupExe = Get-ChildItem (Join-Path $DistDir 'PudimNetMon-Agent-Setup-*.exe') -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $setupExe) {
        throw "ISCC succeeded but no installer appeared under $DistDir"
    }
    Write-Ok "installer produced: $($setupExe.FullName)"
}


# Step 10 - Smoke test installer (install -> config model -> upgrade -> uninstall)
# Verbatim local port of the CI workflow step of the same name. It registers
# the real PudimNetMonAgent Windows service, so it requires an elevated shell
# and is only run when -RunSmokeTest is passed.
function Invoke-InstallerSmokeTest {
    if (-not (Test-Admin)) {
        throw 'The installer smoke test registers a Windows service and needs an ELEVATED PowerShell. Reopen an elevated shell and rerun with -RunSmokeTest.'
    }

    # Every setup/uninstaller launch is bounded: a wedged child of the installer
    # (e.g. the bundled VC++ redistributable / msiexec, or a [Run] step) must
    # never stall this step silently.
    function Invoke-SetupProcess {
        param(
            [string]$Phase,
            [string]$FilePath,
            [string[]]$ArgumentList,
            [string]$LogFile,
            [int]$TimeoutSeconds = 900
        )
        Write-Host "[$Phase] starting: $FilePath $($ArgumentList -join ' ')"
        $p = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -PassThru
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            Write-Host "[$Phase] TIMEOUT after ${TimeoutSeconds}s; killing process tree"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 2
            Get-Process -Name 'pudim-agent', 'unins000', 'Setup*', 'Pudim*' -ErrorAction SilentlyContinue |
                ForEach-Object {
                    Write-Host "  killing leftover $($_.ProcessName) pid=$($_.Id)"
                    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
                }
            Write-Host '---- service state ----'
            sc.exe query PudimNetMonAgent 2>&1 | Out-String | Write-Host
            Write-Host '---- agent processes ----'
            Get-Process pudim-agent -ErrorAction SilentlyContinue |
                Select-Object Id, StartTime, Path | Format-Table -AutoSize | Out-String | Write-Host
            if (Test-Path $LogFile) {
                Write-Host "---- $LogFile (tail 100) ----"
                Get-Content $LogFile -Tail 100 | Write-Host
                Write-Host '---- end log ----'
            }
            Write-ServiceDiagnostics -Label "[$Phase] TIMEOUT"
            throw "[$Phase] timed out after ${TimeoutSeconds}s"
        }
        Write-Host "[$Phase] exit code $($p.ExitCode)"
        if ($p.ExitCode -ne 0) {
            if (Test-Path $LogFile) {
                Write-Host "---- $LogFile (tail 100) ----"
                Get-Content $LogFile -Tail 100 | Write-Host
                Write-Host '---- end log ----'
            }
            Write-ServiceDiagnostics -Label "[$Phase] failed"
            throw "[$Phase] failed with exit code $($p.ExitCode)"
        }
    }

    # Dumps everything needed to diagnose why PudimNetMonAgent is not running
    # after an install/upgrade: the exact ImagePath sc stored, service state,
    # live agent processes and relevant event-log entries.
    function Write-ServiceDiagnostics {
        param([string]$Label)
        Write-Host "---- service diagnostics: $Label ----"
        Write-Host '---- sc qc PudimNetMonAgent (ImagePath) ----'
        sc.exe qc PudimNetMonAgent 2>&1 | Out-String | Write-Host
        Write-Host '---- sc query PudimNetMonAgent (state) ----'
        sc.exe query PudimNetMonAgent 2>&1 | Out-String | Write-Host
        Write-Host '---- Get-Service ----'
        Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue |
            Select-Object Status, Name, DisplayName, StartType |
            Format-Table -AutoSize | Out-String | Write-Host
        Write-Host '---- pudim-agent processes ----'
        Get-Process pudim-agent -ErrorAction SilentlyContinue |
            Select-Object Id, StartTime, Path |
            Format-Table -AutoSize | Out-String | Write-Host
        Write-Host '---- recent events mentioning the agent (System + Application) ----'
        try {
            Get-WinEvent -FilterHashtable @{
                LogName = 'System', 'Application'
                StartTime = (Get-Date).AddMinutes(-15)
            } -MaxEvents 500 -ErrorAction Stop |
                Where-Object { $_.Message -match 'PudimNetMonAgent|pudim-agent|PudimNetMon' } |
                Sort-Object TimeCreated -Descending |
                Select-Object -First 20 TimeCreated, LogName, ProviderName, Id, LevelDisplayName, Message |
                Format-List | Out-String -Width 300 | Write-Host
        } catch {
            Write-Host "  (event log unavailable: $($_.Exception.Message))"
        }
        Write-Host '---- end service diagnostics ----'
    }

    # Waits (with retries) for the service to reach Running. The installer
    # defers the initial start to a detached helper that fires after Setup
    # exits, so right after install the service may legitimately be Stopped.
    function Wait-AgentServiceRunning {
        param([string]$Label)
        for ($i = 1; $i -le 8; $i++) {
            $svc = Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue
            if (-not $svc) { return $false }
            if ($svc.Status -in 'Running', 'StartPending') { return $true }
            Write-Host "[$Label] service not running yet (attempt $i/8); starting it"
            sc.exe start PudimNetMonAgent 2>&1 | Out-String | Write-Host
            Start-Sleep -Seconds 3
        }
        return $false
    }


    Push-Location $Root
    try {
        $setup = Get-ChildItem (Join-Path $DistDir 'PudimNetMon-Agent-Setup-*.exe') |
            Select-Object -First 1
        if (-not $setup) { throw 'Installer was not produced (step 9)' }
        $installLog = Join-Path $env:TEMP 'pudim-setup-install.log'
        $upgradeLog = Join-Path $env:TEMP 'pudim-setup-upgrade.log'
        $uninstallLog = Join-Path $env:TEMP 'pudim-setup-uninstall.log'

        # ---- 0. Agent startup probe (diagnostics only): launch the freshly
        # built binary in the console for a few seconds and capture stdout.
        # Purely diagnostic: a probe failure never fails this step.
        try {
            $payloadAgent = Join-Path $PayloadDir 'pudim-agent.exe'
            if (Test-Path $payloadAgent) {
                $probeOut = Join-Path $env:TEMP 'pudim-agent-probe.out'
                $probeErr = Join-Path $env:TEMP 'pudim-agent-probe.err'
                Write-Host '[probe] launching pudim-agent in console (15s bound)'
                $probe = Start-Process -FilePath $payloadAgent `
                    -ArgumentList '--node-id=ci-console-probe' `
                    -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr `
                    -PassThru -WindowStyle Hidden
                if (-not $probe.WaitForExit(15000)) {
                    Write-Host '[probe] still running after 15s (startup OK); stopping it'
                    Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
                    Start-Sleep -Milliseconds 500
                } else {
                    Write-Host "[probe] exited on its own, code $($probe.ExitCode)"
                }
                Write-Host '---- probe stdout (agent startup log) ----'
                Get-Content $probeOut -ErrorAction SilentlyContinue |
                    Select-Object -First 80 | Write-Host
                Write-Host '---- probe stderr ----'
                Get-Content $probeErr -ErrorAction SilentlyContinue |
                    Select-Object -First 40 | Write-Host
            } else {
                Write-Host "[probe] payload agent not found at $payloadAgent; skipping"
            }
        } catch {
            Write-Host "[probe] diagnostics failed (non-fatal): $($_.Exception.Message)"
        } finally {
            Get-Process pudim-agent -ErrorAction SilentlyContinue |
                ForEach-Object {
                    Write-Host "  killing leftover probe pudim-agent pid=$($_.Id)"
                    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
                }
        }

        # ---- 1. Silent install -----------------------------------------------
        Invoke-SetupProcess -Phase 'install' -FilePath $setup.FullName `
            -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$installLog" `
            -LogFile $installLog

        Start-Sleep -Seconds 3
        $svc = Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue
        if (-not $svc) {
            Write-ServiceDiagnostics -Label 'install: service missing'
            throw 'PudimNetMonAgent service not registered'
        }
        if (-not (Wait-AgentServiceRunning -Label 'install')) {
            Write-ServiceDiagnostics -Label 'install: service not running'
            if (Test-Path $installLog) {
                Write-Host "---- $installLog (tail 100) ----"
                Get-Content $installLog -Tail 100 | Write-Host
                Write-Host '---- end log ----'
            }
            throw "Service not running after install (status: $($svc.Status))"
        }
        $svc = Get-Service PudimNetMonAgent
        Write-Host "[install] OK: service running ($($svc.Status))"


        # ---- 2. Config model: only node-id is baked into the ImagePath; every
        # mutable setting lives in agent.conf (single source of truth). -------
        # Read the service ImagePath through WMI/CIM - `sc.exe qc` output is
        # localized (e.g. pt-BR prints a translated label), so grepping for
        # "BINARY_PATH_NAME" breaks on non-English Windows.
        $svcCfg = Get-CimInstance Win32_Service -Filter "Name='PudimNetMonAgent'"
        if (-not $svcCfg -or [string]::IsNullOrWhiteSpace($svcCfg.PathName)) {
            Write-ServiceDiagnostics -Label 'config model: service config unreadable'
            throw 'Could not read the PudimNetMonAgent service PathName'
        }
        $binPath = 'BINARY_PATH_NAME: ' + $svcCfg.PathName
        Write-Host $binPath
        if ($binPath -notmatch 'pudim-agent\.exe" --node-id=') {
            throw "Expected --node-id on the service command line, got: $binPath"
        }
        if ($binPath -match '--interval|--collector') {
            throw "Mutable settings must not be baked into the ImagePath: $binPath"
        }

        $confPath = Join-Path $env:ProgramData 'PudimNetMon\agent.conf'
        if (-not (Test-Path $confPath)) { throw "agent.conf not written: $confPath" }
        $conf = Get-Content $confPath -Raw
        Write-Host "agent.conf:`n$conf"
        if ($conf -notmatch 'interval=5000') {
            throw 'agent.conf missing the silent-install default interval=5000'
        }
        if ($conf -match 'node-id=') {
            throw 'agent.conf must not contain node-id (it lives on the service command line)'
        }

        # ---- 3. Upgrade path: re-run the installer over the existing install.
        # Exercises the service-exists -> ChangeServiceConfig branch and rewrites
        # agent.conf before the service is restarted. -------------------------
        Invoke-SetupProcess -Phase 'upgrade' -FilePath $setup.FullName `
            -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$upgradeLog" `
            -LogFile $upgradeLog

        Start-Sleep -Seconds 3
        $svc = Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue
        if (-not $svc) {
            Write-ServiceDiagnostics -Label 'upgrade: service missing'
            throw 'PudimNetMonAgent service missing after upgrade'
        }
        if (-not (Wait-AgentServiceRunning -Label 'upgrade')) {
            Write-ServiceDiagnostics -Label 'upgrade: service not running'
            if (Test-Path $upgradeLog) {
                Write-Host "---- $upgradeLog (tail 100) ----"
                Get-Content $upgradeLog -Tail 100 | Write-Host
                Write-Host '---- end log ----'
            }
            throw "Service not running after upgrade (status: $($svc.Status))"
        }
        $svc = Get-Service PudimNetMonAgent
        Write-Host '[upgrade] OK: service running after reinstall'

        # ---- 4. agent.conf is authoritative: change the interval, restart, and
        # confirm the agent still comes up (no stale ImagePath flag overrides
        # the file). ----------------------------------------------------------
        (Get-Content $confPath) -replace 'interval=\d+', 'interval=4321' |
            Set-Content $confPath
        Restart-Service PudimNetMonAgent
        Start-Sleep -Seconds 5
        $svc = Get-Service PudimNetMonAgent
        if ($svc.Status -ne 'Running') {
            Write-ServiceDiagnostics -Label 'agent.conf restart'
            throw "Service not running after agent.conf change + restart (status: $($svc.Status))"
        }
        Write-Host 'agent.conf change OK: service restarted and running'

        # ---- 5. Uninstaller --------------------------------------------------
        # Inno registers DisplayName as AppVerName ("PudimNetMon Agent 0.1.0"),
        # not the bare AppName - match by prefix.
        $entry = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' |
            Where-Object { $_.DisplayName -like 'PudimNetMon Agent*' } |
            Select-Object -First 1
        if (-not $entry) { throw 'Uninstaller not registered in Add/Remove Programs' }
        if ($entry.UninstallString -match '^"([^"]+)"') {
            $uninstExe = $Matches[1]
        } else {
            $uninstExe = $entry.UninstallString
        }

        Invoke-SetupProcess -Phase 'uninstall' -FilePath $uninstExe `
            -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$uninstallLog" `
            -LogFile $uninstallLog

        Start-Sleep -Seconds 3
        if (Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue) {
            Write-ServiceDiagnostics -Label 'uninstall: service still registered'
            throw 'Service still registered after uninstall'
        }
        Write-Host '[uninstall] OK: service removed'
        Write-Host 'Smoke test PASSED'
    } finally {
        Pop-Location
    }
}


# ---- reset of leftover agent state --------------------------------------------
# Removes any leftover state a previous installer/smoke-test run left on this
# machine so each run starts from a CI-fresh state. The smoke test's uninstall
# keeps %ProgramData%\PudimNetMon by design; that stale agent.conf is then read
# by agent-config-tests on the next run (no --config-file => default path),
# which breaks its "defaults" assertions. Requires an elevated shell.
function Reset-LocalAgentState {
    if (-not (Test-Admin)) {
        throw 'Resetting leftover agent state needs an ELEVATED PowerShell. Reopen an elevated shell and rerun (this keeps CTest on a CI-fresh machine).'
    }
    $removed = @()

    # Stop stray agent console processes first so installed files are unlocked.
    Get-Process pudim-agent -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  stopping stray pudim-agent process pid=$($_.Id)"
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }

    # Service (if a previous run aborted before the uninstall phase).
    if (Get-Service PudimNetMonAgent -ErrorAction SilentlyContinue) {
        Write-Host '  removing PudimNetMonAgent service'
        sc.exe stop PudimNetMonAgent 2>&1 | Out-String | Write-Host
        Start-Sleep -Seconds 2
        sc.exe delete PudimNetMonAgent 2>&1 | Out-String | Write-Host
        Start-Sleep -Seconds 1
        $removed += 'service'
    }

    # Add/Remove Programs entries (native and WOW64 registry views).
    foreach ($view in @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )) {
        Get-ChildItem $view -ErrorAction SilentlyContinue | ForEach-Object {
            if ((Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DisplayName -like 'PudimNetMon Agent*') {
                Write-Host "  removing uninstall key $($_.PSPath)"
                Remove-Item $_.PSPath -Recurse -Force -ErrorAction SilentlyContinue
                $removed += 'uninstall key'
            }
        }
    }

    # Program Files payload.
    $pfPath = 'C:\Program Files\PudimNetMon Agent'
    if (Test-Path $pfPath) {
        Write-Host "  removing $pfPath"
        Remove-Item $pfPath -Recurse -Force -ErrorAction SilentlyContinue
        $removed += 'Program Files'
    }

    # ProgramData state (agent.conf, pending.db) - left behind by the uninstaller.
    $pdPath = Join-Path $env:ProgramData 'PudimNetMon'
    if (Test-Path $pdPath) {
        Write-Host "  removing $pdPath"
        Remove-Item $pdPath -Recurse -Force -ErrorAction SilentlyContinue
        $removed += 'ProgramData'
    }

    Start-Sleep -Milliseconds 500
    # Files can be transiently locked (e.g. by the deferred service-start helper
    # or antivirus); retry the deletions once before reporting.
    foreach ($p in @($pfPath, $pdPath)) {
        if (Test-Path $p) {
            Write-Warn "  $p still present after first attempt; retrying"
            Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    if ($removed.Count -eq 0) {
        Write-Host '  nothing to reset - machine already CI-fresh'
    } else {
        Write-Host ('  reset leftover agent state: ' + (($removed | Select-Object -Unique) -join ', '))
    }
}

# ---- main ---------------------------------------------------------------------
function Main {
    Write-Center 'PudimNetMon - Windows CI mirror (local)'
    Write-Host "   repo:     $Root"
    Write-Host "   workflow: .github/workflows/ci.yml (job: cpp-agent-windows)"
    Write-Host "   host:     $env:COMPUTERNAME, PowerShell $($PSVersionTable.PSVersion)"
    Write-Host "   admin:    $(Test-Admin)"
    Write-Host "   triplet:  $Triplet (static vcpkg libs so pudim-agent.exe is self-contained)"
    Write-Host "   jobs:     $Jobs (CI uses the same bounded -j 2)"

    if ($Clean) {
        Write-Center 'Clean CI build dirs'
        Remove-DirIfExists $BuildDir
        Remove-DirIfExists $DistDir
        Remove-DirIfExists $PayloadDir
        Write-Ok 'removed build/agent-win, dist/installer, installer/payload'
    }

    Assert-Prerequisites
    Set-CiEnvironment

    # Every run starts from a CI-fresh machine when the installer/smoke test is
    # involved (or when explicitly requested): leftover service/state from a
    # previous smoke-test run would otherwise break agent-config-tests (stale
    # agent.conf at the default path) and the fresh-install phase.
    if ($RunSmokeTest -or $ResetServiceState) {
        Invoke-Step '0. Reset leftover agent state (CI-fresh machine)' { Reset-LocalAgentState }
    }

    Invoke-Step '1. Checkout vcpkg (pinned baseline)'        { Step-CheckoutVcpkg }
    Invoke-Step '2. Fetch vcpkg tool'                        { Step-FetchVcpkgTool }
    Invoke-Step '3. Inspect restored vcpkg cache'            { Step-InspectVcpkgCache }
    Invoke-Step '4. Configure (MSVC)'                        { Step-Configure }
    Invoke-Step '5. Build'                                   { Step-Build }
    Invoke-Step '6. Test (CTest)'                            { Step-CTest }

    if ($SkipInstaller) {
        Write-Warn '-SkipInstaller: steps 7-10 skipped. CI always runs them; drop the flag for full parity.'
    } else {
        Invoke-Step '7. Install Inno Setup'                  { Step-InstallInnoSetup }
        Invoke-Step '8. Stage installer payload'             { Step-StagePayload }
        Invoke-Step '9. Build installer (Inno Setup)'        { Step-BuildInstaller }
        if ($RunSmokeTest) {
            Invoke-Step '10. Smoke test installer (install -> config model -> upgrade -> uninstall)' {
                Invoke-InstallerSmokeTest
            }
        } else {
            Write-Warn 'step 10 (Smoke test installer) skipped: it registers the PudimNetMonAgent Windows service and needs an ELEVATED shell. Rerun from an admin PowerShell with -RunSmokeTest to execute it the same way CI does.'
        }
    }

    Show-Summary
    if ($script:Failed) { exit 1 }
    exit 0
}

try {
    if ($MyInvocation.InvocationName -ne '.') { Main }
} catch {
    Write-Fail $_.Exception.Message
    Show-Summary
    exit 1
}
