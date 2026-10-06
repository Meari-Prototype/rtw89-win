# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Summarize the compiler errors in buildhal.ps1's per-file logs (build\ARM64\Release\hal\<object name>.log): how often
# each message occurs, with its first location. Reads clang's "file:line:col: error: msg" and MSVC's
# "file(line,col): error: msg" (also "fatal error" and "error C1234:").
#   tools-dev\errsum.ps1 util [-Top 40]
param([Parameter(Mandatory)][string[]]$Name, [int]$Top = 40)
$dir = "$PSScriptRoot\..\build\ARM64\Release\hal"
foreach ($n in $Name) {
  $log = "$dir\$n.log"
  if (-not (Test-Path -LiteralPath $log)) { "== $n : no log ($log)"; continue }
  $errs = @(Get-Content -LiteralPath $log | ForEach-Object {
    if ($_ -match '^(.+?):(\d+):\d+: (?:fatal )?error: (.*)$' -or
        $_ -match '^(.+?)\((\d+)(?:,\d+)?\)\s*: (?:fatal )?error(?: [A-Z]+\d+)?: (.*)$') {
      [pscustomobject]@{ File = (Split-Path $Matches[1] -Leaf); Line = [int]$Matches[2]; Msg = $Matches[3].Trim() }
    }
  })
  "== $n : $($errs.Count) errors"
  $errs | Group-Object Msg | Sort-Object Count -Descending | Select-Object -First $Top |
    ForEach-Object { '{0,4}  {1}:{2}  {3}' -f $_.Count, $_.Group[0].File, $_.Group[0].Line, $_.Name }
}
