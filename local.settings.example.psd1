# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Example of local.settings.psd1: copy it to local.settings.psd1 in the repository root (git ignores that file) and
# keep only the lines you need. The build scripts read it through tools-dev\buildenv.ps1; a script parameter or an
# RTW89WIN_* environment variable overrides a value here. Relative paths are relative to the repository root.
@{
  # NuGet packages of packages.config (WDK, Windows SDK). Default: packages
  # PackagesDir    = 'packages'
  # LLVM bin directory (clang.exe, llvm-lib.exe, llvm-nm.exe). Default: clang.exe on PATH, else %ProgramFiles%\LLVM\bin
  # LlvmBin        = 'C:\Program Files\LLVM\bin'
  # upstream rtw89 sources. Default: third_party\rtw89
  # RtwSrc         = 'third_party\rtw89'
  # firmware: rtw8852b_fw-2.bin and rtl_bt\rtl8852bu_fw.bin, rtl_bt\rtl8852bu_config.bin. Default: firmware
  # FirmwareDir    = 'firmware'
  # thumbprint of your code signing certificate in Cert:\CurrentUser\My (tools-dev\new-testcert.ps1). No default.
  # CertThumbprint = '0123456789ABCDEF0123456789ABCDEF01234567'
  # Visual Studio / Build Tools installation. Default: found with vswhere
  # VsPath         = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
  # hosttools\ (PC-side measurements): the board's WLAN address and its host name or alias for ssh. No default.
  # BoardWlan      = '192.0.2.10'
  # SshHost        = 'board'
}
