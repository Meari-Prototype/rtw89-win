# 构建

[English](../en/build.md)

- **适用读者**：构建者，即在 x64 Windows 电脑上编译驱动包的人员。
- **本文任务**：在构建电脑上构建 Wi-Fi 驱动包和蓝牙驱动包，并运行主机测试。
- **前提**：一台 x64 Windows 电脑；可访问互联网（用于取得工具和 NuGet 包）；已取得 3 个固件文件（见[固件获取说明](../../firmware/README.md)）。

<a id="summary"></a>
## 要点

1. 一条命令生成一个驱动包：`build.ps1` 生成 Wi-Fi 驱动包，`bt\rtkbtflt\build.ps1` 生成蓝牙驱动包。
2. 所需工具：Visual Studio 2022（含 ARM64 生成工具）、WDK 和 Windows SDK 的 NuGet 包、LLVM、PowerShell 7。
3. 上游 rtw89 的源码已位于仓库的 `third_party\rtw89\` 中，无需另行取得。
4. 固件不在仓库中。构建前须将 3 个固件文件放入 `firmware\`。
5. 驱动包须做测试签名。请先创建自己的测试证书。
6. 所有构建脚本均须在 PowerShell 7 中运行。

<a id="requirements"></a>
## 所需工具

| 工具 | 版本 | 用途 | 取得方式 |
|---|---|---|---|
| Visual Studio 2022 或 Build Tools for Visual Studio 2022 | 2022 | MSVC 编译器和 MSBuild：编译驱动、蓝牙工具和主机测试 | 微软的 Visual Studio 安装程序；须包含 x64 和 ARM64 两套 MSVC 生成工具 |
| MSVC ARM64 生成工具 | 随 Visual Studio | 编译 ARM64 的驱动 | Visual Studio 安装程序中的组件 `Microsoft.VisualStudio.Component.VC.Tools.ARM64` |
| WDK 和 Windows SDK 的 NuGet 包 | 见 `packages.config` | 内核头文件、库、`signtool`、`inf2cat` | `nuget restore`，见[准备构建电脑](#prepare) |
| `nuget.exe` | 最低版本未确定 | 取得上述 NuGet 包 | 微软文档：[安装 NuGet 客户端工具](https://learn.microsoft.com/nuget/install-nuget-client-tools) |
| LLVM（`clang.exe`、`llvm-lib.exe`、`llvm-nm.exe`） | 测过 23.1.2；最低版本未确定 | 编译上游 rtw89、兼容层和 mac 层 | LLVM 项目发布的 Windows x64 安装程序 |
| PowerShell 7 | 7.x | 运行构建脚本 | 微软文档：[安装 PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) |

`packages.config` 列出的 NuGet 包：

| 包 | 版本 |
|---|---|
| `Microsoft.Windows.SDK.CPP` | 10.0.26100.1 |
| `Microsoft.Windows.SDK.CPP.x64` | 10.0.26100.1 |
| `Microsoft.Windows.SDK.CPP.arm64` | 10.0.26100.1 |
| `Microsoft.Windows.WDK.ARM64` | 10.0.26100.6584 |

仅在 x64 构建电脑上构建过。构建脚本在 ARM64 电脑上会选用 ARM64 的工具，但该情况未经测试。

<a id="prepare"></a>
## 准备构建电脑

在构建电脑上：

1. 安装 Visual Studio 2022 或 Build Tools for Visual Studio 2022。选择"使用 C++ 的桌面开发"工作负载。
2. 在"单个组件"中添加 MSVC 的 ARM64 生成工具（组件 ID `Microsoft.VisualStudio.Component.VC.Tools.ARM64`）。
3. 确认构建脚本能找到该安装。

   ```powershell
   & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
   ```

   **结果**：输出一个目录。若无输出，返回第 2 步。
4. 安装 PowerShell 7。
5. 安装 LLVM。将其 `bin` 目录加入 `PATH`，或安装在缺省位置 `%ProgramFiles%\LLVM`。
6. 打开 PowerShell 7，确认 `clang` 可用。

   ```powershell
   & "$env:ProgramFiles\LLVM\bin\clang.exe" --version
   ```

   **结果**：输出 LLVM 的版本号。若 LLVM 安装在其它位置，运行该位置的 `clang.exe`。
7. 取得本仓库的源码，进入仓库根目录。
8. 取得 WDK 和 Windows SDK 的 NuGet 包。

   ```powershell
   nuget restore packages.config -PackagesDirectory packages
   ```

   若 `nuget.exe` 不在 `PATH` 中，用其完整路径代替 `nuget`。

   **结果**：`packages\` 下有 4 个目录，名称为上表的包名加版本，例如 `Microsoft.Windows.WDK.ARM64.10.0.26100.6584`。
9. 若 LLVM 或 NuGet 包不在缺省位置，在仓库根目录创建 `local.settings.psd1`，写入其位置。

   ```powershell
   @{
     LlvmBin     = '<LLVM 的 bin 目录>'
     PackagesDir = '<NuGet 包目录>'
   }
   ```

   `<LLVM 的 bin 目录>`：含 `clang.exe` 的目录。`<NuGet 包目录>`：第 8 步 `-PackagesDirectory` 指定的目录。

   所有构建脚本都从该文件取值，此后无需再指定 `-LlvmBin`、`-PackagesDir`。固件目录和证书指纹也可写在该文件中（键 `FirmwareDir`、`CertThumbprint`）。全部键见 `local.settings.example.psd1` 和[参数和设置](#parameters)。

<a id="firmware"></a>
## 放入固件

构建脚本从 `firmware\` 读取固件，并将其复制到驱动包中。

1. 按[固件获取说明](../../firmware/README.md)取得 3 个固件文件，并核对哈希。
2. 将文件放到以下位置：`firmware\rtw8852b_fw-2.bin`、`firmware\rtl_bt\rtl8852bu_fw.bin`、`firmware\rtl_bt\rtl8852bu_config.bin`。

若固件位于其它目录，构建时用 `-FirmwareDir <目录>` 指定该目录。缺少固件文件时，构建脚本报错并停止。固件的哈希与 `SHA256SUMS` 不同时，构建脚本仅显示警告 `untested firmware`，不会停止。

<a id="certificate"></a>
## 准备测试证书

构建脚本使用用户自己的代码签名证书为驱动做测试签名。证书须位于构建电脑的 `Cert:\CurrentUser\My` 中，并带有私钥。

> **注意**：证书的私钥只能保存在构建电脑上，切勿复制给他人。驱动包中的 `rtw89win-test.cer` 仅含公钥。

若构建电脑上已有带私钥的代码签名证书，可直接使用该证书，并跳过第 1 步。用以下命令查询其指纹：

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert
```

