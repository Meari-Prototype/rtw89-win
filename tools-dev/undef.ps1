# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# List the symbols build\ARM64\Release\rtw89hal.lib references but does not define: what driver\os.c (rtwk_*)
# and the kernel (memcpy, strlen, ...) have to provide when the driver links it.
#   .\tools-dev\undef.ps1            names
#   .\tools-dev\undef.ps1 -Where     names with the objects that reference them
#   -LlvmBin <dir>: LLVM's bin directory (llvm-nm.exe); default as buildhal.ps1 (tools-dev\buildenv.ps1)
param([switch]$Where, [string]$LlvmBin)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\buildenv.ps1"
$nm = Join-Path (Get-RtwLlvmBin $LlvmBin) 'llvm-nm.exe'
$lib = "$PSScriptRoot\..\build\ARM64\Release\rtw89hal.lib"
$def = @{}
$ref = @{}
$obj = ''
foreach ($line in & $nm $lib 2>$null) {
  if ($line -match '^(.+\.obj):$') { $obj = $Matches[1]; continue }
  if ($line -match '^\s+U (\S+)$') { if (-not $ref[$Matches[1]]) { $ref[$Matches[1]] = @() }; $ref[$Matches[1]] += $obj }
  elseif ($line -match '^[0-9a-f]+ [A-Z] (\S+)$') { $def[$Matches[1]] = $true }
}
$ref.Keys | Where-Object { -not $def[$_] } | Sort-Object | ForEach-Object {
  if ($Where) { "{0,-40} {1}" -f $_, (($ref[$_] | Sort-Object -Unique) -join ' ') } else { $_ }
}
