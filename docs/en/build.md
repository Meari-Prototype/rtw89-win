# Build

[中文](../zh/build.md)

- **Readers**: Builders. A builder compiles the driver packages on an x64 Windows computer.
- **What you will do**: Build the Wi-Fi driver package and the Bluetooth driver package on the build PC. Then run the host tests.
- **Prerequisites**: An x64 Windows computer. Access to the internet (for the tools and the NuGet packages). The 3 firmware files (see [Firmware files](../../firmware/README.en.md)).

<a id="summary"></a>
## Summary

1. One command makes one driver package. `build.ps1` makes the Wi-Fi driver package. `bt\rtkbtflt\build.ps1` makes the Bluetooth driver package.
2. The necessary tools are Visual Studio 2022 (with the ARM64 build tools), LLVM, and PowerShell 7. The NuGet packages of the WDK and the Windows SDK are also necessary.
3. The source code of the upstream rtw89 is already in `third_party\rtw89\` in the repository. You do not have to get it separately.
4. The firmware is not in the repository. Before you build, put the 3 firmware files in `firmware\`.
5. The driver packages must have a test signature. First, create your own test certificate.
6. Run all build scripts in PowerShell 7.

<a id="requirements"></a>
## Necessary tools

| Tool | Version | Purpose | How to get it |
|---|---|---|---|
| Visual Studio 2022 or Build Tools for Visual Studio 2022 | 2022 | MSVC compiler and MSBuild: compile the drivers, the Bluetooth tool, and the host tests | The Visual Studio Installer from Microsoft. Include the MSVC build tools for x64 and for ARM64. |
| MSVC ARM64 build tools | Included with Visual Studio | Compile the ARM64 drivers | The component `Microsoft.VisualStudio.Component.VC.Tools.ARM64` in the Visual Studio Installer |
| NuGet packages of the WDK and the Windows SDK | See `packages.config` | Kernel headers, libraries, `signtool`, `inf2cat` | `nuget restore`. See [Prepare the build PC](#prepare). |
| `nuget.exe` | Minimum version not determined | Get the NuGet packages above | Microsoft documentation: [Install NuGet client tools](https://learn.microsoft.com/nuget/install-nuget-client-tools) |
| LLVM (`clang.exe`, `llvm-lib.exe`, `llvm-nm.exe`) | Tested: 23.1.2. Minimum version not determined. | Compile the upstream rtw89, the compatibility layer, and the mac layer | The Windows x64 installer from the LLVM project |
| PowerShell 7 | 7.x | Run the build scripts | Microsoft documentation: [Install PowerShell on Windows](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) |

The NuGet packages in `packages.config`:

| Package | Version |
|---|---|
| `Microsoft.Windows.SDK.CPP` | 10.0.26100.1 |
| `Microsoft.Windows.SDK.CPP.x64` | 10.0.26100.1 |
| `Microsoft.Windows.SDK.CPP.arm64` | 10.0.26100.1 |
| `Microsoft.Windows.WDK.ARM64` | 10.0.26100.6584 |

All builds used an x64 build PC. On an ARM64 computer, the build scripts select the ARM64 tools. The tests did not include this condition.

<a id="prepare"></a>
## Prepare the build PC

On the build PC:

1. Install Visual Studio 2022 or Build Tools for Visual Studio 2022. Select the **Desktop development with C++** workload.
2. In **Individual components**, add the MSVC ARM64 build tools (component ID `Microsoft.VisualStudio.Component.VC.Tools.ARM64`).
3. Make sure that the build scripts can find this installation.

   ```powershell
   & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
   ```

   **Result**: The output shows one directory. If there is no output, do step 2 again.
4. Install PowerShell 7.
5. Install LLVM. Add its `bin` directory to `PATH`, or install LLVM in the default location `%ProgramFiles%\LLVM`.
6. Open PowerShell 7. Make sure that `clang` is available.

   ```powershell
   & "$env:ProgramFiles\LLVM\bin\clang.exe" --version
   ```

   **Result**: The output shows the LLVM version. If LLVM is in a different location, run `clang.exe` from that location.
7. Get the source code of this repository. Go to the root directory of the repository.
8. Get the NuGet packages of the WDK and the Windows SDK.

   ```powershell
   nuget restore packages.config -PackagesDirectory packages
   ```

   If `nuget.exe` is not in `PATH`, use its full path instead of `nuget`.

   **Result**: `packages\` contains 4 directories. Each directory name is a package name from the table above plus the version, for example `Microsoft.Windows.WDK.ARM64.10.0.26100.6584`.
9. If LLVM or the NuGet packages are in a different location, create `local.settings.psd1` in the root directory of the repository. Write their locations in this file.

   ```powershell
   @{
     LlvmBin     = '<LLVM bin directory>'
     PackagesDir = '<NuGet package directory>'
   }
   ```

   `<LLVM bin directory>`: the directory that contains `clang.exe`. `<NuGet package directory>`: the directory that you gave to `-PackagesDirectory` in step 8.

   All build scripts read values from this file. After this, you do not have to give `-LlvmBin` and `-PackagesDir`. You can also write the firmware directory and the certificate thumbprint in this file (keys `FirmwareDir` and `CertThumbprint`). For all keys, see `local.settings.example.psd1` and [Parameters and build settings](#parameters).

<a id="firmware"></a>
## Add the firmware

The build scripts read the firmware from `firmware\`. They copy the firmware into the driver packages.

1. Get the 3 firmware files and compare their hashes. See [Firmware files](../../firmware/README.en.md).
2. Put the files in these locations: `firmware\rtw8852b_fw-2.bin`, `firmware\rtl_bt\rtl8852bu_fw.bin`, `firmware\rtl_bt\rtl8852bu_config.bin`.

If the firmware is in a different directory, use `-FirmwareDir <directory>` to specify that directory when you build. If a firmware file is not there, the build script shows an error and stops. If the hash of the firmware is different from `SHA256SUMS`, the build script shows only the warning `untested firmware`. It does not stop.

<a id="certificate"></a>
## Prepare the test certificate

The build scripts use your own code signing certificate to give the driver a test signature. The certificate must be in `Cert:\CurrentUser\My` on the build PC. It must have a private key.

> **Caution**: Keep the private key of the certificate only on the build PC. Do not give a copy to other persons. The `rtw89win-test.cer` file in the driver package contains only the public key.

If the build PC already has a code signing certificate with a private key, you can use that certificate. Then skip step 1. Use this command to find its thumbprint:

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert
```

