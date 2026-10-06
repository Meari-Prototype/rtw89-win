# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run on the board's Windows as administrator (e.g. over SSH) from the package directory (a copy of a build.ps1
# output directory anywhere on the board):
#   pwsh -NoProfile -File install-rtw89win.ps1 [-Country XX] [-WorkDir <dir>]
# 1. checks SHA256SUMS, 2. saves the SYSTEM hive to <WorkDir>\backup\rtw89win-<time>\ (WorkDir: default
#    %ProgramData%\rtw89win; the transcript goes to <WorkDir>\logs\rtw89win-install.log),
# 3. -Country XX (two letters, or 00 for none): sets Parameters\Country (REG_SZ, persistent), the regulatory domain.
#    Without -Country a value already there is left as it is, and none is written: the driver then uses the Country
#    element of the connected BSS, else the world rules. rtwdiag.ps1 country shows or changes it. The driver reads it
#    at every probe. Done before the install when the service exists (the new package's first start then uses it);
#    on a first install after it, so it applies from the device's next start.
# 4. installs this package (its test certificate rtw89win-test.cer, if the package has one, is trusted only
#    meanwhile; a package built with -NoSign must be signed by a certificate the board trusts already):
#    - in-place upgrade (the device already runs rtw89win with the INF's class): the new package is added and
#      installed on the device as a driver update (pnputil /add-driver /install; forced with
#      UpdateDriverForPlugAndPlayDevices when it is not newer, e.g. the same version again or going back), the
#      device is restarted (pnputil /restart-device) if the new driver did not load, and only then are the older
#      rtw89win packages deleted from the driver store, without /uninstall. The device node and so the network
#      adapter's NetCfgInstanceId (the WLAN interface GUID the WLAN profiles belong to) stay as they are.
#    - first install, or the device runs a package of another setup class: the older rtw89win packages are
#      removed from the device and deleted first, the package is installed, and if the device's class does not match
#      the INF's or it did not start: its device node is removed and PnP scans for hardware, waiting (at most 60 s)
#      for it to come back with the right class (a new device node: a new WLAN interface GUID).
# 5. shows the device state, the WLAN adapter's NetCfgInstanceId / InterfaceGuid (before and after), the mode and the
#    crash loop guard (rtwdiag.ps1 mode) and the driver log. The device starts in WiFiCx mode by itself;
#    the script leaves the guard's values (ArmRunning, BootAttempts) and the optional connection gate (AllowConnect,
#    AllowSsid) as they are, and never touches WLAN profiles.
# If Windows then fails to boot: from another system on the same board (e.g. Linux), run rollback-rtw89win.sh of this
# package (it renames rtw89win.sys in the driver store), or restore the SYSTEM hive saved in step 2.
param([ValidatePattern('^([A-Za-z]{2}|00)$')][string]$Country, [string]$WorkDir = (Join-Path $env:ProgramData 'rtw89win'))
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$hwid = 'PCI\VEN_10EC&DEV_B852'
New-Item -ItemType Directory -Force (Join-Path $WorkDir 'logs') | Out-Null
Start-Transcript -Path (Join-Path $WorkDir 'logs\rtw89win-install.log') -Append | Out-Null

