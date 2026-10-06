# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build rtw89win (ARM64 Release): the Linux side with clang (buildhal.ps1), then the driver with MSBuild; stamp the
# INF, make the catalog and test-sign the package with your own code signing certificate (Windows on the board runs
# in test mode), or leave it unsigned (-NoSign).
# Output: out\<timestamp>\ (or -OutDir) = rtw89win.{sys,inf,cat}, rtw8852b_fw-2.bin, LICENCE.rtlwifi_firmware.txt,
# LICENSE, THIRD_PARTY_NOTICES, rtw89win-test.cer (the certificate without its private key; not with -NoSign), the
# board-side scripts from tools\, SHA256SUMS. Copy the whole directory to the board and run install-rtw89win.ps1 there.
#   .\build.ps1 [-Version 0.0.11.2] [-CertThumbprint <thumbprint> | -NoSign] [-OutDir <dir>]
#               [-PackagesDir <dir>] [-LlvmBin <dir>] [-RtwSrc <dir>] [-FirmwareDir <dir>] [-VsPath <dir>]
# Where the tools and inputs are when a parameter is not given (RTW89WIN_* environment variables,
# local.settings.psd1, defaults): tools-dev\buildenv.ps1.
param([string]$Version = '0.0.11.2', [string]$CertThumbprint, [switch]$NoSign, [string]$OutDir,
      [string]$PackagesDir, [string]$LlvmBin, [string]$RtwSrc, [string]$FirmwareDir, [string]$VsPath)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. "$root\tools-dev\buildenv.ps1"
# every tool, input and the output directory is looked up before the long part, so a missing or wrong one stops the
# build at once
$OutDir = Resolve-RtwOutDir $OutDir ''
$pkgs = Get-RtwPackagesDir $PackagesDir
$msbuild = Get-RtwMSBuild (Get-RtwVsPath $VsPath)   # must be the 64-bit MSBuild
$signtool = Get-RtwSignTool $pkgs
$inf2cat = Get-RtwInf2Cat $pkgs
$fw = Get-RtwFirmwareDir $FirmwareDir
Assert-RtwFirmware $fw 'rtw8852b_fw-2.bin' "$root\firmware\SHA256SUMS"
$cert = if ($NoSign) { $null } else { Get-RtwSigningCert $CertThumbprint }
$log = "$root\build\build-arm64.log"
$env:VSLANG = '1033'
New-Item -ItemType Directory -Force "$root\build" | Out-Null

# buildhal.ps1: on success only its last line (the library); when it fails, all it printed (the table of objects and
# the first errors of the failed files), then its error
$hal = [Collections.Generic.List[object]]::new()
try { & "$root\buildhal.ps1" -LlvmBin $LlvmBin -RtwSrc $RtwSrc | ForEach-Object { $hal.Add($_) } }
catch { $hal; throw }
$hal | Select-Object -Last 1

& $msbuild "$root\driver\rtw89win.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:SpectreMitigation=false /p:RtwVersion=$Version `
    "/p:RtwPackagesDir=$pkgs" /nologo /v:minimal "/flp:LogFile=$log;Verbosity=normal;Encoding=UTF-8"
if ($LASTEXITCODE) { Get-Content $log | Select-String 'error|warning' | ForEach-Object { $_.Line.Trim() }; throw "build failed ($LASTEXITCODE)" }

$out = New-RtwOutDir $OutDir ''
Copy-Item "$root\build\ARM64\Release\rtw89win.sys", "$fw\rtw8852b_fw-2.bin", "$root\firmware\LICENCE.rtlwifi_firmware.txt",
    "$root\LICENSE", "$root\THIRD_PARTY_NOTICES" $out
# inf2cat compares DriverVer with the UTC date, so stamp the UTC date
$date = [DateTime]::UtcNow.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
(Get-Content "$root\driver\rtw89win.inf") -replace '^DriverVer\s*=.*$', "DriverVer   = $date,$Version" | Set-Content "$out\rtw89win.inf" -Encoding ascii

if ($cert) {
  Export-Certificate -Cert $cert -FilePath "$out\rtw89win-test.cer" | Out-Null
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\rtw89win.sys"
  if ($LASTEXITCODE) { throw "signtool (sys) failed ($LASTEXITCODE)" }
}
& $inf2cat /os:Server10_arm64 /driver:$out | Select-Object -Last 3
if ($LASTEXITCODE) { throw "inf2cat failed ($LASTEXITCODE)" }
if ($cert) {
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\rtw89win.cat"
  if ($LASTEXITCODE) { throw "signtool (cat) failed ($LASTEXITCODE)" }
} else {
  'not signed (-NoSign): sign rtw89win.sys and rtw89win.cat before installing'
}

Copy-Item "$root\tools\*" $out
Write-RtwSums $out
"output: $out"
