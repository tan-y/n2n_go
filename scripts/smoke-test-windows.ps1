<#
.SYNOPSIS
  Two-node smoke test for the Windows n2n edge.

.DESCRIPTION
  Starts the edge against a supernode, waits for the wintun adapter and its
  IPv4 address, optionally pings a peer, then stops the process and verifies
  that the adapter is gone.

  Must be run from an elevated PowerShell window.

.EXAMPLE
  .\scripts\smoke-test-windows.ps1 -Supernode 1.2.3.4:7777 -Community demo `
    -Address 10.10.10.20/24 -PeerAddress 10.10.10.21
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Supernode,
    [Parameter(Mandatory = $true)][string]$Community,
    [string]$Key = "",
    [string]$Address = "10.10.10.20/24",
    [string]$PeerAddress = "",
    [string]$Device = "n2n0",
    [string]$ExePath = "",
    [int]$ReadyTimeoutSec = 30
)

$ErrorActionPreference = "Stop"
$exitCode = 1

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    # $PSScriptRoot is empty inside param default values when started via
    # "powershell -File", so resolve the default here instead.
    $ExePath = Join-Path $PSScriptRoot "..\bin\n2n-windows-amd64.exe"
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "run this script from an elevated PowerShell window"
    }
}

Assert-Administrator

$ExePath = (Resolve-Path $ExePath).Path
$exeDir = Split-Path -Parent $ExePath
if (-not (Test-Path (Join-Path $exeDir "wintun.dll"))) {
    throw "wintun.dll not found next to $ExePath (run scripts/build-windows.sh)"
}

$ipPart = ($Address -split "/")[0]
if ((Get-NetIPAddress -IPAddress $ipPart -ErrorAction SilentlyContinue)) {
    throw "IPv4 address $ipPart is already configured on this host"
}
if ((Get-NetAdapter -Name $Device -ErrorAction SilentlyContinue)) {
    throw "adapter $Device already exists; remove it or pass -Device another_name"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stdout = Join-Path $env:TEMP "n2n-smoke-$stamp.out.log"
$stderr = Join-Path $env:TEMP "n2n-smoke-$stamp.err.log"

$arguments = @(
    "edge",
    "-c", $Community,
    "-l", $Supernode,
    "-a", $Address,
    "-d", $Device
)
if ($Key -ne "") {
    $arguments += @("-k", $Key)
}

Write-Host "==> starting $ExePath $($arguments -join ' ')"
$proc = Start-Process -FilePath $ExePath -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden

try {
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
    $adapter = $null
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) {
            $tail = ""
            if (Test-Path $stderr) { $tail = (Get-Content $stderr -Tail 20) -join "`n" }
            throw "n2n exited early (code $($proc.ExitCode)):`n$tail"
        }
        $adapter = Get-NetAdapter -Name $Device -ErrorAction SilentlyContinue
        if ($adapter -and $adapter.Status -eq "Up") {
            break
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $adapter -or $adapter.Status -ne "Up") {
        throw "adapter $Device did not come up within $ReadyTimeoutSec seconds"
    }
    Write-Host "==> adapter $Device is Up"

    $configured = Get-NetIPAddress -InterfaceAlias $Device -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $ipPart }
    if (-not $configured) {
        throw "IPv4 address $ipPart is not configured on $Device"
    }
    Write-Host "==> IPv4 $Address configured"

    if ($PeerAddress -ne "") {
        if (-not (Test-Connection -ComputerName $PeerAddress -Count 3 -Quiet)) {
            throw "ping $PeerAddress failed through the overlay"
        }
        Write-Host "==> ping $PeerAddress succeeded"
    }

    Write-Host "PASS"
    $exitCode = 0
}
finally {
    if (-not $proc.HasExited) {
        # PowerShell cannot deliver Ctrl+C; force stop exercises the same
        # adapter cleanup path but skips the graceful exit code check.
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit(10000) | Out-Null
    }
    Start-Sleep -Seconds 1
    if (Get-NetAdapter -Name $Device -ErrorAction SilentlyContinue) {
        Write-Warning "adapter $Device is still present after the process stopped"
        $exitCode = 1
    } else {
        Write-Host "==> adapter $Device removed"
    }
    if ($proc.HasExited) {
        Write-Host "==> process exit code: $($proc.ExitCode)"
    }
    Write-Host "==> stdout: $stdout"
    Write-Host "==> stderr: $stderr"
}

exit $exitCode
