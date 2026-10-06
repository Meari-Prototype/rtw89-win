# 第三方许可说明

[English](../en/licenses.md)

- **适用读者**：许可审阅者；需要将驱动包提供给他人的构建者。
- **本文内容**：本项目代码和每个第三方部分的许可、出处和义务；固件许可的限制。
- **前提**：无。

本文是中文说明，不是法律文本。具有效力的是英文原文：[`LICENSE`](../../LICENSE)、[`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES) 和各文件开头的许可声明。本文与原文不一致时，以原文为准。本文不对任何用法是否合规作出判断。

<a id="summary"></a>
## 要点

- 本项目（非官方）的自有代码按 BSD-3-Clause 授权，版权声明为 `Copyright (c) 2026 Meari`。
- 上游 rtw89 的 49 个文件原样存放在 `third_party\rtw89\`。这些文件采用 GPL-2.0 OR BSD-3-Clause 双许可，本项目按其中的 BSD-3-Clause 使用。
- `compat\include\` 中有 8 个取自 FreeBSD 的头文件，按 BSD-2-Clause 或 BSD-3-Clause 授权。
- 管制规则数据来自 wireless-regdb 和 Linux `net/wireless/reg.c`，均按 ISC 授权。
- 固件不在本仓库中。固件按 Realtek 的固件许可再分发：仅允许不经修改的二进制形式，禁止逆向工程。
- WDK、Windows SDK、Visual Studio 生成工具和 LLVM 不在本仓库中，也不随驱动包分发。
- 构建脚本将 `LICENSE`、`THIRD_PARTY_NOTICES` 和固件许可放入驱动包。

<a id="ours"></a>
## 本项目的代码

本仓库中，除下文列出的第三方部分外，其它文件均按 BSD-3-Clause 授权。许可全文见 [`LICENSE`](../../LICENSE)。

本项目的源文件在开头的注释中有两行声明：

```text
SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2026 Meari
```

注释符号因文件类型而异：`.c`、`.cpp`、`.cs` 用 `//`，`.h` 用 `/* */`，`.ps1`、`.psd1`、`.sh` 用 `#`（`.sh` 在 `#!` 行之后），`.inf` 用 `;`，`.vcxproj`、`.props`、`.targets` 和 `packages.config` 用 XML 注释。

以下目录中的文件按 BSD-3-Clause 授权：

| 目录 | 内容 | 例外 |
|---|---|---|
| 根目录 | `build.ps1`、`buildhal.ps1`、`packages.config`、`local.settings.example.psd1` | 无 |
| `driver\` | Wi-Fi 驱动的驱动层、INF | 无 |
| `wificx\` | WiFiCx 前端 | 无 |
| `mac\` | mac 层 | `mac\regdb.h` 按 ISC 授权；`mac\reg.c` 中的全球通用规则数据按 ISC 授权（见[管制规则数据](#regdb)） |
| `compat\` | 兼容层 | 8 个 FreeBSD 头文件（见 [FreeBSD 头文件](#freebsd)） |
| `bt\` | 蓝牙过滤驱动和 `rtkbtfw` 工具 | 无 |
| `tools\`、`tools-dev\`、`hosttools\` | 安装、诊断、构建和测试脚本 | `tools-dev\regdb-gen.ps1` 本身按 BSD-3-Clause 授权，其生成的 `mac\regdb.h` 按 ISC 授权 |
| `docs\` | 本文档集 | 无 |

BSD-3-Clause 的三项条件（概括，以 [`LICENSE`](../../LICENSE) 原文为准）：

1. 再分发源代码时，须保留版权声明、条件列表和免责声明。
2. 以二进制形式再分发时，须在文档或附带材料中复制版权声明、条件列表和免责声明。
3. 未经书面许可，不得使用版权人或贡献者的名称为衍生产品背书或推广。

<a id="third-party"></a>
## 第三方部分一览

下表对应 [`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES) 的 A～F 节。

