# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# SER (hardware restart) test while connected (WiFiCx mode): trigger one SER, then follow by events, each wait
# bounded: the restart's end in the driver log, the adapter leaving Up, and Wlansvc reconnecting by itself (adapter Up
# with a Preferred IPv4 address; with -Prefix, e.g. 192.0.2., one that starts with it; without, any but 169.254.x.x).
# Prints the run's driver log, link counters, WLAN-AutoConfig events, System warnings/errors and new minidumps.
# Changes state (the connection is dropped and re-made by Wlansvc).
# -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([string]$Prefix = '', [int]$ReconnectSec = 90,
      [string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$diag = $Diag
function Log { pwsh -NoProfile -File $diag log }
$t0 = Get-Date
$n0 = (Log).Count
'before: WLAN ' + (Get-NetAdapter WLAN).Status + ', ' + ((pwsh -NoProfile -File $diag link) -match '^(state|restarts)' -join '; ')
'ser: ' + ((pwsh -NoProfile -File $diag ser) | Select-String 'ser:' | Select-Object -Last 1)
$sw = [Diagnostics.Stopwatch]::StartNew()
do {
  Start-Sleep -Milliseconds 300
  $l = Log | Select-Object -Skip $n0
} until (($l -match 'hardware restart \d+: (driver started|could not)') -or $sw.ElapsedMilliseconds -gt 20000)
'restart end seen after {0} ms' -f $sw.ElapsedMilliseconds
$down = $null
do {
  Start-Sleep -Milliseconds 300
  if ((Get-NetAdapter WLAN).Status -ne 'Up') { $down = $sw.ElapsedMilliseconds }
} until ($down -or $sw.ElapsedMilliseconds -gt 15000)
'adapter seen down at: ' + $(if ($down) { "$down ms" } else { 'never (Wlansvc reconnected within the polling interval)' })
$up = $null
do {
  Start-Sleep -Milliseconds 500
  $ip = Get-NetIPAddress -InterfaceAlias WLAN -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.AddressState -eq 'Preferred' -and $(if ($Prefix) { $_.IPAddress -like "$Prefix*" } else { $_.IPAddress -notlike '169.254.*' }) } |
    Select-Object -First 1
  if ((Get-NetAdapter WLAN).Status -eq 'Up' -and $ip) { $up = $sw.ElapsedMilliseconds }
} until ($up -or $sw.Elapsed.TotalSeconds -gt $ReconnectSec)
'reconnected: ' + $(if ($up) { "$($ip.IPAddress) at $up ms" } else { "no, within $ReconnectSec s" })
'--- log of the run'
Log | Select-Object -Skip $n0 | Where-Object { $_ -notmatch 'fw PC =|mac: bss |Firmware element|first beacon|wdi |R_AX_|FW (BADADDR|EPC|MISC)' }
'--- link'
pwsh -NoProfile -File $diag link | Select-String '^(state|BSS|security|restarts|ba_|tx_failed|rx_dropped_replay|connects|disconnects)'
'--- WLAN-AutoConfig events'
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-WLAN-AutoConfig/Operational'; StartTime = $t0 } -ErrorAction SilentlyContinue |
  Sort-Object TimeCreated | ForEach-Object { '{0:HH:mm:ss.fff} {1}' -f $_.TimeCreated, $_.Id }
'--- System warnings/errors since the start; new minidumps'
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1, 2, 3; StartTime = $t0 } -ErrorAction SilentlyContinue |
  ForEach-Object { '{0:HH:mm:ss} {1} {2}' -f $_.TimeCreated, $_.Id, $_.ProviderName }
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Where-Object LastWriteTime -gt $t0 | ForEach-Object Name
