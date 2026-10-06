# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# PC side: TCP connect round trips (SYN -> SYN/ACK) to the board, Count times with GapMs in between; prints the loss
# (no answer within TimeoutMs) and min/avg/median/p95/max in ms. ICMP echo is blocked by the firewalls on both ends,
# so this stands in for ping (sshd on port 22 answers). With power save on, an idle board answers about one beacon
# interval later (observed with GapMs 300: median about 96 ms, against 1.8 ms without power save; see
# docs\en\validation.md).
#   .\hosttools\rtw89win-tcprtt.ps1 -BoardWlan <the board's WLAN address, e.g. 192.0.2.10> [-Port 22] [-Count 200]
# -BoardWlan may also come from RTW89WIN_BOARD_WLAN or local.settings.psd1 (tools-dev\buildenv.ps1).
param([Alias('Board')][string]$BoardWlan, [int]$Port = 22, [int]$Count = 200, [int]$TimeoutMs = 2000, [int]$GapMs = 300)
. "$PSScriptRoot\..\tools-dev\buildenv.ps1"
$Board = Get-RtwRequiredSetting BoardWlan $BoardWlan "the board's WLAN address"
$rtt = New-Object System.Collections.Generic.List[double]
$lost = 0
for ($i = 0; $i -lt $Count; $i++) {
  $s = New-Object System.Net.Sockets.Socket([System.Net.Sockets.AddressFamily]::InterNetwork, [System.Net.Sockets.SocketType]::Stream, [System.Net.Sockets.ProtocolType]::Tcp)
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $t = $s.ConnectAsync($Board, $Port)
  if ($t.Wait($TimeoutMs) -and $s.Connected) { $rtt.Add($sw.Elapsed.TotalMilliseconds) } else { $lost++ }
  $s.Close()
  Start-Sleep -Milliseconds $GapMs
}
$a = $rtt | Sort-Object
'{0} connects, {1} answered, {2} lost ({3:N1}%)' -f $Count, $rtt.Count, $lost, (100.0 * $lost / $Count)
if ($rtt.Count) { 'rtt ms: min {0:N1} avg {1:N1} median {2:N1} p95 {3:N1} max {4:N1}' -f $a[0], ($rtt | Measure-Object -Average).Average, $a[[int]($a.Count / 2)], $a[[int]([Math]::Min($a.Count - 1, $a.Count * 0.95))], $a[-1] }
