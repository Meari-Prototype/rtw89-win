# 移植指南

[English](../en/porting.md)

- **适用读者**：移植者，包括将 rtw89 的其它芯片、其它硬件平台或其它 Linux Wi-Fi 驱动接入 Windows 的人员。
- **本文任务**：了解添加一颗 rtw89 PCIe 芯片、更换硬件平台、更换驱动家族时需修改哪些文件，以及如何验证。
- **前提**：已阅读[架构](architecture.md)；能按[构建](build.md)编译出驱动包。

<a id="summary"></a>
## 要点

1. 上游 rtw89 不作修改。添加芯片时只加入上游的源文件。
2. 添加一颗 rtw89 PCIe 芯片需修改以下几处：`buildhal.ps1`、INF 和脚本中的硬件 ID、固件文件、INTx 屏蔽寄存器、上报的能力。新芯片的文件用到兼容层尚未提供的 Linux API 时，须将其补入兼容层。一个构建支持多颗芯片时，还须修改兼容层的驱动登记。
3. mac 层按 RTL8852B 的特性编写。新芯片须逐条核对 15 条假设（见 [mac 层的假设](#mac-assumptions)）。
4. 一个构建目前只支持一颗芯片。多芯片登记表尚未实现。
5. USB 和 SDIO 未实现，也未测试。
6. 本指南只在 RTL8852BE 上完整验证过。其它芯片的步骤属于代码推断，未在目标设备上验证。

<a id="boundaries"></a>
## 各层接口边界

结论：添加 rtw89 芯片时，驱动层和 WiFiCx 前端的接口保持不变。改动集中在兼容层的入口、mac 层的假设和能力上报。完整的接口表见[层间接口](architecture.md#interfaces)。

| 层 | 对上的接口 | 对下的接口 | 添加 rtw89 PCIe 芯片时 | 更换驱动家族时 |
|---|---|---|---|---|
| 驱动层 | 诊断 IOCTL；设置项 | 实现 `rtwk_*`（`os.h`）；调用 `rtw_linux_*`（`glue.h`） | 不变：PCI 设备、BAR2、INTx 相同 | USB 或 SDIO 须添加总线原语 |
| WiFiCx 前端 | WDI 消息 | `rtw_linux_*`（`glue.h`） | 核对能力和最大链路速率 | `glue.h` 不变时无需修改 |
| mac 层 | `rtw_linux_*` | `ieee80211_ops` | 按假设表核对 | 补充 txq、软件扫描等 |
| 上游驱动 | `ieee80211_ops` | Linux 内核 API | 加入源文件，不作修改 | 替换为新驱动的源文件 |
| 兼容层 | Linux 内核 API | `rtwk_*`；入口 `rtw_linux_probe` 等 | INTx 寄存器；新芯片文件用到而兼容层尚缺的 Linux API；多芯片时的驱动登记 | 补充缺少的 Linux API |

`glue.h` 的结构由 clang 和 MSVC 两侧共用。修改结构时，须同时修改两侧的静态断言（`driver\diag.c`、`wificx\wifi.h`、`mac\hw.c` 等）。

以下两处代码直接使用 rtw89 的内部函数和结构，更换驱动家族时须替换：

- `mac\iface.c` `rtw_linux_ser_trigger`：调用 `rtw89_fw_h2c_trigger_cpu_exception`，供 `rtwdiag.ps1 ser` 使用。
- `compat\rtw89info.c`：读取 rtw89 的内部状态，供 `IOCTL_RTW_RADIO` 和 `IOCTL_RTW_BTC` 使用。

<a id="add-chip"></a>
## 添加一颗 rtw89 PCIe 芯片

结论：先核对新芯片与 RTL8852B 的差异，然后修改构建、驱动包和芯片专用的代码，最后按[测试](#testing)验证。

### 核对新芯片的差异

在上游 Linux v7.2 的 `drivers/net/wireless/realtek/rtw89/` 中，查阅新芯片的 `rtw89_chip_info`（芯片文件，例如 `rtw8852c.c`）和 PCI 信息（例如 `rtw8852ce.c`）。下表中的字段决定工作量。

| 字段 | RTL8852B 的值 | 新芯片的值不同时 |
|---|---|---|
| `chip_gen` | `RTW89_CHIP_AX` | BE 代（例如 8922A）：INTx 寄存器和 `driver\hw.c` 的寄存器均须另行核对 |
| `hw_sec_hdr` | `false` | `true`（8852C、8922A）：mac 层不能再自行写入 CCMP 头，见假设 5、6 |
| `hw_mgmt_tx_encrypt` | `false` | `true`（8852C、8922A）：现有管理帧保护代码的行为未确定，见假设 8 |
| `support_bands` | 2.4 GHz、5 GHz | 包含 6 GHz（例如 8852C）：mac 层禁用 6 GHz，能力结构只有两个频段 |
| `support_bandwidths` | 20、40、80 MHz | 包含 160 MHz（例如 8852C）：mac 层和能力上报均只支持到 80 MHz |
| PCI 信息的 `disable_intr` | `rtw89_pci_disable_intr` | 8852CE 使用 `_v1`，8922AE 使用 `_v2`：须修改 INTx 寄存器 |
| `fw_basename`、`fw_format_max` | `rtw89/rtw8852b_fw`、`2` | 新的固件文件名 |

8851BE、8852AE、8852BTE 的 `hw_sec_hdr` 和 `hw_mgmt_tx_encrypt` 也为 `false`，`disable_intr` 也是 `rtw89_pci_disable_intr`。这几颗芯片的改动最少。

### 添加上游源文件

上游 Linux v7.2 的 `Makefile` 和 `Kconfig` 定义每颗芯片的文件组。这两个文件不在本仓库中。

| 芯片 | 文件组（`.c` 的基本名） | 须加入的 `.h` | 宏 |
|---|---|---|---|
| RTL8852CE | `rtw8852c`、`rtw8852c_table`、`rtw8852c_rfk`、`rtw8852c_rfk_table`、`rtw8852ce` | `rtw8852c.h`、`rtw8852c_table.h`、`rtw8852c_rfk.h`、`rtw8852c_rfk_table.h` | `CONFIG_RTW89_8852CE` |
| RTL8852BE-VT | `rtw8852bt`、`rtw8852bt_rfk`、`rtw8852bt_rfk_table`、`rtw8852bte`；另需 `rtw8852b_common` | `rtw8852bt.h`、`rtw8852bt_rfk.h`、`rtw8852bt_rfk_table.h` | `CONFIG_RTW89_8852BTE` |
| RTL8851BE | `rtw8851b`、`rtw8851b_table`、`rtw8851b_rfk`、`rtw8851b_rfk_table`、`rtw8851be` | `rtw8851b.h`、`rtw8851b_table.h`、`rtw8851b_rfk.h`、`rtw8851b_rfk_table.h` | `CONFIG_RTW89_8851BE` |

这些 `.c` 文件包含的其它头文件（`core.h`、`pci.h` 等）已在 `third_party\rtw89\` 中。

上游的来源（仓库地址和完整提交号）记录在 `third_party\rtw89\README.md` 开头。

在构建电脑上：

1. 从该提交的 rtw89 目录，将上表中的 `.c` 和 `.h` 文件复制到 `third_party\rtw89\`。
2. 计算每个新文件的 blob id。

   ```powershell
   git hash-object third_party\rtw89\<文件名>
   ```

3. 在 `third_party\rtw89\README.md` 的文件表中，为每个新文件添加一行：文件名、字节数、blob id。
4. 更新文件数。须修改以下位置：`third_party\rtw89\README.md` 的正文和末行，[架构](architecture.md#build-structure)的"构建结构"一节，[第三方许可说明](licenses.md#rtw89)的"上游 rtw89"一节。目前写的是 26 个 `.c`、23 个 `.h`，共 49 个。

也可以不复制文件：构建时用 `-RtwSrc` 指向完整 Linux 源码中的 rtw89 目录。

> **提示**：新文件和现有文件须来自同一个 Linux 版本。更换版本时，须整体替换 `third_party\rtw89\`。

### 修改构建

在构建电脑上：

1. 在 `buildhal.ps1` 的 `$upstream` 列表中加入新芯片的基本名。
2. 若新构建只支持新芯片，从 `$upstream` 中删除 `rtw8852b`、`rtw8852b_table`、`rtw8852b_rfk`、`rtw8852b_rfk_table`、`rtw8852be`。
3. 若新芯片不是 RTL8852BE-VT，同时从 `$upstream` 中删除 `rtw8852b_common`。
4. 在 `buildhal.ps1` 的 `$flags` 中，将 `-DCONFIG_RTW89_8852BE=1` 替换为 `-DCONFIG_RTW89_<芯片>=1`。只有一个构建支持多颗芯片时，才保留多个此类宏。
5. 运行：

   ```powershell
   .\buildhal.ps1
   ```

   **结果**：每个目标文件的 `Errors` 为 0，最后一行为 `library: ...\build\ARM64\Release\rtw89hal.lib`。
6. 若出现编译错误，按消息汇总错误：

   ```powershell
   .\tools-dev\errsum.ps1 <目标文件名>
   ```

7. 若错误为 `__rtw_kcfg_CONFIG_...` 未声明，在 `compat\include\rtw\base.h` 的 `IS_ENABLED` 表中为该选项添加一行。
8. 若错误为兼容层缺少 Linux 的宏、枚举或函数，在 `compat\include\` 中对应的头文件里补充，然后从第 5 步重新执行。

`<芯片>`：Kconfig 中的芯片名，例如 `8852CE`。`<目标文件名>`：`buildhal.ps1` 输出表中的 `Name` 列，例如 `rtw8852c`。

`buildhal.ps1` 的输出末尾列出前 3 个失败目标文件的前几条错误（`---- <目标文件名>: first errors`）。每个目标文件的完整编译输出在 `build\ARM64\Release\hal\<目标文件名>.log`。

上游 v7.2 的 rtw89 源文件不使用 `IS_ENABLED`。只有兼容层或 mac 层用到新选项时，才会出现第 7 步的情况。

RTL8852CE 的 `rtw8852ce.c` 使用 `DMI_PRODUCT_SKU`。`compat\include\rtw\bus.h` 的 `enum dmi_field` 已包含该项。兼容层的 `dmi_first_match` 和 `dmi_check_system` 始终返回不匹配，因此这些 DMI 表项不起作用。其它芯片还缺少哪些定义，未逐个核对。

### 登记 PCI 驱动

`compat\include\rtw\bus.h` 的 `module_pci_driver` 只定义一个全局指针 `rtwk_pci_driver`。`compat\glue.c` 的 `rtw_linux_probe` 只在该驱动的 `id_table` 中查找 PCI ID。

- 一个构建只支持一颗芯片时：无需修改。新芯片的 `*e.c` 文件通过 `module_pci_driver` 登记自身。
- 一个构建支持多颗芯片时：将 `module_pci_driver` 改为向登记表添加一项；将 `rtw_linux_probe` 和 `rtw_linux_remove` 改为按 PCI ID 在登记表中查找驱动。此部分尚未实现。

### 修改驱动包

固件文件名的规则：rtw89 先请求 `<fw_basename>-<fw_format_max>.bin`，失败后依次减小格式号，最后请求 `<fw_basename>.bin`。`rtwk_firmware_load` 只取文件名中最后一个 `/` 之后的部分，并从驱动存储中驱动所在的目录读取。因此 INF 必须将固件复制到驱动文件旁边。

在构建电脑上：

1. 在 `driver\rtw89win.inf` 的 `[Realtek.NTARM64.10.0...26100]` 节中添加新硬件 ID：

   ```ini
   %Rtw89.DeviceDesc% = Rtw89_Device, PCI\VEN_10EC&DEV_<设备号>
   ```

2. 若设备名称不同，修改 `[Strings]` 节的 `Rtw89.DeviceDesc`。
3. 在 `[SourceDisksFiles]` 节和 `[Rtw89_CopyFiles]` 节中添加固件文件名。
4. 在 `build.ps1` 中修改 `Assert-RtwFirmware` 和 `Copy-Item` 使用的固件文件名。
5. 在 `firmware\SHA256SUMS` 中为新固件添加一行。
6. 将下列脚本中的 `PCI\VEN_10EC&DEV_B852` 改为新硬件 ID：`tools\install-rtw89win.ps1`、`tools\uninstall-rtw89win.ps1`、`tools\rtwdiag.ps1`。
7. 以同样方式修改 `tools-dev\board\` 中的 `rtw89win-disen.ps1`、`rtw89win-guardsim.ps1`、`rtw89win-restartset.ps1`、`rtw89win-scanrestart.ps1`。
8. 运行 `.\build.ps1`。

   **结果**：输出目录中有新固件文件，`inf2cat` 未报告错误。

`<设备号>`：新芯片的 PCI 设备 ID，取自上游 `*e.c` 的 `id_table`。

固件须支持扫描卸载和信标过滤。不支持时，rtw89 改用信道上下文模拟，而 `mac\stubs.c` 中的模拟为空实现。

上游 `rtw8852be.c` 的 `id_table` 还包含 `0xB85B`。本项目的 INF 未列出该 ID，也未测试过。

### 修改芯片专用的代码

1. 阅读上游 `pci.c` 中新芯片的 `disable_intr` 函数。
2. 若该函数不是 `rtw89_pci_disable_intr`，按其写入的寄存器修改 `compat\device.c` 的 `INTX_*` 常量和 `intx_stopped`。
3. 若新芯片属于 BE 代，在上游 `reg.h` 中核对 `driver\hw.c` 的 `R_AX_*` 偏移。`RtwHwLogChipInfo` 只用这些偏移写日志；在其它芯片上读取这些偏移是否安全，未确定。
4. 按 [mac 层的假设](#mac-assumptions)逐条核对，修改不满足的部分。
5. 核对 `wificx\device.cpp` `ReportCapabilities`：只上报 2.4 GHz 和 5 GHz，PHY 类型最高为 VHT，5 GHz 宽度最大为 80 MHz。
6. 核对 `wificx\adapter.cpp` 的最大链路速率 `RTW_LINK_SPEED_80`（866.7 Mbit/s，VHT80 两条空间流）。
7. 核对 `wificx\connect.cpp` `IndicateLinkState` 在 rtw89 尚未报告速率时使用的缺省速率。
8. 若新芯片支持 6 GHz，扩展 `compat\include\rtw\glue.h` 的 `struct rtw_caps`。该结构目前只有两个频段，每个频段最多 64 个信道。

`intx_stopped` 修改不正确时，固件错误恢复（SER）期间将出现中断风暴和蓝屏 0x133（[RTW-9](observed-behavior.md#rtw-9)）。空间流数无需修改：`mac\hw.c` `rtw_linux_get_caps` 从 rtw89 读取该值。

<a id="mac-assumptions"></a>
## mac 层的假设

结论：mac 层只实现了 RTL8852B 用到的 mac80211 功能。下表每一行是一个假设。新芯片不满足某一行时，按最后一列处理。

| 假设 | 代码位置 | 新芯片不满足时的处理 |
|---|---|---|
| 1. 固件执行速率控制（rtw89 设置 `HAS_RATE_CONTROL`） | mac 层没有速率控制代码 | 在 mac 层实现速率控制（例如 minstrel） |
| 2. 只使用固件扫描（`hw_scan`） | `mac\scan.c` `scan_start_locked` | 实现软件扫描 |
| 3. 不使用信道上下文模拟 | `mac\stubs.c`（`ieee80211_emulate_*` 为空实现） | 实现模拟，或改用支持扫描卸载和信标过滤的固件 |
| 4. 不使用 txq 调度，数据帧直接调用 `ops->tx` | `mac\stubs.c`；`mac\tx.c` `rtw_linux_tx_data` | 实现 `wake_tx_queue` 和 txq 调度（[RTW-5](observed-behavior.md#rtw-5)） |
| 5. 发送时由 mac 层写入 CCMP 头和 PN（`hw_sec_hdr = false`） | `mac\key.c` `mac_key_ccmp_hdr`；`mac\tx.c` | 按 mac80211 的规则检查密钥标志 `IEEE80211_KEY_FLAG_GENERATE_IV`，无此标志时不写入 |
| 6. 接收时 rtw89 只报告 `RX_FLAG_DECRYPTED`；mac 层检查 PN，并去除 CCMP 头和 MIC | `mac\key.c` `mac_key_rx`；`mac\rx.c` `mac_rx_deliver` | 按 `RX_FLAG_IV_STRIPPED`、`RX_FLAG_MIC_STRIPPED`、`RX_FLAG_PN_VALIDATED` 分别处理 |
| 7. 只支持 CCMP-128 | `mac\key.c`；`wificx\device.cpp` `ReportCapabilities` | 添加 GCMP 或 CCMP-256 的头格式和能力 |
| 8. 管理帧不由硬件加解密；管理帧保护全部使用软件 | `mac\pmf.c` | 在 `hw_mgmt_tx_encrypt = true` 的芯片上，现有代码的行为未确定；按 mac80211 的 `IEEE80211_KEY_FLAG_SW_MGMT_TX` 核对 |
| 9. 重排序由 mac 层执行 | `mac\agg.c` `mac_agg_rx_frame` | 芯片自行重排序时，跳过 mac 层的重排序 |
| 10. A-MPDU 中不接收 A-MSDU；HT 最大 A-MSDU 为 3839 字节；VHT 宣告的最大 MPDU 为 3895 字节 | `mac\agg.c` `ba_params`；`mac\wide.c`；`mac\mlme.c` | 需要更大的帧时，修改 BA 参数和宣告的能力 |
| 11. 热点客户端的省电由 mac 层跟踪（RTL8852B 的固件没有 `NOTIFY_AP_INFO`） | `mac\ap.c` | 固件有 `NOTIFY_AP_INFO` 时，rtw89 设置 `AP_LINK_PS`；mac 层未实现该路径 |
| 12. 最多两个 vif（站点和组主），且只能同信道；不支持 MCC、MLO | `mac\iface.c` `chanctx_share`；`mac\ap.c` | 实现多信道上下文 |
| 13. 最宽 80 MHz；禁用 6 GHz；不支持 HE、EHT | `mac\wide.h`；`mac\reg.c` `reg_apply`；`wificx\device.cpp` | 扩展宽度选择、管制规则和能力上报 |
| 14. 信标的 `sk_buff` 为新分配（rtw89 按此布局查找 TIM） | `mac\ap.c`；`compat\include\rtw\skbuff.h` | 修改 `sk_buff` 时保持这一点（[HS-18](observed-behavior.md#hs-18)） |
| 15. `ieee80211_ops` 在持有 `wiphy->mtx` 时调用；接收在 PASSIVE 下运行 | `mac\hw.c`；`compat\netdev.c` | 新驱动要求其它上下文时，修改调用方式（[RTW-3](observed-behavior.md#rtw-3)、[RTW-4](observed-behavior.md#rtw-4)） |

<a id="new-board"></a>
## 更换硬件平台

结论：更换硬件平台时，须核对平台条件和蓝牙的 USB ID。下表列出 ROCK 5T 上的情况，以及更换硬件平台时需要修改和无需修改的部分。

| 条件 | ROCK 5T（RK3588）上的情况 | 更换硬件平台时的处理 |
|---|---|---|
| M.2 槽供电 | UEFI 为 M.2 E-key 槽上电，并将 `W_DISABLE1#` 置为高电平。此步骤不在本仓库中 | 确认 UEFI 已完成此步骤；否则 Windows 无法发现设备（[PLAT-4](observed-behavior.md#plat-4)） |
| 中断 | 没有 MSI，只有 INTx，全部由 CPU0 处理 | 在有 MSI 的平台上，现有代码仍只使用 INTx：`compat\device.c` `pci_alloc_irq_vectors` 只接受 INTx。使用 MSI 须修改驱动层和兼容层，尚未实现（[PLAT-1](observed-behavior.md#plat-1)） |
| DMA 缓存一致 | PCIe 不做缓存一致，公共缓冲为非缓存映射 | 无需修改：`driver\os.c` 使用 DMA 适配器的缺省缓存类型（[PLAT-2](observed-behavior.md#plat-2)） |
| DMA 地址 | 全部在 4 GB 以下 | 无需修改：驱动层只申请 32 位地址 |
| PCIe 链路 | Gen1 x1；BAR2 为 1 MB 内存 BAR；芯片下电时读取 MAC 区返回 `0xEAEAEAEA` | 无需修改（[PLAT-3](observed-behavior.md#plat-3)） |
| 重新枚举 | 重新枚举后，芯片在断电前无法下载固件 | 其它硬件平台未测试；新增的恢复代码不得调用 `WdfDeviceSetFailed(WdfDeviceFailedAttemptRestart)`（[INST-6](observed-behavior.md#inst-6)） |
| Windows 版本 | 26100 | INF 只安装在 26100 及以上版本（`[Manufacturer]` 节） |
| 蓝牙的 USB ID | `USB\VID_13D3&PID_3572` | 修改 `bt\rtkbtflt\rtkbtflt.inf` 的 `[Realtek.NTARM64.10.0...26100]` 节；脚本使用 `-HardwareId` 或 `-UsbId`（见下文） |
| 蓝牙控制器 | RTL8852B 的 USB 部分：ROM 的 `lmp_subver` 为 `0x8852`，补丁为 project 20 的 v1 格式 | 蓝牙过滤驱动不向其它控制器下载补丁；其它蓝牙芯片须修改 `bt\rtkbtfw\epatch.c` 和 `bt\rtkbtflt\hci.c` |

蓝牙 USB ID 不同时，以下脚本须带参数运行：

- `install-rtkbtflt.ps1`、`uninstall-rtkbtflt.ps1`、`rtkbtflt.ps1`：`-HardwareId 'USB\VID_<厂商号>&PID_<产品号>'`。
- `tools-dev\board\rtw89win-btquery.ps1`：`-UsbId`。

其它厂家的 RTL8852BE 模块使用哪些蓝牙 USB ID，未确定。

<a id="new-family"></a>
## 更换驱动家族

结论：接入其它 Linux mac80211 驱动时，兼容层和 mac 层均须补充。下面两张表列出已知缺少的类别，未逐个核对。先通过构建找出缺少的符号，然后对照两张表。

### 找出缺少的部分

在构建电脑上：

1. 将新驱动的源文件放入一个目录。
2. 将这些文件加入 `buildhal.ps1` 的文件列表，并添加新驱动所需的宏。
3. 运行 `.\buildhal.ps1 -RtwSrc <源文件目录>`。
4. 对每个编译失败的目标文件，运行 `.\tools-dev\errsum.ps1 <目标文件名>`，列出缺少的符号。
5. 编译通过后，运行 `.\tools-dev\undef.ps1`。

   **结果**：列出 `rtw89hal.lib` 引用但未定义的符号。RTL8852BE 的构建有 45 个，均由驱动层或 Windows 内核提供。多出的符号须在兼容层、mac 层或 `driver\os.c` 中实现。

每个目标文件的完整编译输出在 `build\ARM64\Release\hal\<目标文件名>.log`。

### 兼容层需补充的类别

| 类别 | 现状 | 位置 |
|---|---|---|
| USB | 没有 `linux\usb.h` | `compat\include\linux\` |
| SDIO | 无 | — |
| 中断 | 只有一条 INTx；`pci_alloc_irq_vectors` 拒绝 MSI | `compat\device.c` |
| DMA | 32 位地址；DMA 堆以外的内存经反弹缓冲传输 | `compat\device.c`、`driver\os.c` |
| 固件加载 | 只有同步的 `request_firmware` 及其别名 | `compat\include\rtw\bus.h`、`compat\device.c` |
| cfg80211 辅助函数 | `cfg80211_chandef_usable`、`cfg80211_bss_iter` 等为空实现 | `compat\include\net\cfg80211.h` |
| 其它内核子系统 | debugfs、LED、thermal、rfkill、devcoredump 未实现；未定义 `CONFIG_PM`，因此不编译 WoWLAN | `compat\include\linux\` |
| 802.11 头 | FreeBSD 头与 Linux 头存在语义差异（例如 `ieee80211_hdrlen`，[WIDE-6](observed-behavior.md#wide-6)） | `compat\include\linux\ieee80211.h`、`compat\include\net\` |

### mac 层需补充的类别

| 类别 | 现状 |
|---|---|
| txq 调度 | 空实现；数据帧直接调用 `ops->tx` |
| 软件扫描 | 无；只有 `hw_scan` |
| 信道上下文模拟 | 空实现 |
| 速率控制 | 无；依赖固件 |
| 软件加密 | 只有管理帧的软件 CCMP 和 BIP；数据帧依赖硬件加密 |
| 加密套件 | 只有 CCMP-128 和 BIP-CMAC-128；不支持 TKIP、GCMP |

逐条的假设见 [mac 层的假设](#mac-assumptions)。

<a id="usb"></a>
## USB 芯片

结论：USB 路线未实现，也未测试。rtw89 v7.2 有 `usb.c` 和各芯片的 USB 文件（例如 `rtw8852bu.c`、`rtw8852cu.c`），没有 SDIO。

需补充的部分：

| 部分 | 内容 |
|---|---|
| 驱动层 | KMDF USB 设备框架：USB 设备对象、配置、管道、URB 的提交和完成 |
| `os.h` | 供兼容层使用的 USB 原语，代替现有的 BAR2、INTx 和 DMA |
| 兼容层 | `linux\usb.h` 的子集；`module_usb_driver` 的登记；与 `rtw_linux_probe` 对应的 USB 入口 |
| `buildhal.ps1` | 加入 `usb` 和芯片的 USB 文件；添加 `-DCONFIG_RTW89_USB=1` 和芯片的 USB 宏 |
| `compat\include\rtw\base.h` | 新选项的 `IS_ENABLED` 行（编译报错时再添加） |
| INF | USB 硬件 ID；固件文件 |

`bt\rtkbtflt\hci.c` 和 `bt\rtkbtflt\urb.c` 直接通过 URB 和 USB 管道通信，可作参考。蓝牙过滤驱动是过滤驱动，USB Wi-Fi 驱动是功能驱动，两者的设备对象不同。

<a id="testing"></a>
## 测试

结论：按功能顺序验证，前一项通过后再进行下一项。将每项结果与[验证状态](validation.md)中同一功能的结果比较。

> **提示**：`tools-dev\board\` 的脚本须在目标设备上以管理员身份运行：`pwsh -NoProfile -File <脚本>`。调用 `rtwdiag.ps1` 的脚本用 `-Diag <路径>` 或环境变量 `RTW89WIN_DIAG` 指定其位置。各脚本的参数见 `tools-dev\board\README.txt`。

首先进行构建检查。内核栈较小，因此第 1 步检查每个函数的栈帧不超过 1024 字节。

在构建电脑上：

1. 运行 `.\buildhal.ps1 -ExtraFlags '-Wframe-larger-than=1024'`。

   **结果**：每个目标文件的 `Errors` 和 `Warnings` 均为 0。
2. 运行 `.\build.ps1`。

   **结果**：MSBuild 报告 0 个警告、0 个错误（项目使用 `/W4 /WX`）。
3. 运行 `.\tools-dev\undef.ps1`。

   **结果**：列出的每个符号均由 `driver\os.c`、驱动层的其它文件或 Windows 内核提供；第 2 步的链接没有未解析的符号。
4. 运行主机测试 `.\tools-dev\apframe-test.ps1`、`.\tools-dev\wide-test.ps1`、`.\tools-dev\crypto-test.ps1`。

   **结果**：均输出 PASS。
5. 使用 `llvm-readobj --coff-imports` 或 `dumpbin /imports` 查看 `rtw89win.sys` 的导入表。

   **结果**：只有 `ntoskrnl.exe`、`HAL.dll`、`WDFLDR.SYS`、`ksecdd.sys`。

然后在目标设备上按下表的顺序验证。

第 4、8 项的 `hosttools\` 脚本在测量电脑上运行，有两个前提：

- 目标设备上运行 OpenSSH 服务端，缺省 shell 为 PowerShell。
- 提供 `-BoardWlan <目标设备的 WLAN 地址>` 和 `-SshHost <目标设备的 ssh 主机名>`（第 8 项只需 `-BoardWlan`）。这些值也可写入环境变量或 `local.settings.psd1`，见[构建](build.md#parameters)。

| 顺序 | 功能 | 验证内容 | 工具和脚本 | 验证状态中的小节 |
|---|---|---|---|---|
| 1 | 诊断模式下的 probe、启动、扫描 | 固件下载、efuse、中断、接收、H2C/C2H、固件扫描 | `rtw89win-restartset.ps1 -Set BootAttempts=2` 进入诊断模式；`rtwdiag.ps1 probe`、`start`、`monitor <信道>`、`scan`、`txprobe`、`remove`；`rtwdiag.ps1 arm` 返回 WiFiCx 模式 | [恢复和稳定性](validation.md#robustness) |
| 2 | WiFiCx 模式下的扫描 | 能力上报、`TASK_SCAN`、BSS 列表 | 任务栏 Wi-Fi 列表；`rtwdiag.ps1 mode`、`bss`；`rtw89win-wlanlist.ps1`（须打开定位服务，[SCAN-7](observed-behavior.md#scan-7)） | [站点](validation.md#station) |
| 3 | 连接 | WPA2-个人、密钥、豁免 | `rtw89win-profile.ps1`、`rtw89win-connect.ps1`；`rtwdiag.ps1 link` | [站点](validation.md#station)、[安全](validation.md#security) |
| 4 | 数据和 A-MPDU | 吞吐、BA 会话、重排序 | 在测量电脑上运行 `hosttools\rtw89win-thru.ps1`；`rtwdiag.ps1 link` 的 BA 计数 | [站点](validation.md#station) |
| 5 | SER | 恢复、INTx 屏蔽、自动重连 | `rtw89win-serdiag.ps1`（诊断模式）、`rtw89win-serconn.ps1`（已连接） | [恢复和稳定性](validation.md#robustness) |
| 6 | 管理帧保护和 WPA3-SAE | `Pmf=1`、`Pmf=2` | `rtw89win-profile.ps1 -Auth WPA3SAE`、`rtw89win-connect.ps1`；`rtwdiag.ps1 link` | [安全](validation.md#security) |
| 7 | 国家码 | 管制规则、主动和被动信道 | `rtwdiag.ps1 country <国家码>`、`rtwdiag.ps1 radio` | [国家码](validation.md#regulatory) |
| 8 | 省电 | 进入和退出省电、交互延迟 | 设置项 `PowerSave=2`；`rtwdiag.ps1 radio`；在测量电脑上运行 `hosttools\rtw89win-tcprtt.ps1` | [省电](validation.md#power) |
| 9 | 信道宽度 | VHT80、HT40、HT20 | 设置项 `Bandwidth`；`rtwdiag.ps1 link` | [信道宽度](validation.md#width) |
| 10 | 移动热点 | 组主、热点客户端、与站点同信道 | `rtw89win-hotspot.ps1`；`rtwdiag.ps1 ap` | [移动热点](validation.md#hotspot) |
| 11 | 蓝牙 | 补丁下载、BLE、配对 | `rtkbtflt.ps1 status`、`rtkbtfw info`、`rtw89win-btquery.ps1`、`rtw89win-bt.ps1` | [蓝牙](validation.md#bluetooth) |
| 12 | 重启和保护 | 重启设备、禁用和启用、扫描中重启设备、崩溃循环保护 | `rtw89win-disen.ps1`、`rtw89win-scanrestart.ps1`、`rtw89win-guardsim.ps1` | [恢复和稳定性](validation.md#robustness) |

`<信道>`：一个 2.4 GHz 信道号，例如 `6`。`<国家码>`：两个字母，例如 `DE`。

第 5 项的通过标准：驱动日志中每次 SER 最多出现一行 `INTx: the chip interrupted while rtw89 was stopped; masked it`；系统未蓝屏。

术语见[术语表](glossary.md)。各层的接口和控制流见[架构](architecture.md#control-flow)。
