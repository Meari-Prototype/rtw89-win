# 固件获取说明

[English](README.en.md)

- **适用读者**：构建者和使用者。
- **本文任务**：取得 3 个固件文件，核对 SHA256，并将文件放到构建脚本读取的位置。
- **前提**：可从互联网下载文件；构建电脑上已安装 PowerShell 7。用 git 取得文件时，还需要 git。

<a id="summary"></a>
## 要点

1. 本仓库不含固件。构建驱动包前，须自行取得 3 个固件文件。
2. 3 个文件均来自 linux-firmware 项目。按完整提交号下载，即可得到测过的文件。
3. 以 SHA256 判断文件是否正确。下表的 SHA256 是测过的文件的值。
4. 固件采用 Realtek 的许可：仅允许不加修改地再分发二进制文件。

<a id="files"></a>
## 需要的文件

| 放到 | linux-firmware 中的路径 | 大小（字节） | SHA256（测过的） | 用途 |
|---|---|---|---|---|
| `firmware\rtw8852b_fw-2.bin` | `rtw89/rtw8852b_fw-2.bin` | 1 554 594 | `6ff6aeb56bb5b85cba333b755f8cc1dbfe9ff6e5b16e39f6ba01ce7f1eaa7714` | Wi-Fi 固件，放入 Wi-Fi 驱动包 |
| `firmware\rtl_bt\rtl8852bu_fw.bin` | `rtl_bt/rtl8852bu_fw.bin` | 74 324 | `f1e0427317f147a78606ede85f3f8977ed0ab85c41c6b525560e5bd5a51e5818` | 蓝牙补丁，放入蓝牙驱动包 |
| `firmware\rtl_bt\rtl8852bu_config.bin` | `rtl_bt/rtl8852bu_config.bin`（见[取得文件](#get)） | 6 | `6c28a3f07c6a30ed208c4b64862a23f02b7d93543ea980edd24df16bab45095f` | 蓝牙补丁的配置文件，放入蓝牙驱动包 |

相同的哈希也记录在 [`SHA256SUMS`](SHA256SUMS) 和 [`rtl_bt/SHA256SUMS`](rtl_bt/SHA256SUMS) 中。`rtl_bt/SHA256SUMS` 另含固件许可文件的哈希。构建脚本用这两个文件核对固件。

<a id="get"></a>
## 取得文件

测过的 3 个文件取自 linux-firmware 的两个提交：

| 文件 | 测过的文件的来源 |
|---|---|
| `rtw8852b_fw-2.bin`（固件版本 0.29.29.18） | 提交 `f97831a2895009967c50f94f6591787e6fbe3bd7` |
| `rtl8852bu_fw.bin`（蓝牙补丁版本 `0xdbc6b20f`）、`rtl8852bu_config.bin` | 提交 `3b128b6069267b60a46a4074c383183177ee6e6d` |

测过的蓝牙文件取自 Ubuntu 24.04 的软件包 `linux-firmware` `20240318.git3b128b60-0ubuntu2.25`，与上述提交中的文件逐字节相同。这些文件在 linux-firmware 中的更新版本哈希不同，未经测试。

在提交 `3b128b60` 中，`rtl_bt/rtl8852bu_config.bin` 是普通文件，内容与 `rtl_bt/rtl8761bu_config.bin` 相同。较新的提交从仓库中删除了该文件，仅在 `WHENCE` 中将其声明为指向 `rtl8761bu_config.bin` 的链接。使用较新的提交时，请下载 `rtl_bt/rtl8761bu_config.bin`，并另存为 `rtl8852bu_config.bin`。

### 按提交号下载

以下步骤从 linux-firmware 的 GitLab 仓库（<https://gitlab.com/kernel-firmware/linux-firmware>）下载文件。

在构建电脑上，在仓库根目录打开 PowerShell 7。请在同一窗口中执行全部步骤：

1. 设置提交号和下载地址。

   ```powershell
   $c1 = 'f97831a2895009967c50f94f6591787e6fbe3bd7'
   $c2 = '3b128b6069267b60a46a4074c383183177ee6e6d'
   $base = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw'
   ```

2. 创建固件目录。

   ```powershell
   New-Item -ItemType Directory -Force firmware\rtl_bt | Out-Null
   ```

3. 下载 Wi-Fi 固件。

   ```powershell
   Invoke-WebRequest "$base/$c1/rtw89/rtw8852b_fw-2.bin" -OutFile firmware\rtw8852b_fw-2.bin
   ```

4. 下载蓝牙的两个文件。

   ```powershell
   Invoke-WebRequest "$base/$c2/rtl_bt/rtl8852bu_fw.bin" -OutFile firmware\rtl_bt\rtl8852bu_fw.bin
   Invoke-WebRequest "$base/$c2/rtl_bt/rtl8852bu_config.bin" -OutFile firmware\rtl_bt\rtl8852bu_config.bin
   ```

5. 按[核对哈希](#verify)核对 3 个文件。

   **结果**：3 个值均与表中相同。

> **提示**：kernel.org 上的 linux-firmware 仓库提供等价的地址：`https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/<路径>?id=<完整提交号>`。`<路径>` 为[需要的文件](#files)表中"linux-firmware 中的路径"一列的值。

### 用 git 取得

该方法克隆 linux-firmware 时不下载文件内容，检出时只下载所需的文件。

在构建电脑上（PowerShell 7，需要 git），在仓库根目录：

1. 克隆 linux-firmware，不检出文件。

   ```powershell
   git clone --filter=blob:none --no-checkout https://gitlab.com/kernel-firmware/linux-firmware.git ..\linux-firmware
   ```

2. 检出测过的 Wi-Fi 固件。

   ```powershell
   git -C ..\linux-firmware checkout f97831a2895009967c50f94f6591787e6fbe3bd7 -- rtw89/rtw8852b_fw-2.bin
   ```

3. 检出蓝牙的两个文件。

   ```powershell
   git -C ..\linux-firmware checkout 3b128b6069267b60a46a4074c383183177ee6e6d -- rtl_bt/rtl8852bu_fw.bin rtl_bt/rtl8852bu_config.bin
   ```

4. 将文件复制到本仓库的 `firmware\`。

   ```powershell
   New-Item -ItemType Directory -Force firmware\rtl_bt | Out-Null
   Copy-Item ..\linux-firmware\rtw89\rtw8852b_fw-2.bin firmware\
   Copy-Item ..\linux-firmware\rtl_bt\rtl8852bu_fw.bin, ..\linux-firmware\rtl_bt\rtl8852bu_config.bin firmware\rtl_bt\
   ```

5. 按[核对哈希](#verify)核对 3 个文件。

   **结果**：3 个值均与表中相同。

<a id="verify"></a>
## 核对哈希

在构建电脑上，在仓库根目录打开 PowerShell 7：

1. 运行 `Get-FileHash`，计算 3 个文件的 SHA256。

   ```powershell
   Get-FileHash -Algorithm SHA256 firmware\rtw8852b_fw-2.bin, firmware\rtl_bt\rtl8852bu_fw.bin, firmware\rtl_bt\rtl8852bu_config.bin | Format-List Hash, Path
   ```

   **结果**：每个文件对应一个 `Hash`。
2. 将每个 `Hash` 与[需要的文件](#files)表中的 SHA256 比较。比较时不区分大小写。

   **结果**：3 个值均相同。

若哈希不同，构建脚本不会停止，只显示一条警告，警告末尾为 `untested firmware`。这样构建出的驱动包使用的是未经测试的固件。

<a id="place"></a>
## 放置文件

按[需要的文件](#files)表的"放到"一列放置文件。路径相对于仓库根目录：

```text
firmware\
  rtw8852b_fw-2.bin
  rtl_bt\
    rtl8852bu_fw.bin
    rtl8852bu_config.bin
```

固件也可以放在其它目录。该目录须具有相同的结构（蓝牙的两个文件位于子目录 `rtl_bt\` 中）。构建时用 `-FirmwareDir <目录>` 指定该目录，见[构建](../docs/zh/build.md#parameters)。

> **提示**：仓库的 `.gitignore` 忽略 `firmware` 下的 `.bin` 文件，因此固件不会被误提交。

<a id="license"></a>
## 许可

3 个文件均采用 Realtek 的固件许可 [`LICENCE.rtlwifi_firmware.txt`](LICENCE.rtlwifi_firmware.txt)。linux-firmware 的 `WHENCE` 文件写明这些文件可以再分发，摘录见 [`WHENCE-rtw89-excerpt.txt`](WHENCE-rtw89-excerpt.txt) 和 [`rtl_bt/WHENCE-rtl_bt-excerpt.txt`](rtl_bt/WHENCE-rtl_bt-excerpt.txt)。

许可条款的概括（具有效力的是英文原文）：

1. 仅许可以二进制形式、不加修改地再分发和使用。
2. 再分发时，须在文档或附带材料中附上版权声明和免责声明。
3. 不得使用 Realtek 或其供应商的名称为衍生产品背书。
4. 不得逆向工程、反编译或反汇编。
5. 专利许可有范围限制。它只覆盖两种用法所必需的专利：单独使用该软件；与采用 OSI 认可开源许可的操作系统组合使用。原文随后写道："The patent license shall not apply to any other combinations which include this software."
6. 软件按"原样"提供，不附带任何担保。

本文不解释这些条款。在 Windows 上使用固件之前，请自行阅读许可原文。再分发驱动包时的要求见[第三方许可说明](../docs/zh/licenses.md#firmware)和[构建](../docs/zh/build.md#redistribute)。
