# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build rtkbtflt (Bluetooth patch loader, lower filter under BTHUSB), ARM64 Release:
#  1. bt\rtkbtfw\build.ps1 -NoPackage: the x64 unit test of epatch.c (shared with the driver), the user-mode tool's
#     offline runs, and the ARM64 rtkbtfw.exe (goes into this package: "rtkbtfw info" shows the filter's results)
#  2. MSBuild of rtkbtflt.vcxproj (WDK and Windows SDK NuGet packages, KMDF 1.33, /W4 /WX), the driver's imports listed
#  3. DriverVer stamped into the INF, inf2cat, test signing (sys and cat) with your own code signing certificate, as
#     build.ps1 (or -NoSign)
# Output: out\rtkbtflt-<timestamp>\ (or -OutDir) = rtkbtflt.{sys,inf,cat}, rtl8852bu_fw.bin, rtl8852bu_config.bin,
# LICENCE.rtlwifi_firmware.txt, LICENSE, THIRD_PARTY_NOTICES, rtw89win-test.cer (not with -NoSign), rtkbtfw.exe, the
# board-side scripts, SHA256SUMS. Copy the whole directory to the board and run install-rtkbtflt.ps1 there.
#   .\bt\rtkbtflt\build.ps1 [-Version 0.1.0.3] [-CertThumbprint <thumbprint> | -NoSign] [-OutDir <dir>]
#                           [-PackagesDir <dir>] [-FirmwareDir <dir>] [-VsPath <dir>]
# Defaults, RTW89WIN_* environment variables and local.settings.psd1: tools-dev\buildenv.ps1.
param([string]$Version = '0.1.0.3', [string]$CertThumbprint, [switch]$NoSign, [string]$OutDir,
      [string]$PackagesDir, [string]$FirmwareDir, [string]$VsPath)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = Split-Path (Split-Path $src)
. "$root\tools-dev\buildenv.ps1"
$OutDir = Resolve-RtwOutDir $OutDir 'rtkbtflt-'   # a wrong -OutDir stops the build before the compilation
$pkgs = Get-RtwPackagesDir $PackagesDir
$vs = Get-RtwVsPath $VsPath
$msbuild = Get-RtwMSBuild $vs   # the 64-bit MSBuild
$vc = Get-RtwVcTools $vs
$signtool = Get-RtwSignTool $pkgs
$inf2cat = Get-RtwInf2Cat $pkgs
$fw = Join-Path (Get-RtwFirmwareDir $FirmwareDir) 'rtl_bt'
Assert-RtwFirmware $fw 'rtl8852bu_fw.bin', 'rtl8852bu_config.bin' "$root\firmware\rtl_bt\SHA256SUMS"
$cert = if ($NoSign) { $null } else { Get-RtwSigningCert $CertThumbprint }
$bld = "$root\build\rtkbtflt"
$log = "$bld\build-arm64.log"
$env:VSLANG = '1033'
New-Item -ItemType Directory -Force $bld | Out-Null

'== bt\rtkbtfw (unit test, tool)'
& "$root\bt\rtkbtfw\build.ps1" -NoPackage -PackagesDir $pkgs -FirmwareDir (Split-Path $fw) -VsPath $vs |
    Select-String '^== |PASS|FAIL|^\s+\S+\.dll$' | ForEach-Object { $_.Line }

'== rtkbtflt.sys'
& $msbuild "$src\rtkbtflt.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=ARM64 /p:SignMode=Off /p:SpectreMitigation=false /p:FltVersion=$Version `
    "/p:RtwPackagesDir=$pkgs" /nologo /v:minimal "/flp:LogFile=$log;Verbosity=normal;Encoding=UTF-8"
if ($LASTEXITCODE) { Get-Content $log | Select-String 'error|warning' | ForEach-Object { $_.Line.Trim() }; throw "build failed ($LASTEXITCODE)" }
$sys = "$bld\ARM64\Release\rtkbtflt.sys"
$warn = Select-String -Path $log -Pattern 'warning C\d+|error C\d+|warning LNK|error LNK'
if ($warn) { $warn | ForEach-Object { $_.Line.Trim() }; throw 'compiler or linker warnings' }
'imports:'
& (Join-Path (Get-RtwVcHostBin $vc) 'dumpbin.exe') /nologo /imports $sys | Select-String '^\s+\S+\.(sys|SYS|exe|dll)$' | ForEach-Object { '  ' + $_.Line.Trim() }

$out = New-RtwOutDir $OutDir 'rtkbtflt-'
Copy-Item $sys, "$fw\rtl8852bu_fw.bin", "$fw\rtl8852bu_config.bin", "$root\firmware\rtl_bt\LICENCE.rtlwifi_firmware.txt",
    "$root\LICENSE", "$root\THIRD_PARTY_NOTICES" $out
# inf2cat compares DriverVer with the UTC date, so stamp the UTC date
$date = [DateTime]::UtcNow.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
(Get-Content "$src\rtkbtflt.inf") -replace '^DriverVer\s*=.*$', "DriverVer   = $date,$Version" | Set-Content "$out\rtkbtflt.inf" -Encoding ascii

if ($cert) {
  Export-Certificate -Cert $cert -FilePath "$out\rtw89win-test.cer" | Out-Null
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\rtkbtflt.sys"
  if ($LASTEXITCODE) { throw "signtool (sys) failed ($LASTEXITCODE)" }
}
& $inf2cat /os:Server10_arm64 /driver:$out | Select-Object -Last 3
if ($LASTEXITCODE) { throw "inf2cat failed ($LASTEXITCODE)" }
if ($cert) {
  & $signtool sign /fd sha256 /sha1 $cert.Thumbprint /s My "$out\rtkbtflt.cat"
  if ($LASTEXITCODE) { throw "signtool (cat) failed ($LASTEXITCODE)" }
} else {
  'not signed (-NoSign): sign rtkbtflt.sys and rtkbtflt.cat before installing'
}

# the board-side scripts and the user-mode tool (ARM64, from step 1)
Copy-Item "$src\install-rtkbtflt.ps1", "$src\uninstall-rtkbtflt.ps1", "$src\rtkbtflt.ps1", "$src\rollback-rtkbtflt.sh" $out
Copy-Item "$root\build\rtkbtfw\arm64\rtkbtfw.exe" $out
Write-RtwSums $out
"output: $out"
