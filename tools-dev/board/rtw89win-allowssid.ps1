# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Connection gate: sets the volatile Parameters\Allow\AllowConnect = 1 and (with -Ssid) AllowSsid (REG_SZ), so
# that only a request for that SSID goes on; -Clear deletes both. Volatile: gone after a reboot. The driver reads
# them at every TASK_CONNECT. Changes this driver's own values only. The gate is open when no value is set; this
# script is only needed to pin one SSID for a test.
param([string]$Ssid, [switch]$Clear)
$ErrorActionPreference = 'Stop'
$svcKey = 'SYSTEM\CurrentControlSet\Services\rtw89win'
if (-not [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($svcKey)) { throw 'the rtw89win service is not installed' }
$params = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey("$svcKey\Parameters")
if ($Clear) {
  $allow = $params.OpenSubKey('Allow', $true)
  if ($allow) {
    $allow.DeleteValue('AllowConnect', $false)
    $allow.DeleteValue('AllowSsid', $false)
    $allow.Close()
  }
  $params.DeleteValue('AllowConnect', $false)
  'cleared: AllowConnect, AllowSsid'
} else {
  $allow = $params.CreateSubKey('Allow', $true, [Microsoft.Win32.RegistryOptions]::Volatile)
  $allow.SetValue('AllowConnect', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
  if ($Ssid) { $allow.SetValue('AllowSsid', $Ssid, [Microsoft.Win32.RegistryValueKind]::String) }
  else { $allow.DeleteValue('AllowSsid', $false) }
  $allow.Close()
  "set: Parameters\Allow\AllowConnect = 1" + $(if ($Ssid) { ", AllowSsid = '$Ssid'" } else { ' (no SSID pin)' })
}
$params.Close()
