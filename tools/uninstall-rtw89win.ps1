# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run on the board's Windows as administrator: removes every rtw89win driver package, then removes the device node
# and scans for hardware, so the RTL8852BE comes back as "no driver" without the setup class or settings of the old
# package; any package (also an older one) can then be installed cleanly. The SYSTEM hive backups of
# install-rtw89win.ps1 (<WorkDir>\backup\rtw89win-*, default WorkDir %ProgramData%\rtw89win) are kept.
$ErrorActionPreference = 'Stop'
$pkgs = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\rtw89win.inf' }
if (-not $pkgs) { 'no rtw89win package installed' }
foreach ($d in $pkgs) {
  "$($d.Driver) $($d.Version)"
  pnputil /delete-driver $d.Driver /uninstall /force
  if ($LASTEXITCODE) { throw "pnputil delete-driver $($d.Driver) failed ($LASTEXITCODE)" }
}

$dev = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*'
if ($dev) {
  "removing the device node $($dev.InstanceId) (class $($dev.Class)) and scanning for hardware"
  pnputil /remove-device $dev.InstanceId
  "pnputil exit $LASTEXITCODE"
  pnputil /scan-devices
  "pnputil exit $LASTEXITCODE"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  do {
    Start-Sleep -Milliseconds 500
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*'
  } until ($dev -or $sw.ElapsedMilliseconds -ge 60000)
  if (-not $dev) { 'the device did not come back within 60 s' }
}
Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*' | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize
