# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build mac\apframe.c (the group owner's beacon/probe/association frames, pure functions) as a user-mode program
# for this PC and run its checks (tools-dev\apframe-test.c). Output in build\apframe-test\.
# MSVC from Visual Studio 2022 / Build Tools (vswhere; -VsPath or RTW89WIN_VS to choose, see tools-dev\buildenv.ps1).
#   .\tools-dev\apframe-test.ps1 [-VsPath <dir>]
param([string]$VsPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\buildenv.ps1"
$vcvars = Get-RtwVcVars (Get-RtwVsPath $VsPath)
$out = "$root\build\apframe-test"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vcvars`" >nul 2>nul && cl /nologo /W4 /WX /DRTW_APFRAME_HOST /Fo`"$out\\`" /Fe`"$out\apframe-test.exe`" " +
       "`"$root\mac\apframe.c`" `"$root\tools-dev\apframe-test.c`""
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
& "$out\apframe-test.exe"
if ($LASTEXITCODE) { throw 'apframe test failed' }
