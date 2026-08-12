<#
.SYNOPSIS
  Installs (or uninstalls) the PudimNetMon agent as a Windows service.

.DESCRIPTION
  Requires an elevated PowerShell session. Locates pudim-agent.exe (next to
  this script in ..\build-agent-windows\Release or ..\build-agent-windows\, or
  passed via -BinaryPath) and registers it as the "PudimNetMonAgent"
  auto-start service using the binary's built-in --install-service support.

.PARAMETER BinaryPath
  Full path to pudim-agent.exe. Optional; auto-detected by default.

.PARAMETER ServiceArgs
  Extra agent CLI flags baked into the service command line, e.g.
  -ServiceArgs "--node-id=win-01", "--interval=10000",
               "--collector-endpoints=collector.lan:50051"

.PARAMETER Uninstall
  Removes the service instead of installing it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install-agent-windows.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\install-agent-windows.ps1 -Uninstall
#>
param(
    [string]$BinaryPath = "",
    [string[]]$ServiceArgs = @("--node-id=agent-windows", "--interval=10000"),
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

# Elevation check.
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "This script must be run from an elevated PowerShell session (Run as Administrator)."
    exit 1
}

# Locate the agent binary.
if (-not $BinaryPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\build-agent-windows\Release\pudim-agent.exe"),
        (Join-Path $PSScriptRoot "..\build-agent-windows\pudim-agent.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $BinaryPath = (Resolve-Path $c).Path; break }
    }
}
if (-not $BinaryPath -or -not (Test-Path $BinaryPath)) {
    Write-Error "Could not find pudim-agent.exe. Build it first (see docs/windows.md) or pass -BinaryPath."
    exit 1
}
$BinaryPath = (Resolve-Path $BinaryPath).Path

if ($Uninstall) {
    Write-Host "Uninstalling PudimNetMonAgent service..."
    & $BinaryPath --uninstall-service
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Uninstall failed (exit $LASTEXITCODE). The service may not exist."
        exit $LASTEXITCODE
    }
    Write-Host "Service removed."
    exit 0
}

Write-Host "Installing PudimNetMonAgent service from $BinaryPath"
& $BinaryPath --install-service @ServiceArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "Install failed (exit $LASTEXITCODE)."
    exit $LASTEXITCODE
}

Start-Service -Name "PudimNetMonAgent" -ErrorAction Stop
Write-Host "Service started. Manage it with: Get-Service PudimNetMonAgent / sc query PudimNetMonAgent"