On the build PC, open PowerShell 7 in the root directory of the repository:

1. Create a test certificate.

   ```powershell
   .\tools-dev\new-testcert.ps1
   ```

   **Result**: The output shows `thumbprint: <thumbprint>`. The default certificate name is `CN=rtw89win test`. The certificate is valid for 5 years.
2. Record the thumbprint.
3. Select one method to give the thumbprint to the build scripts:
   - Give `-CertThumbprint <thumbprint>` at each build.
   - Set the environment variable `RTW89WIN_CERT`.
   - Write `CertThumbprint = '<thumbprint>'` in `local.settings.psd1`.

The script parameters of `new-testcert.ps1` are `-Subject` (certificate name, default `'CN=rtw89win test'`) and `-Years` (validity period, default `5`).

When you no longer need a test certificate, delete the certificate and its private key:

```powershell
Remove-Item Cert:\CurrentUser\My\<thumbprint> -DeleteKey
```

You can also use `-NoSign` to build a driver package without a signature. This driver package does not contain `rtw89win-test.cer`. Before the installation, sign the `.sys` and `.cat` files yourself. The target device must trust the certificate that you use.

<a id="build-wifi"></a>
## Build the Wi-Fi driver package

`build.ps1` first uses clang to compile the upstream rtw89, the compatibility layer, and the mac layer (`buildhal.ps1`). Then it uses MSBuild to compile the driver. Last, it makes the catalog file and signs the files.

On the build PC:

1. Open PowerShell 7. Go to the root directory of the repository.
2. Run the build script.

   ```powershell
   .\build.ps1 -CertThumbprint <thumbprint>
   ```

   If you gave the thumbprint through the environment variable or `local.settings.psd1`, remove `-CertThumbprint <thumbprint>`.

   **Result**: There are no errors. The last line is `output: <driver package directory>`.
3. Show the files in the driver package.

   ```powershell
   Get-ChildItem <driver package directory> -Name
   ```

   **Result**: The output shows the files in the table below.

