# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# rtw89win diagnostics on the board (administrator; the device must be started).
# Both modes:
#   rtwdiag.ps1 log              print the driver log kept in memory (last 64 KB)
#   rtwdiag.ps1 log -Follow      keep printing new lines until Ctrl+C
#   rtwdiag.ps1 log -Saved       the log tail (16 KB) the last WiFiCx session left in Parameters\SavedLog: saved when
#                                its start failed, when it released the hardware (restart, disable, removal) or at
#                                shutdown
#   rtwdiag.ps1 read 0xF0 0xF4   read 32-bit registers of BAR2
#   rtwdiag.ps1 mode             the mode (WiFiCx, diagnostic, or diagnostic because the guard tripped), the guard's
#                                values, the WiFiCx front end's state and counters, and the WDI messages received per
#                                id (and how many were not supported)
#   rtwdiag.ps1 arm              clear the guard (Parameters\ArmRunning and BootAttempts = 0), restart the device and
#                                wait (at most -WaitMs, default 60000 here) until it is up in WiFiCx mode with its
#                                adapter, or failed; prints the log, the network adapter and Wlansvc
#   rtwdiag.ps1 link             connection state, the PHY mode and width (HT20, VHT80, center channel), the last data
#                                frame's encoding/width/MCS/streams, the TX rate rtw89 reported, and the data path
#                                counters (incl. A-MPDU, A-MSDU, +HTC, management frame protection, restarts)
#   rtwdiag.ps1 ser              trigger a hardware restart (SER) via the firmware; connected or listening only.
#                                The connection drops and Wlansvc reconnects.
#   rtwdiag.ps1 bss              print the BSS table and the receive counters
#   rtwdiag.ps1 bss -Ies [BSSID] the elements of the kept beacon/probe response bodies: Country, RSN (MFPC/MFPR), RSNXE,
#                                TIM (DTIM period), WMM QoS info, Extended Capabilities, HT/VHT Capabilities and
#                                Operation, Operating Mode Notification, HE Capabilities/Operation and MU EDCA
#                                (presence and main fields); all BSSs or one
#   rtwdiag.ps1 radio            regulatory domain (country, its source, active/passive channels), power save (policy,
#                                vif cfg.ps, rtw89 LPS state) and Bluetooth coexistence summary (BT state, antenna,
#                                TDMA, which btc hooks were called how often)
#   rtwdiag.ps1 btc [-Full]      the coexistence summary; -Full: rtw89's whole report (Linux debugfs btc_info), only
#                                while the chip is powered (connected, listening or scanning)
#   rtwdiag.ps1 ap               the mobile hotspot's Wi-Fi Direct GO (running, band, channel, BSSID, beacon interval),
#                                its clients (state, AID, authorized/sleeping/HT/WMM, buffered frames, idle time,
#                                rx/tx) and counters, and the front end's Wi-Fi Direct ports (device port, role port,
#                                START_AP tasks, last channel)
#   rtwdiag.ps1 settings         this driver's settings under Parameters (Country, PowerSave, Pmf, WifiDirect (0 off,
#                                1 on; not set = on), WdiDump, Bandwidth (20/40/80 MHz; not set = 80), the optional
#                                connection gate AllowConnect/AllowSsid), the country and bandwidth in effect now, the
#                                mode and the guard
#   rtwdiag.ps1 country [XX]     show, or set, Parameters\Country (REG_SZ, persistent; install-rtw89win.ps1 -Country
#                                XX sets it too): two letters, or 00 for none (the Country element of the
#                                connected BSS, else the world rules). Read at every probe: pnputil /restart-device
#                                (or arm) to apply.
# Diagnostic mode only (in WiFiCx mode they are refused with -EBUSY):
#   rtwdiag.ps1 probe            start rtw89 (power on, firmware download, efuse, register), then print its log
#   rtwdiag.ps1 remove           stop rtw89 and release its interrupt and DMA memory, then print its log
#   rtwdiag.ps1 start            (after probe) drv_start + station interface (the chip then idles, powered off)
#   rtwdiag.ps1 stop             cancel scan/listening, remove the interface, drv_stop
#   rtwdiag.ps1 monitor 6        listen on channel 6 (20 MHz) and wait for beacons (-WaitMs, default 5000), then
#                                print the BSS table; listening goes on until "monitor 0" or stop
#   rtwdiag.ps1 scan             firmware scan of all channels, then print the BSS table
#                                [-Keep] keep earlier results  [-Passive] no probe requests
#                                [-DwellMs n] per channel  [-TimeoutMs n] (default 15000, at most 30000)
#                                do not uninstall or restart the device during a scan: removal waits for it
#   rtwdiag.ps1 txprobe          while listening on 2.4 GHz channel 1-11 (monitor 1..11): send [-Count 1..4]
#                                broadcast probe requests [-IntervalMs 50..1000] and count the tx status and the
#                                probe responses to us during [-WaitMs 100..2000]; 32 frames per driver load
# Each control command prints the driver log written while it ran. settings and country also work without the device.
# Modes: every start of the device runs WiFiCx mode, behind a crash loop guard. ArmRunning = 1 is flushed before the
# chip is touched and cleared when the session ends in order (release of the hardware, shutdown/restart of the
# system); still 1 at the next start adds one to BootAttempts; at 2 (two sessions in a row did not end in order) the
# device starts in diagnostic mode until "arm". At boot the hive cannot be flushed yet, so PrepareHardware waits (at
# most 60 s) until ArmRunning = 1 is on the disk (log: "guard: registry writable after ... ms"), then probes the chip.
# For tests in diagnostic mode: set BootAttempts = 2 by hand, then pnputil /restart-device (the ending session keeps
# a 2); "arm" goes back to WiFiCx.
# (no positional binding except Cmd and Rest: otherwise "monitor 36" would bind 36 to -DwellMs)
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory, Position = 0)][ValidateSet('log', 'read', 'probe', 'remove', 'start', 'stop', 'monitor', 'scan', 'bss', 'mode', 'arm', 'txprobe', 'link', 'ser', 'radio', 'btc', 'settings', 'country', 'ap')][string]$Cmd,
  [Parameter(Position = 1, ValueFromRemainingArguments)][string[]]$Rest,
  [switch]$Follow,
  [switch]$Full,
  [switch]$Ies,
  [switch]$Keep,
  [switch]$Passive,
  [switch]$Saved,
  [uint32]$DwellMs = 0,
  [uint32]$TimeoutMs = 0,
  [uint32]$WaitMs = 0,
  [uint32]$Count = 1,
  [uint32]$IntervalMs = 100
)
$ErrorActionPreference = 'Stop'
$svcKey = 'SYSTEM\CurrentControlSet\Services\rtw89win'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class RtwDiag {
  // CTL_CODE(0x8852, 0x800..0x811, METHOD_BUFFERED, FILE_ANY_ACCESS), see driver\rtw89win_ioctl.h
  public const uint LogRead = 0x88522000;
  public const uint Read32  = 0x88522004;
  public const uint Probe   = 0x88522008;
  public const uint Remove  = 0x8852200C;
  public const uint Start   = 0x88522010;
  public const uint Stop    = 0x88522014;
  public const uint Monitor = 0x88522018;
  public const uint Scan    = 0x8852201C;
  public const uint BssList = 0x88522020;
  public const uint Mode    = 0x88522024;
  public const uint TxProbe = 0x88522028;
  public const uint Link    = 0x88522030;
  public const uint Ser     = 0x88522034;
  public const uint Radio   = 0x88522038;
  public const uint Btc     = 0x8852203C;
  public const uint BssIes  = 0x88522040;
  public const uint Ap      = 0x88522044;

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr overlapped);

  public static byte[] Ioctl(uint code, byte[] input, int outLen) {
    using (SafeFileHandle h = CreateFileW(@"\\.\rtw89win", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
      if (h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), @"open \\.\rtw89win");
      byte[] output = new byte[outLen];
      int n;
      if (!DeviceIoControl(h, code, input, input.Length, output, outLen, out n, IntPtr.Zero))
        throw new Win32Exception(Marshal.GetLastWin32Error(), "DeviceIoControl 0x" + code.ToString("x8"));
      Array.Resize(ref output, n);
      return output;
    }
  }
}
'@

