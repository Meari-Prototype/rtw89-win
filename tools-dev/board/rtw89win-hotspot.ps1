# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Windows' mobile hotspot through NetworkOperatorTetheringManager, on the wired uplink. Copy to any directory on the
# board and run as administrator over ssh:
#   rtw89win-hotspot.ps1 -Action state                   TetheringCapability, operational state, clients, the access
#                                                        point configuration (SSID, band, authentication; never the
#                                                        passphrase), bands/authentications supported
#   rtw89win-hotspot.ps1 -Action configure -Ssid <name> -Band TwoPointFourGigahertz|FiveGigahertz|Auto
#                                                        ConfigureAccessPointAsync with that SSID and band; the
#                                                        passphrase Windows generated is kept as it is (changes state)
#   rtw89win-hotspot.ps1 -Action start | stop            StartTetheringAsync / StopTetheringAsync (changes state)
#   rtw89win-hotspot.ps1 -Action passphrase -Here        prints the passphrase to stdout only (never in the task log)
# The WinRT calls run in the logged-on user's desktop session (a temporary scheduled task, interactive logon, highest
# run level, deleted afterwards, as rtw89win-bt.ps1 does) under Windows PowerShell 5.1, which has the WinRT
# projection; the task writes rtw89win-hotspot.log next to this script, without the passphrase. -Here runs in this
# session instead. The uplink is the profile of the wired adapter with Internet access (never the Wi-Fi station).
# After a device restart or a SER with the hotspot on, Windows does not start it again by itself (observed):
# -Action start.
param(
    [Parameter(Mandatory)][ValidateSet('state', 'configure', 'start', 'stop', 'passphrase')][string]$Action,
    [string]$Ssid,
    [ValidateSet('Auto', 'TwoPointFourGigahertz', 'FiveGigahertz')][string]$Band = 'TwoPointFourGigahertz',
    [int]$TimeoutSec = 60,
    [switch]$Here,
    [switch]$Inner)
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'rtw89win-hotspot.log'
if ($Action -eq 'configure' -and -not $Ssid) { throw '-Action configure needs -Ssid' }
if ($Action -eq 'passphrase' -and -not $Here) { throw '-Action passphrase only with -Here (stdout, no log file)' }

