# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# set this driver's own Parameters values (name=value,...), restart the device, wait for the new start's outcome
# -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([string]$Set = '',
      [string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$d = $Diag
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters'
foreach ($kv in ($Set -split ',' | Where-Object { $_ })) {
  $n, $v = $kv -split '='
  Set-ItemProperty $p -Name $n -Value ([int]$v) -Type DWord
}
$id = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*').InstanceId
$sw = [Diagnostics.Stopwatch]::StartNew()
pnputil /restart-device "$id" | Select-Object -Last 1
do {
  Start-Sleep -Milliseconds 500
  $l = pwsh -NoProfile -File $d log 2>$null
  $txt = $l -join "`n"
  $done = ($txt -match 'adapter started|the start fails|bring-up failed|mode: diagnostic')
} until ($done -or $sw.ElapsedMilliseconds -gt 60000)
"after $($sw.ElapsedMilliseconds) ms:"
$l | Where-Object { $_ -match 'loaded|mode:|guard|adapter started|bring-up|probe failed' }
"--- saved (previous session end)"
pwsh -NoProfile -File $d log -Saved | Where-Object { $_ -match 'guard|D0 exit|released|loaded|mode:' } | Select-Object -Last 6
Get-ItemProperty $p | Select-Object ArmRunning, BootAttempts | Format-List | Out-String
