# Firmware files

[中文](README.md)

- **Readers**: Builders and users.
- **What you will do**: Get the 3 firmware files, compare their SHA256 values, and put the files where the build scripts read them.
- **Prerequisites**: You can download files from the internet. PowerShell 7 is installed on the build PC. If you use git to get the files, you also need git.

<a id="summary"></a>
## Summary

1. This repository does not contain firmware. Before you build the driver packages, get the 3 firmware files.
2. The 3 files come from the linux-firmware project. Download them by the full commit ID to get the tested files.
3. Use the SHA256 value to identify the correct file. The table below shows the SHA256 values of the tested files.
4. The firmware uses a Realtek license. The license permits redistribution and use only in binary form, without changes.

<a id="files"></a>
## Necessary files

| Put in | Path in linux-firmware | Size (bytes) | SHA256 (tested) | Purpose |
|---|---|---|---|---|
| `firmware\rtw8852b_fw-2.bin` | `rtw89/rtw8852b_fw-2.bin` | 1 554 594 | `6ff6aeb56bb5b85cba333b755f8cc1dbfe9ff6e5b16e39f6ba01ce7f1eaa7714` | Wi-Fi firmware, for the Wi-Fi driver package |
| `firmware\rtl_bt\rtl8852bu_fw.bin` | `rtl_bt/rtl8852bu_fw.bin` | 74 324 | `f1e0427317f147a78606ede85f3f8977ed0ab85c41c6b525560e5bd5a51e5818` | Bluetooth patch, for the Bluetooth driver package |
| `firmware\rtl_bt\rtl8852bu_config.bin` | `rtl_bt/rtl8852bu_config.bin` (see [Get the files](#get)) | 6 | `6c28a3f07c6a30ed208c4b64862a23f02b7d93543ea980edd24df16bab45095f` | Configuration file of the Bluetooth patch, for the Bluetooth driver package |

The same hashes are also in [`SHA256SUMS`](SHA256SUMS) and [`rtl_bt/SHA256SUMS`](rtl_bt/SHA256SUMS). `rtl_bt/SHA256SUMS` also has the hash of the firmware license file. The build scripts use these two files to examine the firmware.

<a id="get"></a>
## Get the files

The 3 tested files come from two commits of linux-firmware:

| File | Source of the tested file |
|---|---|
| `rtw8852b_fw-2.bin` (firmware version 0.29.29.18) | Commit `f97831a2895009967c50f94f6591787e6fbe3bd7` |
| `rtl8852bu_fw.bin` (Bluetooth patch version `0xdbc6b20f`), `rtl8852bu_config.bin` | Commit `3b128b6069267b60a46a4074c383183177ee6e6d` |

The tested Bluetooth files come from the Ubuntu 24.04 package `linux-firmware` `20240318.git3b128b60-0ubuntu2.25`. These files are identical, byte for byte, to the files in the commit above. Newer versions of these files in linux-firmware have different hashes. The tests did not use these newer versions.

In commit `3b128b60`, `rtl_bt/rtl8852bu_config.bin` is a normal file. Its contents are the same as the contents of `rtl_bt/rtl8761bu_config.bin`. Newer commits removed this file from the repository. In these commits, `WHENCE` only declares the file as a link to `rtl8761bu_config.bin`. If you use a newer commit, download `rtl_bt/rtl8761bu_config.bin`. Save it as `rtl8852bu_config.bin`.

### Download by commit ID

These steps download the files from the GitLab repository of linux-firmware (<https://gitlab.com/kernel-firmware/linux-firmware>).

On the build PC, open PowerShell 7 in the root directory of the repository. Do all the steps in the same window:

1. Set the commit IDs and the download address.

   ```powershell
   $c1 = 'f97831a2895009967c50f94f6591787e6fbe3bd7'
   $c2 = '3b128b6069267b60a46a4074c383183177ee6e6d'
   $base = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw'
   ```

2. Create the firmware directory.

   ```powershell
   New-Item -ItemType Directory -Force firmware\rtl_bt | Out-Null
   ```

3. Download the Wi-Fi firmware.

   ```powershell
   Invoke-WebRequest "$base/$c1/rtw89/rtw8852b_fw-2.bin" -OutFile firmware\rtw8852b_fw-2.bin
   ```

4. Download the two Bluetooth files.

   ```powershell
   Invoke-WebRequest "$base/$c2/rtl_bt/rtl8852bu_fw.bin" -OutFile firmware\rtl_bt\rtl8852bu_fw.bin
   Invoke-WebRequest "$base/$c2/rtl_bt/rtl8852bu_config.bin" -OutFile firmware\rtl_bt\rtl8852bu_config.bin
   ```

5. Compare the hashes of the 3 files. See [Compare the hashes](#verify).

   **Result**: All 3 values are the same as the values in the table.

> **Note**: The linux-firmware repository on kernel.org has equivalent addresses: `https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/<path>?id=<full commit ID>`. `<path>` is a value from the "Path in linux-firmware" column of the [Necessary files](#files) table.

### Get the files with git

This method clones linux-firmware without the file contents. The checkout then downloads only the necessary files.

On the build PC (PowerShell 7 and git are necessary), in the root directory of the repository:

1. Clone linux-firmware without a checkout of the files.

   ```powershell
   git clone --filter=blob:none --no-checkout https://gitlab.com/kernel-firmware/linux-firmware.git ..\linux-firmware
   ```

2. Get the tested Wi-Fi firmware file.

   ```powershell
   git -C ..\linux-firmware checkout f97831a2895009967c50f94f6591787e6fbe3bd7 -- rtw89/rtw8852b_fw-2.bin
   ```

3. Get the two tested Bluetooth files.

   ```powershell
   git -C ..\linux-firmware checkout 3b128b6069267b60a46a4074c383183177ee6e6d -- rtl_bt/rtl8852bu_fw.bin rtl_bt/rtl8852bu_config.bin
   ```

4. Copy the files to `firmware\` in this repository.

   ```powershell
   New-Item -ItemType Directory -Force firmware\rtl_bt | Out-Null
   Copy-Item ..\linux-firmware\rtw89\rtw8852b_fw-2.bin firmware\
   Copy-Item ..\linux-firmware\rtl_bt\rtl8852bu_fw.bin, ..\linux-firmware\rtl_bt\rtl8852bu_config.bin firmware\rtl_bt\
   ```

5. Compare the hashes of the 3 files. See [Compare the hashes](#verify).

   **Result**: All 3 values are the same as the values in the table.

<a id="verify"></a>
## Compare the hashes

On the build PC, open PowerShell 7 in the root directory of the repository:

1. Run `Get-FileHash` to calculate the SHA256 values of the 3 files.

   ```powershell
   Get-FileHash -Algorithm SHA256 firmware\rtw8852b_fw-2.bin, firmware\rtl_bt\rtl8852bu_fw.bin, firmware\rtl_bt\rtl8852bu_config.bin | Format-List Hash, Path
   ```

   **Result**: Each file has one `Hash` value.
2. Compare each `Hash` value with the SHA256 value in the [Necessary files](#files) table. Upper case and lower case are equal for this comparison.

   **Result**: All 3 values are the same.

If a hash is different, the build script does not stop. It shows only a warning. The warning ends with `untested firmware`. The driver package from this build then contains untested firmware.

<a id="place"></a>
## Put the files in the firmware directory

Put each file in the location from the "Put in" column of the [Necessary files](#files) table. The paths are relative to the root directory of the repository:

```text
firmware\
  rtw8852b_fw-2.bin
  rtl_bt\
    rtl8852bu_fw.bin
    rtl8852bu_config.bin
```

You can also put the firmware in a different directory. This directory must have the same structure: the two Bluetooth files are in the subdirectory `rtl_bt\`. When you build, use `-FirmwareDir <directory>` to specify this directory. See [Build](../docs/en/build.md#parameters).

> **Note**: The `.gitignore` file of the repository ignores the `.bin` files in `firmware`. This prevents an accidental commit of the firmware.

<a id="license"></a>
## License

The 3 files use the Realtek firmware license [`LICENCE.rtlwifi_firmware.txt`](LICENCE.rtlwifi_firmware.txt). The `WHENCE` file of linux-firmware states that you can redistribute these files. For extracts, see [`WHENCE-rtw89-excerpt.txt`](WHENCE-rtw89-excerpt.txt) and [`rtl_bt/WHENCE-rtl_bt-excerpt.txt`](rtl_bt/WHENCE-rtl_bt-excerpt.txt).

This list gives a summary of the license terms. Only the original license text has legal effect.

1. The license permits redistribution and use only in binary form, without changes.
2. When you redistribute the files, include the copyright notice and the disclaimer. Put them in the documentation or in the other materials that you supply with the files.
3. Do not use the name of Realtek or its suppliers to endorse derived products.
4. Do not reverse engineer, decompile, or disassemble the software.
5. The patent license has a limited scope. It covers only the patents that are necessary for two uses. The first use is the software alone. The second use is the software with an operating system under an OSI-approved open-source license. The original text then states: "The patent license shall not apply to any other combinations which include this software."
6. Realtek supplies the software "as is", without a warranty of any kind.

This document does not interpret these terms. Before you use the firmware on Windows, read the original license text. For the requirements when you redistribute a driver package, see [Third-party licenses](../docs/en/licenses.md#firmware) and [Build](../docs/en/build.md#redistribute).
