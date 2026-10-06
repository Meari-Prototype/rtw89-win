# Porting guide

[中文](../zh/porting.md)

- **Readers**: Porters. This includes persons who connect other rtw89 chips, other hardware platforms, or other Linux Wi-Fi drivers to Windows.
- **What you will do**: Find the files to change when you add an rtw89 PCIe chip, change the hardware platform, or change the driver family. Find how to verify the result.
- **Prerequisites**: You read [Architecture](architecture.md). You can compile a driver package with the procedure in [Build](build.md).

<a id="summary"></a>
## Summary

1. Do not change upstream rtw89. To add a chip, only add the upstream source files.
2. To add an rtw89 PCIe chip, change `buildhal.ps1`, the hardware IDs in the INF and scripts, and the firmware files. Also change the INTx mask registers and the reported capabilities. If the new chip files use Linux APIs that the compatibility layer does not have, add them there. If one build supports more than one chip, also change the driver registration in the compatibility layer.
3. This project wrote the mac layer for the RTL8852B. For a new chip, check the 15 assumptions one by one (see [mac layer assumptions](#mac-assumptions)).
4. At this time, one build supports only one chip. The registration table for more than one chip is not implemented.
5. USB and SDIO are not implemented and not tested.
6. The tests verified this guide fully only on RTL8852BE. The steps for other chips are inferred from code. No test on a target device verified them.

<a id="boundaries"></a>
## Interface limits of each layer

Conclusion: When you add an rtw89 chip, the interfaces of the driver layer and the WiFiCx front end do not change. The changes are in the entry points of the compatibility layer, the mac layer assumptions, and the capability report. For the full interface table, see [Interfaces between layers](architecture.md#interfaces).

| Layer | Interface up | Interface down | Add an rtw89 PCIe chip | Change the driver family |
|---|---|---|---|---|
| Driver layer | Diagnostic IOCTLs; settings | Implements `rtwk_*` (`os.h`); calls `rtw_linux_*` (`glue.h`) | No change: the PCI device, BAR2, and INTx are the same | USB or SDIO needs new bus primitives |
| WiFiCx front end | WDI messages | `rtw_linux_*` (`glue.h`) | Check the capabilities and the maximum link rate | No change if `glue.h` does not change |
| mac layer | `rtw_linux_*` | `ieee80211_ops` | Check against the assumption table | Add txq, software scan, and other items |
| Upstream driver | `ieee80211_ops` | Linux kernel API | Add the source files without changes | Replace with the source files of the new driver |
| Compatibility layer | Linux kernel API | `rtwk_*`; entry points such as `rtw_linux_probe` | INTx registers; Linux APIs that the new chip files use and the compatibility layer does not have; driver registration for more than one chip | Add the Linux APIs that are not available |

Both clang and MSVC use the structures in `glue.h`. When you change a structure, change the static assertions on both sides (`driver\diag.c`, `wificx\wifi.h`, `mac\hw.c`, and others).

The two code locations below use internal rtw89 functions and structures directly. When you change the driver family, replace them:

- `mac\iface.c` `rtw_linux_ser_trigger`: It calls `rtw89_fw_h2c_trigger_cpu_exception` for `rtwdiag.ps1 ser`.
- `compat\rtw89info.c`: It reads the internal rtw89 state for `IOCTL_RTW_RADIO` and `IOCTL_RTW_BTC`.

<a id="add-chip"></a>
## Add an rtw89 PCIe chip

Conclusion: First, check the differences between the new chip and RTL8852B. Then change the build, the driver package, and the chip-specific code. Last, verify the result with the [Test procedure](#testing).

### Check the differences of the new chip

In `drivers/net/wireless/realtek/rtw89/` of upstream Linux v7.2, read the `rtw89_chip_info` of the new chip in the chip file (for example `rtw8852c.c`). Also read the PCI information (for example `rtw8852ce.c`). The fields in this table set the quantity of work.

| Field | RTL8852B value | When the new chip has a different value |
|---|---|---|
| `chip_gen` | `RTW89_CHIP_AX` | BE generation (for example 8922A): check the INTx registers and the registers in `driver\hw.c` again |
| `hw_sec_hdr` | `false` | `true` (8852C, 8922A): the mac layer must not write the CCMP header itself; see assumptions 5 and 6 |
| `hw_mgmt_tx_encrypt` | `false` | `true` (8852C, 8922A): the behavior of the current protected management frames (PMF) code is not determined; see assumption 8 |
| `support_bands` | 2.4 GHz, 5 GHz | Includes 6 GHz (for example 8852C): the mac layer disables 6 GHz, and the capability structure has only two bands |
| `support_bandwidths` | 20, 40, 80 MHz | Includes 160 MHz (for example 8852C): the mac layer and the capability report support only up to 80 MHz |
| `disable_intr` in the PCI information | `rtw89_pci_disable_intr` | 8852CE uses `_v1` and 8922AE uses `_v2`: change the INTx registers |
| `fw_basename`, `fw_format_max` | `rtw89/rtw8852b_fw`, `2` | New firmware file name |

For 8851BE, 8852AE, and 8852BTE, `hw_sec_hdr` and `hw_mgmt_tx_encrypt` are also `false`, and `disable_intr` is also `rtw89_pci_disable_intr`. These chips need the fewest changes.

### Add the upstream source files

The `Makefile` and `Kconfig` of upstream Linux v7.2 define the file group of each chip. These two files are not in this repository.

| Chip | File group (base names of the `.c` files) | `.h` files to add | Macro |
|---|---|---|---|
| RTL8852CE | `rtw8852c`, `rtw8852c_table`, `rtw8852c_rfk`, `rtw8852c_rfk_table`, `rtw8852ce` | `rtw8852c.h`, `rtw8852c_table.h`, `rtw8852c_rfk.h`, `rtw8852c_rfk_table.h` | `CONFIG_RTW89_8852CE` |
| RTL8852BE-VT | `rtw8852bt`, `rtw8852bt_rfk`, `rtw8852bt_rfk_table`, `rtw8852bte`; also `rtw8852b_common` | `rtw8852bt.h`, `rtw8852bt_rfk.h`, `rtw8852bt_rfk_table.h` | `CONFIG_RTW89_8852BTE` |
| RTL8851BE | `rtw8851b`, `rtw8851b_table`, `rtw8851b_rfk`, `rtw8851b_rfk_table`, `rtw8851be` | `rtw8851b.h`, `rtw8851b_table.h`, `rtw8851b_rfk.h`, `rtw8851b_rfk_table.h` | `CONFIG_RTW89_8851BE` |

The other headers that these `.c` files include (`core.h`, `pci.h`, and others) are already in `third_party\rtw89\`.

The start of `third_party\rtw89\README.md` records the upstream source (the repository URL and the full commit ID).

On the build PC:

1. Copy the `.c` and `.h` files in the table from the rtw89 directory of that commit to `third_party\rtw89\`.
2. Calculate the blob id of each new file.

   ```powershell
   git hash-object third_party\rtw89\<file name>
   ```

3. In the file table of `third_party\rtw89\README.md`, add one row for each new file. Each row has the file name, the size in bytes, and the blob id.
4. Update the file count in these three locations. The current values are 26 `.c` files and 23 `.h` files, 49 files in total.

   - The text and the last line of `third_party\rtw89\README.md`.
   - The "Build structure" section of [Architecture](architecture.md#build-structure).
   - The "Upstream rtw89" section of [Third-party licenses](licenses.md#rtw89).

You can also use the files without a copy. At build time, use `-RtwSrc` to specify the rtw89 directory in a full Linux source tree.

> **Note**: Use new files from the same Linux version as the current files. If you change the version, replace all of `third_party\rtw89\`.

### Change the build

On the build PC:

1. In `buildhal.ps1`, add the base names of the new chip to the `$upstream` list.
2. If the new build supports only the new chip, remove `rtw8852b`, `rtw8852b_table`, `rtw8852b_rfk`, `rtw8852b_rfk_table`, and `rtw8852be` from `$upstream`.
3. If the new chip is not RTL8852BE-VT, also remove `rtw8852b_common` from `$upstream`.
4. In `$flags` of `buildhal.ps1`, replace `-DCONFIG_RTW89_8852BE=1` with `-DCONFIG_RTW89_<chip>=1`. Keep more than one such macro only if one build supports more than one chip.
5. Run this command:

   ```powershell
   .\buildhal.ps1
   ```

   **Result**: `Errors` is 0 for each object file. The last line is `library: ...\build\ARM64\Release\rtw89hal.lib`.
6. If compile errors occur, show a summary of the errors for each message:

   ```powershell
   .\tools-dev\errsum.ps1 <object file name>
   ```

7. If the error is an undeclared `__rtw_kcfg_CONFIG_...`, add a row for that option to the `IS_ENABLED` table in `compat\include\rtw\base.h`.
8. If a Linux macro, enum, or function is not in the compatibility layer, add it to a header in `compat\include\`. Then do the procedure again from step 5.

`<chip>`: The chip name in Kconfig, for example `8852CE`. `<object file name>`: The `Name` column in the output table of `buildhal.ps1`, for example `rtw8852c`.

At the end of its output, `buildhal.ps1` shows the first errors of the first 3 object files that failed (`---- <object file name>: first errors`). The full compiler output of each object file is in `build\ARM64\Release\hal\<object file name>.log`.

The upstream v7.2 rtw89 source files do not use `IS_ENABLED`. Step 7 occurs only when the compatibility layer or the mac layer uses a new option.

The RTL8852CE file `rtw8852ce.c` uses `DMI_PRODUCT_SKU`. The `enum dmi_field` in `compat\include\rtw\bus.h` already has this item. The compatibility layer functions `dmi_first_match` and `dmi_check_system` always return no match. For this reason, these DMI table entries have no effect. The definitions that other chips need are not checked one by one.

### Register the PCI driver

`module_pci_driver` in `compat\include\rtw\bus.h` defines only one global pointer, `rtwk_pci_driver`. `rtw_linux_probe` in `compat\glue.c` searches for the PCI ID only in the `id_table` of this driver.

- One build supports one chip: No change is necessary. The `*e.c` file of the new chip registers itself through `module_pci_driver`.
- One build supports more than one chip: Change `module_pci_driver` so that it adds an entry to a registration table. Change `rtw_linux_probe` and `rtw_linux_remove` so that they use the PCI ID to find the driver in the table. This part is not implemented.

### Change the driver package

The firmware file name rule: rtw89 first requests `<fw_basename>-<fw_format_max>.bin`. If this request fails, rtw89 decreases the format number one step at a time. Last, it requests `<fw_basename>.bin`. `rtwk_firmware_load` uses only the part of the file name after the last `/`. It reads the file from the directory of the driver in the driver store. For this reason, the INF must copy the firmware to the same directory as the driver files.

On the build PC:

1. In the `[Realtek.NTARM64.10.0...26100]` section of `driver\rtw89win.inf`, add the new hardware ID:

   ```ini
   %Rtw89.DeviceDesc% = Rtw89_Device, PCI\VEN_10EC&DEV_<device ID>
   ```

2. If the device name is different, change `Rtw89.DeviceDesc` in the `[Strings]` section.
3. Add the firmware file name to the `[SourceDisksFiles]` section and to the `[Rtw89_CopyFiles]` section.
4. In `build.ps1`, change the firmware file names that `Assert-RtwFirmware` and `Copy-Item` use.
5. In `firmware\SHA256SUMS`, add a line for the new firmware.
6. In these scripts, change `PCI\VEN_10EC&DEV_B852` to the new hardware ID: `tools\install-rtw89win.ps1`, `tools\uninstall-rtw89win.ps1`, `tools\rtwdiag.ps1`.
7. Make the same change in these scripts in `tools-dev\board\`: `rtw89win-disen.ps1`, `rtw89win-guardsim.ps1`, `rtw89win-restartset.ps1`, `rtw89win-scanrestart.ps1`.
8. Run `.\build.ps1`.

   **Result**: The output directory contains the new firmware file. `inf2cat` reports no errors.

`<device ID>`: The PCI device ID of the new chip, from the `id_table` in the upstream `*e.c` file.

The firmware must support scan offload and beacon filter. If it does not, rtw89 uses channel context emulation, but the emulation in `mac\stubs.c` is empty.

The `id_table` of the upstream `rtw8852be.c` also contains `0xB85B`. The INF of this project does not list this ID, and this ID is not tested.

### Change the chip-specific code

1. Read the `disable_intr` function of the new chip in the upstream `pci.c`.
2. If this function is not `rtw89_pci_disable_intr`, change the `INTX_*` constants and `intx_stopped` in `compat\device.c`. Use the registers that the function writes.
3. If the new chip is BE generation, compare the `R_AX_*` offsets in `driver\hw.c` with the upstream `reg.h`. `RtwHwLogChipInfo` uses these offsets only to write the log. It is not determined if a read of these offsets is safe on other chips.
4. Check the [mac layer assumptions](#mac-assumptions) one by one. Change the parts that the new chip does not meet.
5. Check `ReportCapabilities` in `wificx\device.cpp`. It reports only 2.4 GHz and 5 GHz and PHY types up to VHT. The maximum 5 GHz width is 80 MHz.
6. Check the maximum link rate `RTW_LINK_SPEED_80` in `wificx\adapter.cpp` (866.7 Mbit/s, VHT80 with two spatial streams).
7. Check the default rate that `IndicateLinkState` in `wificx\connect.cpp` uses before rtw89 reports a rate.
8. If the new chip supports 6 GHz, extend `struct rtw_caps` in `compat\include\rtw\glue.h`. At this time, this structure has only two bands, with a maximum of 64 channels for each band.

If `intx_stopped` is not correct, an interrupt storm and bug check 0x133 occur during system error recovery (SER) ([RTW-9](observed-behavior.md#rtw-9)). You do not need to change the number of spatial streams. `rtw_linux_get_caps` in `mac\hw.c` reads this value from rtw89.

<a id="mac-assumptions"></a>
## mac layer assumptions

Conclusion: The mac layer implements only the mac80211 functions that RTL8852B uses. Each row in the table is one assumption. If the new chip does not meet a row, do the action in the last column.

| Assumption | Code location | Action when the new chip does not meet the assumption |
|---|---|---|
| 1. The firmware does the rate control (rtw89 sets `HAS_RATE_CONTROL`) | The mac layer has no rate control code | Implement rate control in the mac layer (for example minstrel) |
| 2. The driver uses only the firmware scan (`hw_scan`) | `mac\scan.c` `scan_start_locked` | Implement a software scan |
| 3. The driver does not use channel context emulation | `mac\stubs.c` (`ieee80211_emulate_*` are empty) | Implement the emulation, or use firmware that supports scan offload and beacon filter |
| 4. The driver does not use the txq scheduler; data frames call `ops->tx` directly | `mac\stubs.c`; `mac\tx.c` `rtw_linux_tx_data` | Implement `wake_tx_queue` and the txq scheduler ([RTW-5](observed-behavior.md#rtw-5)) |
| 5. At TX, the mac layer writes the CCMP header and the PN (`hw_sec_hdr = false`) | `mac\key.c` `mac_key_ccmp_hdr`; `mac\tx.c` | Use the mac80211 rule: check the key flag `IEEE80211_KEY_FLAG_GENERATE_IV`; if the flag is not set, do not write them |
| 6. At RX, rtw89 reports only `RX_FLAG_DECRYPTED`; the mac layer checks the PN and removes the CCMP header and the MIC | `mac\key.c` `mac_key_rx`; `mac\rx.c` `mac_rx_deliver` | Process `RX_FLAG_IV_STRIPPED`, `RX_FLAG_MIC_STRIPPED`, and `RX_FLAG_PN_VALIDATED` separately |
| 7. The driver supports only CCMP-128 | `mac\key.c`; `wificx\device.cpp` `ReportCapabilities` | Add the header formats and the capabilities for GCMP or CCMP-256 |
| 8. The hardware does not encrypt or decrypt management frames; PMF uses only software | `mac\pmf.c` | On chips with `hw_mgmt_tx_encrypt = true`, the behavior of the current code is not determined; compare it with the mac80211 flag `IEEE80211_KEY_FLAG_SW_MGMT_TX` |
| 9. The mac layer does the reorder | `mac\agg.c` `mac_agg_rx_frame` | If the chip does the reorder itself, skip the mac layer reorder |
| 10. At RX, no A-MSDU in an A-MPDU; the HT maximum A-MSDU is 3839 bytes; the VHT maximum MPDU in the capabilities is 3895 bytes | `mac\agg.c` `ba_params`; `mac\wide.c`; `mac\mlme.c` | If larger frames are necessary, change the BA parameters and the advertised capabilities |
| 11. The mac layer tracks the power save state of hotspot clients (RTL8852B firmware has no `NOTIFY_AP_INFO`) | `mac\ap.c` | If the firmware has `NOTIFY_AP_INFO`, rtw89 sets `AP_LINK_PS`; the mac layer does not implement this path |
| 12. A maximum of two vifs (station and group owner), on the same channel only; no MCC or MLO | `mac\iface.c` `chanctx_share`; `mac\ap.c` | Implement more than one channel context |
| 13. The maximum width is 80 MHz; 6 GHz is disabled; no HE or EHT | `mac\wide.h`; `mac\reg.c` `reg_apply`; `wificx\device.cpp` | Extend the width selection, the regulatory rules, and the capability report |
| 14. The beacon `sk_buff` is newly allocated (rtw89 uses this layout to find the TIM) | `mac\ap.c`; `compat\include\rtw\skbuff.h` | If you change `sk_buff`, keep this property ([HS-18](observed-behavior.md#hs-18)) |
| 15. The mac layer calls `ieee80211_ops` while it holds `wiphy->mtx`; RX runs at PASSIVE | `mac\hw.c`; `compat\netdev.c` | If the new driver needs a different context, change the calls ([RTW-3](observed-behavior.md#rtw-3), [RTW-4](observed-behavior.md#rtw-4)) |

<a id="new-board"></a>
## Change the hardware platform

Conclusion: When you change the hardware platform, check the platform conditions and the Bluetooth USB ID. The table shows the conditions on ROCK 5T. It also shows what to change and what not to change on a different hardware platform.

| Condition | On ROCK 5T (RK3588) | Action on a different hardware platform |
|---|---|---|
| M.2 slot power | UEFI supplies power to the M.2 E-key slot and sets `W_DISABLE1#` high; this step is not in this repository | Make sure that UEFI does this step; if not, Windows cannot find the device ([PLAT-4](observed-behavior.md#plat-4)) |
| Interrupts | No MSI, only INTx; CPU0 handles all interrupts | On platforms with MSI, the current code still uses only INTx: `pci_alloc_irq_vectors` in `compat\device.c` accepts only INTx; MSI needs changes in the driver layer and the compatibility layer, which are not implemented ([PLAT-1](observed-behavior.md#plat-1)) |
| DMA cache coherency | PCIe is not cache coherent; common buffers have a non-cached map | No change: `driver\os.c` uses the default cache type of the DMA adapter ([PLAT-2](observed-behavior.md#plat-2)) |
| DMA addresses | All below 4 GB | No change: the driver layer requests only 32-bit addresses |
| PCIe link | Gen1 x1; BAR2 is a 1 MB memory BAR; when the chip has no power, a read of the MAC area returns `0xEAEAEAEA` | No change ([PLAT-3](observed-behavior.md#plat-3)) |
| Re-enumeration | After re-enumeration, the chip cannot download firmware until it loses power | Not tested on other hardware platforms; new recovery code must not call `WdfDeviceSetFailed(WdfDeviceFailedAttemptRestart)` ([INST-6](observed-behavior.md#inst-6)) |
| Windows version | 26100 | The INF installs only on build 26100 and later (`[Manufacturer]` section) |
| Bluetooth USB ID | `USB\VID_13D3&PID_3572` | Change the `[Realtek.NTARM64.10.0...26100]` section of `bt\rtkbtflt\rtkbtflt.inf`; give `-HardwareId` or `-UsbId` to the scripts (see below) |
| Bluetooth controller | USB part of RTL8852B: the ROM `lmp_subver` is `0x8852`; the patch is project 20 in v1 format | The Bluetooth filter driver does not download patches to other controllers; for other Bluetooth chips, change `bt\rtkbtfw\epatch.c` and `bt\rtkbtflt\hci.c` |

If the Bluetooth USB ID is different, run these scripts with a parameter:

- `install-rtkbtflt.ps1`, `uninstall-rtkbtflt.ps1`, `rtkbtflt.ps1`: `-HardwareId 'USB\VID_<vendor ID>&PID_<product ID>'`.
- `tools-dev\board\rtw89win-btquery.ps1`: `-UsbId`.

It is not determined which Bluetooth USB IDs the RTL8852BE modules from other manufacturers use.

<a id="new-family"></a>
## Change the driver family

Conclusion: To connect a different Linux mac80211 driver, you must add code to the compatibility layer and to the mac layer. The two tables below show the known categories that are not available. These categories are not checked one by one. First, use a build to find the symbols that are not available. Then compare them with the two tables.

### Find the parts that are not available

On the build PC:

1. Put the source files of the new driver in a directory.
2. Add these files and the macros that the new driver needs to `buildhal.ps1`.
3. Run `.\buildhal.ps1 -RtwSrc <source directory>`.
4. For each object file that does not compile, run `.\tools-dev\errsum.ps1 <object file name>`. The script lists the symbols that are not available.
5. When the compilation is successful, run `.\tools-dev\undef.ps1`.

   **Result**: The script lists the symbols that `rtw89hal.lib` uses but does not define. The RTL8852BE build has 45 such symbols, and the driver layer or the Windows kernel supplies all of them. Implement each new symbol in the compatibility layer, the mac layer, or `driver\os.c`.

The full compiler output of each object file is in `build\ARM64\Release\hal\<object file name>.log`.

### Categories to add to the compatibility layer

| Category | Status | Location |
|---|---|---|
| USB | No `linux\usb.h` | `compat\include\linux\` |
| SDIO | None | — |
| Interrupts | Only one INTx; `pci_alloc_irq_vectors` rejects MSI | `compat\device.c` |
| DMA | 32-bit addresses; data in memory outside the DMA heap moves through bounce buffers | `compat\device.c`, `driver\os.c` |
| Firmware load | Only the synchronous `request_firmware` and its aliases | `compat\include\rtw\bus.h`, `compat\device.c` |
| cfg80211 helper functions | `cfg80211_chandef_usable`, `cfg80211_bss_iter`, and others are empty | `compat\include\net\cfg80211.h` |
| Other kernel subsystems | debugfs, LED, thermal, rfkill, and devcoredump are not implemented; `CONFIG_PM` is not defined, so the build does not include WoWLAN | `compat\include\linux\` |
| 802.11 headers | Some definitions in the FreeBSD headers are different from the Linux headers (for example `ieee80211_hdrlen`, [WIDE-6](observed-behavior.md#wide-6)) | `compat\include\linux\ieee80211.h`, `compat\include\net\` |

### Categories to add to the mac layer

| Category | Status |
|---|---|
| txq scheduler | Empty; data frames call `ops->tx` directly |
| Software scan | None; only `hw_scan` |
| Channel context emulation | Empty |
| Rate control | None; the driver uses the firmware |
| Software encryption | Only software CCMP and BIP for management frames; data frames use hardware encryption |
| Cipher suites | Only CCMP-128 and BIP-CMAC-128; no TKIP or GCMP |

For each assumption, see [mac layer assumptions](#mac-assumptions).

<a id="usb"></a>
## USB chips

Conclusion: The USB path is not implemented and not tested. rtw89 v7.2 has `usb.c` and USB files for each chip (for example `rtw8852bu.c`, `rtw8852cu.c`). It has no SDIO.

Parts to add:

| Part | Content |
|---|---|
| Driver layer | KMDF USB device framework: USB device object, configuration, pipes, URB submission and completion |
| `os.h` | USB primitives for the compatibility layer, in place of the current BAR2, INTx, and DMA |
| Compatibility layer | A subset of `linux\usb.h`; registration for `module_usb_driver`; a USB entry point that agrees with `rtw_linux_probe` |
| `buildhal.ps1` | Add `usb` and the USB files of the chip; add `-DCONFIG_RTW89_USB=1` and the USB macro of the chip |
| `compat\include\rtw\base.h` | `IS_ENABLED` rows for new options (add them when a compile error occurs) |
| INF | USB hardware ID; firmware files |

`bt\rtkbtflt\hci.c` and `bt\rtkbtflt\urb.c` communicate directly through URBs and USB pipes. You can use them as a reference. The Bluetooth filter driver is a filter driver, but a USB Wi-Fi driver is a function driver. For this reason, their device objects are different.

<a id="testing"></a>
## Test procedure

Conclusion: Verify the functions in sequence. Start the next item only after the previous item passes. Compare each result with the result for the same function in [Validation status](validation.md).

> **Note**: Run the `tools-dev\board\` scripts on the target device as administrator: `pwsh -NoProfile -File <script>`. For the scripts that call `rtwdiag.ps1`, use `-Diag <path>` to specify its location, or set the environment variable `RTW89WIN_DIAG`. For the parameters of each script, see `tools-dev\board\README.txt`.

First, do the build checks. The kernel stack is small. For this reason, step 1 checks that no function has a stack frame larger than 1024 bytes.

On the build PC:

1. Run `.\buildhal.ps1 -ExtraFlags '-Wframe-larger-than=1024'`.

   **Result**: `Errors` and `Warnings` are 0 for each object file.
2. Run `.\build.ps1`.

   **Result**: MSBuild reports 0 warnings and 0 errors (the project uses `/W4 /WX`).
3. Run `.\tools-dev\undef.ps1`.

   **Result**: `driver\os.c`, other files of the driver layer, or the Windows kernel supply each listed symbol. The link in step 2 has no unresolved symbols.
4. Run the host tests `.\tools-dev\apframe-test.ps1`, `.\tools-dev\wide-test.ps1`, and `.\tools-dev\crypto-test.ps1`.

   **Result**: Each test shows PASS.
5. Use `llvm-readobj --coff-imports` or `dumpbin /imports` to show the import table of `rtw89win.sys`.

   **Result**: The import table has only `ntoskrnl.exe`, `HAL.dll`, `WDFLDR.SYS`, and `ksecdd.sys`.

Then verify on the target device in the sequence of the table.

The `hosttools\` scripts of items 4 and 8 run on the measuring PC. They have two prerequisites:

- The target device runs the OpenSSH server, and its default shell is PowerShell.
- You give `-BoardWlan <WLAN address of the target device>` and `-SshHost <ssh host name of the target device>`. Item 8 needs only `-BoardWlan`. You can also put these values in environment variables or in `local.settings.psd1` (see [Build](build.md#parameters)).

| Sequence | Function | What to verify | Tools and scripts | Section in validation status |
|---|---|---|---|---|
| 1 | Probe, start, and scan in diagnostic mode | Firmware download, efuse, interrupts, RX, H2C/C2H, firmware scan | `rtw89win-restartset.ps1 -Set BootAttempts=2` to enter diagnostic mode; `rtwdiag.ps1 probe`, `start`, `monitor <channel>`, `scan`, `txprobe`, `remove`; `rtwdiag.ps1 arm` to return to WiFiCx mode | [Recovery and stability](validation.md#robustness) |
| 2 | Scan in WiFiCx mode | Capability report, `TASK_SCAN`, BSS list | Wi-Fi list on the taskbar; `rtwdiag.ps1 mode`, `bss`; `rtw89win-wlanlist.ps1` (location services must be on, [SCAN-7](observed-behavior.md#scan-7)) | [Station](validation.md#station) |
| 3 | Connection | WPA2-Personal, keys, exemptions | `rtw89win-profile.ps1`, `rtw89win-connect.ps1`; `rtwdiag.ps1 link` | [Station](validation.md#station), [Security](validation.md#security) |
| 4 | Data and A-MPDU | Throughput, BA sessions, reorder | `hosttools\rtw89win-thru.ps1` on the measuring PC; BA counters in `rtwdiag.ps1 link` | [Station](validation.md#station) |
| 5 | SER | Recovery, INTx mask, automatic connection again | `rtw89win-serdiag.ps1` (diagnostic mode), `rtw89win-serconn.ps1` (connected) | [Recovery and stability](validation.md#robustness) |
| 6 | PMF and WPA3-SAE | `Pmf=1`, `Pmf=2` | `rtw89win-profile.ps1 -Auth WPA3SAE`, `rtw89win-connect.ps1`; `rtwdiag.ps1 link` | [Security](validation.md#security) |
| 7 | Country code | Regulatory rules, active and passive channels | `rtwdiag.ps1 country <country code>`, `rtwdiag.ps1 radio` | [Country code](validation.md#regulatory) |
| 8 | Power save | Entry to and exit from power save, interactive latency | Setting `PowerSave=2`; `rtwdiag.ps1 radio`; `hosttools\rtw89win-tcprtt.ps1` on the measuring PC | [Power save](validation.md#power) |
| 9 | Channel width | VHT80, HT40, HT20 | Setting `Bandwidth`; `rtwdiag.ps1 link` | [Channel width](validation.md#width) |
| 10 | Mobile hotspot | Group owner, hotspot clients, same channel as the station | `rtw89win-hotspot.ps1`; `rtwdiag.ps1 ap` | [Mobile hotspot](validation.md#hotspot) |
| 11 | Bluetooth | Patch download, BLE, pairing | `rtkbtflt.ps1 status`, `rtkbtfw info`, `rtw89win-btquery.ps1`, `rtw89win-bt.ps1` | [Bluetooth](validation.md#bluetooth) |
| 12 | Restart and protection | Restart the device, disable and enable, restart the device during a scan, crash loop guard | `rtw89win-disen.ps1`, `rtw89win-scanrestart.ps1`, `rtw89win-guardsim.ps1` | [Recovery and stability](validation.md#robustness) |

`<channel>`: A 2.4 GHz channel number, for example `6`. `<country code>`: Two letters, for example `DE`.

Pass criteria for item 5: The driver log has a maximum of one line `INTx: the chip interrupted while rtw89 was stopped; masked it` for each SER. The system has no bug check.

For the terms, see the [Glossary](glossary.md). For the interfaces and the control flow of each layer, see [Architecture](architecture.md#control-flow).