在构建电脑上，在仓库根目录打开 PowerShell 7：

1. 创建测试证书。

   ```powershell
   .\tools-dev\new-testcert.ps1
   ```

   **结果**：输出 `thumbprint: <指纹>`。证书名缺省为 `CN=rtw89win test`，有效期 5 年。
2. 记下指纹。
3. 选择一种方式将指纹提供给构建脚本：
   - 每次构建时指定 `-CertThumbprint <指纹>`；
   - 设置环境变量 `RTW89WIN_CERT`；
   - 在 `local.settings.psd1` 中写入 `CertThumbprint = '<指纹>'`。

`new-testcert.ps1` 的脚本参数：`-Subject`（证书名，缺省 `'CN=rtw89win test'`）、`-Years`（有效期，缺省 `5`）。

不再需要某个测试证书时，删除该证书及其私钥：

```powershell
Remove-Item Cert:\CurrentUser\My\<指纹> -DeleteKey
```

也可以用 `-NoSign` 构建未签名的驱动包。这样的驱动包中没有 `rtw89win-test.cer`。安装前须自行为 `.sys` 和 `.cat` 签名，所用证书须受目标设备信任。

<a id="build-wifi"></a>
## 构建 Wi-Fi 驱动包

`build.ps1` 先用 clang 编译上游 rtw89、兼容层和 mac 层（`buildhal.ps1`），然后用 MSBuild 编译驱动，最后生成目录文件并签名。

在构建电脑上：

1. 打开 PowerShell 7，进入仓库根目录。
2. 运行构建脚本。

   ```powershell
   .\build.ps1 -CertThumbprint <指纹>
   ```

   若已通过环境变量或 `local.settings.psd1` 提供指纹，请删去 `-CertThumbprint <指纹>`。

   **结果**：没有报错；最后一行为 `output: <驱动包目录>`。
3. 查看驱动包中的文件。

   ```powershell
   Get-ChildItem <驱动包目录> -Name
   ```

   **结果**：列出下表中的文件。

