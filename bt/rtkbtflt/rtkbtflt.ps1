# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run on the board's Windows (as administrator for "clear"). -HardwareId: the Bluetooth device's
# USB ID (default USB\VID_13D3&PID_3572, the module of the ROCK 5T).
#   rtkbtflt.ps1 [status]   the Bluetooth device (status, driver, stack), the filter's service and its values under
#                           Services\rtkbtflt\Parameters: mode switches, guard, the last session's result and log
#   rtkbtflt.ps1 status -Download   also the log of the last session that downloaded (LastDownload)
#   rtkbtflt.ps1 clear      after the crash loop guard tripped (BadSessions >= 2) and its logs were looked at:
#                           SessionRunning = 0, BadSessions = 0, then pnputil /restart-device (the filter runs a
#                           session again at the device's next start; changes state)
param([ValidateSet('status', 'clear')][string]$Action = 'status', [switch]$Download,
      [string]$HardwareId = 'USB\VID_13D3&PID_3572')
$ErrorActionPreference = 'Stop'
$hwid = $HardwareId
$svcKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\rtkbtflt'
$results = 'patched (downloaded now)', 'already patched (nothing sent)', 'not this controller / not its ROM (nothing sent)',
  'download failed, the controller runs its ROM', 'state unknown after the last fragment', 'no answer to HCI_Read_Local_Version',
  'no usable patch file', 'pass-through (PassThrough = 1, guard, or setup)', 'USB error of the filter', 'registry not writable (nothing sent)'

function Get-BtDevice { Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like "$hwid\*" }
function Get-BtProp([string]$id, [string]$key) {
  (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction SilentlyContinue).Data
}

if ($Action -eq 'clear') {
  $p = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\rtkbtflt\Parameters', $true)
  if (-not $p) { throw 'no Services\rtkbtflt\Parameters key (the filter never ran)' }
  "before: SessionRunning $($p.GetValue('SessionRunning')), BadSessions $($p.GetValue('BadSessions'))"
  $p.SetValue('SessionRunning', 0, [Microsoft.Win32.RegistryValueKind]::DWord)
  $p.SetValue('BadSessions', 0, [Microsoft.Win32.RegistryValueKind]::DWord)
  $p.Close()
  $dev = Get-BtDevice
  if ($dev) {
    "restarting $($dev.InstanceId)"
    pnputil /restart-device $dev.InstanceId
    "pnputil exit $LASTEXITCODE"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do { Start-Sleep -Milliseconds 500; $dev = Get-BtDevice } until (($dev -and $dev.Status -eq 'OK') -or $sw.ElapsedMilliseconds -ge 30000)
  }
}

$dev = Get-BtDevice
if ($dev) {
  "device $($dev.InstanceId): status $($dev.Status), problem $(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_ProblemCode')"
  "  driver $(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_DriverInfPath') ($(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_DriverInfSection'))"
  "  stack: $((@(Get-BtProp $dev.InstanceId 'DEVPKEY_Device_Stack')) -join ' / ')"
} else {
  "device ${hwid}: not present"
}

$svc = Get-ItemProperty $svcKey -ErrorAction SilentlyContinue
if (-not $svc) { 'service rtkbtflt: not installed'; return }
"service rtkbtflt: Start $($svc.Start), ImagePath $($svc.ImagePath)"
$p = Get-ItemProperty "$svcKey\Parameters" -ErrorAction SilentlyContinue
if (-not $p) { '  no Parameters key yet'; return }
'  DriverVersion {0}, PassThrough {1}, SessionRunning {2}, BadSessions {3}, Downloads {4}' -f $p.DriverVersion, $p.PassThrough,
  $p.SessionRunning, $p.BadSessions, $p.Downloads
if ($null -ne $p.LastResult) {
  $text = if ($p.LastResult -lt $results.Count) { $results[$p.LastResult] } else { '?' }
  '  LastResult {0} = {1}; controller at the end: hci_rev 0x{2:x4} lmp_subver 0x{3:x4}' -f $p.LastResult, $text,
    $p.LastHciRev, $p.LastLmpSubver
}
if ($p.SessionStage) { "  SessionStage: $($p.SessionStage)" }
if ($p.LastGuardRelease) { "  LastGuardRelease: $($p.LastGuardRelease)" }
if ($p.LastSession) { '--- LastSession'; $p.LastSession.TrimEnd() }
if ($Download -and $p.LastDownload) { '--- LastDownload'; $p.LastDownload.TrimEnd() }
if ($p.LastPassThrough) { '--- LastPassThrough (a start or D3 -> D0 in pass-through)'; $p.LastPassThrough.TrimEnd() }
if ($p.LastBadSession) { "--- LastBadSession (did not end in order; its stage: $($p.LastBadStage))"; $p.LastBadSession.TrimEnd() }
