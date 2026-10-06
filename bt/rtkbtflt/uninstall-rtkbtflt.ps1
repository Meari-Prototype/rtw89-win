# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run on the board's Windows as administrator: deletes every rtkbtflt package (pnputil /delete-driver /uninstall
# /force). PnP takes the extension off the Bluetooth device and restarts it on Microsoft's bth.inf alone, without the
# filter; the device node stays (pairings are kept). The controller is back on its ROM firmware from that restart on
# (a D3). The service key Services\rtkbtflt (with its Parameters) stays; nothing loads it. The SYSTEM hive backups of
# install-rtkbtflt.ps1 (<WorkDir>\backup\rtkbtflt-*, default WorkDir %ProgramData%\rtw89win) are kept.
# -HardwareId: the Bluetooth device's USB ID (default USB\VID_13D3&PID_3572, the module of the ROCK 5T).
param([string]$HardwareId = 'USB\VID_13D3&PID_3572')
$ErrorActionPreference = 'Stop'
$hwid = $HardwareId
function Get-BtDevice { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like "$hwid\*" }
function Get-BtProp([string]$id, [string]$key) {
  (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction SilentlyContinue).Data
}
function Wait-BtOk([int]$ms = 30000) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  do {
    $d = Get-BtDevice
    if ($d -and $d.Status -eq 'OK') { break }
    Start-Sleep -Milliseconds 500
  } until ($sw.ElapsedMilliseconds -ge $ms)
  $d
}

$pkgs = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\rtkbtflt.inf' }
if (-not $pkgs) { 'no rtkbtflt package installed' }
$reboot = $false
foreach ($d in $pkgs) {
  "$($d.Driver) $($d.Version)"
  pnputil /delete-driver $d.Driver /uninstall /force
  # 3010 (ERROR_SUCCESS_REBOOT_REQUIRED): deleted, the device finishes the change at the next restart
  if ($LASTEXITCODE -eq 3010) { $reboot = $true; 'deleted; a reboot is asked for' }
  elseif ($LASTEXITCODE) { throw "pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE)" }
}

$dev = Wait-BtOk
if (-not $dev) { throw "the Bluetooth device ($hwid) is not present" }
$stack = @(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_Stack')
if (($stack -contains '\Driver\rtkbtflt') -or ($dev.Status -ne 'OK')) {
  "the device still has rtkbtflt in its stack, or is not OK ($($dev.Status)): restarting it"
  pnputil /restart-device $dev.InstanceId
  "pnputil exit $LASTEXITCODE"
  $dev = Wait-BtOk
  $stack = @(if ($dev) { Get-BtProp $dev.InstanceId 'DEVPKEY_Device_Stack' })
}
"device $($dev.InstanceId): status $($dev.Status), INF $(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath')" +
  " ($(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_DriverInfSection')), stack: $($stack -join ' / ')"
if ($stack -contains '\Driver\rtkbtflt') { 'WARNING: rtkbtflt is still in the device stack (a reboot should drop it)' }
if ($reboot) { 'NOTE: pnputil asked for a reboot (exit 3010); reboot Windows to finish (not done here)' }
