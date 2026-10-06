# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Build driver\crypto.c as a user-mode program for this PC (RTW_CRYPTO_USER: bcrypt.dll instead of ksecdd.sys) and
# run its known-answer tests (RFC 4493 AES-CMAC, IEEE 802.11-2020 J.6.4 CCMP). Output in build\crypto-test\.
# MSVC from Visual Studio 2022 / Build Tools (vswhere; -VsPath or RTW89WIN_VS to choose, see tools-dev\buildenv.ps1).
#   .\tools-dev\crypto-test.ps1 [-VsPath <dir>]
param([string]$VsPath)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$PSScriptRoot\buildenv.ps1"
$vcvars = Get-RtwVcVars (Get-RtwVsPath $VsPath)
$out = "$root\build\crypto-test"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vcvars`" >nul 2>nul && cl /nologo /W4 /WX /DRTW_CRYPTO_USER /Fo`"$out\\`" /Fe`"$out\crypto-test.exe`" " +
       "`"$root\driver\crypto.c`" `"$root\tools-dev\crypto-test.c`" bcrypt.lib"
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
& "$out\crypto-test.exe"
if ($LASTEXITCODE) { throw 'self-test failed' }
