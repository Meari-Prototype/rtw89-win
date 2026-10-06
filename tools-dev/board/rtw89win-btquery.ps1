# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Read-only survey of the Bluetooth half of the RTL8852BE module (-UsbId, default VID_13D3&PID_3572, the module of
# the ROCK 5T): devices, driver package, device properties, local radio info (IOCTL_BTH_GET_LOCAL_INFO), filters,
# services, Bluetooth events and whether any Realtek Bluetooth package is present. Changes nothing; never reads
# BTHPORT\Parameters\Keys values. The output has the local radio's address: mask it before you share it.
param([int]$MaxSystemEvents = 40000, [string]$UsbId = 'VID_13D3&PID_3572')
$vid = $UsbId -replace '^VID_([0-9A-Fa-f]{4}).*$', '$1'   # the USB vendor ID alone (13D3)

$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
# Not "H": that is the built-in alias of Get-History and wins over a function.
function Section($t) { "`n===== $t =====" }
function Short($v) {
    if ($null -eq $v) { return '' }
    if ($v -is [byte[]]) { $s = ($v | ForEach-Object { $_.ToString('x2') }) -join '' }
    elseif ($v -is [array]) { $s = ($v | ForEach-Object { "$_" }) -join '; ' }
    else { $s = "$v" }
    if ($s.Length -gt 300) { $s = $s.Substring(0, 300) + '...' }
    return $s
}

Section 'host'
$os = Get-CimInstance Win32_OperatingSystem
"$($os.Caption) $($os.Version) build $($os.BuildNumber), boot $($os.LastBootUpTime)"

Section "devices ($UsbId and Bluetooth class, including non-present)"
$all = Get-PnpDevice
$bt = $all | Where-Object { $_.InstanceId -match [regex]::Escape($UsbId) }
$bt | Format-Table -AutoSize Status, Present, Class, FriendlyName, InstanceId | Out-String -Width 250
$all | Where-Object { $_.Class -eq 'Bluetooth' -or $_.InstanceId -like 'BTH*' } |
    Sort-Object InstanceId | Format-Table -AutoSize Status, Present, Class, FriendlyName, InstanceId | Out-String -Width 250

Section 'driver packages (Win32_PnPSignedDriver)'
Get-CimInstance Win32_PnPSignedDriver |
    Where-Object { $_.DeviceID -match [regex]::Escape($UsbId) -or $_.DeviceClass -eq 'BLUETOOTH' -or $_.DeviceID -like 'BTH*' } |
    Select-Object DeviceName, DeviceID, InfName, DriverVersion, DriverDate, DriverProviderName, Manufacturer, IsSigned |
    Format-List | Out-String -Width 250

Section 'device properties'
foreach ($d in $bt) {
    "--- $($d.InstanceId)"
    Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue |
        Where-Object { $null -ne $_.Data } |
        ForEach-Object { '{0} [{1}] = {2}' -f $_.KeyName, $_.Type, (Short $_.Data) }
}

Section 'parent chain'
foreach ($d in ($bt | Where-Object Present)) {
    $id = $d.InstanceId
    for ($i = 0; $i -lt 6 -and $id; $i++) {
        $p = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue).Data
        if (-not $p) { break }
        $pd = $all | Where-Object InstanceId -eq $p
        "  <- $p ($($pd.FriendlyName), $($pd.Status))"
        $id = $p
    }
}

Section 'registry: device keys (values only, no Keys subkey)'
foreach ($d in $bt) {
    foreach ($sub in @('', '\Device Parameters', '\Device Parameters\Ceip', '\Device Parameters\e5b3b5ac-9725-4f78-963f-03dfb1d828c7')) {
        $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)$sub"
        if (Test-Path -LiteralPath $k) {
            "--- $k"
            $item = Get-Item -LiteralPath $k
            foreach ($n in $item.GetValueNames()) { '  {0} = {1}' -f $n, (Short $item.GetValue($n)) }
            '  subkeys: ' + (($item.GetSubKeyNames()) -join ', ')
        }
    }
}
$cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}'
"--- Bluetooth class key filters"
$ci = Get-Item -LiteralPath $cls -ErrorAction SilentlyContinue
if ($ci) { foreach ($n in @('UpperFilters', 'LowerFilters')) { '  {0} = {1}' -f $n, (Short $ci.GetValue($n)) } }
foreach ($s in @('BTHUSB', 'BTHPORT', 'BthEnum', 'BthLEEnum', 'BthMini', 'BthA2dp', 'BthHFEnum', 'bthserv', 'BTAGService', 'BthAvctpSvc', 'RtkBtFilter', 'RtkBtManServ', 'rtkfilter')) {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Services\$s"
    if (-not (Test-Path -LiteralPath $k)) { "--- service $s : not present"; continue }
    $item = Get-Item -LiteralPath $k
    "--- service $s : Start=$($item.GetValue('Start')) Type=$($item.GetValue('Type')) ImagePath=$($item.GetValue('ImagePath'))"
    $pk = "$k\Parameters"
    if (Test-Path -LiteralPath $pk) {
        $pi = Get-Item -LiteralPath $pk
        foreach ($n in $pi.GetValueNames()) { '  Parameters\{0} = {1}' -f $n, (Short $pi.GetValue($n)) }
        foreach ($sk in $pi.GetSubKeyNames()) {
            # Keys holds link keys (SYSTEM only): not opened at all.
            if ($sk -eq 'Keys') { '  Parameters\Keys\ (not read)'; continue }
            $si = Get-Item -LiteralPath "$pk\$sk" -ErrorAction SilentlyContinue
            '  Parameters\{0}\ ({1} subkeys, {2} values)' -f $sk, $si.SubKeyCount, $si.ValueCount
            if ($sk -in @('ExceptionDB', 'HciBypassServices', 'PnpId', 'SupportedServices')) {
                foreach ($n in $si.GetValueNames()) { '    {0} = {1}' -f $n, (Short $si.GetValue($n)) }
                foreach ($c in (Get-ChildItem -LiteralPath "$pk\$sk" -ErrorAction SilentlyContinue)) {
                    '    {0}\' -f $c.PSChildName
                    foreach ($n in $c.GetValueNames()) { '      {0} = {1}' -f $n, (Short $c.GetValue($n)) }
                }
            }
        }
    }
}

