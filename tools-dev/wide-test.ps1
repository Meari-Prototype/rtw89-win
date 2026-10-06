# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build mac\wide.c (the station's channel width and VHT capabilities, pure functions) as a user-mode program
# for this PC and run its checks (tools-dev\wide-test.c). Output in build\wide-test\.
# MSVC from Visual Studio 2022 / Build Tools (vswhere; -VsPath or RTW89WIN_VS to choose, see tools-dev\buildenv.ps1).
#   .\tools-dev\wide-test.ps1 [-VsPath <dir>]
param([string]$VsPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\buildenv.ps1"
$vcvars = Get-RtwVcVars (Get-RtwVsPath $VsPath)
$out = "$root\build\wide-test"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vcvars`" >nul 2>nul && cl /nologo /W4 /WX /DRTW_WIDE_HOST /Fo`"$out\\`" /Fe`"$out\wide-test.exe`" " +
       "`"$root\mac\wide.c`" `"$root\tools-dev\wide-test.c`""
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
& "$out\wide-test.exe"
if ($LASTEXITCODE) { throw 'wide test failed' }
