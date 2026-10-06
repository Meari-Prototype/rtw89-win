# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build environment, dot-sourced by build.ps1, buildhal.ps1, bt\rtkbtflt\build.ps1, bt\rtkbtfw\build.ps1 and the
# tools-dev scripts:  . "$root\tools-dev\buildenv.ps1"
# It finds the tools and the inputs of the build. Each setting comes from the first of:
#   1. the calling script's parameter (-PackagesDir, -LlvmBin, ...),
#   2. an environment variable,
#   3. local.settings.psd1 in the repository root (optional and not in git; see local.settings.example.psd1),
#   4. a default.
#   Setting         Environment variable   Default
#   PackagesDir     RTW89WIN_PACKAGES      packages\ : the NuGet packages of packages.config (WDK, Windows SDK)
#   LlvmBin         RTW89WIN_LLVM          the directory of clang.exe on PATH, else %ProgramFiles%\LLVM\bin
#   RtwSrc          RTW89WIN_RTW89_SRC     third_party\rtw89 : the upstream rtw89 sources
#   FirmwareDir     RTW89WIN_FIRMWARE      firmware\ : rtw8852b_fw-2.bin, rtl_bt\rtl8852bu_fw.bin and
#                                          rtl_bt\rtl8852bu_config.bin (not in git, see firmware\SHA256SUMS)
#   CertThumbprint  RTW89WIN_CERT          none: build.ps1 and bt\rtkbtflt\build.ps1 then need -NoSign
#   VsPath          RTW89WIN_VS            the newest Visual Studio / Build Tools with the MSVC ARM64 tools (vswhere)
# and, for the PC-side measurement scripts in hosttools\:
#   BoardWlan       RTW89WIN_BOARD_WLAN    none: the board's WLAN address
#   SshHost         RTW89WIN_SSH_HOST      none: the board's host name or alias for ssh
# A relative path is taken relative to the repository root.

$RtwRoot = Split-Path $PSScriptRoot -Parent
$RtwSettingVars = @{
  PackagesDir = 'RTW89WIN_PACKAGES'; LlvmBin = 'RTW89WIN_LLVM'; RtwSrc = 'RTW89WIN_RTW89_SRC'
  FirmwareDir = 'RTW89WIN_FIRMWARE'; CertThumbprint = 'RTW89WIN_CERT'; VsPath = 'RTW89WIN_VS'
  BoardWlan = 'RTW89WIN_BOARD_WLAN'; SshHost = 'RTW89WIN_SSH_HOST'
}
$RtwLocalSettings = @{}
if (Test-Path (Join-Path $RtwRoot 'local.settings.psd1')) {
  $RtwLocalSettings = Import-PowerShellDataFile (Join-Path $RtwRoot 'local.settings.psd1')
}
# the NuGet packages whose tools the scripts call (the versions also appear in the Directory.Build.props files)
$RtwSdkPackage = 'Microsoft.Windows.SDK.CPP.10.0.26100.1'
$RtwWdkPackage = 'Microsoft.Windows.WDK.ARM64.10.0.26100.6584'
# the host's tool directories (an ARM64 host is untested)
$RtwHostArm64 = ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64')

# The value of a setting: the parameter's value if given, else the environment variable, else local.settings.psd1;
# $null if none. -Path: made absolute (relative to the repository root).
function Get-RtwSetting([string]$Name, [string]$Value, [switch]$Path) {
  if (-not $Value) { $Value = [Environment]::GetEnvironmentVariable($RtwSettingVars[$Name]) }
  if (-not $Value -and $RtwLocalSettings.ContainsKey($Name)) { $Value = $RtwLocalSettings[$Name] }
  if (-not $Value) { return $null }
  if ($Path) {
    if (-not [IO.Path]::IsPathRooted($Value)) { $Value = Join-Path $RtwRoot $Value }
    $Value = [IO.Path]::GetFullPath($Value).TrimEnd('\')
  }
  $Value
}

# A setting that must have a value (an error naming the parameter, variable and file otherwise).
function Get-RtwRequiredSetting([string]$Name, [string]$Value, [string]$What) {
  $v = Get-RtwSetting $Name $Value
  if (-not $v) {
    throw "$What is not given: use -$Name, the environment variable $($RtwSettingVars[$Name]) or $Name in local.settings.psd1."
  }
  $v
}

# The NuGet packages directory; checks that every package of packages.config is there.
function Get-RtwPackagesDir([string]$Value) {
  $dir = Get-RtwSetting PackagesDir $Value -Path
  if (-not $dir) { $dir = Join-Path $RtwRoot 'packages' }
  $missing = @(([xml](Get-Content (Join-Path $RtwRoot 'packages.config') -Raw)).packages.package |
    ForEach-Object { "$($_.id).$($_.version)" } | Where-Object { -not (Test-Path (Join-Path $dir $_)) })
  if ($missing) {
    throw ("WDK / Windows SDK NuGet packages not found in ${dir}: $($missing -join ', ').`n" +
      "Restore them into packages\ (nuget restore packages.config -PackagesDirectory packages), or give their " +
      "directory with -PackagesDir, the environment variable RTW89WIN_PACKAGES or local.settings.psd1.")
  }
  $dir
}

# Visual Studio 2022 or its Build Tools, with the MSVC ARM64 build tools.
function Get-RtwVsPath([string]$Value) {
  $vs = Get-RtwSetting VsPath $Value -Path
  if ($vs) {
    if (-not (Test-Path (Join-Path $vs 'VC\Tools\MSVC'))) { throw "no MSVC tools under $vs (VC\Tools\MSVC)" }
    return $vs
  }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found ($vswhere). Install Visual Studio 2022 or its Build Tools, or give the installation with -VsPath or RTW89WIN_VS."
  }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath |
    Select-Object -First 1
  if (-not $vs) {
    throw ("vswhere found no Visual Studio with the component 'MSVC ARM64 build tools' " +
      "(Microsoft.VisualStudio.Component.VC.Tools.ARM64). Add it in the Visual Studio Installer, or give -VsPath / RTW89WIN_VS.")
  }
  $vs
}

