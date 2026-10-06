# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build rtkbtfw (user-mode Bluetooth patch tool and the patch code shared with rtkbtflt):
#  - x64 test-epatch.exe and rtkbtfw.exe, run on this PC: the unit test of the patch parsing / fragmenting / event
#    search against the firmware's rtl_bt\ directory, then "rtkbtfw parse" and "rtkbtfw load -DryRun -Rom 1" (no
#    radio needed)
#  - ARM64 rtkbtfw.exe for the board, static CRT (/MT): nothing to install there
# Compiler: MSVC of Visual Studio 2022 / Build Tools (found with vswhere); headers and libraries from the Windows SDK
# NuGet packages (as build.ps1). Output: out\rtkbtfw-<timestamp>\ (or -OutDir) = rtkbtfw.exe (ARM64),
# rtl8852bu_fw.bin, rtl8852bu_config.bin, LICENCE.rtlwifi_firmware.txt, LICENSE, THIRD_PARTY_NOTICES, SHA256SUMS;
# build logs in build\rtkbtfw\.
# -NoPackage: build and test only (bt\rtkbtflt\build.ps1 takes build\rtkbtfw\arm64\rtkbtfw.exe into its package).
#   .\bt\rtkbtfw\build.ps1 [-NoPackage] [-OutDir <dir>] [-PackagesDir <dir>] [-FirmwareDir <dir>] [-VsPath <dir>]
# Defaults, RTW89WIN_* environment variables and local.settings.psd1: tools-dev\buildenv.ps1.
param([switch]$NoPackage, [string]$OutDir, [string]$PackagesDir, [string]$FirmwareDir, [string]$VsPath)
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$root = Split-Path (Split-Path $src)
. "$root\tools-dev\buildenv.ps1"
if (-not $NoPackage) { $OutDir = Resolve-RtwOutDir $OutDir 'rtkbtfw-' }   # a wrong -OutDir stops before the compilation
$pkgs = Get-RtwPackagesDir $PackagesDir
$vc = Get-RtwVcTools (Get-RtwVsPath $VsPath)
$sdkInc = "$pkgs\$RtwSdkPackage\c\Include\10.0.26100.0"
$fw = Join-Path (Get-RtwFirmwareDir $FirmwareDir) 'rtl_bt'
Assert-RtwFirmware $fw 'rtl8852bu_fw.bin', 'rtl8852bu_config.bin' "$root\firmware\rtl_bt\SHA256SUMS"
$bld = "$root\build\rtkbtfw"
New-Item -ItemType Directory -Force "$bld\x64", "$bld\arm64" | Out-Null

function Build($arch, $sdkArch, $sources, $exe, $libs) {
    $env:INCLUDE = "$vc\include;$sdkInc\ucrt;$sdkInc\um;$sdkInc\shared"
    $env:LIB = "$vc\lib\$arch;$pkgs\Microsoft.Windows.SDK.CPP.$sdkArch.10.0.26100.1\c\ucrt\$arch;" +
        "$pkgs\Microsoft.Windows.SDK.CPP.$sdkArch.10.0.26100.1\c\um\$arch"
    $env:PATH = "$(Get-RtwVcHostBin $vc);$env:PATH"   # the cross compilers need the host's DLLs
    $cl = Join-Path (Get-RtwVcBin $vc $arch) 'cl.exe'
    $log = "$bld\$arch\$exe.log"
    & $cl /nologo /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS "/Fo$bld\$arch\\" "/Fe$bld\$arch\$exe" `
        $sources /link $libs *> $log
    if ($LASTEXITCODE) { Get-Content $log; throw "$arch ${exe}: build failed ($LASTEXITCODE)" }
}

$winLibs = 'setupapi.lib', 'bcrypt.lib', 'advapi32.lib', 'kernel32.lib'
Build 'x64' 'x64' @("$src\epatch.c", "$src\test-epatch.c") 'test-epatch.exe' @()
Build 'x64' 'x64' @("$src\rtkbtfw.c", "$src\epatch.c") 'rtkbtfw.exe' $winLibs
Build 'arm64' 'arm64' @("$src\rtkbtfw.c", "$src\epatch.c") 'rtkbtfw.exe' $winLibs

'== unit test (x64)'
& "$bld\x64\test-epatch.exe" "$fw\rtl8852bu_fw.bin" "$fw\rtl8852bu_config.bin"
if ($LASTEXITCODE) { throw 'unit test failed' }
'== rtkbtfw parse (x64)'
& "$bld\x64\rtkbtfw.exe" parse "$fw\rtl8852bu_fw.bin"
if ($LASTEXITCODE) { throw "parse failed ($LASTEXITCODE)" }
'== rtkbtfw load -DryRun -Rom 1 (x64, the radio is not opened)'
& "$bld\x64\rtkbtfw.exe" load "$fw\rtl8852bu_fw.bin" -DryRun -Rom 1
if ($LASTEXITCODE) { throw "dry run failed ($LASTEXITCODE)" }

$hdr = [IO.File]::ReadAllBytes("$bld\arm64\rtkbtfw.exe")
$pe = [BitConverter]::ToInt32($hdr, 0x3c)
$machine = [BitConverter]::ToUInt16($hdr, $pe + 4)
if ($machine -ne 0xAA64) { throw ('rtkbtfw.exe machine 0x{0:x4}, not ARM64' -f $machine) }
"== ARM64 rtkbtfw.exe: machine 0x{0:x4}, imports:" -f $machine
& (Join-Path (Get-RtwVcHostBin $vc) 'dumpbin.exe') /nologo /imports "$bld\arm64\rtkbtfw.exe" | Select-String '^\s+\S+\.dll$' |
    ForEach-Object { '  ' + $_.Line.Trim() }
if ($NoPackage) { return }

$out = New-RtwOutDir $OutDir 'rtkbtfw-'
Copy-Item "$bld\arm64\rtkbtfw.exe", "$fw\rtl8852bu_fw.bin", "$fw\rtl8852bu_config.bin", "$root\firmware\rtl_bt\LICENCE.rtlwifi_firmware.txt",
    "$root\LICENSE", "$root\THIRD_PARTY_NOTICES" $out
Write-RtwSums $out
"output: $out"
