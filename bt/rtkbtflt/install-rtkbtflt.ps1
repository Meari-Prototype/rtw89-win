# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run on the board's Windows as administrator (e.g. over SSH) from the package directory (a copy of a
# bt\rtkbtflt\build.ps1 output directory anywhere on the board).
#   pwsh -NoProfile -File install-rtkbtflt.ps1 [-WorkDir <dir>] [-HardwareId 'USB\VID_xxxx&PID_xxxx']
# 1. checks SHA256SUMS, 2. saves the SYSTEM hive to <WorkDir>\backup\rtkbtflt-<time>\ (WorkDir: default
#    %ProgramData%\rtw89win; the transcript goes to <WorkDir>\logs\rtkbtflt-install.log),
# 3. installs this extension package (its test certificate rtw89win-test.cer, if the package has one, is trusted
#    only meanwhile): pnputil /add-driver /install.
#    The Bluetooth device (-HardwareId, default USB\VID_13D3&PID_3572, the module of the ROCK 5T; the INF must list
#    the same ID) keeps Microsoft's bth.inf; PnP restarts it with the lower filter.
#    In place: a newer package replaces the rtkbtflt package the device has (same ExtensionId); if this one is not
#    newer (the same version again, or going back), the other rtkbtflt packages are deleted with /uninstall so that
#    PnP takes this one. The device node is never removed (pairings live under BTHPORT, by address).
# 4. checks that the device is OK, that its lower filters include rtkbtflt and that the service runs this package's
#    image (Parameters\DriverVersion, written by the driver when it loads; otherwise pnputil /restart-device), deletes
#    the older rtkbtflt packages (no longer used; without /uninstall), then prints the filter's last session and
#    "rtkbtfw info" (bthport's view of the controller: after a good download lmpSubversion 0xb20f).
# If Windows then fails to boot: from another system on the same board (e.g. Linux), run rollback-rtkbtflt.sh of this
# package (it renames rtkbtflt.sys in the driver store), or restore the SYSTEM hive saved in step 2.
param([string]$WorkDir = (Join-Path $env:ProgramData 'rtw89win'), [string]$HardwareId = 'USB\VID_13D3&PID_3572')
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$hwid = $HardwareId
$svcKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\rtkbtflt'
New-Item -ItemType Directory -Force (Join-Path $WorkDir 'logs') | Out-Null
Start-Transcript -Path (Join-Path $WorkDir 'logs\rtkbtflt-install.log') -Append | Out-Null

