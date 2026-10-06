# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Connection test, event driven: netsh wlan connect to the profile's SSID (or -Watch: only watch, e.g. after a
# click on the screen), then poll until the WLAN adapter has a usable IPv4 address (success; with -Prefix, e.g.
# 192.0.2., only one that starts with it; without, any but 169.254.x.x), WLAN-AutoConfig reports a failed connection
# (8002), or -TimeoutSec. Prints what happened when, the WLAN-AutoConfig events, the driver log lines of the attempt,
# the interface, the link counters. -Disconnect: netsh wlan disconnect and wait until the adapter is down (or
# -TimeoutSec). -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([Parameter(Mandatory)][string]$Ssid, [int]$TimeoutSec = 60, [string]$Prefix = '',
      [switch]$Watch, [switch]$Disconnect, [string]$Interface = 'WLAN',
      [string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$d = $Diag
function LogLines { pwsh -NoProfile -File $d log }
function LogEnd { $l = LogLines; for ($i = $l.Count - 1; $i -ge 0; $i--) { if ($l[$i] -match '^\[\s*(\d+\.\d+)\]') { return [double]$Matches[1] } }; 0 }
$from = LogEnd
$t0 = Get-Date
$sw = [Diagnostics.Stopwatch]::StartNew()
$seen = @{}
function Mark($what) { if (-not $seen[$what]) { $seen[$what] = $true; '{0,7:N2} s  {1}' -f $sw.Elapsed.TotalSeconds, $what } }

if ($Disconnect) {
  netsh wlan disconnect interface="$Interface"
  while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    $a = Get-NetAdapter -Name $Interface -ErrorAction SilentlyContinue
    if ($a.Status -ne 'Up') { Mark "adapter $($a.Status)"; break }
    Start-Sleep -Milliseconds 200
  }
} else {
  if (-not $Watch) { netsh wlan connect name="$Ssid" ssid="$Ssid" interface="$Interface" }
  $ok = $false
  while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    $a = Get-NetAdapter -Name $Interface -ErrorAction SilentlyContinue
    if ($a.Status -eq 'Up') { Mark 'adapter Up' }
    $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-WLAN-AutoConfig/Operational'; StartTime = $t0 } -ErrorAction SilentlyContinue
    foreach ($e in $ev) { if ($e.Id -in 8000, 8001, 8002, 8003, 11000, 11001, 11002, 11004, 11005, 11010, 12011, 12012, 12013) { Mark "event $($e.Id)" } }
    if ($ev | Where-Object Id -eq 8002) { break }
    $ip = Get-NetIPAddress -InterfaceAlias $Interface -AddressFamily IPv4 -ErrorAction SilentlyContinue |
      Where-Object { if ($Prefix) { $_.IPAddress -like "$Prefix*" } else { $_.IPAddress -notlike '169.254.*' } } | Select-Object -First 1
    if ($ip -and $ip.AddressState -eq 'Preferred') { Mark "IPv4 $($ip.IPAddress)"; $ok = $true; break }
    Start-Sleep -Milliseconds 200
  }
  if (-not $ok) { Mark 'no address (timeout or failure)' }
}

'--- WLAN-AutoConfig events'
Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-WLAN-AutoConfig/Operational'; StartTime = $t0 } -ErrorAction SilentlyContinue |
  Sort-Object TimeCreated | ForEach-Object {
    $m = ($_.Message -split "`r?`n" | Where-Object { $_ -match '\S' -and $_ -notmatch '(?i)GUID|guid' }) -join ' | '
    '{0:HH:mm:ss.fff} {1,5} {2}' -f $_.TimeCreated, $_.Id, $(if ($m.Length -gt 400) { $m.Substring(0, 400) } else { $m })
  }
'--- driver log'
foreach ($l in LogLines) {
  if ($l -match '^\[\s*(\d+\.\d+)\]' -and [double]$Matches[1] -gt $from -and $l -notmatch 'wdi dump:|fw PC') { $l }
}
'--- interface'
netsh wlan show interfaces
Get-NetIPAddress -InterfaceAlias $Interface -AddressFamily IPv4 -ErrorAction SilentlyContinue | Format-Table IPAddress, PrefixLength, AddressState, PrefixOrigin -AutoSize | Out-String -Width 200
'--- link'
& $d link