# The 64-bit MSBuild of that installation.
function Get-RtwMSBuild([string]$VsPath) {
  $arch = if ($RtwHostArm64) { 'arm64' } else { 'amd64' }
  $msbuild = Join-Path $VsPath "MSBuild\Current\Bin\$arch\MSBuild.exe"
  if (-not (Test-Path $msbuild)) { throw "64-bit MSBuild not found: $msbuild" }
  $msbuild
}

# The newest MSVC tool set of that installation (VC\Tools\MSVC\<version>).
function Get-RtwVcTools([string]$VsPath) {
  $vc = Get-ChildItem (Join-Path $VsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty FullName
  if (-not $vc) { throw "no MSVC tool set in $VsPath\VC\Tools\MSVC" }
  $vc
}
# The compiler directory for a target (x64, arm64) on this host, and the host's own tool directory.
function Get-RtwVcBin([string]$Vc, [string]$Target) {
  $h = if ($RtwHostArm64) { 'Hostarm64' } else { 'Hostx64' }
  Join-Path $Vc "bin\$h\$Target"
}
function Get-RtwVcHostBin([string]$Vc) {
  if ($RtwHostArm64) { Get-RtwVcBin $Vc 'arm64' } else { Get-RtwVcBin $Vc 'x64' }
}
# vcvars for user-mode programs that run on this PC (the tools-dev tests).
function Get-RtwVcVars([string]$VsPath) {
  $bat = if ($RtwHostArm64) { 'vcvarsarm64.bat' } else { 'vcvars64.bat' }
  $p = Join-Path $VsPath "VC\Auxiliary\Build\$bat"
  if (-not (Test-Path $p)) { throw "$bat not found: $p" }
  $p
}

# The LLVM bin directory (clang.exe, llvm-lib.exe, llvm-nm.exe; tested with LLVM 23.1.2).
function Get-RtwLlvmBin([string]$Value) {
  $bin = Get-RtwSetting LlvmBin $Value -Path
  if (-not $bin) {
    $clang = Get-Command clang.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($clang) { $bin = Split-Path $clang.Source }
    elseif (Test-Path (Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe')) { $bin = Join-Path $env:ProgramFiles 'LLVM\bin' }
  }
  if (-not $bin -or -not (Test-Path (Join-Path $bin 'clang.exe'))) {
    throw ("LLVM not found (clang.exe, llvm-lib.exe; tested with LLVM 23.1.2). Give its bin directory with -LlvmBin, " +
      "the environment variable RTW89WIN_LLVM or local.settings.psd1, or put clang.exe on PATH.")
  }
  $bin
}

# The upstream rtw89 sources (drivers/net/wireless/realtek/rtw89 of Linux; third_party\rtw89 has the files used).
function Get-RtwSrc([string]$Value) {
  $src = Get-RtwSetting RtwSrc $Value -Path
  if (-not $src) { $src = Join-Path $RtwRoot 'third_party\rtw89' }
  if (-not (Test-Path (Join-Path $src 'core.c'))) {
    throw "rtw89 sources not found in $src (core.c). Give the directory with -RtwSrc, RTW89WIN_RTW89_SRC or local.settings.psd1."
  }
  $src
}

# The firmware directory (not in git).
function Get-RtwFirmwareDir([string]$Value) {
  $dir = Get-RtwSetting FirmwareDir $Value -Path
  if (-not $dir) { $dir = Join-Path $RtwRoot 'firmware' }
  $dir
}
# Checks that the firmware files are there (an error otherwise) and compares them with the tested SHA256 of the
# repository's SHA256SUMS file (a warning when they differ: the build goes on, but that firmware is untested).
function Assert-RtwFirmware([string]$Dir, [string[]]$Names, [string]$SumsFile) {
  $tested = @{}
  if (Test-Path $SumsFile) {
    foreach ($line in Get-Content $SumsFile) {
      if ($line -match '^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$') { $tested[$Matches[2]] = $Matches[1].ToLower() }
    }
  }
  foreach ($n in $Names) {
    $f = Join-Path $Dir $n
    if (-not (Test-Path $f)) {
      throw ("firmware file not found: $f`nThe firmware is not part of this repository. Get it from linux-firmware " +
        "(see the README and SHA256SUMS in the repository's firmware directory) and put it there, or give the " +
        "directory with -FirmwareDir, the environment variable RTW89WIN_FIRMWARE or local.settings.psd1.")
    }
    $leaf = Split-Path $n -Leaf
    $hash = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    if ($tested.ContainsKey($leaf) -and $tested[$leaf] -ne $hash) {
      Write-Warning "$f has SHA256 $hash, not the tested $($tested[$leaf]) ($SumsFile): untested firmware"
    }
  }
}

# The test-signing certificate (with its private key) in Cert:\CurrentUser\My.
function Get-RtwSigningCert([string]$Value) {
  $thumb = Get-RtwSetting CertThumbprint $Value
  if (-not $thumb) {
    throw ("No test-signing certificate. Give -CertThumbprint <thumbprint of a code signing certificate in " +
      "Cert:\CurrentUser\My> (tools-dev\new-testcert.ps1 makes one), set RTW89WIN_CERT or CertThumbprint in " +
      "local.settings.psd1, or build an unsigned package with -NoSign.")
  }
  $thumb = ($thumb -replace '[\s:]', '').ToUpper()
  $cert = Get-Item "Cert:\CurrentUser\My\$thumb" -ErrorAction SilentlyContinue
  if (-not $cert) { throw "certificate $thumb not found in Cert:\CurrentUser\My" }
  if (-not $cert.HasPrivateKey) { throw "certificate $thumb has no private key" }
  $cert
}

# signtool (Windows SDK package) and inf2cat (WDK package).
function Get-RtwSignTool([string]$Packages) {
  $arch = if ($RtwHostArm64) { 'arm64' } else { 'x64' }
  $p = Join-Path $Packages "$RtwSdkPackage\c\bin\10.0.26100.0\$arch\signtool.exe"
  if (-not (Test-Path $p)) { throw "signtool.exe not found: $p" }
  $p
}
function Get-RtwInf2Cat([string]$Packages) {
  $p = Join-Path $Packages "$RtwWdkPackage\c\bin\10.0.26100.0\x86\inf2cat.exe"
  if (-not (Test-Path $p)) { throw "inf2cat.exe not found: $p" }
  $p
}

# The package directory as a full path: -OutDir if given, else out\<prefix><timestamp>. Stops when it exists and is
# not empty. The build scripts call it before the compilation, so a wrong -OutDir stops the build at once.
function Resolve-RtwOutDir([string]$OutDir, [string]$Prefix) {
  if (-not $OutDir) { $OutDir = Join-Path $RtwRoot ('out\' + $Prefix + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
  elseif (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path (Get-Location) $OutDir }
  if ((Test-Path $OutDir) -and (Get-ChildItem $OutDir -Force | Select-Object -First 1)) {
    throw "output directory $OutDir exists and is not empty"
  }
  [IO.Path]::GetFullPath($OutDir)
}

# A new, empty package directory (the same checks as Resolve-RtwOutDir).
function New-RtwOutDir([string]$OutDir, [string]$Prefix) {
  $OutDir = Resolve-RtwOutDir $OutDir $Prefix
  New-Item -ItemType Directory -Force $OutDir | Out-Null
  (Resolve-Path $OutDir).Path
}

# SHA256SUMS of every file in a package directory ("<sha256>  <name>", what the install scripts check).
function Write-RtwSums([string]$Dir) {
  Get-ChildItem $Dir -File | Where-Object Name -ne 'SHA256SUMS' | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
  } | Set-Content (Join-Path $Dir 'SHA256SUMS')
  Get-Content (Join-Path $Dir 'SHA256SUMS')
}