function Get-BtDevice { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like "$hwid\*" }
function Get-BtProp([string]$id, [string]$key) {
  (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction SilentlyContinue).Data
}
function Get-FltPackages { Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\rtkbtflt.inf' } }
# the driver store directory the service's image is in (one per package)
function Get-FltImageDir {
  $p = (Get-ItemProperty $svcKey -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
  if ($p) { Split-Path (Split-Path $p -Parent) -Leaf } else { $null }
}
function Get-FltParam([string]$name) { (Get-ItemProperty "$svcKey\Parameters" -Name $name -ErrorAction SilentlyContinue).$name }
function Wait-BtOk([int]$ms = 30000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  do {
    $d = Get-BtDevice
    if ($d -and $d.Status -eq 'OK') { break }
    Start-Sleep -Milliseconds 500
  } until ($sw.ElapsedMilliseconds -ge $ms)
  $d
}
# the drivers in the device's stack, top first (\Driver\BTHUSB ... \Driver\usbhub): rtkbtflt is there once it works
function Get-BtStack([string]$id) { @(Get-BtProp $id 'DEVPKEY_Device_Stack') }
function Show-BtDevice($d) {
  if (-not $d) { 'device: not present'; return }
  "device $($d.InstanceId): status $($d.Status), problem $(Get-BtProp $d.InstanceId 'DEVPKEY_Device_ProblemCode')"
  "  service $(Get-BtProp $d.InstanceId 'DEVPKEY_Device_Service'), INF $(Get-BtProp $d.InstanceId 'DEVPKEY_Device_DriverInfPath')" +
    " ($(Get-BtProp $d.InstanceId 'DEVPKEY_Device_DriverInfSection')), stack: $((Get-BtStack $d.InstanceId) -join ' / ')"
}

"=== verify package"
foreach ($line in Get-Content "$dir\SHA256SUMS") {
  $want, $name = $line -split '\s+', 2
  if ((Get-FileHash "$dir\$name").Hash.ToLower() -ne $want) { throw "$name hash mismatch" }
}
"ok"
$infVersion = (Select-String -Path "$dir\rtkbtflt.inf" -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
$infHash = (Get-FileHash "$dir\rtkbtflt.inf").Hash
$dev = Get-BtDevice
if (-not $dev) { throw "Bluetooth device ($hwid) not present" }
Show-BtDevice $dev
$old = @(Get-FltPackages)
"package $infVersion; rtkbtflt packages in the driver store: " + $(if ($old.Count) { ($old | ForEach-Object { "$($_.Driver) $($_.Version)" }) -join ', ' } else { 'none (first install)' })

"=== save SYSTEM hive"
$bk = Join-Path $WorkDir ('backup\rtkbtflt-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $bk | Out-Null
reg.exe save HKLM\SYSTEM "$bk\SYSTEM.hiv" /y | Out-Null
if ($LASTEXITCODE) { throw "reg save failed ($LASTEXITCODE)" }
"$bk\SYSTEM.hiv"

"=== install"
# the package's test certificate (build.ps1 -CertThumbprint), trusted only while installing; none with -NoSign
$cert = $null
if (Test-Path "$dir\rtw89win-test.cer") {
  $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new("$dir\rtw89win-test.cer")
} else {
  'no rtw89win-test.cer in the package: the package must be signed by a certificate this Windows trusts already'
}
$stores = 'Root', 'TrustedPublisher'
if ($cert) {
  foreach ($s in $stores) { $st = [Security.Cryptography.X509Certificates.X509Store]::new($s, 'LocalMachine'); $st.Open('ReadWrite'); $st.Add($cert); $st.Close() }
}
try {
  pnputil /add-driver "$dir\rtkbtflt.inf" /install
  "pnputil exit $LASTEXITCODE" + $(if ($LASTEXITCODE -eq 3010) { ' (a reboot is asked for; the device is restarted below instead)' } else { '' })
  $newPkg = Get-FltPackages | Where-Object { (Get-FileHash $_.OriginalFileName).Hash -eq $infHash } | Select-Object -First 1
  if (-not $newPkg) { throw 'the new package is not in the driver store' }
  $newDir = Split-Path (Split-Path $newPkg.OriginalFileName -Parent) -Leaf
  "new package $($newPkg.Driver) ($($newPkg.Version)), $newDir"
  $null = Wait-BtOk
  if ((Get-FltImageDir) -ne $newDir) {
    # not newer than the one the device has: PnP keeps that one. Take the others off the device; PnP then
    # reinstalls the device with the only rtkbtflt package left (still on bth.inf, still the same device node).
    "the service still runs from $(Get-FltImageDir); deleting the other rtkbtflt packages with /uninstall"
    foreach ($d in Get-FltPackages | Where-Object Driver -ne $newPkg.Driver) {
      "  $($d.Driver) $($d.Version)"
      pnputil /delete-driver $d.Driver /uninstall | Out-Null
      if ($LASTEXITCODE -eq 3010) { '  deleted; a reboot is asked for (the device is restarted below instead)' }
      elseif ($LASTEXITCODE) { throw "pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE)" }
    }
  }
} finally {
  if ($cert) {
    foreach ($s in $stores) { $st = [Security.Cryptography.X509Certificates.X509Store]::new($s, 'LocalMachine'); $st.Open('ReadWrite'); $st.Remove($cert); $st.Close() }
    "test certificate removed again: " + (($stores | ForEach-Object { "$_=" + (-not (Get-ChildItem "Cert:\LocalMachine\$_\$($cert.Thumbprint)" -EA 0)) }) -join ' ')
  }
}

"=== check"
$dev = Wait-BtOk
if (-not $dev) { Stop-Transcript | Out-Null; throw 'the Bluetooth device is not present after the install' }
$stack = Get-BtStack $dev.InstanceId
$loaded = Get-FltParam 'DriverVersion'
"image directory $(Get-FltImageDir) (package $newDir); stack: $($stack -join ' / '); driver running: $loaded (package $infVersion); status $($dev.Status)"
if (($stack -notcontains '\Driver\rtkbtflt') -or ($loaded -ne $infVersion) -or ($dev.Status -ne 'OK')) {
  "restarting the device (pnputil /restart-device)"
  pnputil /restart-device $dev.InstanceId
  "pnputil exit $LASTEXITCODE"
  $dev = Wait-BtOk
  $stack = @(if ($dev) { Get-BtStack $dev.InstanceId })
  $loaded = Get-FltParam 'DriverVersion'
  "after the restart: status $($dev.Status), stack: $($stack -join ' / '), driver running: $loaded"
}
if (-not $dev -or $dev.Status -ne 'OK') {
  'WARNING: the Bluetooth device did not start; uninstall-rtkbtflt.ps1 takes the filter off again'
} elseif ($stack -notcontains '\Driver\rtkbtflt') {
  'WARNING: \Driver\rtkbtflt is not in the device stack'
} elseif ($loaded -ne $infVersion) {
  "WARNING: rtkbtflt $loaded still runs; a reboot is needed to load $infVersion (not done here)"
}

"=== delete the older rtkbtflt packages (no longer used; without /uninstall)"
foreach ($d in Get-FltPackages | Where-Object Driver -ne $newPkg.Driver) {
  "$($d.Driver) $($d.Version)"
  # without /force: pnputil refuses a package a device still uses, which would be a reason to keep it
  pnputil /delete-driver $d.Driver | Out-Null
  if ($LASTEXITCODE -and $LASTEXITCODE -ne 3010) { "WARNING: pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE); kept" }
}

"=== result"
Show-BtDevice (Get-BtDevice)
Stop-Transcript | Out-Null
& "$dir\rtkbtflt.ps1" status -HardwareId $hwid
if (Test-Path "$dir\rtkbtfw.exe") {
  "=== rtkbtfw info"
  & "$dir\rtkbtfw.exe" info
}