| File | Contents |
|---|---|
| `rtw89win.sys`, `rtw89win.inf`, `rtw89win.cat` | Wi-Fi driver, INF, catalog file |
| `rtw8852b_fw-2.bin` | Wi-Fi firmware |
| `LICENCE.rtlwifi_firmware.txt` | License of the firmware |
| `LICENSE`, `THIRD_PARTY_NOTICES` | License of this project and third-party license notices |
| `rtw89win-test.cer` | Public key of the test certificate (not present with `-NoSign`) |
| `install-rtw89win.ps1`, `uninstall-rtw89win.ps1`, `rtwdiag.ps1` | Install, uninstall, and diagnostic scripts that run on the target device |
| `rollback-rtw89win.sh` | Rollback script that runs on a Linux system when Windows does not start |
| `SHA256SUMS` | The SHA256 value of each file in the driver package. The install script uses these values to compare the files. |

The default driver package directory is `out\<time>\`. `<time>` is the build time in the format `yyyyMMdd-HHmmss`. Copy the full directory to the target device. Then install the driver as [Installation and use](install.md) shows.

<a id="build-bt"></a>
## Build the Bluetooth driver package

`bt\rtkbtflt\build.ps1` first builds `rtkbtfw` and runs its host test. Then it compiles the Bluetooth filter driver. Last, it makes the catalog file and signs the files.

On the build PC, open PowerShell 7 in the root directory of the repository:

1. Run the build script.

   ```powershell
   .\bt\rtkbtflt\build.ps1 -CertThumbprint <thumbprint>
   ```

   **Result**: The output contains a line that starts with `PASS:` (the host test of `rtkbtfw`). The last line is `output: <driver package directory>`.

| File | Contents |
|---|---|
| `rtkbtflt.sys`, `rtkbtflt.inf`, `rtkbtflt.cat` | Bluetooth filter driver, INF, catalog file |
| `rtl8852bu_fw.bin`, `rtl8852bu_config.bin` | Bluetooth patch and its configuration file |
| `LICENCE.rtlwifi_firmware.txt` | License of the firmware |
| `LICENSE`, `THIRD_PARTY_NOTICES` | License of this project and third-party license notices |
| `rtw89win-test.cer` | Public key of the test certificate (not present with `-NoSign`) |
| `rtkbtfw.exe` | ARM64 user-mode tool. `rtkbtfw info` shows the controller version. |
| `install-rtkbtflt.ps1`, `uninstall-rtkbtflt.ps1`, `rtkbtflt.ps1` | Install, uninstall, and diagnostic scripts that run on the target device |
| `rollback-rtkbtflt.sh` | Rollback script that runs on a Linux system when Windows does not start |
| `SHA256SUMS` | The SHA256 value of each file in the driver package |

The default driver package directory is `out\rtkbtflt-<time>\`.

To build only `rtkbtfw`, run `.\bt\rtkbtfw\build.ps1`. This script runs the host test on the build PC. Then it makes the ARM64 `rtkbtfw.exe`. The output directory is `out\rtkbtfw-<time>\`. This directory contains the tool, the Bluetooth patch, and the license files. It is not a driver package.

With `-NoPackage`, the script only builds and tests. It does not make the output directory. The host test reads the Bluetooth patch. For this reason, with `-NoPackage`, the two files in `firmware\rtl_bt\` are also necessary. Alternatively, use `-FirmwareDir` to specify their location.

<a id="host-tests"></a>
## Run the host tests

The host tests run on the build PC. They do not need the target device. Each test compiles a part of the source code into an x64 program. Then the test runs its checks.

On the build PC, open PowerShell 7 in the root directory of the repository:

1. Run the test of the group owner frame format (`mac\apframe.c`).

   ```powershell
   .\tools-dev\apframe-test.ps1
   ```

   **Result**: The last line is `PASS`.
2. Run the channel width test (`mac\wide.c`).

   ```powershell
   .\tools-dev\wide-test.ps1
   ```

   **Result**: The last line is `PASS`.
3. Run the known-answer test for encryption (`driver\crypto.c`).

   ```powershell
   .\tools-dev\crypto-test.ps1
   ```

   **Result**: The last line is `PASS`.
4. Run the host test of the Bluetooth patch parser (`bt\rtkbtfw\epatch.c`). The two files in `firmware\rtl_bt\` are necessary.

   ```powershell
   .\bt\rtkbtfw\build.ps1 -NoPackage
   ```

   **Result**: The output has the line `PASS: 723 checks, 0 failed`.
5. After you build the Wi-Fi driver package, count the external symbols that `rtw89hal.lib` refers to but does not define.

   ```powershell
   (.\tools-dev\undef.ps1).Count
   ```

   If LLVM is not in the default location and not in `local.settings.psd1`, add `-LlvmBin <LLVM bin directory>`.

   **Result**: `45`. If the number changes, the upstream rtw89, the compatibility layer, or the mac layer uses a new external function.

`undef.ps1 -Where` also shows the object files that refer to each symbol. Each build script reads its build settings separately. For example, `-LlvmBin` for `build.ps1` does not go to `undef.ps1`.

<a id="parameters"></a>
## Parameters and build settings

Each build setting gets its value in this sequence: script parameter, environment variable, `local.settings.psd1` in the root directory of the repository, default value. Git does not track `local.settings.psd1`. For its format, see `local.settings.example.psd1`. A relative path in a build setting is relative to the root directory of the repository.

| Build setting | Script parameter | Environment variable | `local.settings.psd1` key | Default |
|---|---|---|---|---|
| NuGet packages of the WDK and the Windows SDK | `-PackagesDir` | `RTW89WIN_PACKAGES` | `PackagesDir` | `packages\` |
| `bin` directory of LLVM | `-LlvmBin` | `RTW89WIN_LLVM` | `LlvmBin` | The directory of `clang.exe` in `PATH`. If there is no such directory, `%ProgramFiles%\LLVM\bin`. |
| Source code of the upstream rtw89 | `-RtwSrc` | `RTW89WIN_RTW89_SRC` | `RtwSrc` | `third_party\rtw89\` |
| Firmware directory | `-FirmwareDir` | `RTW89WIN_FIRMWARE` | `FirmwareDir` | `firmware\` |
| Thumbprint of the test certificate | `-CertThumbprint`, or `-NoSign` | `RTW89WIN_CERT` | `CertThumbprint` | None. Without a thumbprint, you must give `-NoSign`. |
| Installation directory of Visual Studio | `-VsPath` | `RTW89WIN_VS` | `VsPath` | The newest installation with the ARM64 build tools that `vswhere` finds |
| Driver package directory | `-OutDir` | None | None | `out\<time>\`; for Bluetooth, `out\rtkbtflt-<time>\` |
| Driver version | `-Version` | None | None | The version of the current source code, in the `param` line of the build script |
| WLAN address of the target device (only for `hosttools\`) | `-BoardWlan` | `RTW89WIN_BOARD_WLAN` | `BoardWlan` | None |
| ssh host name of the target device (only for `hosttools\`) | `-SshHost` | `RTW89WIN_SSH_HOST` | `SshHost` | None |

A relative path in `-OutDir` is relative to the current directory. If the `-OutDir` directory exists and is not empty, the build script shows an error and stops. The script does this check before the compilation starts.

The script parameters of each script:

| Script | Script parameters |
|---|---|
| `build.ps1` | `-Version`, `-CertThumbprint`, `-NoSign`, `-OutDir`, `-PackagesDir`, `-LlvmBin`, `-RtwSrc`, `-FirmwareDir`, `-VsPath` |
| `bt\rtkbtflt\build.ps1` | `-Version`, `-CertThumbprint`, `-NoSign`, `-OutDir`, `-PackagesDir`, `-FirmwareDir`, `-VsPath` |
| `bt\rtkbtfw\build.ps1` | `-NoPackage`, `-OutDir`, `-PackagesDir`, `-FirmwareDir`, `-VsPath` |
| `buildhal.ps1` | `-LlvmBin`, `-RtwSrc`, `-Only <object file name>`, `-ExtraFlags <compiler options>`, `-ShowErrors <number>` (number of errors to show for each failed object file, default 15) |
| `tools-dev\errsum.ps1` | `<object file name>` (one or more), `-Top <number>` (number of error message types to show for each object file, default 40) |
| `tools-dev\apframe-test.ps1`, `wide-test.ps1`, `crypto-test.ps1` | `-VsPath` |
| `tools-dev\undef.ps1` | `-LlvmBin`, `-Where` |
| `tools-dev\new-testcert.ps1` | `-Subject`, `-Years` |
| `tools-dev\regdb-gen.ps1` | `-Path <regulatory.db>`, `-Version <wireless-regdb version>` (both are necessary) |

Usually, `build.ps1` calls `buildhal.ps1`. You can run `buildhal.ps1` alone to compile only some files (`-Only`). You can also add compiler options for checks, for example `-ExtraFlags '-Wframe-larger-than=1024'`. For the build changes when you change the chip, see the [Porting guide](porting.md).

<a id="troubleshooting"></a>
## Build problems

| Symptom | Cause | Solution |
|---|---|---|
| The script reports `WDK / Windows SDK NuGet packages not found in <directory>` | You did not get the NuGet packages, or they are not in the default location | Run `nuget restore packages.config -PackagesDirectory packages`. If the packages are in a different location, use `-PackagesDir` to specify it. |
| The script reports `LLVM not found` | The script cannot find `clang.exe` | Install LLVM. Or use `-LlvmBin` to specify its `bin` directory. |
| The script reports `vswhere.exe not found` | Visual Studio 2022 and its Build Tools are not installed | Install Visual Studio 2022 or its Build Tools. Or use `-VsPath` to specify the installation directory. |
| The script reports `vswhere found no Visual Studio with the component 'MSVC ARM64 build tools'` | The ARM64 build tools are not installed | In the Visual Studio Installer, add the component `Microsoft.VisualStudio.Component.VC.Tools.ARM64` |
| The script reports `no MSVC tools under <directory>` | `-VsPath` does not specify a Visual Studio installation directory | Correct `-VsPath` |
| The script reports `rtw89 sources not found` | The `-RtwSrc` directory does not contain `core.c` | Remove `-RtwSrc` to use `third_party\rtw89\`. Or correct the path. |
| The script reports `firmware file not found: <file>` | The firmware file is not in the firmware directory | Add the firmware as [Firmware files](../../firmware/README.en.md) shows. Or use `-FirmwareDir` to specify the firmware directory. |
| The script shows the warning `<file> has SHA256 <hash>, not the tested <hash>` | The firmware is different from the tested version | Use the tested firmware. If you keep this firmware, the build continues, but the driver package contains untested firmware. |
| The script reports `No test-signing certificate` | You did not give a certificate thumbprint | Do the steps in [Prepare the test certificate](#certificate). Or use `-NoSign`. |
| The script reports `certificate <thumbprint> not found in Cert:\CurrentUser\My` | The thumbprint is not correct, or the certificate is in the certificate store of a different user | Run the build as the Windows user who created the certificate. Correct the thumbprint. |
| The script reports `certificate <thumbprint> has no private key` | The certificate has no private key | Use `new-testcert.ps1` to create a new certificate |
| The script reports `output directory <directory> exists and is not empty` | The `-OutDir` directory contains files | Use a different `-OutDir`. Or empty the directory first. |
| The script reports `<number> file(s) failed` | The clang compilation failed | Read the object file table above this line. Read `---- <object file name>: first errors` (the first 15 errors of each of the first 3 failed object files). For the full output, see `build\ARM64\Release\hal\<object file name>.log`. To group the errors by message, run `.\tools-dev\errsum.ps1 <object file name>`. |
| The script reports `build failed (<number>)` | The MSBuild compilation or link failed | See `build\build-arm64.log` (for Bluetooth: `build\rtkbtflt\build-arm64.log`) |
| MSBuild shows the warning MSB8029 | The repository is in a temporary directory (`%TEMP%`) | The warning has no effect on the result. You can move the repository out of the temporary directory. |
| An error occurs in Windows PowerShell 5.1 (`powershell.exe`) | The build scripts use a function that only PowerShell 7 has (`ForEach-Object -Parallel`) | Run the scripts in PowerShell 7 (`pwsh`) |

<a id="signing"></a>
## Driver signatures

This project supplies only a build method with test signing. Windows loads a driver with a test signature only in test-signing mode. Disable Secure Boot before you enable this mode.

A production signature does not need test-signing mode. For a production signature, these conditions are necessary:

- An organization account in the Microsoft Hardware Developer Program.
- An EV code signing certificate.
- WHQL certification of the driver, or Microsoft attestation signing.

This project does not supply drivers with a production signature.

<a id="redistribute"></a>
## Give a driver package to other persons

The driver packages contain Realtek firmware files. The firmware license permits redistribution and use only in binary form, without changes. The license also says that you must include its copyright notice and its disclaimer.

The driver binary files contain code from this project and from the upstream rtw89. Their BSD-3-Clause license has a condition for redistribution in binary form: you must include the copyright notice and the license terms.

1. Do not change the firmware files in the driver package.
2. Keep `LICENSE`, `THIRD_PARTY_NOTICES`, and `LICENCE.rtlwifi_firmware.txt` in the driver package. The build scripts put these files in the driver package.

For the licenses and the obligations of each part, see [Third-party licenses](licenses.md#redistribute).