function Invoke-Action {
    $null = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $ext = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 }
    $asOp = $ext | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
    $asAct = $ext | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
    function AwaitOp($op, [type]$t) {
        $task = $asOp.MakeGenericMethod($t).Invoke($null, @($op))
        if (-not $task.Wait($TimeoutSec * 1000)) { throw "no result within $TimeoutSec s" }
        $task.Result
    }
    function AwaitAct($act) {
        $task = $asAct.Invoke($null, @($act))
        if (-not $task.Wait($TimeoutSec * 1000)) { throw "no result within $TimeoutSec s" }
    }

    $ni = [Windows.Networking.Connectivity.NetworkInformation]
    $p = $ni::GetConnectionProfiles() | Where-Object {
        $_.NetworkAdapter -and $_.NetworkAdapter.IanaInterfaceType -eq 6 -and
        $_.GetNetworkConnectivityLevel() -eq [Windows.Networking.Connectivity.NetworkConnectivityLevel]::InternetAccess
    } | Select-Object -First 1
    if ($null -eq $p) { throw 'no wired connection profile with Internet access (the uplink)' }
    $tm = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]
    "uplink: wired adapter {$($p.NetworkAdapter.NetworkAdapterId)}"
    "tethering capability: " + $tm::GetTetheringCapabilityFromConnectionProfile($p)
    $m = $tm::CreateFromConnectionProfile($p)
    $c = $m.GetCurrentAccessPointConfiguration()
    switch ($Action) {
        'passphrase' { $c.Passphrase; return }
        'configure' {
            $n = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration]::new()
            $n.Ssid = $Ssid
            $n.Passphrase = $c.Passphrase           # Windows' own, unchanged
            $n.Band = [Windows.Networking.NetworkOperators.TetheringWiFiBand]::$Band
            try { $n.AuthenticationKind = [Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind]::Wpa2 } catch { }
            AwaitAct ($m.ConfigureAccessPointAsync($n))
            "configured: SSID $Ssid, band $Band"
            $c = $m.GetCurrentAccessPointConfiguration()
        }
        'start' {
            $r = AwaitOp ($m.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
            "start: $($r.Status) $($r.AdditionalErrorMessage)"
        }
        'stop' {
            $r = AwaitOp ($m.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
            "stop: $($r.Status) $($r.AdditionalErrorMessage)"
        }
    }
    "operational state: $($m.TetheringOperationalState), clients $($m.ClientCount)/$($m.MaxClientCount)"
    $auth = try { $c.AuthenticationKind } catch { 'n/a' }
    "configuration: SSID '$($c.Ssid)', band $($c.Band), authentication $auth, passphrase length $($c.Passphrase.Length)"
    foreach ($b in 'TwoPointFourGigahertz', 'FiveGigahertz', 'SixGigahertz') {
        try { "band ${b} supported: " + $c.IsBandSupported([Windows.Networking.NetworkOperators.TetheringWiFiBand]::$b) } catch { "band ${b}: $($_.Exception.Message)" }
    }
    foreach ($k in 'Wpa2', 'Wpa3TransitionMode', 'Wpa3') {
        try { "authentication ${k} supported: " + $c.IsAuthenticationKindSupported([Windows.Networking.NetworkOperators.TetheringWiFiAuthenticationKind]::$k) } catch { "authentication ${k}: $($_.Exception.Message)" }
    }
}

if ($Inner) {
    "started as $([Environment]::UserName) in session $((Get-Process -Id $PID).SessionId), $(Get-Date -Format 'HH:mm:ss.fff')" |
        Out-File $log -Encoding utf8
    try { Invoke-Action 2>&1 | Out-File $log -Append -Encoding utf8 }
    catch { "error: $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8 }
    "done $(Get-Date -Format 'HH:mm:ss.fff')" | Out-File $log -Append -Encoding utf8
    return
}

if ($Here) {
    if ($PSVersionTable.PSEdition -ne 'Desktop') {
        $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Here -Action $Action -Band $Band -TimeoutSec $TimeoutSec"
        if ($Ssid) { $a += " -Ssid `"$Ssid`"" }
        $psi = [Diagnostics.ProcessStartInfo]::new('powershell.exe', $a)
        $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true
        $pr = [Diagnostics.Process]::Start($psi); $pr.StandardOutput.ReadToEnd(); $pr.WaitForExit()
        return
    }
    Invoke-Action
    return
}

$user = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $user) { throw 'nobody is logged on at the desktop' }
$name = 'rtw89win-hotspot-once'
$argList = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Inner -Action $Action -Band $Band -TimeoutSec $TimeoutSec"
if ($Ssid) { $argList += " -Ssid `"$Ssid`"" }
Remove-Item $log -ErrorAction SilentlyContinue
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argList
$pri = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName $name -Action $act -Principal $pri -Force | Out-Null
try {
    "task registered for $user ($Action); starting $(Get-Date -Format 'HH:mm:ss.fff')"
    Start-ScheduledTask -TaskName $name
    $end = (Get-Date).AddSeconds($TimeoutSec + 30)
    do {
        Start-Sleep -Milliseconds 500
        $st = (Get-ScheduledTask -TaskName $name).State
    } while ($st -eq 'Running' -and (Get-Date) -lt $end)
    if ($st -eq 'Running') { "still running: stopping it"; Stop-ScheduledTask -TaskName $name }
    "task state: $st, last result: " + (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
    if (Test-Path $log) { Get-Content $log } else { 'no log written' }
} finally {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    "task deleted: " + (-not (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue))
}
