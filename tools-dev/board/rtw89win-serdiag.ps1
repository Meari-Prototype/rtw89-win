# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# SER (hardware restart) test in diagnostic mode, nothing connected: probe, start, listen on a channel, trigger
# SER, wait (bounded, by the log) for the restart to end, then scan and remove; prints the run's driver log, System
# warnings/errors and new minidumps. Changes state (probes and removes rtw89). Only in diagnostic mode.
# -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([int]$Channel = 6,
      [string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$diag = $Diag
function Log { pwsh -NoProfile -File $diag log }
$t0 = Get-Date
$n0 = (Log).Count
'probe: ' + ((pwsh -NoProfile -File $diag probe) | Select-String 'probe:' | Select-Object -Last 1)
'start: ' + ((pwsh -NoProfile -File $diag start) | Select-String 'start:' | Select-Object -Last 1)
'monitor: ' + ((pwsh -NoProfile -File $diag monitor $Channel -WaitMs 3000) | Select-String 'monitor:' | Select-Object -Last 1)
'ser: ' + ((pwsh -NoProfile -File $diag ser) | Select-String 'ser:' | Select-Object -Last 1)
$sw = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 300
  $l = Log | Select-Object -Skip $n0
} until (($l -match 'hardware restart \d+: (driver started|could not)') -or $sw.ElapsedMilliseconds -gt 20000)
'restart end seen after {0} ms' -f $sw.ElapsedMilliseconds
'scan after the restart: ' + ((pwsh -NoProfile -File $diag scan) | Select-String 'IOCTL scan: -?\d' | Select-Object -Last 1)
'remove: ' + ((pwsh -NoProfile -File $diag remove) | Select-String 'remove' | Select-Object -Last 1)
'--- log of the run'
Log | Select-Object -Skip $n0 | Where-Object { $_ -notmatch 'fw PC =|mac: bss |Firmware element|first beacon' }
'--- System warnings/errors since the start; new minidumps'
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1, 2, 3; StartTime = $t0 } -ErrorAction SilentlyContinue |
  ForEach-Object { '{0:HH:mm:ss} {1} {2}' -f $_.TimeCreated, $_.Id, $_.ProviderName }
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0 | ForEach-Object Name
