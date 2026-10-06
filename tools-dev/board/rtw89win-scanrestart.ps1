# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Restart the device (rtwdiag arm), wait for the WiFiCx adapter, then restart the device as soon as a scan is
# running; show the saved log.
# -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$diag = $Diag
pwsh -NoProfile -File $diag arm | Select-Object -First 1
$id = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*').InstanceId
$sw = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 50
  $log = (pwsh -NoProfile -File $diag log) -join "`n"
  # a scan is running: its "scan started" line comes after the last "scan done"/"scan aborted"
  $running = $log.LastIndexOf('mac: scan started') -gt [Math]::Max($log.LastIndexOf('mac: scan done'), $log.LastIndexOf('mac: scan aborted'))
} until (($running -and $log -match 'WiFiCx: adapter started') -or $sw.ElapsedMilliseconds -gt 120000)
$last = ($log -split "`n" | Select-Object -Last 3) -join ' / '
"after {0} ms the log ends with: {1}" -f $sw.ElapsedMilliseconds, $last
pnputil /restart-device $id
$sw.Restart()
do { Start-Sleep -Milliseconds 300; $d = Get-PnpDevice -InstanceId $id } until ($d.Status -eq 'OK' -or $sw.ElapsedMilliseconds -gt 60000)
"device {0} after {1} ms" -f $d.Status, $sw.ElapsedMilliseconds
'--- saved log (tail)'
pwsh -NoProfile -File $diag log -Saved | Select-Object -Last 25
'--- new log'
pwsh -NoProfile -File $diag log | Select-Object -First 3
