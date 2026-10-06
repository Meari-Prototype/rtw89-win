# Third-party licenses

[中文](../zh/licenses.md)

- **Readers**: License reviewers, and builders who give the driver package to other persons.
- **What you will learn**: The license, the source, and the obligations of the code of this project and of each third-party part. The limits of the firmware license.
- **Prerequisites**: None.

This document is an explanation. It is not a legal text. The legal texts are the original English files: [`LICENSE`](../../LICENSE), [`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES), and the license statement at the start of each file. If this document and the original texts are different, the original texts apply. This document does not tell you if a use complies with the licenses.

<a id="summary"></a>
## Summary

- The code of this project (not official) uses the BSD-3-Clause license. The copyright notice is `Copyright (c) 2026 Meari`.
- The 49 upstream rtw89 files are in `third_party\rtw89\` without changes. These files have a dual license, GPL-2.0 OR BSD-3-Clause. This project uses them under BSD-3-Clause.
- `compat\include\` contains 8 header files from FreeBSD. They use BSD-2-Clause or BSD-3-Clause.
- The regulatory rules data comes from wireless-regdb and from Linux `net/wireless/reg.c`. Both use the ISC license.
- The firmware is not in this repository. linux-firmware redistributes the firmware under the Realtek firmware license. This license permits redistribution and use only in binary form, without changes. You must not reverse engineer the firmware.
- The WDK, the Windows SDK, the Visual Studio Build Tools, and LLVM are not in this repository. The driver package does not contain them.
- The build scripts put `LICENSE`, `THIRD_PARTY_NOTICES`, and the firmware license into the driver package.

<a id="ours"></a>
## Code of this project

All files in this repository use BSD-3-Clause, except the third-party parts below. For the full license text, see [`LICENSE`](../../LICENSE).

The source files of this project have two lines in their first comment:

```text
SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2026 Meari
```

The comment mark depends on the file type: `//` for `.c`, `.cpp`, and `.cs`; `/* */` for `.h`; `#` for `.ps1`, `.psd1`, and `.sh` (in `.sh`, after the `#!` line); `;` for `.inf`; XML comments for `.vcxproj`, `.props`, `.targets`, and `packages.config`.

The files in these directories use BSD-3-Clause:

| Directory | Content | Exceptions |
|---|---|---|
| Root directory | `build.ps1`, `buildhal.ps1`, `packages.config`, `local.settings.example.psd1` | None |
| `driver\` | Driver layer and INF of the Wi-Fi driver | None |
| `wificx\` | WiFiCx front end | None |
| `mac\` | mac layer | `mac\regdb.h` uses ISC; the world rules data in `mac\reg.c` uses ISC (see [Regulatory rules data](#regdb)) |
| `compat\` | Compatibility layer | 8 FreeBSD header files (see [FreeBSD header files](#freebsd)) |
| `bt\` | Bluetooth filter driver and the `rtkbtfw` tool | None |
| `tools\`, `tools-dev\`, `hosttools\` | Scripts for installation, diagnosis, build, and tests | `tools-dev\regdb-gen.ps1` uses BSD-3-Clause, but the `mac\regdb.h` that it generates uses ISC |
| `docs\` | This document set | None |

The three conditions of BSD-3-Clause (summary; the text in [`LICENSE`](../../LICENSE) applies):

1. If you redistribute source code, keep the copyright notice, the list of conditions, and the disclaimer.
2. If you redistribute in binary form, copy the copyright notice, the list of conditions, and the disclaimer. Put them in the documentation or the other materials.
3. Without written permission, do not use the names of the copyright holder or contributors to endorse or promote derived products.

<a id="third-party"></a>
## Third-party parts

This table agrees with sections A to F of [`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES).