| 部分 | 位置 | 来源和版本 | 许可 | 义务 |
|---|---|---|---|---|
| A. 上游 rtw89 | `third_party\rtw89\`（49 个文件） | Linux，标签 v7.2，提交 `8d3ae59288f1e7d58d76558a6ee96d533bc5019f` | GPL-2.0 OR BSD-3-Clause，本项目按 BSD-3-Clause 使用 | 保留文件头和版权声明；以二进制形式再分发时复制 BSD-3-Clause 的声明 |
| B. FreeBSD LinuxKPI 802.11 头文件 | `compat\include\linux\`、`compat\include\net\`（6 个文件） | FreeBSD，提交 `262fa4695d90207b9080f2cc5cb01eccf07fe243` | BSD-2-Clause | 保留文件开头的许可全文；以二进制形式再分发时复制声明 |
| C. FreeBSD net80211 头文件 | `compat\include\net80211\`（2 个文件） | 同上 | BSD-2-Clause（`ieee80211.h`）；BSD-3-Clause（`ieee80211_radiotap.h`） | 同上 |
| D. 管制规则数据 | `mac\regdb.h`；`mac\reg.c` 的 `reg_world_builtin` | wireless-regdb 2025.10.07；Linux v7.2 `net/wireless/reg.c` | ISC | 在所有副本中保留版权声明和许可声明 |
| E. 只作参考的资料 | 无文件 | Linux mac80211、cfg80211、内核头文件、btrtl；微软 Windows-driver-samples；RFC 4493；IEEE Std 802.11-2020 | 未复制代码 | 无 |
| F. 不随附的内容 | 不在本仓库中 | 固件；WDK、Windows SDK、Visual Studio 生成工具、LLVM | 各自的许可 | 固件见[固件许可](#firmware) |

<a id="rtw89"></a>
## 上游 rtw89

- **来源**：Linux 仓库 `https://github.com/torvalds/linux.git`，标签 v7.2，提交 `8d3ae59288f1e7d58d76558a6ee96d533bc5019f`，路径 `drivers/net/wireless/realtek/rtw89/`。
- **文件**：`buildhal.ps1` 编译的 26 个 `.c` 文件，加上其包含的 23 个 `.h` 文件，共 49 个。本项目未修改其中任何文件。
- **许可**：每个文件的第一行均为 `SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause`。本项目按 BSD-3-Clause 使用。两份许可原文均位于 `third_party\rtw89\LICENSES\`：[`BSD-3-Clause`](../../third_party/rtw89/LICENSES/BSD-3-Clause) 和 [`GPL-2.0`](../../third_party/rtw89/LICENSES/GPL-2.0)。
- **版权**：Copyright (c) 2019-2026 Realtek Corporation。各文件的年份见文件头。
- **其它用法**：`driver\hw.c` 使用了 rtw89 `reg.h` 中的寄存器偏移。

[`third_party\rtw89\README.md`](../../third_party/rtw89/README.md) 列出每个文件的 git blob id。按以下步骤核对文件未被修改。

在任意电脑上（需要 git），在仓库根目录：

1. 运行 `git hash-object`，计算一个文件的 blob id。

   ```sh
   git hash-object third_party/rtw89/core.c
   ```

   **结果**：输出的 40 位十六进制值与 `third_party\rtw89\README.md` 中同一文件的 blob id 相同。

<a id="freebsd"></a>
## FreeBSD 头文件

rtw89 按 Linux 的 802.11 接口编译。本项目的接口头文件取自 FreeBSD 的 LinuxKPI，而非 Linux。来源提交记录在 `compat\FREEBSD_REV` 中：`262fa4695d90207b9080f2cc5cb01eccf07fe243`。

| 文件 | FreeBSD 路径 | 许可 | 是否有改动 |
|---|---|---|---|
| `compat\include\linux\ieee80211.h` | `sys/compat/linuxkpi/common/include/linux/ieee80211.h` | BSD-2-Clause | 有 |
| `compat\include\linux\nl80211.h` | `sys/compat/linuxkpi/common/include/linux/nl80211.h` | BSD-2-Clause | 无 |
| `compat\include\net\cfg80211.h` | `sys/compat/linuxkpi/common/include/net/cfg80211.h` | BSD-2-Clause | 无 |
| `compat\include\net\ieee80211_radiotap.h` | `sys/compat/linuxkpi/common/include/net/ieee80211_radiotap.h` | BSD-2-Clause | 无 |
| `compat\include\net\mac80211.h` | `sys/compat/linuxkpi/common/include/net/mac80211.h` | BSD-2-Clause | 有 |
| `compat\include\net\regulatory.h` | `sys/compat/linuxkpi/common/include/net/regulatory.h` | BSD-2-Clause | 无 |
| `compat\include\net80211\ieee80211.h` | `sys/net80211/ieee80211.h` | BSD-2-Clause | 无 |
| `compat\include\net80211\ieee80211_radiotap.h` | `sys/net80211/ieee80211_radiotap.h` | BSD-3-Clause | 无 |

- 每个文件开头的版权声明和许可全文保持原样。
- LinuxKPI 的 6 个文件在许可块之后有一段注释，写明来源路径和提交。
- 在有改动的两个文件中，改动处标有 `rtw89win`。这些改动的版权为 Copyright (c) 2026 Meari，按相同的 BSD-2-Clause 条款授权。
- 版权人：LinuxKPI 头文件为 The FreeBSD Foundation 和 Bjoern A. Zeeb（`net\ieee80211_radiotap.h` 只有 The FreeBSD Foundation）；`net80211\ieee80211.h` 为 Atsushi Onoe、Sam Leffler（Errno Consulting）和 The FreeBSD Foundation；`net80211\ieee80211_radiotap.h` 为 David Young 和 The FreeBSD Foundation。年份见各文件。

<a id="regdb"></a>
## 管制规则数据

| 数据 | 位置 | 来源 | 许可 |
|---|---|---|---|
| 各国的管制规则 | `mac\regdb.h` | wireless-regdb 2025.10.07 的 `regulatory.db`（SHA256 `5560f4f0fdac7d1bb2adf8d4d083f39e3bee5ba55192feadadc091df55a813eb`），由 `tools-dev\regdb-gen.ps1` 转换生成 | ISC |
| 内置的全球通用规则 | `mac\reg.c` 的 `reg_world_builtin` | Linux v7.2 `net/wireless/reg.c` 的 `world_regdom` 中 2.4 GHz 和 5 GHz 的规则 | ISC |

- `mac\regdb.h` 的文件头带有 wireless-regdb 的版权声明和 ISC 许可全文。
- `world_regdom` 的版权人列在 [`THIRD_PARTY_NOTICES`](../../THIRD_PARTY_NOTICES) 的 D 节。
- ISC 许可的条件：在所有副本中保留版权声明和本许可声明。

<a id="references"></a>
## 只作参考的资料

下列资料仅用于了解行为和接口。本项目未复制其代码。

| 资料 | 许可 | 本项目的使用方式 |
|---|---|---|
| Linux mac80211、cfg80211（`net/mac80211`、`net/wireless`）和 Linux 内核头文件 | GPL-2.0 等 | mac 层和兼容层由本项目自行实现。兼容层提供 Linux 内核接口（名称、结构布局、宏和函数签名），因为原样的上游 rtw89 须按这些接口编译。 |
| Linux btrtl（`drivers/bluetooth/btrtl.c`） | GPL-2.0-or-later | `bt\rtkbtfw\epatch.c` 含 btrtl 的 project id 与 LMP 子版本对照表和补丁文件规则，作为互通所需的事实。蓝牙补丁的下载代码由本项目自行实现。 |
| 微软 Windows-driver-samples（`network/wlan/wificx`、`network/wlan/WDI`、`network/netadaptercx`） | MS-PL | WiFiCx 前端（`wificx\`）参照 WiFiCx 示例的结构。未包含示例代码。 |
| RFC 4493（AES-CMAC）、IEEE Std 802.11-2020 附录 J.6.4（CCMP） | 标准文本 | `driver\crypto.c` 的已知答案测试向量取自这两份文档。 |

<a id="firmware"></a>
## 固件许可

本仓库不含固件。驱动需要 3 个固件文件：`rtw8852b_fw-2.bin`、`rtl8852bu_fw.bin` 和 `rtl8852bu_config.bin`。测过的文件取自 linux-firmware 的指定提交。提交号、获取方法和 SHA256 见[固件获取说明](../../firmware/README.md)。

固件的许可为 [`LICENCE.rtlwifi_firmware.txt`](../../firmware/LICENCE.rtlwifi_firmware.txt)（Copyright (c) 2010, Realtek Semiconductor Corporation）。其条款概括如下（以英文原文为准）：

1. 仅许可以二进制形式、不经修改地再分发和使用。
2. 再分发时，须在文档或附带材料中复制版权声明和免责声明。
3. 未经书面许可，不得使用 Realtek 或其供应商的名称为衍生产品背书或推广。
4. 不得逆向工程、反编译或反汇编。
5. 专利许可有范围限制。它只覆盖两种用法所必需的专利：单独使用该软件；与采用 OSI 认可开源许可的操作系统组合使用。原文写明："The patent license shall not apply to any other combinations which include this software." 本项目的驱动在 Windows 上加载这些固件。本文不解释该条款，请阅读许可原文。
6. 不提供任何担保。

本项目对固件仅执行两项操作：

- 将固件文件原样复制到驱动包中，由驱动下载到芯片。
- 蓝牙过滤驱动按 btrtl 的容器格式读取补丁文件头，并将选中的补丁原样发送给控制器。

本项目不修改、不反汇编固件。

<a id="not-included"></a>
## 不随仓库分发的内容

| 内容 | 说明 |
|---|---|
| 固件 | `rtw8852b_fw-2.bin`、`rtl8852bu_fw.bin`、`rtl8852bu_config.bin`。从 linux-firmware 取得，见[固件获取说明](../../firmware/README.md)。测过的 SHA256 记录在 `firmware\SHA256SUMS` 和 `firmware\rtl_bt\SHA256SUMS` 中。 |
| WDK 和 Windows SDK | 从微软取得。`packages.config` 列出所需的 NuGet 包和版本。构建时，驱动链接 WDK 的库 `WificxTLVGenParse.lib`、`libcntpr.lib`、`ksecdd.lib`、`usbd.lib`。 |
| Visual Studio 生成工具、LLVM | 从各自的发布者取得。 |
| 编译好的驱动包 | 本项目不提供。构建方法见[构建](build.md)。 |

<a id="redistribute"></a>
## 再分发驱动包时

自行构建的驱动包含三类内容：本项目的二进制（BSD-3-Clause）、编译进去的上游 rtw89 等第三方代码，以及 Realtek 的固件。各自许可的条件见上文。

构建脚本将 `LICENSE`、`THIRD_PARTY_NOTICES` 和 `LICENCE.rtlwifi_firmware.txt` 复制到驱动包中。这三个文件均列在 `SHA256SUMS` 中，安装脚本会核对它们。

在构建电脑上（PowerShell，当前目录为仓库根目录），将驱动包提供给他人之前：

1. 确认驱动包中有这三个许可文件。

   ```powershell
   Test-Path <驱动包目录>\LICENSE, <驱动包目录>\THIRD_PARTY_NOTICES, <驱动包目录>\LICENCE.rtlwifi_firmware.txt
   ```

   `<驱动包目录>`：构建脚本输出的目录，例如 `out\<时间>`。

   **结果**：输出三个 `True`。

2. 计算驱动包中固件文件的 SHA256。

   ```powershell
   Get-FileHash -Algorithm SHA256 <驱动包目录>\*.bin
   ```

   **结果**：每个 `.bin` 文件的哈希与 `firmware\SHA256SUMS` 或 `firmware\rtl_bt\SHA256SUMS` 中同名文件的值相同。

<a id="trademarks"></a>
## 名称和非官方声明

- 本项目为非官方项目，与 Realtek、Microsoft、Radxa 无关，也未获得其认可或支持。
- Realtek、Microsoft、Windows、Radxa 和 ROCK 是各自所有者的名称或商标。本文档仅用这些名称说明硬件和软件环境。
- 两个 INF 的提供者（`Provider`）均为 `rtw89win`。两个 INF 中的设备描述（`DeviceDesc`）分别为 "Realtek RTL8852BE Wi-Fi (rtw89win)" 和 "Realtek RTL8852BE Bluetooth patch loader (rtkbtflt)"。其中 "Realtek" 是 INF 按惯例填写的设备制造商，括号中的名称表示驱动来自本项目。

术语见[术语表](glossary.md)。