| 文件 | 内容 |
|---|---|
| `rtw89win.sys`、`rtw89win.inf`、`rtw89win.cat` | Wi-Fi 驱动、INF、目录文件 |
| `rtw8852b_fw-2.bin` | Wi-Fi 固件 |
| `LICENCE.rtlwifi_firmware.txt` | 固件的许可 |
| `LICENSE`、`THIRD_PARTY_NOTICES` | 本项目的许可和第三方许可声明 |
| `rtw89win-test.cer` | 测试证书的公钥（使用 `-NoSign` 时没有） |
| `install-rtw89win.ps1`、`uninstall-rtw89win.ps1`、`rtwdiag.ps1` | 在目标设备上运行的安装、卸载、诊断脚本 |
| `rollback-rtw89win.sh` | Windows 无法启动时，在 Linux 系统中运行的回滚脚本 |
| `SHA256SUMS` | 驱动包中每个文件的 SHA256；安装脚本据此核对文件 |

驱动包目录缺省为 `out\<时间>\`，`<时间>` 为构建时间，格式为 `yyyyMMdd-HHmmss`。将整个目录复制到目标设备，然后按[安装与使用](install.md)安装。

<a id="build-bt"></a>
## 构建蓝牙驱动包

`bt\rtkbtflt\build.ps1` 先构建 `rtkbtfw`（并运行其主机测试），然后编译蓝牙过滤驱动，最后生成目录文件并签名。

在构建电脑上，在仓库根目录打开 PowerShell 7：

1. 运行构建脚本。

   ```powershell
   .\bt\rtkbtflt\build.ps1 -CertThumbprint <指纹>
   ```

   **结果**：输出中有一行以 `PASS:` 开头（`rtkbtfw` 的主机测试）；最后一行为 `output: <驱动包目录>`。

| 文件 | 内容 |
|---|---|
| `rtkbtflt.sys`、`rtkbtflt.inf`、`rtkbtflt.cat` | 蓝牙过滤驱动、INF、目录文件 |
| `rtl8852bu_fw.bin`、`rtl8852bu_config.bin` | 蓝牙补丁及其配置文件 |
| `LICENCE.rtlwifi_firmware.txt` | 固件的许可 |
| `LICENSE`、`THIRD_PARTY_NOTICES` | 本项目的许可和第三方许可声明 |
| `rtw89win-test.cer` | 测试证书的公钥（使用 `-NoSign` 时没有） |
| `rtkbtfw.exe` | ARM64 的用户态工具；`rtkbtfw info` 显示控制器的版本 |
| `install-rtkbtflt.ps1`、`uninstall-rtkbtflt.ps1`、`rtkbtflt.ps1` | 在目标设备上运行的安装、卸载、诊断脚本 |
| `rollback-rtkbtflt.sh` | Windows 无法启动时，在 Linux 系统中运行的回滚脚本 |
| `SHA256SUMS` | 驱动包中每个文件的 SHA256 |

驱动包目录缺省为 `out\rtkbtflt-<时间>\`。

仅构建 `rtkbtfw` 时，运行 `.\bt\rtkbtfw\build.ps1`。该脚本在构建电脑上运行主机测试，然后生成 ARM64 的 `rtkbtfw.exe`。输出目录为 `out\rtkbtfw-<时间>\`，含工具、蓝牙补丁和许可文件，不是驱动包。

加 `-NoPackage` 时只构建和测试，不生成该输出目录。主机测试须读取蓝牙补丁，因此加 `-NoPackage` 时也须有 `firmware\rtl_bt\` 中的两个文件，或用 `-FirmwareDir` 指定其位置。

<a id="host-tests"></a>
## 运行主机测试

主机测试在构建电脑上运行，不需要目标设备。每个测试将一部分源码编译为 x64 程序，然后运行检查。

在构建电脑上，在仓库根目录打开 PowerShell 7：

1. 运行组主帧格式的测试（`mac\apframe.c`）。

   ```powershell
   .\tools-dev\apframe-test.ps1
   ```

   **结果**：最后一行为 `PASS`。
2. 运行信道宽度的测试（`mac\wide.c`）。

   ```powershell
   .\tools-dev\wide-test.ps1
   ```

   **结果**：最后一行为 `PASS`。
3. 运行加密的已知答案测试（`driver\crypto.c`）。

   ```powershell
   .\tools-dev\crypto-test.ps1
   ```

   **结果**：最后一行为 `PASS`。
4. 运行蓝牙补丁解析的主机测试（`bt\rtkbtfw\epatch.c`）。须有 `firmware\rtl_bt\` 中的两个文件。

   ```powershell
   .\bt\rtkbtfw\build.ps1 -NoPackage
   ```

   **结果**：输出中有一行 `PASS: 723 checks, 0 failed`。
5. 构建 Wi-Fi 驱动包之后，统计 `rtw89hal.lib` 引用但未自行定义的外部符号数目。

   ```powershell
   (.\tools-dev\undef.ps1).Count
   ```

   若 LLVM 不在缺省位置，且未写入 `local.settings.psd1`，请加上 `-LlvmBin <LLVM 的 bin 目录>`。

   **结果**：`45`。数目变化说明上游 rtw89、兼容层或 mac 层用到了新的外部函数。

`undef.ps1 -Where` 同时列出引用每个符号的目标文件。各构建脚本分别读取设置：指定给 `build.ps1` 的 `-LlvmBin` 不会传给 `undef.ps1`。

<a id="parameters"></a>
## 参数和设置

每个设置按以下顺序取值：脚本参数 → 环境变量 → 仓库根目录的 `local.settings.psd1` → 缺省值。`local.settings.psd1` 不纳入 git，格式见 `local.settings.example.psd1`。设置中的相对路径相对于仓库根目录。

| 设置 | 脚本参数 | 环境变量 | `local.settings.psd1` 键 | 缺省 |
|---|---|---|---|---|
| WDK 和 Windows SDK 的 NuGet 包 | `-PackagesDir` | `RTW89WIN_PACKAGES` | `PackagesDir` | `packages\` |
| LLVM 的 `bin` 目录 | `-LlvmBin` | `RTW89WIN_LLVM` | `LlvmBin` | `PATH` 中 `clang.exe` 所在的目录，否则为 `%ProgramFiles%\LLVM\bin` |
| 上游 rtw89 源码 | `-RtwSrc` | `RTW89WIN_RTW89_SRC` | `RtwSrc` | `third_party\rtw89\` |
| 固件目录 | `-FirmwareDir` | `RTW89WIN_FIRMWARE` | `FirmwareDir` | `firmware\` |
| 测试证书的指纹 | `-CertThumbprint`；或 `-NoSign` | `RTW89WIN_CERT` | `CertThumbprint` | 无；没有指纹时须指定 `-NoSign` |
| Visual Studio 的安装目录 | `-VsPath` | `RTW89WIN_VS` | `VsPath` | `vswhere` 找到的、带 ARM64 生成工具的最新安装 |
| 驱动包目录 | `-OutDir` | 无 | 无 | `out\<时间>\`；蓝牙为 `out\rtkbtflt-<时间>\` |
| 驱动版本 | `-Version` | 无 | 无 | 当前源码的版本，写在构建脚本的 `param` 行 |
| 目标设备的 WLAN 地址（仅用于 `hosttools\`） | `-BoardWlan` | `RTW89WIN_BOARD_WLAN` | `BoardWlan` | 无 |
| 目标设备的 ssh 主机名（仅用于 `hosttools\`） | `-SshHost` | `RTW89WIN_SSH_HOST` | `SshHost` | 无 |

`-OutDir` 的相对路径相对于当前目录。`-OutDir` 指定的目录已存在且不为空时，构建脚本报错并停止。该检查在编译开始前进行。

各脚本接受的脚本参数：

| 脚本 | 脚本参数 |
|---|---|
| `build.ps1` | `-Version`、`-CertThumbprint`、`-NoSign`、`-OutDir`、`-PackagesDir`、`-LlvmBin`、`-RtwSrc`、`-FirmwareDir`、`-VsPath` |
| `bt\rtkbtflt\build.ps1` | `-Version`、`-CertThumbprint`、`-NoSign`、`-OutDir`、`-PackagesDir`、`-FirmwareDir`、`-VsPath` |
| `bt\rtkbtfw\build.ps1` | `-NoPackage`、`-OutDir`、`-PackagesDir`、`-FirmwareDir`、`-VsPath` |
| `buildhal.ps1` | `-LlvmBin`、`-RtwSrc`、`-Only <目标文件名>`、`-ExtraFlags <编译选项>`、`-ShowErrors <数目>`（每个失败的目标文件显示的错误条数，缺省 15） |
| `tools-dev\errsum.ps1` | `<目标文件名>`（可给多个）、`-Top <数目>`（每个目标文件显示的错误消息种数，缺省 40） |
| `tools-dev\apframe-test.ps1`、`wide-test.ps1`、`crypto-test.ps1` | `-VsPath` |
| `tools-dev\undef.ps1` | `-LlvmBin`、`-Where` |
| `tools-dev\new-testcert.ps1` | `-Subject`、`-Years` |
| `tools-dev\regdb-gen.ps1` | `-Path <regulatory.db>`、`-Version <wireless-regdb 版本>`（均须指定） |

`buildhal.ps1` 通常由 `build.ps1` 调用。单独运行该脚本，可以只编译部分文件（`-Only`），或加入检查用的编译选项，例如 `-ExtraFlags '-Wframe-larger-than=1024'`。更换芯片时的构建改动见[移植指南](porting.md)。

<a id="troubleshooting"></a>
## 构建问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 报告 `WDK / Windows SDK NuGet packages not found in <目录>` | NuGet 包未取得，或不在缺省位置 | 运行 `nuget restore packages.config -PackagesDirectory packages`；包位于其它位置时，用 `-PackagesDir` 指定 |
| 报告 `LLVM not found` | 找不到 `clang.exe` | 安装 LLVM；或用 `-LlvmBin` 指定其 `bin` 目录 |
| 报告 `vswhere.exe not found` | 未安装 Visual Studio 2022 或其 Build Tools | 安装 Visual Studio 2022 或其 Build Tools；或用 `-VsPath` 指定安装目录 |
| 报告 `vswhere found no Visual Studio with the component 'MSVC ARM64 build tools'` | 缺少 ARM64 生成工具 | 在 Visual Studio 安装程序中添加组件 `Microsoft.VisualStudio.Component.VC.Tools.ARM64` |
| 报告 `no MSVC tools under <目录>` | `-VsPath` 指向的不是 Visual Studio 的安装目录 | 更正 `-VsPath` |
| 报告 `rtw89 sources not found` | `-RtwSrc` 指向的目录中没有 `core.c` | 删去 `-RtwSrc`，使用 `third_party\rtw89\`；或更正路径 |
| 报告 `firmware file not found: <文件>` | 固件文件不在固件目录中 | 按[固件获取说明](../../firmware/README.md)放入固件；或用 `-FirmwareDir` 指定固件目录 |
| 警告 `<文件> has SHA256 <哈希>, not the tested <哈希>` | 固件与测过的版本不同 | 换用测过的固件；不更换也能构建，但驱动包使用的是未经测试的固件 |
| 报告 `No test-signing certificate` | 未提供证书指纹 | 按[准备测试证书](#certificate)操作；或用 `-NoSign` |
| 报告 `certificate <指纹> not found in Cert:\CurrentUser\My` | 指纹有误，或证书位于其他用户的证书存储中 | 以创建该证书的 Windows 用户运行构建；更正指纹 |
| 报告 `certificate <指纹> has no private key` | 证书没有私钥 | 用 `new-testcert.ps1` 创建新证书 |
| 报告 `output directory <目录> exists and is not empty` | `-OutDir` 指定的目录中已有文件 | 换用其它 `-OutDir`；或先清空该目录 |
| 报告 `<数目> file(s) failed` | clang 编译失败 | 查看该行之前的目标文件表和 `---- <目标文件名>: first errors`（前 3 个失败的目标文件，各列前 15 条错误）；完整输出见 `build\ARM64\Release\hal\<目标文件名>.log`；运行 `.\tools-dev\errsum.ps1 <目标文件名>` 可按消息汇总错误 |
| 报告 `build failed (<数字>)` | MSBuild 编译或链接失败 | 查看 `build\build-arm64.log`（蓝牙为 `build\rtkbtflt\build-arm64.log`） |
| MSBuild 显示警告 MSB8029 | 仓库位于临时目录（`%TEMP%`）下 | 不影响结果；可将仓库移到临时目录以外 |
| 在 Windows PowerShell 5.1（`powershell.exe`）中运行时报错 | 构建脚本使用了 PowerShell 7 才有的功能（`ForEach-Object -Parallel`） | 在 PowerShell 7（`pwsh`）中运行 |

<a id="signing"></a>
## 关于签名

本项目只提供测试签名的构建方法。测试签名的驱动仅在测试签名模式下加载，打开该模式前须关闭安全启动。

不依赖测试签名模式的正式签名须满足以下条件：

- 拥有微软硬件开发者计划的组织账号；
- 拥有 EV 代码签名证书；
- 驱动通过 WHQL 认证，或经过微软的证明签名（attestation signing）。

本项目不提供正式签名的驱动。

<a id="redistribute"></a>
## 向他人提供驱动包

驱动包中含 Realtek 的固件文件。固件的许可只允许不加修改地再分发二进制文件，并要求附上其版权声明和免责声明。

驱动的二进制文件含本项目的代码和上游 rtw89 的代码。其 BSD-3-Clause 许可要求：以二进制形式再分发时，须附带版权声明和许可条款。

1. 不得修改驱动包中的固件文件。
2. 保留驱动包中的 `LICENSE`、`THIRD_PARTY_NOTICES` 和 `LICENCE.rtlwifi_firmware.txt`。构建脚本已将它们放入驱动包。

各部分的许可和义务见[第三方许可说明](licenses.md#redistribute)。
