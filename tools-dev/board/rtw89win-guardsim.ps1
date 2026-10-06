# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Crash loop guard test: pnputil /restart-device; with -Crash, as soon as the ending session has cleared
# ArmRunning (release of the hardware), put back ArmRunning = 1 and the BootAttempts it had, so the next start sees a
# session that "did not end in order" (two such runs in a row trip the guard: diagnostic mode). Then the new start's
# log lines, the saved log of the ended session, the mode and the guard. Changes this driver's own values and
# restarts the device only; "rtwdiag.ps1 arm" goes back to WiFiCx.
# -Diag: rtwdiag.ps1 of the driver package (default: next to this script, or RTW89WIN_DIAG).
param([switch]$Crash,
      [string]$Diag = $(if ($env:RTW89WIN_DIAG) { $env:RTW89WIN_DIAG } else { Join-Path $PSScriptRoot 'rtwdiag.ps1' }))
if (-not (Test-Path $Diag)) { throw "rtwdiag.ps1 not found ($Diag): give -Diag <rtwdiag.ps1 of the driver package> or set RTW89WIN_DIAG" }
$d = $Diag
$p = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\rtw89win\Parameters', $true)
$b0 = $p.GetValue('BootAttempts'); $r0 = $p.GetValue('ArmRunning')
"before: ArmRunning $r0, BootAttempts $b0"
$id = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*').InstanceId
$proc = Start-Process pnputil -ArgumentList '/restart-device', "`"$id`"" -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\rtw89win-guardsim.txt"
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Crash) {
  if ($r0 -ne 1) { 'no session running (ArmRunning not 1): nothing to simulate' }
  else {
    # the driver clears BootAttempts, then ArmRunning (flushed), in its release of the hardware; the next start
    # (driver unload and load in between) follows 60-90 ms later (observed), so this busy-waits instead of sleeping
    while ($p.GetValue('ArmRunning') -ne 0 -and $sw.ElapsedMilliseconds -lt 30000) { }
    $t = $sw.Elapsed.TotalMilliseconds
    $p.SetValue('BootAttempts', [int]$b0, [Microsoft.Win32.RegistryValueKind]::DWord)
    $p.SetValue('ArmRunning', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
    $p.Flush()
    'ArmRunning cleared by the release after {0:N1} ms; put back ArmRunning 1, BootAttempts {1} after {2:N1} ms' -f $t, $b0, $sw.Elapsed.TotalMilliseconds
  }
}
$proc.WaitForExit()
Remove-Item "$env:TEMP\rtw89win-guardsim.txt" -ErrorAction SilentlyContinue
do {
  Start-Sleep -Milliseconds 300
  $txt = (pwsh -NoProfile -File $d log 2>$null) -join "`n"
} until ($txt -match 'adapter started|the start fails|bring-up failed|mode: diagnostic' -or $sw.ElapsedMilliseconds -gt 60000)
"after $($sw.ElapsedMilliseconds) ms:"
$txt -split "`n" | Select-String 'loaded|mode:|guard|adapter started|bring-up|probe failed|chip version|WiFiCx: probe|rtw89: probing' | ForEach-Object Line
'--- saved log of the ended session (tail)'
pwsh -NoProfile -File $d log -Saved | Select-String 'guard|loaded|mode:' | Select-Object -Last 3 | ForEach-Object Line
'--- now'
pwsh -NoProfile -File $d mode 2>&1 | Select-Object -First 2
$dev = Get-PnpDevice -InstanceId $id
'device {0}; adapter: {1}' -f $dev.Status, ((Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object PnPDeviceID -eq $id | ForEach-Object { "$($_.Name) $($_.Status)" }) -join ', ')
$p.Close()
