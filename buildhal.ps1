# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build the Linux-side code with clang (LLVM; tested with 23.1.2) into build\ARM64\Release\rtw89hal.lib:
# the rtw89 subset (unmodified upstream sources, third_party\rtw89 by default), compat\*.c and mac\*.c, against
# compat\include.
#   .\buildhal.ps1                 build everything, then the library
#   .\buildhal.ps1 -Only core,pci  compile just these (object names, see below), no library
#   .\buildhal.ps1 -ExtraFlags '-Wframe-larger-than=1024'   add compiler flags (checks; the library is still built)
#   -LlvmBin <dir>   LLVM's bin directory; -RtwSrc <dir>   the rtw89 sources (another Linux tree, e.g. for a new chip)
#   (defaults and the RTW89WIN_* environment variables: tools-dev\buildenv.ps1)
# Object names: upstream base names (core, pci, ...); compat\ and mac\ files get their directory as a prefix
# (compat_glue, mac_hw), because upstream has util.c, mac.c, ... as well.
# Per-file compiler output: build\ARM64\Release\hal\<object name>.log
param([string[]]$Only, [string[]]$ExtraFlags, [int]$ShowErrors = 15, [string]$LlvmBin, [string]$RtwSrc)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. "$root\tools-dev\buildenv.ps1"
$llvm = Get-RtwLlvmBin $LlvmBin
$up = Get-RtwSrc $RtwSrc
$out = "$root\build\ARM64\Release\hal"
New-Item -ItemType Directory -Force $out | Out-Null

# rtw89_core + rtw89_pci + rtw89_8852b_common + rtw89_8852b + rtw89_8852be (Makefile), without wow.o/debug.o
$upstream = 'core mac80211 mac mac_be phy phy_be fw cam efuse efuse_be regd sar coex ps chan ser acpi util ' +
            'pci pci_be rtw8852b_common rtw8852b rtw8852b_table rtw8852b_rfk rtw8852b_rfk_table rtw8852be'
$srcs = @($upstream -split ' ' | ForEach-Object { [pscustomobject]@{ Src = "$up\$_.c"; Name = $_ } })
foreach ($dir in 'compat', 'mac') {
  $srcs += @(Get-ChildItem "$root\$dir\*.c" -EA 0 | ForEach-Object { [pscustomobject]@{ Src = $_.FullName; Name = "${dir}_$($_.BaseName)" } })
}
if ($Only) { $srcs = @($srcs | Where-Object { $Only -contains $_.Name }) }

# The MinGW target is used (not -windows-msvc) because only it can lay out bitfields like GCC (-mno-ms-bitfields);
# rtw89 overlays bitfield structs on firmware and efuse data. For C on ARM64 the two targets share the calling
# convention, LLP64 types, COFF relocations and x18 reservation, so the objects link into the MSVC-built driver.
# Like the arm64 kernel: no FP/SIMD registers, no strict aliasing, wrapping signed overflow.
$flags = @(
  '--target=aarch64-w64-mingw32', '-c', '-O2', '-gcodeview', '-std=gnu11', '-ffreestanding', '-fno-common',
  '-mno-ms-bitfields', '-mgeneral-regs-only', '-fms-extensions', '-fno-auto-import', '-fno-stack-protector',
  '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks', '-ffunction-sections', '-fdata-sections',
  '-ferror-limit=0', '-Wall',
  '-Wno-pointer-arith', '-Wno-gnu', '-Wno-microsoft', '-Wno-unused-parameter', '-Wno-sign-compare',
  '-Wno-missing-field-initializers', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
  '-Wno-format', '-Wno-implicit-fallthrough', '-Wno-address-of-packed-member', '-Wno-pointer-sign',
  '-Wno-incompatible-pointer-types-discards-qualifiers', '-Wno-tautological-constant-out-of-range-compare',
  '-Wno-c23-extensions', '-Wno-missing-braces', '-Wno-unused-const-variable',
  '-D__KERNEL__', '-DCONFIG_RTW89_8852BE=1', '-DCONFIG_RTW89_PCI=1',
  '-I', "$root\compat\include", '-I', $up
) + @($ExtraFlags | Where-Object { $_ })

$results = $srcs | ForEach-Object -ThrottleLimit 8 -Parallel {
  $src = $_.Src
  $name = $_.Name
  $obj = "$using:out\$name.obj"
  $log = "$using:out\$name.log"
  $t = [Diagnostics.Stopwatch]::StartNew()
  $text = & "$using:llvm\clang.exe" @using:flags $src -o $obj 2>&1 | Out-String
  $ok = ($LASTEXITCODE -eq 0)
  Set-Content $log $text -Encoding utf8
  [pscustomobject]@{ Name = $name; Ok = $ok; Sec = [math]::Round($t.Elapsed.TotalSeconds, 1)
    Errors = ([regex]::Matches($text, ': error:')).Count; Warnings = ([regex]::Matches($text, ': warning:')).Count; Log = $log }
}
$results | Sort-Object Ok, Name | Format-Table Name, Ok, Errors, Warnings, Sec -AutoSize | Out-String -Width 200
$bad = $results | Where-Object { -not $_.Ok }
foreach ($b in $bad | Select-Object -First 3) {
  "---- $($b.Name): first errors"
  Get-Content $b.Log | Select-String ': error:' | Select-Object -First $ShowErrors | ForEach-Object { $_.Line.Trim() }
}
if ($bad) { throw "$(@($bad).Count) file(s) failed" }
if (-not $Only) {
  & "$llvm\llvm-lib.exe" /nologo "/out:$root\build\ARM64\Release\rtw89hal.lib" ($srcs | ForEach-Object { "$out\$($_.Name).obj" })
  if ($LASTEXITCODE) { throw "llvm-lib failed ($LASTEXITCODE)" }
  "library: $root\build\ARM64\Release\rtw89hal.lib"
}
