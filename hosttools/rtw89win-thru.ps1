# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# PC side: board <-> PC throughput over the board's WLAN address, one bounded transfer each way (scp, at most
# 64 MB; keep a pause between runs and do not loop it). Checks that the bytes went through the board's WLAN adapter
# (its counters) and compares sha256. The payload is pseudo-random, generated next to this script.
#   .\hosttools\rtw89win-thru.ps1 -BoardWlan <the board's WLAN address, e.g. 192.0.2.10> -SshHost <ssh host of the
#        board, e.g. board> [-SizeMB 48] [-Tag run] [-Adapter WLAN] [-RemoteDir C:/Windows/Temp]
# -SshHost: the board's name or alias for ssh (e.g. a Host entry of ~/.ssh/config). The transfers go to -BoardWlan,
# and ssh still checks the host key known for -SshHost (HostKeyAlias). The board's OpenSSH server must use
# PowerShell as its default shell. -Adapter: the board's WLAN network adapter name. -RemoteDir: where the payload is
# put on the board (removed at the end). -BoardWlan and -SshHost may also come from RTW89WIN_BOARD_WLAN,
# RTW89WIN_SSH_HOST or local.settings.psd1 (tools-dev\buildenv.ps1).
param([int]$SizeMB = 48, [string]$Tag = 'run', [string]$BoardWlan, [string]$SshHost, [string]$Adapter = 'WLAN',
      [string]$RemoteDir = 'C:/Windows/Temp')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\tools-dev\buildenv.ps1"
$BoardWlan = Get-RtwRequiredSetting BoardWlan $BoardWlan "the board's WLAN address"
$SshHost = Get-RtwRequiredSetting SshHost $SshHost "the board's ssh host"
if ($SizeMB -gt 64) { throw 'at most 64 MB per transfer' }
$f = Join-Path $PSScriptRoot 'payload.bin'
if (-not (Test-Path $f) -or ((Get-Item $f).Length -ne $SizeMB * 1MB)) {
  $buf = New-Object byte[] (1MB)
  $rng = [System.Random]::new(12345)
  $fs = [IO.File]::Create($f)
  for ($i = 0; $i -lt $SizeMB; $i++) { $rng.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }
  $fs.Close()
}
$sha = (Get-FileHash $f -Algorithm SHA256).Hash
$remote = $RemoteDir.TrimEnd('/', '\') + '/rtw89win-payload.bin'
# HostKeyAlias: the host key is still checked against the one known for $SshHost
$opt = @('-o', "HostName=$BoardWlan", '-o', "HostKeyAlias=$SshHost", '-o', 'ConnectTimeout=8')
function BoardCounter([string]$name) { [int64](ssh @opt $SshHost "(Get-NetAdapterStatistics -Name '$Adapter').$name") }

"--- ${Tag}: PC -> board"
$rx0 = BoardCounter 'ReceivedBytes'
$sw = [Diagnostics.Stopwatch]::StartNew()
scp @opt $f "${SshHost}:$remote" | Out-Null
$sw.Stop()
$rx1 = BoardCounter 'ReceivedBytes'
$boardSha = (ssh @opt $SshHost "(Get-FileHash '$remote' -Algorithm SHA256).Hash").Trim()
'up: {0} MB in {1:n2} s = {2:n1} Mbit/s; board WLAN rx delta {3:n1} MB; sha {4}' -f $SizeMB, $sw.Elapsed.TotalSeconds,
  ($SizeMB * 8 / $sw.Elapsed.TotalSeconds), (($rx1 - $rx0) / 1MB), $(if ($boardSha -eq $sha) { 'OK' } else { 'MISMATCH' })

Start-Sleep -Seconds 5
"--- ${Tag}: board -> PC"
$back = Join-Path $PSScriptRoot 'back.bin'
$tx0 = BoardCounter 'SentBytes'
$sw = [Diagnostics.Stopwatch]::StartNew()
scp @opt "${SshHost}:$remote" $back | Out-Null
$sw.Stop()
$tx1 = BoardCounter 'SentBytes'
'down: {0} MB in {1:n2} s = {2:n1} Mbit/s; board WLAN tx delta {3:n1} MB; sha {4}' -f $SizeMB, $sw.Elapsed.TotalSeconds,
  ($SizeMB * 8 / $sw.Elapsed.TotalSeconds), (($tx1 - $tx0) / 1MB),
  $(if ((Get-FileHash $back -Algorithm SHA256).Hash -eq $sha) { 'OK' } else { 'MISMATCH' })
ssh @opt $SshHost "Remove-Item '$remote' -ErrorAction SilentlyContinue" | Out-Null
Remove-Item $back, $f -ErrorAction SilentlyContinue