| Part | Location | Source and version | License | Obligations |
|---|---|---|---|---|
| A. Upstream rtw89 | `third_party\rtw89\` (49 files) | Linux, tag v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f` | GPL-2.0 OR BSD-3-Clause; this project uses BSD-3-Clause | Keep the file headers and the copyright notices; for binary redistribution, copy the BSD-3-Clause notice |
| B. FreeBSD LinuxKPI 802.11 header files | `compat\include\linux\`, `compat\include\net\` (6 files) | FreeBSD, commit `262fa4695d90207b9080f2cc5cb01eccf07fe243` | BSD-2-Clause | Keep the full license text at the start of each file; for binary redistribution, copy the notice |
| C. FreeBSD net80211 header files | `compat\include\net80211\` (2 files) | Same as above | BSD-2-Clause (`ieee80211.h`); BSD-3-Clause (`ieee80211_radiotap.h`) | Same as above |
| D. Regulatory rules data | `mac\regdb.h`; `reg_world_builtin` in `mac\reg.c` | wireless-regdb 2025.10.07; Linux v7.2 `net/wireless/reg.c` | ISC | Keep the copyright notice and the license notice in all copies |
| E. Reference materials only | No files | Linux mac80211, cfg80211, kernel header files, btrtl; Microsoft Windows-driver-samples; RFC 4493; IEEE Std 802.11-2020 | This project does not copy code | None |
| F. Items that are not included | Not in this repository | Firmware; WDK, Windows SDK, Visual Studio Build Tools, LLVM | Their own licenses | For the firmware, see [Firmware license](#firmware) |

<a id="rtw89"></a>
## Upstream rtw89

- **Source**: The Linux repository `https://github.com/torvalds/linux.git`, tag v7.2, commit `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`, path `drivers/net/wireless/realtek/rtw89/`.
- **Files**: The 26 `.c` files that `buildhal.ps1` compiles, and the 23 `.h` files that they include, 49 files in total. This project does not change any of these files.
- **License**: The first line of each file is `SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause`. This project uses the files under BSD-3-Clause. The two license texts are in `third_party\rtw89\LICENSES\`: [`BSD-3-Clause`](../../third_party/rtw89/LICENSES/BSD-3-Clause) and [`GPL-2.0`](../../third_party/rtw89/LICENSES/GPL-2.0).
- **Copyright**: Copyright (c) 2019-2026 Realtek Corporation. For the year of each file, see the file header.
- **Other use**: `driver\hw.c` uses register offsets from the rtw89 `reg.h`.

[`third_party\rtw89\README.md`](../../third_party/rtw89/README.md) lists the git blob id of each file. Use this procedure to make sure that the files are not changed.

On a computer with git, in the root directory of the repository:

1. Run `git hash-object` to calculate the blob id of a file.

   ```sh
   git hash-object third_party/rtw89/core.c
   ```

   **Result**: The 40-digit hexadecimal value is the same as the blob id of the same file in `third_party\rtw89\README.md`.

<a id="freebsd"></a>
## FreeBSD header files

rtw89 compiles against the Linux 802.11 interfaces. This project takes these interface header files from FreeBSD LinuxKPI, not from Linux. `compat\FREEBSD_REV` records the source commit: `262fa4695d90207b9080f2cc5cb01eccf07fe243`.

| File | FreeBSD path | License | Changed |
|---|---|---|---|
| `compat\include\linux\ieee80211.h` | `sys/compat/linuxkpi/common/include/linux/ieee80211.h` | BSD-2-Clause | Yes |
| `compat\include\linux\nl80211.h` | `sys/compat/linuxkpi/common/include/linux/nl80211.h` | BSD-2-Clause | No |
| `compat\include\net\cfg80211.h` | `sys/compat/linuxkpi/common/include/net/cfg80211.h` | BSD-2-Clause | No |
| `compat\include\net\ieee80211_radiotap.h` | `sys/compat/linuxkpi/common/include/net/ieee80211_radiotap.h` | BSD-2-Clause | No |
| `compat\include\net\mac80211.h` | `sys/compat/linuxkpi/common/include/net/mac80211.h` | BSD-2-Clause | Yes |
| `compat\include\net\regulatory.h` | `sys/compat/linuxkpi/common/include/net/regulatory.h` | BSD-2-Clause | No |
| `compat\include\net80211\ieee80211.h` | `sys/net80211/ieee80211.h` | BSD-2-Clause | No |
| `compat\include\net80211\ieee80211_radiotap.h` | `sys/net80211/ieee80211_radiotap.h` | BSD-3-Clause | No |

- This project does not change the copyright notice and the full license text at the start of each file.
- In the 6 LinuxKPI files, a comment after the license block gives the source path and the commit.
- In the two changed files, `rtw89win` marks each change. The copyright of these changes is Copyright (c) 2026 Meari, under the same BSD-2-Clause terms.
- Copyright holders: The FreeBSD Foundation and Bjoern A. Zeeb for the LinuxKPI header files (`net\ieee80211_radiotap.h`: The FreeBSD Foundation only). Atsushi Onoe, Sam Leffler (Errno Consulting), and The FreeBSD Foundation for `net80211\ieee80211.h`. David Young and The FreeBSD Foundation for `net80211\ieee80211_radiotap.h`. For the years, see each file.

<a id="regdb"></a>
## Regulatory rules data

| Data | Location | Source | License |
|---|---|---|---|
| Regulatory rules of each country | `mac\regdb.h` | `regulatory.db` of wireless-regdb 2025.10.07 (SHA256 `5560f4f0fdac7d1bb2adf8d4d083f39e3bee5ba55192feadadc091df55a813eb`); `tools-dev\regdb-gen.ps1` converts it | ISC |
| Built-in world rules | `reg_world_builtin` in `mac\reg.c` | The 2.4 GHz and 5 GHz rules of `world_regdom` in Linux v7.2 `net/wireless/reg.c` | ISC |

- The file header of `mac\regdb.h` contains the wireless-regdb copyright notice and the full ISC license text.
- Section D of [`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES) lists the copyright holders of `world_regdom`.
- The condition of the ISC license: keep the copyright notice and this license notice in all copies.

