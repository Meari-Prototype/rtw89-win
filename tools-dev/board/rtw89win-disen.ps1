# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Disable, then enable the rtw89win network adapter (only this one); poll the device state.
$dev = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*'
$na = Get-NetAdapter | Where-Object { $_.PnPDeviceID -eq $dev.InstanceId -and $_.Status -ne "Not Present" } | Select-Object -First 1
if (-not $na) { throw 'no network adapter of rtw89win' }
"adapter $($na.Name) $($na.Status)"
$sw = [Diagnostics.Stopwatch]::StartNew()
Disable-NetAdapter -Name $na.Name -Confirm:$false
do { Start-Sleep -Milliseconds 300; $d = Get-PnpDevice -InstanceId $dev.InstanceId; $n = Get-NetAdapter -Name $na.Name -ErrorAction SilentlyContinue }
until (($n -and $n.Status -eq 'Disabled') -or $sw.ElapsedMilliseconds -gt 30000)
'disabled after {0} ms: adapter {1}, device {2} {3}' -f $sw.ElapsedMilliseconds, $n.Status, $d.Status, $d.Problem
$sw.Restart()
Enable-NetAdapter -Name $na.Name -Confirm:$false
do { Start-Sleep -Milliseconds 300; $d = Get-PnpDevice -InstanceId $dev.InstanceId; $n = Get-NetAdapter -Name $na.Name -ErrorAction SilentlyContinue }
until (($d.Status -eq 'OK' -and $n -and $n.Status -ne 'Disabled') -or $sw.ElapsedMilliseconds -gt 30000)
'enabled after {0} ms: adapter {1}, device {2}' -f $sw.ElapsedMilliseconds, $n.Status, $d.Status
