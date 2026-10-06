# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Bluetooth test helper. Runs one action of RtwBt.cs (next to this script) in the logged-on
# user's desktop session: WinRT discovery and pairing are refused from the ssh session (AccessDenied), but work
# there. A temporary scheduled task (interactive logon of that user, highest run level) starts this script again
# with -Inner under Windows PowerShell 5.1, which compiles RtwBt.cs once with the in-box csc (Add-Type cannot
# reference .winmd files), runs the action and writes rtw89win-bt.log next to this script; this side waits, prints
# the log and deletes the task in any case. Copy RtwBt.cs and this script to the same directory (any directory).
# The discover action prints the addresses of nearby devices: mask them before you share the output.
#   rtw89win-bt.ps1 -Action le       [-Seconds 20]     LE advertisements: how many devices / adverts in that time
#   rtw89win-bt.ps1 -Action discover [-Seconds 30]     classic inquiry: the devices found (paired ones too)
#   rtw89win-bt.ps1 -Action pair     -Address <addr>   pair by address, accepting the numeric comparison here; confirm
#                                                      on the other device (-PairTimeoutSec, default 60)
#   rtw89win-bt.ps1 -Action sdp      -Address <addr>   RFCOMM services by a fresh (uncached) SDP query (pages the device)
#   rtw89win-bt.ps1 -Action unpair   -Address <addr>   remove that pairing (changes state)
#   rtw89win-bt.ps1 -Action radios   [-State On|Off]   the Windows radio list; -State sets the Bluetooth radio
#   -Here: run the action in this session instead (to see the AccessDenied, or from a desktop prompt)
param(
    [Parameter(Mandatory)][ValidateSet('le', 'discover', 'pair', 'unpair', 'sdp', 'radios')][string]$Action,
    [int]$Seconds = 0,
    [string]$Address,
    [ValidateSet('On', 'Off')][string]$State,
    [int]$PairTimeoutSec = 60,
    [switch]$Here,
    [switch]$Inner)
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'rtw89win-bt.log'
if ($Seconds -le 0) { $Seconds = if ($Action -eq 'discover') { 30 } else { 20 } }
if ($Action -in 'pair', 'unpair', 'sdp' -and -not $Address) { throw "-Action $Action needs -Address" }

function Invoke-Action {
    $rt = [System.Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
    $cs = Join-Path $PSScriptRoot 'RtwBt.cs'
    $dll = Join-Path $PSScriptRoot 'RtwBt.dll'
    if (-not (Test-Path $dll) -or (Get-Item $dll).LastWriteTime -lt (Get-Item $cs).LastWriteTime) {
        $md = "$env:windir\System32\WinMetadata"
        $refs = "$md\Windows.Devices.winmd", "$md\Windows.Foundation.winmd", "${rt}System.Runtime.dll",
            "${rt}System.Runtime.InteropServices.WindowsRuntime.dll"
        $o = & "${rt}csc.exe" /nologo /target:library "/out:$dll" ($refs | ForEach-Object { "/reference:$_" }) $cs 2>&1
        if ($LASTEXITCODE) { throw "csc failed ($LASTEXITCODE): $o" }
    }
    Add-Type -Path $dll
    switch ($Action) {
        'le' { [RtwBt]::LeCount($Seconds) }
        'discover' { [RtwBt]::Discover($Seconds) }
        'pair' { [RtwBt]::Pair([RtwBt]::ParseAddr($Address), $PairTimeoutSec) }
        'unpair' { [RtwBt]::Unpair([RtwBt]::ParseAddr($Address)) }
        'sdp' { [RtwBt]::Sdp([RtwBt]::ParseAddr($Address)) }
        'radios' { [RtwBt]::Radios($(if ($State) { $State } else { $null })) }
    }
}

if ($Inner) {
    "started as $([Environment]::UserName) in session $((Get-Process -Id $PID).SessionId), $(Get-Date -Format 'HH:mm:ss')" |
        Out-File $log -Encoding utf8
    try { Invoke-Action 2>&1 | Out-File $log -Append -Encoding utf8 }
    catch { "error: $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8 }
    "done $(Get-Date -Format 'HH:mm:ss')" | Out-File $log -Append -Encoding utf8
    return
}

if ($Here) {
    if ($PSVersionTable.PSEdition -ne 'Desktop') { throw '-Here needs Windows PowerShell 5.1 (powershell.exe)' }
    Invoke-Action
    return
}

$user = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $user) { throw 'nobody is logged on at the desktop' }
$name = 'rtw89win-bt-once'
$argList = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Inner -Action $Action " +
    "-Seconds $Seconds -PairTimeoutSec $PairTimeoutSec"
if ($Address) { $argList += " -Address $Address" }
if ($State) { $argList += " -State $State" }
$timeout = $Seconds + $PairTimeoutSec + 90
Remove-Item $log -ErrorAction SilentlyContinue
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argList
$pri = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName $name -Action $act -Principal $pri -Force | Out-Null
try {
    "task registered for $user ($Action); starting"
    Start-ScheduledTask -TaskName $name
    $end = (Get-Date).AddSeconds($timeout)
    do {
        Start-Sleep -Milliseconds 500
        $st = (Get-ScheduledTask -TaskName $name).State
    } while ($st -eq 'Running' -and (Get-Date) -lt $end)
    if ($st -eq 'Running') { "still running after $timeout s: stopping it"; Stop-ScheduledTask -TaskName $name }
    "task state: $st, last result: " + (Get-ScheduledTaskInfo -TaskName $name).LastTaskResult
    if (Test-Path $log) { Get-Content $log } else { 'no log written' }
} finally {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    "task deleted: " + (-not (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue))
}