Section 'services'
Get-Service -Name bthserv, BTAGService, BthAvctpSvc, 'BluetoothUserService*', DeviceAssociationService -ErrorAction SilentlyContinue |
    Format-Table -AutoSize Name, Status, StartType | Out-String -Width 200

Section 'local radio (BluetoothFindFirstRadio + IOCTL_BTH_GET_LOCAL_INFO)'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BtQ {
    [StructLayout(LayoutKind.Sequential)]
    public struct FIND_RADIO_PARAMS { public uint dwSize; }
    [DllImport("bthprops.cpl", SetLastError = true)]
    public static extern IntPtr BluetoothFindFirstRadio(ref FIND_RADIO_PARAMS p, out IntPtr hRadio);
    [DllImport("bthprops.cpl", SetLastError = true)]
    public static extern bool BluetoothFindRadioClose(IntPtr hFind);
    [DllImport("bthprops.cpl")]
    public static extern bool BluetoothIsConnectable(IntPtr hRadio);
    [DllImport("bthprops.cpl")]
    public static extern bool BluetoothIsDiscoverable(IntPtr hRadio);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(IntPtr h, uint code, IntPtr inBuf, int inLen, byte[] outBuf, int outLen, out int ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);
}
'@
$p = New-Object BtQ+FIND_RADIO_PARAMS
$p.dwSize = 4
$h = [IntPtr]::Zero
$f = [BtQ]::BluetoothFindFirstRadio([ref]$p, [ref]$h)
if ($f -eq [IntPtr]::Zero) {
    "BluetoothFindFirstRadio: no radio (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
} else {
    "connectable=$([BtQ]::BluetoothIsConnectable($h)) discoverable=$([BtQ]::BluetoothIsDiscoverable($h))"
    $buf = New-Object byte[] 512
    $ret = 0
    $ok = [BtQ]::DeviceIoControl($h, 0x00410000, [IntPtr]::Zero, 0, $buf, $buf.Length, [ref]$ret, [IntPtr]::Zero)
    if (-not $ok) {
        "IOCTL_BTH_GET_LOCAL_INFO failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    } else {
        # BTH_LOCAL_RADIO_INFO (pack 1): BTH_DEVICE_INFO { flags u32, BTH_ADDR @8, classOfDevice @16, name[248] @20 }
        # (natural alignment) | flags u32 | hciRevision u16 | hciVersion u8 |
        # BTH_RADIO_INFO { lmpSupportedFeatures u64, mfg u16, lmpSubversion u16, lmpVersion u8 } = 20 bytes after it
        $b = $ret - 20
        $name = [Text.Encoding]::UTF8.GetString($buf, 20, 248).TrimEnd([char]0)
        $addr = [BitConverter]::ToUInt64($buf, 8)
        'bytes={0} name="{1}" address={2:x12} cod=0x{3:x6} deviceFlags=0x{4:x}' -f $ret, $name, $addr, [BitConverter]::ToUInt32($buf, 16), [BitConverter]::ToUInt32($buf, 0)
        'radioFlags=0x{0:x} hciRevision=0x{1:x4} hciVersion=0x{2:x2} lmpFeatures=0x{3:x16} mfg=0x{4:x4} lmpSubversion=0x{5:x4} lmpVersion=0x{6:x2}' -f `
            [BitConverter]::ToUInt32($buf, $b), [BitConverter]::ToUInt16($buf, $b + 4), $buf[$b + 6],
            [BitConverter]::ToUInt64($buf, $b + 7), [BitConverter]::ToUInt16($buf, $b + 15), [BitConverter]::ToUInt16($buf, $b + 17), $buf[$b + 19]
    }
    [void][BtQ]::CloseHandle($h)
    [void][BtQ]::BluetoothFindRadioClose($f)
}

Section 'Bluetooth providers in the System log (grouped)'
$ev = Get-WinEvent -LogName System -MaxEvents $MaxSystemEvents -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'bth|bluetooth' -or ($_.ProviderName -match 'Kernel-PnP|UserPnp|DriverFrameworks' -and $_.Message -match $vid) }
$ev | Group-Object ProviderName, Id | Sort-Object Count -Descending | ForEach-Object {
    $g = $_.Group | Sort-Object TimeCreated
    $m = ($g[-1].Message -replace '\s+', ' ')
    if ($m.Length -gt 220) { $m = $m.Substring(0, 220) + '...' }
    '{0,5} x {1} | {2:MM-dd HH:mm:ss} .. {3:MM-dd HH:mm:ss} | {4}' -f $_.Count, $_.Name, $g[0].TimeCreated, $g[-1].TimeCreated, $m
}
Section 'Bluetooth System events since the last boot (newest 40)'
$ev | Where-Object { $_.TimeCreated -ge $os.LastBootUpTime } | Select-Object -First 40 | ForEach-Object {
    $m = ($_.Message -replace '\s+', ' ')
    if ($m.Length -gt 220) { $m = $m.Substring(0, 220) + '...' }
    $pr = ($_.Properties | ForEach-Object { Short $_.Value }) -join ' | '
    '{0:MM-dd HH:mm:ss} {1}/{2} {3} | {4} | props: {5}' -f $_.TimeCreated, $_.ProviderName, $_.Id, $_.LevelDisplayName, $m, $pr
}

Section 'Bluetooth event logs'
Get-WinEvent -ListLog '*Bluetooth*', '*Bth*' -ErrorAction SilentlyContinue |
    Format-Table -AutoSize LogName, IsEnabled, RecordCount | Out-String -Width 200
foreach ($l in (Get-WinEvent -ListLog '*Bluetooth*', '*Bth*' -ErrorAction SilentlyContinue | Where-Object { $_.RecordCount -gt 0 })) {
    "--- $($l.LogName) (newest 15)"
    Get-WinEvent -LogName $l.LogName -MaxEvents 15 -ErrorAction SilentlyContinue | ForEach-Object {
        $m = ($_.Message -replace '\s+', ' ')
        if ($m.Length -gt 220) { $m = $m.Substring(0, 220) + '...' }
        '{0:MM-dd HH:mm:ss} {1} {2} | {3}' -f $_.TimeCreated, $_.Id, $_.LevelDisplayName, $m
    }
}

Section "setupapi.dev.log sections for $UsbId"
$sl = 'C:\Windows\INF\setupapi.dev.log'
if (Test-Path $sl) {
    Select-String -Path $sl -Pattern $UsbId -SimpleMatch -Context 0, 0 | Select-Object -First 20 | ForEach-Object { "{0}: {1}" -f $_.LineNumber, $_.Line.Trim() }
    Select-String -Path $sl -Pattern 'Selected Driver|Driver Version|Driver Rank|Matching Device Id|Outranked' -Context 0, 0 |
        Where-Object { $_.Line -match "bth\.inf|$vid|Class_E0" } | Select-Object -First 20 | ForEach-Object { "{0}: {1}" -f $_.LineNumber, $_.Line.Trim() }
}

Section 'Realtek / vendor Bluetooth packages present?'
pnputil /enum-drivers /class Bluetooth 2>&1 | Out-String -Width 200
"inf files naming $UsbId or Realtek Bluetooth:"
Get-ChildItem C:\Windows\INF\*.inf -ErrorAction SilentlyContinue |
    Select-String -Pattern $UsbId, 'RtkBt', 'Realtek Bluetooth', 'rtl8852b' -SimpleMatch -List |
    ForEach-Object { "  $($_.Path): $($_.Line.Trim())" }
"bth.inf mentions of Realtek/0BDA/13D3/firmware/patch:"
Select-String -Path C:\Windows\INF\bth.inf -Pattern 'realtek', '0BDA', '13D3', 'firmware', 'patch' -SimpleMatch |
    Select-Object -First 20 | ForEach-Object { "  {0}: {1}" -f $_.LineNumber, $_.Line.Trim() }
"driver files named rtk*/Rtk*/rtl*bt* in System32\drivers and DriverStore:"
Get-ChildItem C:\Windows\System32\drivers -Filter 'rtk*' -ErrorAction SilentlyContinue | ForEach-Object { "  $($_.FullName)" }
Get-ChildItem C:\Windows\System32\DriverStore\FileRepository -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(rtk|rtl|realtek)|bt' } | ForEach-Object { "  $($_.Name)" }
"bthusb.sys / bthport.sys versions:"
foreach ($n in 'bthusb.sys', 'bthport.sys', 'bthmini.sys') {
    $fp = "C:\Windows\System32\drivers\$n"
    if (Test-Path $fp) { '  {0} {1}' -f $n, (Get-Item $fp).VersionInfo.FileVersion }
}