<a id="references"></a>
## Reference materials only

This project uses the materials below only to learn the behavior and the interfaces. This project does not copy their code.

| Material | License | How this project uses it |
|---|---|---|
| Linux mac80211, cfg80211 (`net/mac80211`, `net/wireless`), and the Linux kernel header files | GPL-2.0 and others | This project has its own implementation of the mac layer and the compatibility layer. The compatibility layer supplies the Linux kernel interfaces (names, structure layouts, macros, and function signatures), because unmodified upstream rtw89 must compile against them. |
| Linux btrtl (`drivers/bluetooth/btrtl.c`) | GPL-2.0-or-later | `bt\rtkbtfw\epatch.c` contains the btrtl table of project ids and LMP subversions, and the patch file rules, as facts necessary for interoperability. This project has its own implementation of the Bluetooth patch download code. |
| Microsoft Windows-driver-samples (`network/wlan/wificx`, `network/wlan/WDI`, `network/netadaptercx`) | MS-PL | The WiFiCx front end (`wificx\`) follows the structure of the WiFiCx sample. It does not contain sample code. |
| RFC 4493 (AES-CMAC), IEEE Std 802.11-2020 Annex J.6.4 (CCMP) | Standard texts | The known-answer test vectors in `driver\crypto.c` come from these two documents. |

<a id="firmware"></a>
## Firmware license

This repository does not contain firmware. The driver needs 3 firmware files: `rtw8852b_fw-2.bin`, `rtl8852bu_fw.bin`, and `rtl8852bu_config.bin`. The tested files come from a specified commit of linux-firmware. For the commit ID, the procedure to get the files, and the SHA256 values, see [Firmware files](../../firmware/README.en.md).

The firmware license is [`LICENCE.rtlwifi_firmware.txt`](../../firmware/LICENCE.rtlwifi_firmware.txt) (Copyright (c) 2010, Realtek Semiconductor Corporation). This is a summary of its terms (the original English text applies):

1. The license permits redistribution and use only in binary form, without changes.
2. If you redistribute, copy the copyright notice and the disclaimer into the documentation or the other materials.
3. Without written permission, do not use the name of Realtek or its suppliers to endorse or promote derived products.
4. Do not reverse engineer, decompile, or disassemble the software.
5. The patent license has a limited scope. It covers only the patents that are necessary for two uses. The first use is the software alone. The second use is the software with an operating system under an OSI-approved open-source license. The original text states: "The patent license shall not apply to any other combinations which include this software." The driver of this project loads this firmware on Windows.
6. Realtek supplies the software without a warranty of any kind.

This document does not interpret clause 5. Read the original license text.

This project does only two operations on the firmware:

- It copies the firmware files without changes into the driver package. The driver downloads them to the chip.
- The Bluetooth filter driver reads the patch file header with the btrtl container format. Then it sends the selected patch to the controller without changes.

This project does not change or disassemble the firmware.

<a id="not-included"></a>
## Items that are not included

| Item | Description |
|---|---|
| Firmware | `rtw8852b_fw-2.bin`, `rtl8852bu_fw.bin`, `rtl8852bu_config.bin`. Get them from linux-firmware (see [Firmware files](../../firmware/README.en.md)). `firmware\SHA256SUMS` and `firmware\rtl_bt\SHA256SUMS` record the tested SHA256 values. |
| WDK and Windows SDK | Get them from Microsoft. `packages.config` lists the necessary NuGet packages and versions. At build time, the driver links the WDK libraries `WificxTLVGenParse.lib`, `libcntpr.lib`, `ksecdd.lib`, and `usbd.lib`. |
| Visual Studio Build Tools, LLVM | Get them from their publishers. |
| Compiled driver packages | This project does not supply them. For the build procedure, see [Build](build.md). |

<a id="redistribute"></a>
## When you redistribute the driver package

A driver package that you build contains three types of content. These are the binaries of this project (BSD-3-Clause), third-party code in these binaries (for example upstream rtw89), and the Realtek firmware. For the conditions of each license, see the sections above.

The build scripts copy `LICENSE`, `THIRD_PARTY_NOTICES`, and `LICENCE.rtlwifi_firmware.txt` into the driver package. `SHA256SUMS` lists all three files, and the install script checks them.

Before you give the driver package to other persons, do these steps. Do them on the build PC (PowerShell, in the root directory of the repository):

1. Make sure that the driver package contains the three license files.

   ```powershell
   Test-Path <driver package directory>\LICENSE, <driver package directory>\THIRD_PARTY_NOTICES, <driver package directory>\LICENCE.rtlwifi_firmware.txt
   ```

   `<driver package directory>`: The directory that the build script made, for example `out\<time>`.

   **Result**: The output is three lines of `True`.

2. Calculate the SHA256 of the firmware files in the driver package.

   ```powershell
   Get-FileHash -Algorithm SHA256 <driver package directory>\*.bin
   ```

   **Result**: The hash of each `.bin` file is the same as the value for the same file name in `firmware\SHA256SUMS` or `firmware\rtl_bt\SHA256SUMS`.

<a id="trademarks"></a>
## Names and statement of no official status

- This project is not official. It has no relation to Realtek, Microsoft, or Radxa, and they do not approve or support it.
- Realtek, Microsoft, Windows, Radxa, and ROCK are names or trademarks of their owners. This documentation uses these names only to identify the hardware and software environment.
- The provider (`Provider`) of both INFs is `rtw89win`. The device descriptions (`DeviceDesc`) in the two INFs are "Realtek RTL8852BE Wi-Fi (rtw89win)" and "Realtek RTL8852BE Bluetooth patch loader (rtkbtflt)". In these descriptions, "Realtek" is the device manufacturer, as is usual in an INF. The name in parentheses shows that the driver comes from this project.

For the terms, see the [Glossary](glossary.md).