function Get-RtwDevice { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like "$hwid*" }
function Get-RtwDevProp([string]$id, [string]$key) {
  (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction SilentlyContinue).Data
}
# The network adapter's NetCfgInstanceId, from the device's driver (software) key; $null without one.
function Get-RtwNetCfgId([string]$id) {
  $drv = Get-RtwDevProp $id 'DEVPKEY_Device_Driver'
  if (-not $drv) { return $null }
  (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv" -Name NetCfgInstanceId -ErrorAction SilentlyContinue).NetCfgInstanceId
}
function Get-RtwPackages { Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\rtw89win.inf' } }
# The version in the last "rtw89win <version> loaded" line of the driver log: the image that runs now.
function Get-RtwLoadedVersion {
  # in a child process: rtwdiag.ps1 log writes the log with [Console]::Write, which bypasses the pipeline
  try { $text = (& (Get-Process -Id $PID).Path -NoProfile -File "$dir\rtwdiag.ps1" log 2>&1 | Out-String) } catch { return $null }
  $m = [regex]::Matches($text, 'rtw89win (\S+) loaded')
  if ($m.Count) { $m[$m.Count - 1].Groups[1].Value } else { $null }
}
# Waits (at most $ms) until the device is present with status OK; returns it (or what is there at the end).
function Wait-RtwDeviceOk([int]$ms = 60000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  do {
    $d = Get-RtwDevice
    if ($d -and $d.Status -eq 'OK') { break }
    Start-Sleep -Milliseconds 500
  } until ($sw.ElapsedMilliseconds -ge $ms)
  $d
}

"=== verify package"
foreach ($line in Get-Content "$dir\SHA256SUMS") {
  $want, $name = $line -split '\s+', 2
  if ((Get-FileHash "$dir\$name").Hash.ToLower() -ne $want) { throw "$name hash mismatch" }
}
"ok"
$infClass = (Select-String -Path "$dir\rtw89win.inf" -Pattern '^\s*Class\s*=\s*(\S+)').Matches[0].Groups[1].Value
$infVersion = (Select-String -Path "$dir\rtw89win.inf" -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
$infHash = (Get-FileHash "$dir\rtw89win.inf").Hash
$dev = Get-RtwDevice
if (-not $dev) { throw "RTL8852BE ($hwid) not present" }
$oldService = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_Service'
$oldInf = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath'
$oldNetCfg = Get-RtwNetCfgId $dev.InstanceId
"device $($dev.InstanceId) status $($dev.Status), class $($dev.Class), service $oldService, INF $oldInf"
"NetCfgInstanceId before: $oldNetCfg"
# in place: the device runs some rtw89win package of the same class already (otherwise: first install / class change)
$inPlace = ($oldService -eq 'rtw89win') -and ($dev.Class -eq $infClass)
"package $infVersion, class ${infClass}: " + $(if ($inPlace) { 'in-place upgrade (device node and adapter GUID kept)' } else { 'first install or class change' })

"=== save SYSTEM hive"
$bk = Join-Path $WorkDir ('backup\rtw89win-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $bk | Out-Null
reg.exe save HKLM\SYSTEM "$bk\SYSTEM.hiv" /y | Out-Null
if ($LASTEXITCODE) { throw "reg save failed ($LASTEXITCODE)" }
"$bk\SYSTEM.hiv"

# Parameters\Country = -Country when given, else the value there is left as it is (or none written); returns
# $false without the service
function Set-RtwCountry {
  $svc = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\rtw89win', $true)
  if (-not $svc) { return $false }
  $params = $svc.CreateSubKey('Parameters')
  $svc.Close()
  $old = $params.GetValue('Country')
  if ($Country) {
    $params.SetValue('Country', $Country.ToUpper(), [Microsoft.Win32.RegistryValueKind]::String)
    $was = if ($null -eq $old) { 'not set' } else { "'$old'" }
    "Parameters\Country set to '$($Country.ToUpper())' (was: $was)" | Out-Host
  } elseif ($null -eq $old) {
    'Parameters\Country is not set: the Country element of the connected BSS applies, else the world rules' +
      ' (rtwdiag.ps1 country XX, or -Country XX here, sets one)' | Out-Host
  } else {
    "Parameters\Country is `"$old`" ($($params.GetValueKind('Country'))); left as it is" | Out-Host
  }
  $params.Close()
  return $true
}

"=== country"
# before the install, so the new package's first start (it probes the chip at once) uses it
$countryDone = Set-RtwCountry
if (-not $countryDone) { 'no rtw89win service yet (first install): done after the install' }

if (-not $inPlace) {
  # First install or a class change: the old package must not stay on the device (its class would stick).
  "=== remove older rtw89win packages"
  foreach ($d in Get-RtwPackages) {
    "$($d.Driver) $($d.Version)"
    pnputil /delete-driver $d.Driver /uninstall /force | Out-Null
    if ($LASTEXITCODE) { throw "pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE)" }
  }
}

"=== install"
# the package's test certificate (build.ps1 -CertThumbprint), trusted only while installing; none with -NoSign
$cert = $null
if (Test-Path "$dir\rtw89win-test.cer") {
  $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new("$dir\rtw89win-test.cer")
} else {
  'no rtw89win-test.cer in the package: the package must be signed by a certificate this Windows trusts already'
}
$stores = 'Root', 'TrustedPublisher'
$newPkg = $null
if ($cert) {
  foreach ($s in $stores) { $st = [Security.Cryptography.X509Certificates.X509Store]::new($s, 'LocalMachine'); $st.Open('ReadWrite'); $st.Add($cert); $st.Close() }
}
try {
  # adds the package to the driver store and installs it on the device when it ranks better (a newer DriverVer):
  # for a device that runs rtw89win already that is a driver update of the same device node
  pnputil /add-driver "$dir\rtw89win.inf" /install
  "pnputil exit $LASTEXITCODE"
  if ($inPlace) {
    # the new package in the store: the one whose INF is this INF (the store keeps it byte for byte)
    $newPkg = Get-RtwPackages | Where-Object { (Get-FileHash $_.OriginalFileName).Hash -eq $infHash } | Select-Object -First 1
    if (-not $newPkg) { throw 'the new package is not in the driver store' }
    "new package $($newPkg.Driver) ($($newPkg.Version))"
    $cur = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath'
    if ($cur -ne $newPkg.Driver) {
      # not newer than the installed one (the same version again, or going back): pnputil leaves the device alone,
      # so update it by force - still a driver update of the same device node (devcon update does the same)
      "the device still uses $cur; updating it to $($newPkg.Driver) by force"
      Add-Type -Namespace Rtw -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string hardwareId, string fullInfPath,
    uint installFlags, out bool rebootRequired);
'@
      $reboot = $false
      # INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE
      if (-not [Rtw.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $hwid, "$dir\rtw89win.inf", 0x5, [ref]$reboot)) {
        throw "UpdateDriverForPlugAndPlayDevices failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
      }
      "updated by force (reboot asked: $reboot)"
    }
  }
} finally {
  if ($cert) {
    foreach ($s in $stores) { $st = [Security.Cryptography.X509Certificates.X509Store]::new($s, 'LocalMachine'); $st.Open('ReadWrite'); $st.Remove($cert); $st.Close() }
    "test certificate removed again: " + (($stores | ForEach-Object { "$_=" + (-not (Get-ChildItem "Cert:\LocalMachine\$_\$($cert.Thumbprint)" -EA 0)) }) -join ' ')
  }
}

if ($inPlace) {
  "=== in-place upgrade: check the device"
  $dev = Wait-RtwDeviceOk
  if (-not $dev) { Stop-Transcript | Out-Null; throw 'the device is not present after the update' }
  $cur = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath'
  if ($cur -ne $newPkg.Driver) {
    # nothing was deleted yet: the device still runs its old package
    Stop-Transcript | Out-Null
    throw "the device uses $cur, not the new $($newPkg.Driver); old packages kept"
  }
  $loaded = if ($dev.Status -eq 'OK') { Get-RtwLoadedVersion } else { $null }
  "device on $cur, status $($dev.Status), driver running: $loaded (package $infVersion)"
  if ($dev.Status -ne 'OK' -or ($loaded -and $loaded -ne $infVersion)) {
    # the old image is still loaded, or the device did not start with the new one: restart the device (not a
    # re-enumeration: the device node stays)
    "restarting the device"
    pnputil /restart-device $dev.InstanceId
    "pnputil exit $LASTEXITCODE"
    $dev = Wait-RtwDeviceOk
    $loaded = if ($dev -and $dev.Status -eq 'OK') { Get-RtwLoadedVersion } else { $null }
    "after the restart: status $($dev.Status), driver running: $loaded"
  }
  if (-not $dev -or $dev.Status -ne 'OK') {
    'WARNING: the device did not start with the new package. The old packages are kept; to recover, install the' +
      ' previous package again, or run uninstall-rtw89win.ps1 and then this script (a fresh install: new adapter GUID)'
  } elseif (-not $loaded) {
    'WARNING: no "rtw89win <version> loaded" line in the driver log; cannot tell which image runs'
  } elseif ($loaded -ne $infVersion) {
    "WARNING: rtw89win $loaded still runs; a reboot is needed to load $infVersion (not done here)"
  }

  "=== delete the older rtw89win packages (no longer on the device; without /uninstall)"
  foreach ($d in Get-RtwPackages | Where-Object Driver -ne $newPkg.Driver) {
    "$($d.Driver) $($d.Version)"
    # without /force: pnputil refuses a package a device still uses, which would be a reason to keep it
    pnputil /delete-driver $d.Driver | Out-Null
    if ($LASTEXITCODE) { "WARNING: pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE); kept" }
  }
} else {
  "=== device class"
  # A device node does not always take the setup class of a newly installed INF. If the device did not follow the
  # INF's class, or did not start, remove its device node and let PnP enumerate it again.
  $dev = Get-RtwDevice
  "INF class $infClass; device class $($dev.Class), status $($dev.Status)"
  if ($dev.Class -ne $infClass -or $dev.Status -ne 'OK') {
    "removing the device node and scanning for hardware"
    pnputil /remove-device $dev.InstanceId
    "pnputil exit $LASTEXITCODE"
    pnputil /scan-devices
    "pnputil exit $LASTEXITCODE"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do {
      Start-Sleep -Milliseconds 500
      $dev = Get-RtwDevice
    } until (($dev -and $dev.Class -eq $infClass -and $dev.Status -eq 'OK') -or $sw.ElapsedMilliseconds -ge 60000)
    if ($dev) { "after {0} ms: class {1}, status {2}" -f $sw.ElapsedMilliseconds, $dev.Class, $dev.Status }
    else { 'the device did not come back within 60 s' }
  }
}

if (-not $countryDone) {
  "=== country"
  # a first install: the device has started with the world rules already; a country applies from its next start
  if (Set-RtwCountry) { if ($Country) { 'applies from the next start of the device (pnputil /restart-device, or a reboot)' } }
  else { 'WARNING: no rtw89win service key; Parameters\Country not checked' }
}

"=== result"
$dev = Get-RtwDevice
if (-not $dev) { Stop-Transcript | Out-Null; throw 'the device is not present' }
$code = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_ProblemCode'
$inf = Get-RtwDevProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath'
"$($dev.Status) problem $code $($dev.FriendlyName) $inf"
# the WLAN adapter's identity: WLAN profiles belong to this GUID (the interface exists only in WiFiCx mode)
$netCfg = Get-RtwNetCfgId $dev.InstanceId
$na = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object PnPDeviceID -eq $dev.InstanceId | Select-Object -First 1
"NetCfgInstanceId after:  $netCfg (before: $oldNetCfg) " + $(if (-not $oldNetCfg) { '' } elseif ($netCfg -eq $oldNetCfg) { 'kept' } else { 'CHANGED: the WLAN profiles of the old GUID no longer apply' })
if ($na) { "network adapter '$($na.Name)': InterfaceGuid $($na.InterfaceGuid), status $($na.Status)" }
else { 'network adapter: none now (not up yet, or diagnostic mode: see the mode below)' }
Stop-Transcript | Out-Null
if ($dev.Status -eq 'OK') {
  & "$dir\rtwdiag.ps1" mode
  & "$dir\rtwdiag.ps1" log
}
