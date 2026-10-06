# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Adds a WPA2-Personal (AES) WLAN profile for one SSID on the WLAN interface, connectionMode manual (never
# connects by itself). The key is given on the command line; the profile XML exists only for netsh wlan add profile
# in %TEMP% and is deleted right after (also on failure). -Delete removes the profile instead.
# -Auth WPA3SAE makes a WPA3-Personal profile instead (the profile name is the SSID either way). With
# Parameters\Pmf 2, Windows connects with SAE even through a WPA2PSK profile when the network is in WPA3 transition
# mode (observed).
#   pwsh -NoProfile -File rtw89win-profile.ps1 -Ssid '<SSID>' -Key '<passphrase>' [-Auth WPA3SAE]
#   pwsh -NoProfile -File rtw89win-profile.ps1 -Ssid '<SSID>' -Delete
param([Parameter(Mandatory)][string]$Ssid, [string]$Key, [string]$Interface = 'WLAN', [switch]$Delete,
      [ValidateSet('WPA2PSK', 'WPA3SAE')][string]$Auth = 'WPA2PSK')
$ErrorActionPreference = 'Stop'
if ($Delete) {
  netsh wlan delete profile name="$Ssid" interface="$Interface"
  exit $LASTEXITCODE
}
if (-not $Key) { throw '-Key is required' }
$e = [Security.SecurityElement]
$hex = -join ([Text.Encoding]::UTF8.GetBytes($Ssid) | ForEach-Object { $_.ToString('X2') })
$xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$($e::Escape($Ssid))</name>
  <SSIDConfig><SSID><hex>$hex</hex><name>$($e::Escape($Ssid))</name></SSID><nonBroadcast>false</nonBroadcast></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>$Auth</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$($e::Escape($Key))</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@
$file = Join-Path ([IO.Path]::GetTempPath()) ('rtw89win-profile-' +[Guid]::NewGuid().ToString('N') + '.xml')
try {
  [IO.File]::WriteAllText($file, $xml, [Text.UTF8Encoding]::new($false))
  netsh wlan add profile filename="$file" interface="$Interface"
  $rc = $LASTEXITCODE
} finally {
  Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
  $xml = $null
}
"profile XML deleted: $(-not (Test-Path -LiteralPath $file))"
exit $rc