function Write-LogFrom([uint64]$from) {
  while ($true) {
    $out = [RtwDiag]::Ioctl([RtwDiag]::LogRead, [BitConverter]::GetBytes($from), 0x10000 + 16)
    $from = [BitConverter]::ToUInt64($out, 0)
    if ($out.Length -le 16) { break }
    [Console]::Write([Text.Encoding]::UTF8.GetString($out, 16, $out.Length - 16))
  }
}

# log position now (reading from the end returns no text, only the position)
function Get-LogEnd { [BitConverter]::ToUInt64([RtwDiag]::Ioctl([RtwDiag]::LogRead, [BitConverter]::GetBytes([uint64]::MaxValue), 64), 0) }

$errno = @{ 1 = 'EPERM'; 2 = 'ENOENT'; 5 = 'EIO'; 12 = 'ENOMEM'; 16 = 'EBUSY'; 19 = 'ENODEV (not probed)'; 22 = 'EINVAL';
            95 = 'EOPNOTSUPP'; 100 = 'ENETDOWN'; 107 = 'ENOTCONN (not started / not listening)'; 110 = 'ETIMEDOUT';
            122 = 'EDQUOT (frame budget of this driver load used up)'; 125 = 'ECANCELED' }
function Format-Result([string]$what, [int]$ret) {
  if ($ret -eq 0) { "${what}: ok" } else { '{0}: failed, {1} ({2})' -f $what, $ret, $errno[-$ret] }
}

# Runs a control IOCTL, then prints the driver log written meanwhile (also when it fails).
function Invoke-Control([uint32]$code, [byte[]]$in, [int]$outLen) {
  $start = Get-LogEnd
  try { $out = [RtwDiag]::Ioctl($code, $in, $outLen) } finally { Write-LogFrom $start }
  return ,$out
}

$band = @{ 0 = '2.4'; 1 = '5'; 3 = '6' }   # NL80211_BAND_*
function Get-Security($e) {
  $sec = $e.Security; $akm = $e.Akm
  if ($sec -band 4) {
    $names = @()
    if ($akm -band 2) { $names += 'WPA2-PSK' }
    if ($akm -band 4) { $names += 'WPA3-SAE' }
    if ($akm -band 1) { $names += 'WPA2-Ent' }
    if ($akm -band 8) { $names += 'OWE' }
    if (-not $names) { $names += 'RSN' }
    $s = $names -join '/'
  } elseif ($sec -band 2) { $s = if ($akm -band 1) { 'WPA-Ent' } else { 'WPA-PSK' } }
  elseif ($sec -band 1) { $s = 'WEP' }
  else { return 'Open' }
  $c = @()
  if ($e.Pairwise -band 4) { $c += 'CCMP' }
  if ($e.Pairwise -band 8) { $c += 'GCMP' }
  if ($e.Pairwise -band 0x10) { $c += 'CCMP256' }
  if ($e.Pairwise -band 0x20) { $c += 'GCMP256' }
  if ($e.Pairwise -band 2) { $c += 'TKIP' }
  if ($c) { $s += ' ' + ($c -join '+') }
  return $s
}

function Get-Bss {
  $out = [RtwDiag]::Ioctl([RtwDiag]::BssList, [byte[]]@(), 40 + 64 * 256)
  $u = { param($o) [BitConverter]::ToUInt32($out, $o) }
  $hdr = [pscustomobject]@{
    Count = & $u 0; Total = & $u 4; State = & $u 8; Channel = & $u 12; RxFrames = & $u 16; RxBeacons = & $u 20
    RxProbeResps = & $u 24; RxBad = & $u 28; Scans = & $u 32
  }
  $list = for ($i = 0; $i -lt $hdr.Count; $i++) {
    $o = 40 + 64 * $i
    $ssidLen = [Math]::Min([int]$out[$o + 20], 32)
    $ssid = [Text.Encoding]::UTF8.GetString($out, $o + 24, $ssidLen)
    $e = [pscustomobject]@{
      BSSID = ($out[$o..($o + 5)] | ForEach-Object { $_.ToString('x2') }) -join ':'
      SSID = if ($ssidLen) { $ssid } elseif ($out[$o + 21] -band 4) { '<hidden>' } else { '' }
      Band = $band[[int]$out[$o + 7]]
      Ch = $out[$o + 6]
      MHz = [BitConverter]::ToUInt16($out, $o + 8)
      dBm = [sbyte]$(if ($out[$o + 14] -gt 127) { $out[$o + 14] - 256 } else { $out[$o + 14] })
      Phy = $out[$o + 15]; Security = $out[$o + 16]; Akm = $out[$o + 17]; Pairwise = $out[$o + 18]
      Beacon = [BitConverter]::ToUInt16($out, $o + 12)
      AgeS = [Math]::Round([BitConverter]::ToUInt32($out, $o + 56) / 1000.0, 1)
      Frames = [BitConverter]::ToUInt32($out, $o + 60)
    }
    $e | Add-Member Sec (Get-Security $e)
    $e | Add-Member Std $(if ($e.Phy -band 8) { 'be' } elseif ($e.Phy -band 4) { 'ax' } elseif ($e.Phy -band 2) { 'ac' } elseif ($e.Phy -band 1) { 'n' } else { 'a/b/g' })
    $e
  }
  return $hdr, @($list)
}

function Show-Bss {
  $hdr, $list = Get-Bss
  $st = @(); foreach ($k in @{ 1 = 'probed'; 2 = 'started'; 4 = 'listening'; 8 = 'scanning' }.GetEnumerator() | Sort-Object Name) { if ($hdr.State -band $k.Name) { $st += $k.Value } }
  'state: {0}{1}; rx {2} frames, {3} beacons, {4} probe responses, {5} bad; {6} BSS ({7} shown); scans {8}' -f `
    ($(if ($st) { $st -join ', ' } else { 'not probed' })), $(if ($hdr.Channel) { " on channel $($hdr.Channel)" } else { '' }),
    $hdr.RxFrames, $hdr.RxBeacons, $hdr.RxProbeResps, $hdr.RxBad, $hdr.Total, $hdr.Count, $hdr.Scans
  $list | Sort-Object Band, @{ Expression = 'dBm'; Descending = $true } |
    Format-Table BSSID, SSID, Band, Ch, MHz, dBm, Sec, Std, Beacon, AgeS, Frames -AutoSize | Out-String -Width 200
}

function Get-RtwDevice {
  $d = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*' | Select-Object -First 1
  if (-not $d) { throw 'RTL8852BE (PCI\VEN_10EC&DEV_B852) not present' }
  return $d
}

# up to $len bytes of log text from position $from (one read), or $null while there is no control device
function Get-LogChunk([uint64]$from, [int]$len) {
  try {
    $out = [RtwDiag]::Ioctl([RtwDiag]::LogRead, [BitConverter]::GetBytes($from), $len + 16)
    return [Text.Encoding]::UTF8.GetString($out, 16, $out.Length - 16)
  } catch { return $null }
}

# the log kept in memory from position $from on (default: all of it), or $null while there is no control device
# (driver not loaded)
function Get-LogText([uint64]$from = 0) {
  try {
    $sb = [Text.StringBuilder]::new()
    while ($true) {
      $out = [RtwDiag]::Ioctl([RtwDiag]::LogRead, [BitConverter]::GetBytes($from), 0x10000 + 16)
      $from = [BitConverter]::ToUInt64($out, 0)
      if ($out.Length -le 16) { break }
      [void]$sb.Append([Text.Encoding]::UTF8.GetString($out, 16, $out.Length - 16))
    }
    return $sb.ToString()
  } catch { return $null }
}

function Show-SavedLog {
  $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$svcKey\Parameters")
  $v = if ($k) { $k.GetValue('SavedLog') } else { $null }
  if ($v) { [Text.Encoding]::UTF8.GetString([byte[]]$v) } else { '(no Parameters\SavedLog)' }
}

# The crash loop guard's values (driver\driver.c; persistent, under Parameters)
function Show-Guard {
  $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$svcKey\Parameters")
  $run = if ($k) { $k.GetValue('ArmRunning') } else { $null }
  $att = if ($k) { $k.GetValue('BootAttempts') } else { $null }
  if ($k) { $k.Close() }
  $n = if ($att) { [int]$att } else { 0 }
  'guard: ArmRunning {0} ({1}), BootAttempts {2} of 2 (sessions in a row that did not end in order){3}' -f
    $(if ($null -eq $run) { 'not set' } else { $run }), $(if ($run) { 'a WiFiCx session is running, or the last one did not end in order' } else { 'no session open' }),
    $n, $(if ($n -ge 2) { ': TRIPPED, the device starts in diagnostic mode; "rtwdiag.ps1 arm" clears it' } else { '' })
}

$modeNames = @{ 0 = 'diagnostic: probe/start/scan/... by IOCTL'; 1 = 'WiFiCx';
                2 = 'diagnostic, because the crash loop guard tripped (two WiFiCx sessions in a row did not end in order; "rtwdiag.ps1 arm" clears it): probe/start/scan/... by IOCTL' }
function Show-Mode {
  # the per-message counters follow the 48 bytes when there is room (an older driver returns 48 bytes only)
  $out = [RtwDiag]::Ioctl([RtwDiag]::Mode, [byte[]]@(), 48 + 2048)
  $u = { param($i) [BitConverter]::ToUInt32($out, 4 * $i) }
  $mode = & $u 0
  'mode: {0}' -f $(if ($modeNames.ContainsKey([int]$mode)) { $modeNames[[int]$mode] } else { $mode })
  Show-Guard
  if ($mode -ne 1) { return }
  $port = & $u 2
  $wdi = & $u 6
  'hardware ready {0}, adapter port {1}, radio {2}, mac layer {3}, scan task {4}' -f (& $u 1),
    $(if ($port -eq [uint32]::MaxValue) { 'none' } else { $port }), $(if (& $u 3) { 'on' } else { 'off' }),
    $(if (& $u 4) { 'started' } else { 'stopped' }), $(if (& $u 5) { 'running' } else { 'none' })
  'OS WDI version {0}.{1}.{2}; commands received {3}, completed {4}, not supported {5}; scans {6}, last {7} BSS' -f
    ($wdi -shr 16), (($wdi -shr 8) -band 0xFF), ($wdi -band 0xFF), (& $u 7), (& $u 8), (& $u 9), (& $u 10), (& $u 11)
  if ($out.Length -ge 48 + 2048) {
    $ids = for ($i = 0; $i -lt 256; $i++) {
      $n = [BitConverter]::ToUInt32($out, 48 + 4 * $i)
      $ns = [BitConverter]::ToUInt32($out, 48 + 1024 + 4 * $i)
      if ($n) { if ($ns) { '{0}:{1} ({2} not supported)' -f $i, $n, $ns } else { '{0}:{1}' -f $i, $n } }
    }
    'WDI messages by id (km\dot11wificxintf.h), id:received: {0}' -f $(if ($ids) { $ids -join ', ' } else { 'none' })
  }
}

$regSrc = @('world', 'user (Parameters\Country)', 'Country element of the connected BSS', 'efuse')
$rtwRegd = @('WW', 'ETSI', 'FCC', 'MKK', 'NA', 'IC', 'KCC', 'ACMA', 'NCC', 'MEXICO', 'CHILE', 'UKRAINE', 'CN', 'QATAR', 'UK', 'THAILAND')
$btcNotify = @('POWER_ON', 'POWER_OFF', 'INIT_COEX', 'SCAN_START', 'SCAN_FINISH', 'SPECIAL_PACKET', 'SWITCH_BAND',
               'RFK_TIMEOUT', 'SHOW_COEX_INFO', 'ROLE_INFO', 'CONTROL', 'RADIO_STATE', 'CUSTOMERIZE', 'WL_RFK', 'WL_STA',
               'WL_STA_LAST', 'FWINFO', 'TIMER', 'SWITCH_CHBW', 'RESUME_DL_FW', 'COUNTRYCODE')
function Get-Alpha2([byte[]]$b, [int]$o) { if ($b[$o]) { [string][char]$b[$o] + [char]$b[$o + 1] } else { '-' } }

# struct rtw_radio_info (glue.h, 200 bytes)
function Show-Radio([switch]$BtcOnly) {
  $out = [RtwDiag]::Ioctl([RtwDiag]::Radio, [byte[]]@(), 200)
  $u = { param($o) [BitConverter]::ToUInt32($out, $o) }
  $plan = [int]$out[29]; if ($plan -gt 127) { $plan -= 256 }
  $hints = @('none', 'auto power save', 'low latency', 'high throughput', 'no power save')
  if (-not $BtcOnly) {
    'power save policy: {0} (Parameters\PowerSave {1}: {2}; power plan saving mode {3}; connection quality hint {4})' -f
      $(if ($out[31]) { 'save power' } else { 'no power save' }), $out[28], @('never', 'follow the OS', 'always')[[Math]::Min([int]$out[28], 2)],
      $(if ($plan -lt 0) { 'unknown' } else { $plan }), $hints[[Math]::Min([int]$out[30], 4)]
  }
  if (-not $out[0]) { 'not probed'; return }
  if (-not $BtcOnly) {
    'regulatory: {0} from {1}, DFS region {2}; user {3}, Country element {4}{5}, efuse {6}; {7} rule sets applied' -f
      (Get-Alpha2 $out 2), $regSrc[[Math]::Min([int]$out[1], 3)], $out[10], (Get-Alpha2 $out 4), (Get-Alpha2 $out 6),
      $(if ($out[11]) { ' (ignored: REGULATORY_COUNTRY_IE_IGNORE)' } else { '' }), (Get-Alpha2 $out 8), (& $u 24)
    'channels: 2.4 GHz {0} active, {1} passive, {2} disabled; 5 GHz {3} active, {4} passive, {5} disabled' -f
      $out[16], $out[18], $out[20], $out[17], $out[19], $out[21]
    'rtw89 TX power tables: {0}, 2.4 GHz {1}, 5 GHz {2}' -f (Get-Alpha2 $out 12), $rtwRegd[[Math]::Min([int]$out[14], 15)],
      $rtwRegd[[Math]::Min([int]$out[15], 15)]
    $f = $out[35]
    $flags = @(); foreach ($k in @(@(1, 'POWERON'), @(2, 'LEISURE_PS'), @(4, 'LOW_POWER_MODE'), @(8, 'INACTIVE_PS'), @(16, 'RUNNING'))) { if ($f -band $k[0]) { $flags += $k[1] } }
    'power save: vif cfg.ps {0}; rtw89 lps_enabled {1}, flags {2}, deep mode {3}; btc holds LPS off {4}; {5} changes, {6} s on in all; beacon interval {7}' -f
      $out[32], $out[33], $(if ($flags) { $flags -join ' ' } else { 'none' }),
      @('none', 'RF off', 'clock gated', 'power gated')[[Math]::Min([int]$out[34], 3)], $out[44], (& $u 36),
      [Math]::Round((& $u 40) / 1000.0, 1), $(if ([BitConverter]::ToUInt16($out, 22)) { '{0} TU' -f [BitConverter]::ToUInt16($out, 22) } else { '-' })
  }
  $prof = @(); foreach ($k in @(@(1, 'HFP'), @(2, 'HID'), @(4, 'A2DP'), @(8, 'PAN'))) { if ($out[47] -band $k[0]) { $prof += $k[1] } }
  'BT: enabled {0}, profiles {1} ({2}), BT firmware 0x{3:x8}, scoreboard 0x{4:x8}' -f $out[45], $out[46],
    $(if ($prof) { $prof -join ' ' } else { 'none' }), (& $u 48), (& $u 52)
  'btc: policy 0x{0:x4}, run reason {1}, action {2}, TDMA {3}; wl_only {4}, bt_only {5}, freerun {6}' -f
    [BitConverter]::ToUInt16($out, 56), $out[58], $out[59], (($out[60..67] | ForEach-Object { $_.ToString('x2') }) -join ' '),
    $out[73], $out[74], $out[75]
  'antenna: type {0}, number {1}, isolation {2}, single at S{3}, BTG at {4}' -f $out[68], $out[69], $out[70], $out[71], $out[72]
  'btc counters: C2H {0}, H2C {1} (failed {2}), firmware reports {3}, BT info {4}, scoreboard updates {5}, runs {6}' -f
    (& $u 76), (& $u 80), (& $u 84), (& $u 88), (& $u 92), (& $u 96), (& $u 100)
  $n = for ($i = 0; $i -lt $btcNotify.Count; $i++) { '{0} {1}' -f $btcNotify[$i], (& $u (104 + 4 * $i)) }
  'btc hooks called: {0}' -f ($n -join ', ')
}

# This driver's Parameters values as the registry has them (driver\probe.c reads them at every probe), and what is in
# effect when the driver answers
function Show-Settings([switch]$CountryOnly) {
  $k = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("$svcKey\Parameters")
  $get = { param($n) if ($k) { $k.GetValue($n) } else { $null } }
  $c = & $get 'Country'
  'Parameters\Country: {0}' -f $(if ($null -eq $c) { 'not set (none: the Country element of the connected BSS, else the world rules)' }
                                 elseif ($c -eq '00') { '"00" (none: the Country element of the connected BSS, else the world rules)' }
                                 else { '"{0}"' -f $c })
  if (-not $CountryOnly) {
    $v = & $get 'PowerSave'
    'Parameters\PowerSave: {0}' -f $(if ($null -eq $v) { 'not set (1: follow the OS)' } else { $v })
    $v = & $get 'Pmf'
    'Parameters\Pmf: {0}' -f $(if ($null -eq $v) { 'not set (0: no management frame protection, no WPA3-SAE)' } else { $v })
    $v = & $get 'WifiDirect'
    'Parameters\WifiDirect: {0}' -f $(if ($null -eq $v) { 'not set (1: Wi-Fi Direct / mobile hotspot on)' } else { $v })
    $v = & $get 'WdiDump'
    'Parameters\WdiDump: {0}' -f $(if ($null -eq $v) { 'not set (0)' } else { $v })
    $v = & $get 'Bandwidth'
    'Parameters\Bandwidth: {0}' -f $(if ($null -eq $v) { 'not set (80: 5 GHz up to VHT80)' } elseif (@(20, 40, 80) -notcontains $v) { "$v (not 20, 40 or 80: ignored, 80)" } else { "$v MHz" })
    # the optional connection gate (wificx\connect.cpp): the volatile value wins over the persistent one
    $a = if ($k) { $k.OpenSubKey('Allow') } else { $null }
    $vc = if ($a) { $a.GetValue('AllowConnect') } else { $null }
    $vs = if ($a) { $a.GetValue('AllowSsid') } else { $null }
    if ($a) { $a.Close() }
    $pc = & $get 'AllowConnect'
    $eff = if ($null -ne $vc) { $vc } elseif ($null -ne $pc) { $pc } else { $null }
    'connection gate: {0} (Parameters\AllowConnect {1}, volatile Parameters\Allow\AllowConnect {2}); SSID pin: {3}' -f
      $(if ($null -eq $eff -or $eff -ne 0) { 'open' } else { 'CLOSED, every connect is refused' }),
      $(if ($null -eq $pc) { 'not set' } else { $pc }), $(if ($null -eq $vc) { 'not set' } else { $vc }),
      $(if ($null -eq $vs) { 'none (Parameters\Allow\AllowSsid not set)' } else { 'only "{0}" (Parameters\Allow\AllowSsid)' -f $vs })
  }
  if ($k) { $k.Close() }
  try {
    $out = [RtwDiag]::Ioctl([RtwDiag]::Radio, [byte[]]@(), 200)
    if ($out[0]) { 'in effect: {0} from {1}' -f (Get-Alpha2 $out 2), $regSrc[[Math]::Min([int]$out[1], 3)] }
    else { 'in effect: - (not probed)' }
  } catch { 'in effect: - (no answer from the driver)' }
  if (-not $CountryOnly) {
    try {
      $m = [BitConverter]::ToUInt32([RtwDiag]::Ioctl([RtwDiag]::Mode, [byte[]]@(), 48), 0)
      'mode: {0}' -f $(if ($modeNames.ContainsKey([int]$m)) { $modeNames[[int]$m] } else { $m })
    } catch { 'mode: - (no answer from the driver)' }
    try {
      $l = [RtwDiag]::Ioctl([RtwDiag]::Link, [byte[]]@(), 288)
      'Bandwidth in effect: {0}' -f $(if ($l.Length -lt 288) { '20 MHz (an older driver without the Bandwidth setting)' } elseif ($l[259]) { '5 GHz up to {0} MHz' -f $l[259] } else { '- (not probed)' })
    } catch { 'Bandwidth in effect: - (no answer from the driver)' }
    Show-Guard
  }
}

# IOCTL_RTW_AP: struct rtw_ap_stats (340 bytes, compat\include\rtw\glue.h), then RTW_WFD_OUT (32 bytes, driver\rtw89win.h)
function Show-Ap {
  $b = [RtwDiag]::Ioctl([RtwDiag]::Ap, [byte[]]@(), 340 + 32)
  $mac = { param($o) ($b[$o..($o + 5)] | ForEach-Object { $_.ToString('x2') }) -join ':' }
  $names = 'starts', 'stops', 'beacons', 'probe_req', 'probe_resp', 'auth_rx', 'assoc_req', 'assoc_ok', 'assoc_refused',
           'assoc_tx_failed', 'deauth_rx', 'deauth_tx', 'class3', 'inactive', 'unauthorized', 'tx_data', 'tx_dropped',
           'tx_nokey', 'ps_buffered', 'ps_dropped', 'ps_released', 'pspoll', 'rx_data', 'rx_dropped', 'rx_dup',
           'rx_decrypted', 'rx_dropped_unencrypted', 'rx_dropped_replay', 'key_adds', 'key_dels', 'mgmt_tx',
           'mgmt_dropped', 'inds_dropped'
  if ($b[0]) {
    'GO: running, band {0} GHz, channel {1}, BSSID {2}, beacon {3} TU, {4} clients' -f $band[[int]$b[1]], $b[2], (& $mac 4),
      [BitConverter]::ToUInt16($b, 10), $b[3]
  } else { 'GO: not running' }
  for ($i = 0; $i -lt $b[3]; $i++) {
    $o = 148 + 24 * $i
    $f = $b[$o + 7]
    $flags = @(); if ($f -band 1) { $flags += 'authorized' }; if ($f -band 2) { $flags += 'sleeping' }
    if ($f -band 4) { $flags += 'HT' }; if ($f -band 8) { $flags += 'WMM' }
    '  client {0}: state {1}, AID {2}, {3}, buffered {4}, idle {5} ms, rx {6}, tx {7}' -f (& $mac $o), $b[$o + 6],
      [BitConverter]::ToUInt16($b, $o + 8), ($flags -join ' '), [BitConverter]::ToUInt16($b, $o + 10),
      [BitConverter]::ToUInt32($b, $o + 12), [BitConverter]::ToUInt32($b, $o + 16), [BitConverter]::ToUInt32($b, $o + 20)
  }
  for ($i = 0; $i -lt $names.Count; $i++) { '{0,-24} {1}' -f $names[$i], [BitConverter]::ToUInt32($b, 12 + 4 * $i) }
  if ($b.Count -ge 372) {
    $u = { param($o) [BitConverter]::ToUInt32($b, $o) }
    $port = { param($v) if ($v -eq [uint32]::MaxValue) { 'none' } else { $v } }
    'front end: Wi-Fi Direct {0}, device port {1}, role port {2}, GO running {3}, clients {4}, START_AP tasks {5}, last band {6} GHz channel {7}' -f
      $(if (& $u 340) { 'on' } else { 'off' }), (& $port (& $u 344)), (& $port (& $u 348)), (& $u 352), (& $u 356),
      (& $u 360), $band[[int](& $u 364)], (& $u 368)
  }
}

function Show-BtcFull {
  $out = [RtwDiag]::Ioctl([RtwDiag]::Btc, [byte[]]@(), 4 + 0x4000)
  $ret = [BitConverter]::ToInt32($out, 0)
  if ($ret -lt 0) {
    if ($ret -eq -100) { 'btc report: the chip is powered off (idle); connect, listen or scan first' } else { Format-Result 'btc report' $ret }
    return
  }
  [Text.Encoding]::UTF8.GetString($out, 4, $out.Length - 4)
}

$suite = { param($b, $o) '{0:x2}-{1:x2}-{2:x2}:{3}' -f $b[$o], $b[$o + 1], $b[$o + 2], $b[$o + 3] }
function Format-Elements([byte[]]$b, [int]$o, [int]$end) {
  $ids = @(); $lines = @()
  while ($o + 2 -le $end -and $o + 2 + $b[$o + 1] -le $end) {
    $id = $b[$o]; $l = $b[$o + 1]; $d = $o + 2
    $ids += $id
    switch ($id) {
      5 { if ($l -ge 2) { $lines += 'TIM: DTIM count {0}, period {1}' -f $b[$d], $b[$d + 1] } }
      7 {
        $t = @(); for ($k = $d + 3; $k + 3 -le $d + $l; $k += 3) { $t += '{0}+{1} @{2} dBm' -f $b[$k], $b[$k + 1], $b[$k + 2] }
        $lines += 'Country: {0}{1} (environment 0x{2:x2}) {3}' -f [char]$b[$d], [char]$b[$d + 1], $b[$d + 2], ($t -join ', ')
      }
      48 {
        $p = $d; $e = $d + $l; $s = 'RSN: version {0}' -f [BitConverter]::ToUInt16($b, $p); $p += 2
        if ($p + 4 -le $e) { $s += ', group ' + (& $suite $b $p); $p += 4 }
        foreach ($what in 'pairwise', 'AKM') {
          if ($p + 2 -le $e) {
            $n = [BitConverter]::ToUInt16($b, $p); $p += 2; $l2 = @()
            for ($k = 0; $k -lt $n -and $p + 4 -le $e; $k++) { $l2 += & $suite $b $p; $p += 4 }
            $s += ', {0} {1}' -f $what, ($l2 -join ' ')
          }
        }
        if ($p + 2 -le $e) {
          $c = [BitConverter]::ToUInt16($b, $p); $p += 2
          $s += ', capabilities 0x{0:x4} (MFPR {1}, MFPC {2}, PTKSA replay counters {3})' -f $c, (($c -shr 6) -band 1), (($c -shr 7) -band 1), @(1, 2, 4, 16)[($c -shr 2) -band 3]
        }
        if ($p + 2 -le $e) { $n = [BitConverter]::ToUInt16($b, $p); $p += 2 + 16 * $n; if ($n) { $s += ", $n PMKIDs" } }
        if ($p + 4 -le $e) { $s += ', group management ' + (& $suite $b $p) }
        $lines += $s
      }
      # the elements that decide the channel width (mac\wide.c)
      45 {
        if ($l -ge 26) {
          $c = [BitConverter]::ToUInt16($b, $d)
          $lines += 'HT Capabilities: 0x{0:x4} (40 MHz {1}, SGI20 {2}, SGI40 {3}, LDPC {4}), RX MCS 0x{5:x2} 0x{6:x2}' -f $c,
            (($c -shr 1) -band 1), (($c -shr 5) -band 1), (($c -shr 6) -band 1), ($c -band 1), $b[$d + 3], $b[$d + 4]
        }
      }
      61 {
        if ($l -ge 22) {
          $sec = @('none', 'above', 'reserved', 'below')[$b[$d + 1] -band 3]
          $lines += 'HT Operation: primary channel {0}, secondary {1}, STA channel width {2}, operation mode 0x{3:x4}' -f $b[$d],
            $sec, $(if ($b[$d + 1] -band 4) { 'any' } else { '20 MHz' }), [BitConverter]::ToUInt16($b, $d + 2)
        }
      }
      191 {
        if ($l -ge 12) {
          $c = [BitConverter]::ToUInt32($b, $d)
          $lines += 'VHT Capabilities: 0x{0:x8} (max MPDU {1}, widths {2}, SGI80 {3}, SU beamformer {4}, MU beamformer {5}, beamformee STS {6}, +HTC {7}), RX MCS map 0x{8:x4}, TX MCS map 0x{9:x4}' -f $c,
            @(3895, 7991, 11454, 'reserved')[$c -band 3], @('80', '80/160', '80/160/80+80', 'reserved')[($c -shr 2) -band 3],
            (($c -shr 5) -band 1), (($c -shr 11) -band 1), (($c -shr 19) -band 1), ((($c -shr 13) -band 7) + 1),
            (($c -shr 22) -band 1), [BitConverter]::ToUInt16($b, $d + 4), [BitConverter]::ToUInt16($b, $d + 8)
        }
      }
      192 {
        if ($l -ge 5) {
          $w = @('20/40 MHz (HT)', '80 MHz', '160 MHz (old encoding)', '80+80 MHz (old encoding)')
          $lines += 'VHT Operation: channel width {0}, center segments {1} {2}, basic MCS 0x{3:x4}' -f $(if ($b[$d] -lt 4) { $w[$b[$d]] } else { $b[$d] }),
            $b[$d + 1], $b[$d + 2], [BitConverter]::ToUInt16($b, $d + 3)
        }
      }
      199 { if ($l -ge 1) { $lines += 'Operating Mode Notification: 0x{0:x2} (width {1}, RX streams {2})' -f $b[$d], @(20, 40, 80, 160)[$b[$d] -band 3], ((($b[$d] -shr 4) -band 7) + 1) } }
      255 {
        if ($l -ge 1) {
          switch ($b[$d]) {
            35 {
              if ($l -ge 22) {
                $p0 = $b[$d + 7]
                $lines += 'HE Capabilities: MAC 0x{0}, PHY widths 0x{1:x2} (5 GHz 40/80 {2}, 160 {3}), RX/TX MCS <=80 MHz 0x{4:x4}/0x{5:x4}' -f
                  (($b[($d + 1)..($d + 6)] | ForEach-Object { $_.ToString('x2') }) -join ''), $p0, (($p0 -shr 2) -band 1), (($p0 -shr 3) -band 1),
                  [BitConverter]::ToUInt16($b, $d + 18), [BitConverter]::ToUInt16($b, $d + 20)
              } else { $lines += 'HE Capabilities (short)' }
            }
            36 {
              if ($l -ge 7) {
                $lines += 'HE Operation: parameters 0x{0:x2}{1:x2}{2:x2}, BSS color {3}{4}' -f $b[$d + 3], $b[$d + 2], $b[$d + 1],
                  ($b[$d + 4] -band 0x3f), $(if ($b[$d + 4] -band 0x80) { ' (disabled)' } else { '' })
              } else { $lines += 'HE Operation (short)' }
            }
            38 { $lines += 'MU EDCA Parameter Set' }
          }
        }
      }
      127 { $lines += 'Extended Capabilities: ' + (($b[$d..($d + $l - 1)] | ForEach-Object { $_.ToString('x2') }) -join ' ') }
      221 { if ($l -ge 7 -and $b[$d] -eq 0 -and $b[$d + 1] -eq 0x50 -and $b[$d + 2] -eq 0xf2 -and $b[$d + 3] -eq 2 -and $b[$d + 4] -le 1) { $lines += 'WMM {0}: QoS info 0x{1:x2}' -f $(if ($b[$d + 4]) { 'parameter' } else { 'information' }), $b[$d + 6] } }
      244 { if ($l) { $lines += 'RSNXE: ' + (($b[$d..($d + $l - 1)] | ForEach-Object { $_.ToString('x2') }) -join ' ') + $(if ($b[$d] -band 0x20) { ' (SAE hash-to-element)' } else { '' }) } }
    }
    $o += 2 + $l
  }
  $lines += 'elements: ' + ($ids -join ' ') + ' (255 = extension)'
  return $lines
}

# IOCTL_RTW_BSS_IES: struct rtw_bss_rec (32 bytes) + beacon body + probe response body, padded to 8
function Show-BssIes([string]$filter) {
  $cursor = [uint32]0
  $shown = 0
  do {
    $out = [RtwDiag]::Ioctl([RtwDiag]::BssIes, [BitConverter]::GetBytes($cursor), 0x10000 + 8)
    $next = [BitConverter]::ToUInt32($out, 0)
    $o = 8
    while ($o + 32 -le $out.Length) {
      $bssid = ($out[$o..($o + 5)] | ForEach-Object { $_.ToString('x2') }) -join ':'
      $bl = [BitConverter]::ToUInt16($out, $o + 12); $pl = [BitConverter]::ToUInt16($out, $o + 14)
      $sig = [int]$out[$o + 8]; if ($sig -gt 127) { $sig -= 256 }
      $body = $o + 32
      if (-not $filter -or $bssid -eq $filter.ToLower()) {
        $shown++
        $use = if ($bl) { $body } else { $body + $bl }
        $len = if ($bl) { $bl } else { $pl }
        $ssid = ''
        if ($len -ge 14 -and $out[$use + 12] -eq 0) { $ssid = [Text.Encoding]::UTF8.GetString($out, $use + 14, [Math]::Min([int]$out[$use + 13], 32)) }
        '{0} "{1}" channel {2} ({3} GHz), {4} dBm, beacon {5} bytes, probe response {6} bytes; elements of the {7}:' -f $bssid, $ssid,
          $out[$o + 6], $band[[int]$out[$o + 7]], $sig, $bl, $pl, $(if ($bl) { 'beacon' } else { 'probe response' })
        if ($len -ge 12) { Format-Elements $out ($use + 12) ($use + $len) | ForEach-Object { "  $_" } }
      }
      $o = $body + $bl + $pl
      $o = ($o + 7) -band (-bnot 7)
    }
    if ($next -eq $cursor -and $out.Length -le 8) { break }
    $cursor = $next
  } while ($cursor -lt 128)
  if (-not $shown) { if ($filter) { "no BSS $filter with a kept body" } else { 'no BSS with a kept body' } }
}

switch ($Cmd) {
  'probe' {
    $out = Invoke-Control ([RtwDiag]::Probe) ([byte[]]@()) 4
    Format-Result 'probe' ([BitConverter]::ToInt32($out, 0))
  }
  'remove' {
    Invoke-Control ([RtwDiag]::Remove) ([byte[]]@()) 0 | Out-Null
    'remove: done'
  }
  'start' {
    $out = Invoke-Control ([RtwDiag]::Start) ([byte[]]@()) 4
    Format-Result 'start' ([BitConverter]::ToInt32($out, 0))
  }
  'stop' {
    Invoke-Control ([RtwDiag]::Stop) ([byte[]]@()) 0 | Out-Null
    'stop: done'
  }
  'monitor' {
    if ($Rest.Count -ne 1) { throw 'usage: rtwdiag.ps1 monitor <channel>   (0 = stop listening)' }
    $ch = [uint32]$Rest[0]
    $out = Invoke-Control ([RtwDiag]::Monitor) ([BitConverter]::GetBytes($ch)) 4
    $ret = [BitConverter]::ToInt32($out, 0)
    Format-Result "monitor $ch" $ret
    if ($ret -eq 0 -and $ch -ne 0) {
      # wait for the first beacon (or the time limit), then a little longer to collect more
      $wait = if ($WaitMs) { $WaitMs } else { 5000 }
      $sw = [Diagnostics.Stopwatch]::StartNew()
      $start = Get-LogEnd
      do {
        Start-Sleep -Milliseconds 200
        $hdr, $null = Get-Bss
      } until ($hdr.RxBeacons -gt 0 -or $sw.ElapsedMilliseconds -ge $wait)
      if ($hdr.RxBeacons -gt 0) {
        "first beacon after {0} ms; collecting until {1} ms" -f $sw.ElapsedMilliseconds, $wait
        while ($sw.ElapsedMilliseconds -lt $wait) { Start-Sleep -Milliseconds 200 }
      } else { "no beacon within $wait ms" }
      Write-LogFrom $start
      Show-Bss
    }
  }
  'scan' {
    $flags = 0
    if ($Keep) { $flags = $flags -bor 1 }
    if ($Passive) { $flags = $flags -bor 2 }
    $in = [BitConverter]::GetBytes([uint32]$flags) + [BitConverter]::GetBytes($DwellMs) + [BitConverter]::GetBytes($TimeoutMs)
    $out = Invoke-Control ([RtwDiag]::Scan) ([byte[]]$in) 16
    $ret = [BitConverter]::ToInt32($out, 0)
    '{0}; {1} ms, {2} BSS{3}' -f (Format-Result 'scan' $ret), [BitConverter]::ToUInt32($out, 8),
      [BitConverter]::ToUInt32($out, 12), $(if ([BitConverter]::ToUInt32($out, 4)) { ', aborted' } else { '' })
    Show-Bss
  }
  'bss' { if ($Ies) { Show-BssIes $(if ($Rest) { $Rest[0] } else { '' }) } else { Show-Bss } }
  'radio' { Show-Radio }
  'settings' { Show-Settings }
  'ap' { Show-Ap }
  'country' {
    if ($Rest.Count -gt 1) { throw 'usage: rtwdiag.ps1 country [XX]   (two letters, or 00 for none)' }
    if ($Rest.Count -eq 1) {
      $c = $Rest[0].ToUpperInvariant()
      if ($c -notmatch '^([A-Z]{2}|00)$') { throw "country '$($Rest[0])': two letters (ISO 3166-1 alpha-2), or 00 for none" }
      if (-not [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($svcKey)) { throw 'the rtw89win service is not installed' }
      $params = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("$svcKey\Parameters")
      $old = $params.GetValue('Country')
      $params.SetValue('Country', $c, [Microsoft.Win32.RegistryValueKind]::String)
      $params.Close()
      'Parameters\Country: {0} -> "{1}"; applies at the next probe (WiFiCx: pnputil /restart-device or rtwdiag.ps1 arm; diagnostic: remove, probe). A country wireless-regdb does not know is ignored (driver log).' -f
        $(if ($null -eq $old) { 'not set' } else { '"{0}"' -f $old }), $c
    }
    Show-Settings -CountryOnly
  }
  'btc' { if ($Full) { Show-BtcFull } else { Show-Radio -BtcOnly } }
  'txprobe' {
    $wait = if ($WaitMs) { $WaitMs } else { 500 }
    $in = [BitConverter]::GetBytes($Count) + [BitConverter]::GetBytes($IntervalMs) + [BitConverter]::GetBytes([uint32]$wait)
    $out = Invoke-Control ([RtwDiag]::TxProbe) ([byte[]]$in) 40
    $u = { param($i) [BitConverter]::ToUInt32($out, 4 * $i) }
    Format-Result 'txprobe' ([BitConverter]::ToInt32($out, 0))
    'channel {0}: sent {1}, tx done {2}, failed {3}, no status {4}; probe responses to us {5} from {6} BSS, first after {7} ms; {8} frames left for this driver load' -f
      (& $u 1), (& $u 2), (& $u 3), (& $u 4), (& $u 5), (& $u 6), (& $u 7), (& $u 8), (& $u 9)
    Show-Bss
  }
  'ser' {
    $out = Invoke-Control ([RtwDiag]::Ser) ([byte[]]@()) 4
    Format-Result 'ser' ([BitConverter]::ToInt32($out, 0))
  }
  'link' {
    $out = [RtwDiag]::Ioctl([RtwDiag]::Link, [byte[]]@(), 288)
    $u = { param($o) [BitConverter]::ToUInt32($out, $o) }
    # an older driver returns less: from 256 bytes on, the management frame protection and SAE fields; from 288 bytes
    # on, the width, the last received rate and the A-MSDU and +HTC counters
    $hasMfp = $out.Length -ge 256
    $hasWidth = $out.Length -ge 288
    $states = @('idle', 'preparing', 'authenticating', 'associating', 'connected', 'disconnecting')
    $st = & $u 0
    $rssi = [int]$out[14]; if ($rssi -gt 127) { $rssi -= 256 }
    'state: {0}' -f $(if ($st -lt $states.Count) { $states[$st] } else { $st })
    if ($st -ne 0) {
      'BSS {0}, band {1}, channel {2}, AID {3}, {4} dBm{5}{6}' -f (($out[4..9] | ForEach-Object { $_.ToString('x2') }) -join ':'),
        $band[[int]$out[10]], $out[11], [BitConverter]::ToUInt16($out, 12), $rssi,
        $(if ($out[15] -band 1) { ', WMM' } else { '' }), $(if ($out[15] -band 2) { ', HT' } else { '' })
      if ($hasWidth) {
        $phys = @('legacy', 'HT', 'VHT', 'HE')
        $p = if ($out[256] -lt 4) { $phys[$out[256]] } else { $out[256] }
        '{0}: {1}{2} (center channel {3}); TX rate {4}' -f $(if ($st -eq 4) { 'in use' } else { 'asked for' }), $p,
          $(if ($out[256]) { $out[257] } else { '' }), $out[258],
          $(if (& $u 284) { '{0:0.0} Mbit/s (rtw89)' -f ((& $u 284) / 1000) } else { 'not reported yet' })
        if ($out[261]) {
          'last data frame from the AP: {0}, {1} MHz, MCS {2}, {3} stream(s)' -f $(if ($out[260] -lt 4) { $phys[$out[260]] } else { $out[260] }),
            $out[261], $out[262], $out[263]
        }
      }
    }
    if ($st -ne 0) {
      $keys = $out[153]
      $gk = @(0..3 | Where-Object { $keys -band (2 -shl $_) })
      $ik = @(4..5 | Where-Object { $keys -band (0x20 -shl ($_ - 4)) })
      $sec = switch ($out[152]) { 1 { 'WPA2-PSK/CCMP' } 2 { 'WPA3-SAE/CCMP' } default { 'open' } }
      'security: {0}{1}; keys: pairwise {2}, group {3}{4}' -f $sec, $(if ($hasMfp -and $out[192]) { ', MFP (BIP-CMAC-128)' } else { '' }),
        $(if ($keys -band 1) { 'yes' } else { 'no' }), $(if ($gk) { $gk -join ',' } else { 'none' }),
        $(if ($ik) { ', integrity group ' + ($ik -join ',') } else { '' })
    }
    'privacy exemption entries: {0}' -f $out[154]
    $names = 'tx_data', 'tx_dropped_disconnected', 'tx_dropped_bad', 'tx_busy', 'tx_inflight', 'tx_failed', 'tx_nomem',
             'rx_data', 'rx_indicated', 'rx_dropped_notours', 'rx_dropped_protected', 'rx_dropped_dup', 'rx_dropped_frag',
             'rx_dropped_full', 'rx_dropped_toolarge', 'rx_dropped_other', 'rx_mlme', 'mgmt_tx', 'mgmt_acked',
             'mgmt_failed', 'queues_stopped', 'connects', 'disconnects', 'beacon_losses',
             'tx_protected', 'tx_clear', 'tx_dropped_nokey', 'rx_decrypted', 'rx_exempt', 'rx_dropped_unencrypted',
             'rx_dropped_nokey', 'rx_dropped_replay', 'key_adds', 'key_dels'
    for ($i = 0; $i -lt $names.Count; $i++) { '{0,-24} {1}' -f $names[$i], (& $u (16 + 4 * $i)) }
    '{0,-24} {1}' -f 'restarts', (& $u 156)
    $ba = 'rx_reordered', 'rx_reorder_timeouts', 'rx_dropped_old', 'rx_bar', 'ba_rx_sessions', 'ba_tx_sessions', 'tx_ampdu'
    for ($i = 0; $i -lt $ba.Count; $i++) { '{0,-24} {1}' -f $ba[$i], (& $u (160 + 4 * $i)) }
    'ba sessions now:        rx TIDs 0x{0:x2}, tx TIDs 0x{1:x2}' -f $out[188], $out[189]
    if ($hasMfp) {
      $pmf = @('off', 'on, without SAE', 'on, with SAE')
      $crypto = @('not run yet', 'passed', 'FAILED')
      $sae = @('idle', 'waiting for commit parameters', 'committed', 'waiting for confirm parameters', 'confirmed',
               'waiting for the result')
      'management frame protection: {0} (Parameters\Pmf and the self-tests); crypto self-test {1}{2}' -f
        $(if ($out[193] -lt 3) { $pmf[$out[193]] } else { $out[193] }), $(if ($out[194] -lt 3) { $crypto[$out[194]] } else { $out[194] }),
        $(if ($st -ne 0 -and $out[195]) { '; SAE ' + $(if ($out[195] -lt 6) { $sae[$out[195]] } else { $out[195] }) } else { '' })
      $pm = 'mgmt_protected_tx', 'mgmt_protected_rx', 'mgmt_bip_rx', 'mgmt_dropped_unprot', 'mgmt_dropped_replay',
            'mgmt_dropped_mic', 'mgmt_dropped_nokey', 'sa_query_answered', 'sa_query_started', 'sa_query_timeouts',
            'assoc_comebacks', 'sae_tx', 'sae_rx', 'sae_ind', 'sae_params'
      for ($i = 0; $i -lt $pm.Count; $i++) { '{0,-24} {1}' -f $pm[$i], (& $u (196 + 4 * $i)) }
    }
    if ($hasWidth) {
      'Parameters\Bandwidth in effect: 5 GHz up to {0} MHz' -f $out[259]
      $wn = 'rx_amsdu', 'rx_dropped_amsdu', 'rx_amsdu_skipped', 'rx_htc', 'rx_mgmt_htc'
      for ($i = 0; $i -lt $wn.Count; $i++) { '{0,-24} {1}' -f $wn[$i], (& $u (264 + 4 * $i)) }
    }
  }
  'mode' { Show-Mode }
  'arm' {
    if (-not [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($svcKey)) { throw 'the rtw89win service is not installed' }
    # clear the crash loop guard; a running WiFiCx session clears it at its release anyway, and the restarted
    # device sets ArmRunning again before it touches the chip
    $params = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("$svcKey\Parameters")
    'guard was: ArmRunning {0}, BootAttempts {1}; both set to 0' -f $params.GetValue('ArmRunning'), $params.GetValue('BootAttempts')
    $params.SetValue('BootAttempts', 0, [Microsoft.Win32.RegistryValueKind]::DWord)
    $params.SetValue('ArmRunning', 0, [Microsoft.Win32.RegistryValueKind]::DWord)
    $params.Flush()
    $params.Close()

    $dev = Get-RtwDevice
    # Where the log ends now, and the 64 bytes before that: if the driver stays loaded over the restart (a handle
    # open on \\.\rtw89win keeps it), the log goes on and still holds the lines of the earlier start, so only what
    # follows $p0 counts; if those bytes changed, the driver was reloaded and its log starts afresh.
    $p0 = $null; $tail = $null
    try { $p0 = Get-LogEnd } catch { }
    if ($p0 -ge 64) { $tail = Get-LogChunk ($p0 - 64) 64 }
    "restarting $($dev.InstanceId)"
    $text = pnputil /restart-device $dev.InstanceId 2>&1 | Out-String
    $text.Trim()
    if ($LASTEXITCODE -eq 3010 -or $text -match 'reboot') {
      'pnputil wants a reboot: not rebooting. The device stays as it is; the guard is cleared for its next start.'
      break
    }
    # up in WiFiCx mode with its adapter, or a definite failure, or the time limit
    $wait = if ($WaitMs) { $WaitMs } else { 60000 }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $result = 'time limit'
    do {
      Start-Sleep -Milliseconds 500
      $dev = Get-RtwDevice
      $log = Get-LogText
      if ($log -and $tail -and (Get-LogChunk ($p0 - 64) 64) -ceq $tail) { $log = Get-LogText $p0 }
      if ($log -match 'mode: diagnostic') { $result = 'started in diagnostic mode'; break }
      if ($log -match 'WiFiCx bring-up failed|WifiDeviceInitialize failed|WiFiCx: setup failed|the start fails') { $result = 'WiFiCx start failed'; break }
      if ($log -match 'WiFiCx: adapter started') { $result = 'WiFiCx adapter started'; break }
      if ($dev.Status -eq 'Error' -and $sw.ElapsedMilliseconds -gt 5000) { $result = 'device error'; break }
    } while ($sw.ElapsedMilliseconds -lt $wait)
    '{0} after {1} ms; device {2} ({3})' -f $result, $sw.ElapsedMilliseconds, $dev.Status, $dev.Class
    if ($log) { $log } else { 'no log (no control device); the saved log of a failed start:'; Show-SavedLog }
    Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.PnPDeviceID -eq $dev.InstanceId } |
      Format-List Name, InterfaceDescription, Status, MacAddress, MediaType, PhysicalMediaType | Out-String -Width 200
    Get-Service Wlansvc | Format-Table Name, Status, StartType -AutoSize | Out-String
  }
  'log' {
    if ($Saved) { Show-SavedLog; break }
    [uint64]$from = 0
    while ($true) {
      $out = [RtwDiag]::Ioctl([RtwDiag]::LogRead, [BitConverter]::GetBytes($from), 0x10000 + 16)
      $from = [BitConverter]::ToUInt64($out, 0)
      $dropped = [BitConverter]::ToUInt64($out, 8)
      if ($dropped) { "... ($dropped bytes of older log overwritten)" }
      if ($out.Length -gt 16) { [Console]::Write([Text.Encoding]::UTF8.GetString($out, 16, $out.Length - 16)); [Console]::Out.Flush() }
      elseif (-not $Follow) { break }
      else { Start-Sleep -Milliseconds 300 }
    }
  }
  'read' {
    foreach ($o in $Rest) {
      $off = [uint32]$o   # accepts 0x.. as well
      $v = [BitConverter]::ToUInt32([RtwDiag]::Ioctl([RtwDiag]::Read32, [BitConverter]::GetBytes($off), 4), 0)
      '0x{0:x4} = 0x{1:x8}' -f $off, $v
    }
  }
}
