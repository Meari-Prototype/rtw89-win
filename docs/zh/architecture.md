# 架构

[English](../en/architecture.md)

- **适用读者**：驱动开发者，包括修改本驱动、编写其它 WiFiCx 驱动、将 rtw89 的其它芯片接入 Windows 的人员。
- **本文内容**：各层的职责和边界、层间接口、主要的控制流和数据流、设计决策及其理由。
- **前提**：了解 KMDF 和 802.11 的基本概念。

<a id="summary"></a>
## 要点

1. Wi-Fi 驱动 `rtw89win.sys` 由五部分组成：驱动层、WiFiCx 前端、mac 层、上游 rtw89、兼容层。蓝牙过滤驱动 `rtkbtflt.sys` 是另一个独立的驱动。
2. 上游 rtw89（Linux v7.2）不修改任何一行。mac 层通过 Linux mac80211 的接口 `ieee80211_ops` 调用上游 rtw89。
3. mac 层代替 Linux 的 mac80211 和 cfg80211。扫描、连接、密钥、组主等 802.11 逻辑均在 mac 层实现。
4. WiFiCx 前端只将 WDI 消息和 NetAdapterCx 队列转换为 mac 层的调用。它不构造 802.11 帧，也不直接调用 rtw89。
5. clang 编译 Linux 侧（rtw89、兼容层、mac 层），MSVC 编译 Windows 侧（驱动层、WiFiCx 前端）。两部分链接为同一个 `.sys`。
6. 一个 PASSIVE 工作项按顺序处理全部 WDI 消息。mac 层的回调只记录状态，然后排入 DPC 或工作项。
7. 中断只使用 INTx。rtw89 停止时，由兼容层屏蔽芯片中断。

<a id="overview"></a>
## 总体结构

Wi-Fi 驱动分为五层。图中箭头左侧为向下的调用，右侧为向上的回调。

```text
 Windows        Wlansvc、icssvc（移动热点）
                    | WDI 消息（任务、属性、指示）
                WiFiCx + NetAdapterCx（收发队列）
                    |
 ===================== rtw89win.sys =====================
 WiFiCx 前端    wificx\              C++，MSVC
                    | rtw_linux_*（glue.h）      ^ 回调（glue.h）
 mac 层         mac\                 C，clang
                    | ieee80211_ops              ^ linuxkpi_ieee80211_*
 上游 rtw89     third_party\rtw89\   C，clang，不修改
                    | Linux 内核 API
 兼容层         compat\              C，clang
                    | rtwk_*（os.h）             ^ rtw_linux_isr 等（glue.h）
 驱动层         driver\              C，MSVC，KMDF
 ========================================================
                    | PCIe：BAR2、INTx 中断、DMA
                模块的 Wi-Fi 部分（RTL8852BE）
```

驱动层承担两个角色。第一，它是 KMDF 的入口：PnP 回调首先到达驱动层，再由驱动层调用 WiFiCx 前端和兼容层。第二，它是系统服务的提供者：Linux 侧的代码通过 `rtwk_*` 使用这些服务。

蓝牙过滤驱动独立工作，与 Wi-Fi 驱动之间没有调用关系：

```text
 bthport、BTHUSB（Windows 自带，基础驱动 bth.inf）
        | START / D0 请求向上返回时，先经过过滤驱动
 rtkbtflt.sys（下层过滤驱动）     <- rtl8852bu_fw.bin、rtl8852bu_config.bin
        | URB：控制端点发送 HCI 命令，中断端点接收事件
 USB 栈
        |
 模块的蓝牙部分（USB）
```

Wi-Fi 与蓝牙之间只在共存方面存在联系。共存由 rtw89 的 btc 模块在 Wi-Fi 侧完成（[RTW-12](observed-behavior.md#rtw-12)）。

<a id="layers"></a>
## 各层职责

| 部分 | 目录 | 语言和编译器 | 负责 | 不负责 |
|---|---|---|---|---|
| 驱动层 | `driver\` | C，MSVC | `DriverEntry`、PnP 和电源回调；映射 BAR2；连接 INTx 中断；DMA 适配器和 DMA 堆；实现 `rtwk_*`；读取设置项；诊断接口；崩溃循环保护；驱动日志；基于 CNG 的 AES | 不涉及 802.11 逻辑；不解析 WDI 消息 |
| WiFiCx 前端 | `wificx\` | C++，MSVC | WiFiCx 设备和网卡对象；能力上报；WDI 消息的分派、解析和生成（`WificxTLVGenParse.lib`）；NetAdapterCx 收发队列；端口状态 | 不构造 802.11 帧；不直接调用 rtw89 |
| mac 层 | `mac\` | C，clang | 按 mac80211 的顺序和锁规则调用 rtw89；扫描、认证、关联、密钥和豁免、CCMP 头和 PN、管理帧保护、SAE 帧收发、块确认会话和重排序、管制规则、省电、信道宽度、组主、固件错误恢复（SER）之后的状态恢复 | 不直接使用 Windows API（只通过 `os.h`）；不执行 SAE 计算和四次握手（由 Windows 完成） |
| 上游 rtw89 | `third_party\rtw89\` | C，clang | 芯片初始化、固件下载、DMA 环、H2C/C2H、固件扫描、速率控制、共存、省电（rtw89 的 LPS 和 IPS）、SER 的检测和恢复、安全 CAM | 本项目不修改其中任何文件 |
| 兼容层 | `compat\` | C，clang | rtw89 用到的 Linux API 子集；FreeBSD LinuxKPI 的 802.11 头；`rtw_linux_init`、`rtw_linux_probe` 等入口；INTx 屏蔽 | 不包含 802.11 逻辑 |
| 蓝牙过滤驱动 | `bt\rtkbtflt\` | C，MSVC | 在 BTHUSB 启动之前向蓝牙控制器下载蓝牙补丁 | 不处理 Wi-Fi；不修改 Windows 蓝牙栈的其它部分 |

兼容层提供的 Linux API 子集包括：内存、锁、工作队列、定时器、RCU、NAPI、`sk_buff`、PCI 配置空间、中断、DMA 映射、固件加载、`printk`。

<a id="interfaces"></a>
## 层间接口

两种编译器共用的结构只使用定宽字段。两侧均用静态断言核对结构大小（例如 `driver\diag.c`、`wificx\wifi.h`、`mac\hw.c`）。

| 方向 | 接口 | 定义位置 | 调用上下文 |
|---|---|---|---|
| 驱动层 → WiFiCx 前端 | `RtwWifiPreDeviceCreate`、`RtwWifiPostDeviceCreate`、`RtwWifiPrepareHardware`、`RtwWifiReleaseHardware`、`RtwWifiD0Entry`、`RtwWifiD0Exit` | `driver\rtw89win.h`；实现在 `wificx\device.cpp` | KMDF 的 PnP 和电源回调，PASSIVE |
| WiFiCx 前端 → 驱动层 | `RtwProbe`、`RtwRemove` | `driver\rtw89win.h`；实现在 `driver\probe.c` | PASSIVE；调用者持有 `Dev->CtlLock` |
| 驱动层 → 兼容层 | `rtw_linux_init`、`rtw_linux_probe`、`rtw_linux_remove`、`rtw_linux_exit` | `compat\include\rtw\glue.h` | PASSIVE；调用者持有 `Dev->CtlLock` |
| 驱动层 → 兼容层（中断） | `rtw_linux_isr`、`rtw_linux_irq_dpc` | `glue.h` | DIRQL；DISPATCH |
| 驱动层 → mac 层（设置项） | `rtw_linux_set_pmf`、`rtw_linux_set_wfd`、`rtw_linux_set_wide`、`rtw_linux_set_ps_policy`、`rtw_linux_reg_set_user` | `glue.h`；调用位置为 `driver\probe.c` `RtwApplySettings` | PASSIVE；probe 之后、能力上报之前 |
| Linux 侧 → 驱动层 | `rtwk_*`：内存、自旋锁、KMUTEX、KEVENT、线程、定时器、时间、日志、bugcheck、DMA、PCI 配置空间、固件文件、中断同步、AES-CCM 和 AES-CMAC | `compat\include\rtw\os.h`；实现在 `driver\os.c`、`driver\crypto.c` | 每个函数的注释注明 IRQL |
| WiFiCx 前端 → mac 层（站点） | `rtw_linux_start`、`rtw_linux_scan_start`、`rtw_linux_bss_export`、`rtw_linux_connect`、`rtw_linux_add_key`、`rtw_linux_set_exemptions`、`rtw_linux_sae_set`、`rtw_linux_tx_data`、`rtw_linux_rx_data` 等 | `glue.h`；实现在 `mac\` | 控制面为 PASSIVE；数据面 ≤ DISPATCH |
| WiFiCx 前端 → mac 层（组主） | `rtw_linux_ap_*` | `glue.h`；实现在 `mac\ap.c` | 同上 |
| mac 层 → WiFiCx 前端 | 回调 `rtw_scan_done_fn`、`rtw_conn_event_fn`、`rtw_ap_event_fn`；`struct rtw_dp_ops` 的 `tx_ready`、`rx_ready` | `glue.h` | 在 mac 层或 rtw89 的锁内调用，≤ DISPATCH；只记录状态并发出通知 |
| mac 层 → rtw89 | `struct ieee80211_ops`：`start`、`add_interface`、`hw_scan`、`sta_state`、`set_key`、`tx`、`ampdu_action` 等 | `compat\include\net\mac80211.h` | 持有 `wiphy->mtx`（KMUTEX，PASSIVE）；`tx` 在 RCU 读者区内调用 |
| rtw89 → mac 层 | `linuxkpi_ieee80211_*`：`rx`、`tx_status`、`scan_completed`、`restart_hw`、`iterate_keys` 等 | `mac80211.h`；实现在 `mac\` | `rx` 在 NAPI 工作队列线程中运行（PASSIVE）；`tx_status` 在 DISPATCH 下运行，持有 rtw89 的 `trx_lock` |
| rtw89 ↔ 兼容层 | Linux 内核头（`linux\*.h`、`net\*.h`） | `compat\include\` | 语义与 Linux 相同 |
| 用户态 → 驱动层 | 诊断 IOCTL `IOCTL_RTW_*`；使用者为 `tools\rtwdiag.ps1` | `driver\rtw89win_ioctl.h`；数据结构在 `glue.h` | `\\.\rtw89win`，只对 SYSTEM 和管理员开放 |
| 用户 → 驱动层 | 设置项（`Services\rtw89win\Parameters`） | `driver\probe.c` `RtwApplySettings`；`wificx\connect.cpp` `AllowConnect` | 设备每次启动时读取；`AllowConnect` 在每次连接时读取 |
| 蓝牙过滤驱动 → 蓝牙控制器 | URB：控制端点上的 HCI 命令，中断端点上的事件 | `bt\rtkbtflt\hci.c`、`bt\rtkbtflt\urb.c` | 只在 BTHUSB 收到 START 或 D0 之前 |

<a id="contexts"></a>
## 运行上下文和锁

结论：控制面全部在 PASSIVE 下串行运行；数据面和回调不调用会重新获取已持有锁的代码。

- **WDI 消息**：`RtwEvtWifiSendCommand` 在 ≤ DISPATCH 下运行，只将请求入队。工作项 `RtwWifiCmdWork` 在 PASSIVE 下逐个处理请求，处理时持有 `Dev->CtlLock`。KMDF 工作项会在另一个线程上并发重入，因此前端用 `WorkRunning` 标志阻止第二个实例。每个请求恰好完成一次（[INIT-3](observed-behavior.md#init-3)）。
- **锁的顺序**：先获取 `Dev->CtlLock`，然后获取 mac 层的 `wiphy->mtx`。mac 层在持有 `wiphy->mtx` 时调用回调，回调不获取 `Dev->CtlLock`。
- **mac 层的回调**：扫描、连接、组主的回调只设置标志，然后排入 WDI 工作项；工作项随后发送指示。数据面的 `tx_ready`、`rx_ready` 只排入 KDPC `DpDpc`，由 `DpDpc` 调用 NetAdapterCx。原因：`tx_status` 回调在 rtw89 的 `trx_lock` 内运行，直接调用 NetAdapterCx 将造成自锁（[RTW-3](observed-behavior.md#rtw-3)）。
- **rtw89 的调用**：mac 层与 mac80211 相同，在持有 `wiphy->mtx` 时调用 `ieee80211_ops`。wiphy 工作项每次只运行一个，运行时持有 `wiphy->mtx`。
- **接收**：rtw89 的 NAPI 轮询在工作队列线程上运行（PASSIVE），不在 DPC 中运行。因此 `linuxkpi_ieee80211_rx` 在 PASSIVE 下运行。
- **RCU**：读者区通过一个全局读者计数实现。`synchronize_rcu` 等待计数归零；`call_rcu` 的回调挂入链表，由工作项在同步之后运行（`compat\rcu.c`，[RTW-4](observed-behavior.md#rtw-4)）。
- **工作队列**：每个工作队列对应一个 PASSIVE 内核线程，按顺序运行其工作项（`compat\workqueue.c`）。
- **自旋锁**：`rtwk_spin_lock_irq` 先将 IRQL 提升到设备中断级，因此也能排除 ISR。
- **诊断接口**：会修改状态的 IOCTL 也获取 `Dev->CtlLock`，因此不会与 WDI 消息处理或硬件释放重叠。

<a id="control-flow"></a>
## 控制流

各流程中 WDI 消息的顺序和时序见[实测结论](observed-behavior.md)。

### 启动

1. `RtwEvtDeviceAdd` 调用 `RtwGuardDecide`，根据崩溃循环保护的计数选择 WiFiCx 模式或诊断模式。
2. 在 WiFiCx 模式下，`RtwWifiPreDeviceCreate` 配置 NetAdapterCx 和 WiFiCx；`RtwWifiPostDeviceCreate` 创建 WDI 工作项。
3. `RtwCdoCreate` 创建诊断接口 `\\.\rtw89win`。
4. `RtwEvtPrepareHardware` 调用 `RtwHwMap` 映射 BAR2，并调用 `RtwHwLogChipInfo` 记录芯片版本。
5. 若注册表配置单元尚不可写，`RtwGuardWait` 每 500 ms 重试一次，最长 60 s。`ArmRunning = 1` 写入磁盘之前，驱动不访问芯片。
6. `RtwWifiPrepareHardware` 调用 `RtwProbe`。首次调用时，`rtw_linux_init` 创建工作队列、RCU 和 mac 层。
7. `RtwOsAttach` 创建 DMA 适配器（32 位地址），并连接 INTx 中断。
8. `rtw_linux_probe` 运行 rtw89 的 PCI probe：芯片上电、下载固件、读取 efuse、下电，然后向 mac 层注册。此步骤不发射任何帧。
9. `RtwApplySettings` 将设置项传给 mac 层。`rtw_linux_get_caps` 和 `ReportCapabilities` 上报能力；Wi-Fi Direct 启用时，`RtwWifiReportWfdCapabilities` 另行上报 Wi-Fi Direct 能力。
10. WiFiCx 调用 `RtwEvtWifiCreateAdapter` 创建站点网卡。第一条需要射频的消息（`TASK_DOT11_RESET`、`TASK_SCAN`、`TASK_CONNECT` 或 `TASK_START_AP`）通过 `RtwWifiEnsureStarted` 调用 `rtw_linux_start`。rtw89 启动并添加一个站点 vif（mac80211 的虚拟接口 `ieee80211_vif`），然后进入 IPS（芯片下电）。

诊断模式只执行第 4 步。此后由诊断 IOCTL（`rtwdiag.ps1 probe`、`start`、`monitor`、`scan`）手动启动 rtw89。

### 扫描

1. Wlansvc 发送 `TASK_SCAN`。`RtwWifiTaskScan` 先完成请求，然后调用 `rtw_linux_scan_start`。
2. mac 层的 `scan_start_locked` 构造扫描请求，并调用 rtw89 的 `hw_scan`。固件逐个信道扫描。
3. 收到的信标和探测响应经 `linuxkpi_ieee80211_rx` 和 `mac_bss_rx` 进入 BSS 表（`mac\bss.c`）。BSS 表保存每个 BSS 最近一次的信标帧体和探测响应帧体。
4. 固件报告扫描结束。rtw89 调用 `linuxkpi_ieee80211_scan_completed`。`scan_done_work` 在持有 `wiphy->mtx` 时释放请求，然后调用 `rtw_scan_done_fn`。
5. 前端的回调 `ScanDone` 只设置标志，然后排入 WDI 工作项。工作项中的 `RtwWifiScanEvents` 通过 `rtw_linux_bss_export` 取出本次收到的 BSS，发送 `BSS_ENTRY_LIST` 指示，然后发送 `SCAN_COMPLETE`。
6. `RtwWifiAbortTask` 处理 `ABORT_TASK`：它调用 `rtw_linux_scan_cancel`。被中止的扫描同样发送这两条指示（[SCAN-4](observed-behavior.md#scan-4)、[SCAN-5](observed-behavior.md#scan-5)）。

### 连接（WPA2-个人和 WPA3-SAE）

1. Windows 发送 `TASK_DOT11_RESET` 和 `SET_PRIVACY_EXEMPTION_LIST`。豁免表经 `rtw_linux_set_exemptions` 传给 `mac\key.c`。
2. Windows 发送 `TASK_CONNECT`。`RtwWifiTaskConnect` 筛选候选 BSS（[CONN-2](observed-behavior.md#conn-2)），然后调用 `rtw_linux_connect`。
3. `mac\mlme.c` 按 mac80211 的顺序执行：`wide_choose` 确定信道宽度 → 创建信道上下文（chanctx）→ 站点进入 NONE → 认证 → AUTH → 关联（`mlme_send_assoc`）→ ASSOC。
4. mac 层的连接事件经工作项转换为指示：`ASSOCIATION_RESULT`（附带关联请求帧体和 BSS 帧体）→ `LINK_STATE_CHANGE` → 连接任务的完成指示（[CONN-5](observed-behavior.md#conn-5)）。
5. Windows 通过数据面完成四次握手。EAPOL 帧按豁免表以明文收发。
6. Windows 发送 `SET_ADD_CIPHER_KEYS`。`RtwWifiSetAddCipherKeys` 调用 `rtw_linux_add_key`，mac 层随后调用 rtw89 的 `set_key`。第一把成对密钥使代表接入点的站点条目进入 AUTHORIZED。

WPA3-SAE 只多出一段认证。Windows 负责 SAE 计算，`mac\sae.c` 负责收发认证帧。两者之间交换两种消息：指示 `SAE_AUTH_PARAMS_NEEDED`（`RtwWifiSaeIndicate`），属性 `SET_SAE_AUTH_PARAMS`（`RtwWifiSetSaeAuthParams` → `rtw_linux_sae_set`）。见 [SEC-1](observed-behavior.md#sec-1)。

### 移动热点

1. 设置项 `WifiDirect` 启用时，驱动上报 Wi-Fi Direct 能力。WiFiCx 调用 `RtwEvtWifiCreateWifiDirectDevice` 创建 Wi-Fi Direct 设备端口。该端口只有控制面，mac 层不为其创建 vif。
2. WiFiCx 创建角色端口的网卡（`RtwWifiCreateRoleAdapter`），然后在角色端口上发送 `TASK_START_AP`（[HS-1](observed-behavior.md#hs-1)）。
3. `RtwWifiApTaskStartAp` 调用 `ChooseChannel` 选择信道，然后调用 `rtw_linux_ap_start`。mac 层添加一个 AP 类型的 vif、信道上下文和信标模板，然后调用 rtw89 的 `start_ap`。
4. 前端发送 `P2P_GROUP_OPERATING_CHANNEL` 指示，然后通过 `IndicateApLinkState` 发送 `LINK_STATE_CHANGE`（[HS-7](observed-behavior.md#hs-7)），最后发送 `START_AP_COMPLETE`。
5. 热点客户端连接时，mac 层回复开放认证并检查关联请求，然后上报 `AP_ASSOCIATION_REQUEST_RECEIVED`。
6. Windows 决定接受后，发送 `TASK_SEND_AP_ASSOCIATION_RESPONSE`。`rtw_linux_ap_assoc_resp` 发送关联响应。
7. 关联响应得到确认后，前端发送 `SEND_AP_ASSOCIATION_RESPONSE_COMPLETE`，附带关联响应帧体和信标帧体（[HS-6](observed-behavior.md#hs-6)）。
8. Windows 通过角色端口的数据面完成四次握手，然后安装密钥。

固件按 TBTT 发送信标。mac 层构造探测响应，每秒最多 30 个（[HS-14](observed-behavior.md#hs-14)）。

### 固件错误恢复（SER）

1. 固件或 MAC 出错。rtw89 收到 C2H 后进入 SER 第 2 级：停止并复位芯片，在持有 `wiphy->mtx` 时丢弃密钥（[RTW-8](observed-behavior.md#rtw-8)）。
2. 复位期间，芯片持续拉起中断线。兼容层的 `intx_stopped` 屏蔽芯片中断（[RTW-9](observed-behavior.md#rtw-9)）。
3. rtw89 调用 `linuxkpi_ieee80211_restart_hw`。mac 层设置 `restarting`，然后排入 `mac_restart_work`。
4. `mac_restart_work` 放弃扫描，停放组主（`mac_ap_restart`），断开连接并上报掉线（`mac_mlme_restart`）。
5. `mac_restart_work` 将旧 vif 和信道上下文停放至释放 hw 时，然后通过 `start_locked` 重新启动 rtw89。
6. `mac_restart_work` 调用 `mac_ps_restart` 和 `reconfig_complete`，结束本次 SER。
7. 前端发送 `DISASSOCIATION`。Wlansvc 发送带 `RoamRequest=1` 的 `TASK_CONNECT`，自动重连（[CONN-7](observed-behavior.md#conn-7)）。

组主运行时，前端对每个热点客户端发送 `DISASSOCIATION`，然后发送 `STOP_AP` 和 `CAN_SUSTAIN_AP`。Windows 不会自动重新打开热点（[HS-13](observed-behavior.md#hs-13)、[HS-19](observed-behavior.md#hs-19)）。

### 停止和关机

释放硬件（重启设备、禁用、卸载、更新驱动）时按以下顺序执行：

1. `RtwEvtReleaseHardware` 调用 `RtwWifiReleaseHardware`：停止组主、断开连接、停止扫描，然后调用 `rtw_linux_stop`。
2. `RtwRemove` 调用 `rtw_linux_remove`：执行 rtw89 的 PCI remove，释放 devm 内存，然后由 `rtw_skb_reclaim` 回收剩余的 `sk_buff`。
3. `RtwHwUnmap` 解除 BAR2 映射。
4. `RtwGuardEnd` 结束本次运行（`ArmRunning = 0`）。
5. `RtwSaveLog` 将驱动日志的最后 16 KB 存入设置项 `SavedLog`。

关机或重启电脑时，Windows 不释放设备。D0 退出发生在注册表最后一次写回磁盘之后。因此，诊断接口注册关机通知；`RtwEvtShutdown` 调用 `RtwGuardEnd` 和 `RtwSaveLog`（[INST-5](observed-behavior.md#inst-5)）。

<a id="data-flow"></a>
## 数据流

结论：发送和接收的均为原生 802.11 数据帧。Windows 负责 802.3 与 802.11 之间的转换。加密头、序号、重排序和 PN 检查均在 mac 层处理。

| 处理 | 方向 | 在哪一层 | 函数 |
|---|---|---|---|
| 复制帧，立即归还 Windows 的缓冲区 | 发送 | mac 层 | `mac\tx.c` `rtw_linux_tx_data` |
| 添加 QoS 控制字段和各 TID 的序号 | 发送 | mac 层 | `rtw_linux_tx_data` |
| 按豁免表决定是否加密 | 发送、接收 | mac 层 | `mac\key.c` `mac_key_tx`、`mac_key_rx_clear` |
| 写入 CCMP 头和 PN | 发送 | mac 层 | `mac\key.c` `mac_key_ccmp_hdr` |
| 加密数据，添加 MIC | 发送 | 芯片（由 rtw89 设置） | `ops->tx` |
| A-MPDU 聚合 | 发送 | 芯片（mac 层建立 BA 会话） | `mac\agg.c` `mac_agg_tx_check` |
| 解密数据 | 接收 | 芯片 | — |
| 去除 FCS | 接收 | mac 层 | `mac\rx.c` `linuxkpi_ieee80211_rx` |
| 去除重复帧 | 接收 | mac 层 | `mac\rx.c` `rx_data` |
| 重排序（64 个槽，100 ms 超时） | 接收 | mac 层 | `mac\agg.c` `mac_agg_rx_frame` |
| PN 检查，去除 CCMP 头和 MIC | 接收 | mac 层 | `mac\key.c` `mac_key_rx`；`mac\rx.c` `mac_rx_deliver` |
| 检查 A-MSDU，重建 24 字节数据帧头 | 接收 | mac 层 | `mac\rx.c` `mac_rx_deliver`、`rx_fifo_put` |

### 发送

```text
NetAdapterCx 发送队列
  -> TxAdvance、TxOne（wificx\adapter.cpp）：取出数据包和每包豁免动作
  -> rtw_linux_tx_data（mac\tx.c）
       未连接：返回 -ENOTCONN，前端丢弃数据包并计数
       在途 >= 64 帧，或 rtw89 已停止队列：返回 -EBUSY，帧留在队列中
       EAPOL 使用 TID 7；其它帧使用 Windows 给出的优先级
  -> ops->tx（rtw89）-> 芯片
  -> tx_status（DISPATCH，持有 trx_lock）：只记账
  -> tx_ready 回调 -> DpDpc（KDPC）-> NetAdapterCx 再次调用 Advance
```

### 接收

```text
INTx 中断 -> driver\os.c 的 ISR -> rtw_linux_isr（DIRQL，调用 rtw89 的中断处理函数）
  -> DPC：rtw_linux_irq_dpc -> rtw89 的中断线程（工作队列）-> NAPI 工作队列线程（PASSIVE）
  -> linuxkpi_ieee80211_rx（mac\rx.c）
       信标和探测响应 -> mac_bss_rx（BSS 表）
       发给组主的帧 -> mac_ap_rx
       管理帧 -> mac_mlme_rx；BlockAckReq -> mac_agg_rx_bar
       数据帧 -> rx_data：丢弃分片、去除重复帧 -> 重排序 -> mac_rx_deliver
  -> 接收 FIFO（最多 256 帧；满时丢弃并计数）
  -> rx_ready 回调 -> DpDpc -> NetAdapterCx -> RxAdvance -> rtw_linux_rx_data（每个缓冲区一个 MSDU）
```

### 组主的数据路径

组主有一套独立的入口：`rtw_linux_ap_tx_data`、`rtw_linux_ap_rx_data` 等。组主有独立的在途计数（上限 64 帧）、接收 FIFO（上限 256 帧）和回调。

- 发送：单播帧按地址 1 查找热点客户端。客户端处于省电状态时，帧进入该客户端的缓存（每个客户端最多 64 帧，10 s 后过期）。组播帧使用组密钥。
- 接收：只接收 ToDS 帧，按地址 2 查找热点客户端。
- 设置项 `WifiDirect=0` 时，组主的代码不运行，站点的数据路径保持不变。

<a id="protection"></a>
## 保护机制

### 崩溃循环保护

结论：连续两次运行未正常结束时，下一次启动进入诊断模式，不访问芯片。

- 计数保存在 `Parameters` 下的 `ArmRunning` 和 `BootAttempts` 中（`driver\driver.c`）。这两个值由驱动自行写入。
- 驱动在访问芯片之前将 `ArmRunning = 1` 写入磁盘（`RtwGuardBegin`）。注册表配置单元不可写时，`RtwGuardWait` 每 500 ms 重试一次，最长 60 s；超时后本次启动失败，驱动不访问芯片（[INST-4](observed-behavior.md#inst-4)）。
- 运行正常结束有两种方式：释放硬件，或收到关机通知。两种方式都调用 `RtwGuardEnd`，将两个值清零。
- `BootAttempts` 达到 2 时，`RtwGuardDecide` 选择诊断模式。`rtwdiag.ps1 arm` 清零计数并重启设备。
- 同一次启动中无法切换到诊断模式，因为切换需要重新枚举（`WdfDeviceFailedAttemptRestart`）。重新枚举后，芯片在断电前无法下载固件（[INST-6](observed-behavior.md#inst-6)）。

### 诊断接口和日志

- 诊断接口 `\\.\rtw89win` 是一个 KMDF 控制设备对象（CDO），其 SDDL 只允许 SYSTEM 和管理员访问。两种模式下均提供该接口。原因：NetAdapterCx 接管了 FDO（[INIT-4](observed-behavior.md#init-4)）。
- `RtwEvtDeviceCleanup` 删除 CDO。若不删除，驱动无法卸载。
- 驱动日志是内存中的 64 KB 环形缓冲区（`driver\log.c`）。每一行同时输出到 `DbgPrintEx`。
- 驱动映像随设备停止而卸载。`RtwSaveLog` 在释放硬件、关机和启动失败时写入保存的日志（[INST-7](observed-behavior.md#inst-7)）。
- 驱动加载时，对 AES-CCM 和 AES-CMAC 执行已知答案自检（`driver\crypto.c` `RtwCryptoSelfTest`）。
- 帧级自检（CCMP、BIP）在首次调用 `rtw_linux_set_pmf` 时运行（`mac\pmf.c` `pmf_selftest`）。该调用发生在设备首次启动、读取设置项时。
- 任一自检失败时，`rtw_linux_set_pmf` 均关闭管理帧保护。

<a id="bluetooth"></a>
## 蓝牙过滤驱动

结论：蓝牙过滤驱动在 BTHUSB 之前向控制器下载蓝牙补丁；任何失败均转为直通，BTHUSB 照常以 ROM 固件启动。

- **位置**：扩展 INF（`bt\rtkbtflt\rtkbtflt.inf`）通过 `AddFilter` 和 `FilterPosition = Lower` 将过滤驱动安装在 BTHUSB 之下。基础驱动仍为 Windows 的 `bth.inf`（[BT-7](observed-behavior.md#bt-7)）。
- **时机**：它是 KMDF 过滤驱动，不是电源策略所有者。其 `PrepareHardware` 和 `D0Entry` 在 START 或 D0 请求向上返回时运行，此时 BTHUSB 尚未收到该请求（[BT-5](observed-behavior.md#bt-5)）。
- **会话**：从 D3 回到 D0 时（包括启动），`hci.c` 读取版本；控制器运行 ROM 固件时下载补丁。从 D1、D2 返回时不执行任何操作，因为补丁仍然有效（[BT-4](observed-behavior.md#bt-4)）。
- **USB 访问**：首次启动时，驱动自行选择 USB 配置并获取中断管道。D3 → D0 时，驱动使用 BTHUSB 选择配置时记录的中断管道（`urb.c`）。
- **截取**：`urb.c` 是 WDM 预处理例程。BTHUSB 的事件读请求取得本次会话正在等待的命令完成事件时，`urb.c` 将该事件留给会话，BTHUSB 不会收到。
- **失败**：`D0Entry` 始终返回成功。补丁文件不正确、命令失败、控制器不是 8852B 的 ROM 时，均转为直通。
- **崩溃循环保护**：计数保存在 `Services\rtkbtflt\Parameters` 下的 `SessionRunning` 和 `BadSessions` 中，由驱动自行写入。连续 2 次会话未正常结束时转为直通，直到运行 `rtkbtflt.ps1 clear`。会话修改过设备时，`SessionRunning = 1` 再保持 60 s，或保持到会话正常结束。设置项 `PassThrough=1` 强制直通。
- **关机通知**：通过单独的 CDO 接收（[INST-5](observed-behavior.md#inst-5)）。

`bt\rtkbtfw\` 包含两部分：

- `epatch.c`：解析补丁文件并计算分段。蓝牙过滤驱动和工具共用该文件，并有主机测试（`test-epatch.c`）。
- `rtkbtfw.exe`：用户态工具。`rtkbtfw info` 显示控制器版本和蓝牙过滤驱动的结果；`load` 仅用于诊断（[BT-3](observed-behavior.md#bt-3)）。

<a id="build-structure"></a>
## 构建结构

结论：`buildhal.ps1` 用 clang 将 Linux 侧编译为 `rtw89hal.lib`；`build.ps1` 随后用 MSBuild 编译 Windows 侧并完成链接。构建步骤见[构建](build.md)。

```text
third_party\rtw89\*.c（26 个）+ compat\*.c + mac\*.c
   --clang, --target=aarch64-w64-mingw32--> build\ARM64\Release\rtw89hal.lib
driver\*.c + wificx\*.cpp + rtw89hal.lib + WificxTLVGenParse.lib + libcntpr.lib + ksecdd.lib
   --MSBuild（MSVC）--> rtw89win.sys
```

- **clang 的目标**：使用 MinGW 目标，因为只有该目标支持以 `-mno-ms-bitfields` 按 GCC 规则排列位域。rtw89 将位域结构直接套用在固件和 efuse 数据上（[BUILD-1](observed-behavior.md#build-1)）。
- **ABI**：在 ARM64 上，两个目标的调用约定、LLP64 类型、COFF 重定位和 x18 保留规则均相同，因此 MSVC 能链接 clang 生成的目标文件。
- **clang 选项**：参照 arm64 Linux 内核：`-mgeneral-regs-only`（不使用 FP/SIMD 寄存器）、`-fno-strict-aliasing`、`-fwrapv`、`-ffreestanding`。宏为 `-D__KERNEL__`、`-DCONFIG_RTW89_8852BE=1`、`-DCONFIG_RTW89_PCI=1`。
- **目标文件名**：上游文件使用原名；`compat\`、`mac\` 的文件加目录前缀（例如 `mac_util`），因为上游也有 `util.c`。
- **`IS_ENABLED`**：`compat\include\rtw\base.h` 使用一张显式的表。表中没有的选项将导致编译错误。
- **MSVC**：`/W4 /WX`；C++ 文件使用 `/Zc:sizedDealloc-`；KMDF 1.33、WiFiCx 1.1、NetAdapterCx 2.4（[BUILD-2](observed-behavior.md#build-2)）。
- **导入表**：只有 `ntoskrnl.exe`、`HAL.dll`、`WDFLDR.SYS`、`ksecdd.sys`。NetAdapterCx 和 WiFiCx 通过 `WDFLDR.SYS` 绑定（[BUILD-3](observed-behavior.md#build-3)）。
- **INF**：`driver\rtw89win.inf` 的 `[Manufacturer]` 节只面向 `NTARM64.10.0...26100`，即 Windows 11 ARM64 版本 26100 及以上。
- **蓝牙**：`bt\rtkbtflt\` 和 `bt\rtkbtfw\` 只使用 MSVC，不经过 clang。

<a id="decisions"></a>
## 设计决策

| 决策 | 理由 | 代价或限制 |
|---|---|---|
| 上游 rtw89 不修改任何一行 | 升级时可整体替换该目录；层间接口即 mac80211 的 `ieee80211_ops` | mac 层和兼容层须模拟 Linux 的细节，例如 `sk_buff` 的布局（[HS-18](observed-behavior.md#hs-18)） |
| 自行编写 mac 层 | Windows 没有 mac80211；WiFiCx 要求驱动自行完成认证和关联；mac80211 采用 GPL；FreeBSD 的 LinuxKPI 证明此方案可行 | 802.11 逻辑全部由本项目维护；只实现了用到的部分（见[移植指南](porting.md#mac-assumptions)） |
| 使用 FreeBSD LinuxKPI 的 BSD 头，不复制 Linux 的 GPL 头；改动处标注 `rtw89win:` | 整个项目可按 BSD-3-Clause 发布 | FreeBSD 头与 Linux 头存在语义差异，例如 `ieee80211_hdrlen`（[WIDE-6](observed-behavior.md#wide-6)） |
| clang 使用 MinGW 目标，并加 `-mno-ms-bitfields` | rtw89 使用 GNU 扩展；位域须按 GCC 规则排列；ABI 与 MSVC 一致 | 构建需要两套工具链 |
| Windows 侧使用 MSVC | WiFiCx 的 TLV 库是 MSVC 编译的 C++ 库 | 须满足 TLV 库的链接要求（[BUILD-2](observed-behavior.md#build-2)） |
| 编译选项参照 arm64 内核 | 与 rtw89 在 Linux 上的假设一致 | — |
| NAPI 在工作队列线程中运行；工作队列为单线程、按序执行；RCU 使用读者计数 | rtw89 的轮询函数可在 PASSIVE 下运行；RCU 写者必须等待全部读者退出 | 所有工作队列串行运行；不能在 DISPATCH 下调用 `synchronize_rcu` |
| 自建 DMA 适配器和 DMA 堆 | 需控制 4 GB 以下的地址和缓存类型；RK3588 的 PCIe 不做缓存一致（[PLAT-2](observed-behavior.md#plat-2)） | DMA 只使用 32 位地址；堆以外的内存经反弹缓冲传输 |
| 兼容层代替 rtw89 屏蔽 INTx | rtw89 按 MSI 编写；电平触发的 INTx 在 rtw89 停止时形成中断风暴（[RTW-9](observed-behavior.md#rtw-9)） | 屏蔽寄存器属于 RTL8852BE（AX 代）；其它芯片须另行核对 |
| rtw89 remove 之后回收 `sk_buff` | 上游停止时遗留一个 C2H 帧，导致 DMA 堆无法归还，下次 probe 失败（[RTW-1](observed-behavior.md#rtw-1)） | — |
| 崩溃循环保护；配置单元不可写时本次启动失败 | 防止每次开机都在同一位置蓝屏；未先记录就访问芯片时，计数将失效 | 开机早期最长等待 60 s；连续两次异常结束（包括断电）后须手动恢复 |
| 组主使用 AP 类型的 vif；Wi-Fi Direct 设备端口不创建 vif；组主的 vif 只在热点运行时存在 | 避开 MCC 和 NoA；不影响站点的省电 | 不支持 P2P 发现和协商 |
| 组主的数据面单独实现 | `WifiDirect=0` 时，站点路径与没有热点功能时相同 | 存在两套相似的代码 |
| 不实现 MCC | 站点和组主共用一个信道上下文即可满足移动热点的需求 | 站点连接时，热点只能使用同一信道（[HS-12](observed-behavior.md#hs-12)） |
| 2.4 GHz 保持 HT20；组主不加宽；不实现 HE | 无法测试的功能不进入缺省路径 | 2.4 GHz 和热点的速率受限；不支持 802.11ax |
| `Bandwidth=20` 时只使用 HT20，关联请求不带 VHT | 出现宽信道问题时有确定的回退方式 | 吞吐降低 |
| 只实现 CCMP-128；管理帧保护使用软件和 CNG | RTL8852B 不加密管理帧（[SEC-5](observed-behavior.md#sec-5)）；CNG 是内核自带的 AES 实现 | 不支持 TKIP、GCMP；导入表多出 `ksecdd.sys` |
| 管制规则使用完整的 wireless-regdb；优先级：`Country` > 接入点的国家元素 > efuse > 全球通用规则；注册时先应用全球通用规则 | WDI 不下发国家码（[REG-1](observed-behavior.md#reg-1)）；rtw89 没有管制规则时将访问空指针（[REG-2](observed-behavior.md#reg-2)） | 用户须自行设置国家码 |
| 蓝牙使用扩展 INF 和 `AddFilter`；WDM 预处理；原始 URB，不创建 `WDFUSBDEVICE` | 测试签名的完整 INF 排名低于 Windows 自带的驱动；基础驱动保持 `bth.inf`，卸载后恢复 Windows 自带的蓝牙栈 | 过滤驱动须自行选择 USB 配置并截取事件 |
| 蓝牙失败时直通；保护计数保持 60 s；提供 `PassThrough` | 补丁失败时 Windows 仍能启动蓝牙；无关的蓝屏不计入过滤驱动 | 直通时控制器运行 ROM 固件，无法扫描到设备（[BT-1](observed-behavior.md#bt-1)） |

<a id="source-tree"></a>
## 源码目录

| 目录或文件 | 内容 |
|---|---|
| `build.ps1` | 构建 Wi-Fi 驱动包：调用 `buildhal.ps1`，然后用 MSBuild 编译、签名并生成驱动包 |
| `buildhal.ps1` | 用 clang 编译 `rtw89hal.lib` |
| `packages.config` | WDK 和 SDK 的 NuGet 包 |
| `local.settings.example.psd1` | 构建设置的示例 |
| `LICENSE`、`THIRD_PARTY_NOTICES` | 本项目的许可和第三方许可声明（构建脚本将其放入驱动包） |
| `driver\` | 驱动层：`driver.c`（入口、PnP、崩溃循环保护、诊断接口）、`probe.c`（启停 rtw89、读取设置项）、`os.c`（`rtwk_*`、DMA、中断）、`hw.c`（BAR2、PCI 配置）、`diag.c`（诊断 IOCTL）、`log.c`（驱动日志）、`crypto.c`（基于 CNG 的 AES）、`rtw89win.inf` |
| `wificx\` | WiFiCx 前端（见下表） |
| `mac\` | mac 层（见下表） |
| `compat\` | 兼容层：`glue.c`（入口）、`device.c`（PCI、中断、DMA、固件）、`skbuff.c`、`netdev.c`（NAPI）、`workqueue.c`、`rcu.c`、`printk.c`、`lib.c`、`rtw89info.c`（读取 rtw89 内部状态，供诊断使用） |
| `compat\include\rtw\` | 本项目的接口头：`glue.h`、`os.h`、`base.h`、`bus.h`、`skbuff.h` 等 |
| `compat\include\linux\`、`asm\`、`sys\`、`net\`、`net80211\` | Linux API 头；其中 6 个 802.11 头来自 FreeBSD LinuxKPI，2 个来自 FreeBSD net80211（提交见 `compat\FREEBSD_REV`） |
| `third_party\rtw89\` | 上游 rtw89 的 49 个文件（原样）、两份许可、`README.md`（来源和每个文件的 blob id） |
| `bt\rtkbtflt\` | 蓝牙过滤驱动、INF，以及构建、安装、卸载、状态和回滚脚本 |
| `bt\rtkbtfw\` | `epatch.c`、`rtkbtfw.exe` 的源码和主机测试 |
| `tools\` | 放入驱动包的脚本：`install-rtw89win.ps1`、`uninstall-rtw89win.ps1`、`rtwdiag.ps1`、`rollback-rtw89win.sh` |
| `tools-dev\` | 构建辅助脚本和主机测试：`buildenv.ps1`、`new-testcert.ps1`、`undef.ps1`、`errsum.ps1`、`regdb-gen.ps1`、`apframe-test`、`wide-test`、`crypto-test` |
| `tools-dev\board\` | 开发者测试脚本（在目标设备上运行；说明见 `tools-dev\board\README.txt`） |
| `hosttools\` | 在测量电脑上测试目标设备：`rtw89win-thru.ps1`（吞吐）、`rtw89win-tcprtt.ps1`（TCP 往返时间） |
| `firmware\` | 固件许可、WHENCE 摘录、`SHA256SUMS`、获取说明；固件文件本身不在仓库中 |
| `docs\` | 本文档集 |

WiFiCx 前端（`wificx\`）：

| 文件 | 内容 |
|---|---|
| `device.cpp` | 设备初始化、能力上报（`ReportCapabilities`）、`PrepareHardware`、D0 进出 |
| `command.cpp` | WDI 消息的入队和分派（`RtwEvtWifiSendCommand`、`RtwWifiCmdWork`）、`TASK_DOT11_RESET`、射频状态、省电提示、`WdiDump` |
| `scan.cpp` | `TASK_SCAN`、`ABORT_TASK`、`BSS_ENTRY_LIST` |
| `connect.cpp` | `TASK_CONNECT`、`TASK_DISCONNECT`、关联结果、链路状态、掉线 |
| `security.cpp` | 豁免表、密钥的安装和删除 |
| `sae.cpp` | WPA3-SAE 的指示和参数 |
| `adapter.cpp` | 站点和角色端口的网卡、收发队列、数据路径 |
| `wfd.cpp` | Wi-Fi Direct 能力、Wi-Fi Direct 设备端口、端口分派 |
| `ap.cpp` | 组主：`TASK_START_AP`、选择信道、热点客户端的关联、`TASK_STOP_AP` |
| `alloc.cpp` | TLV 库所需的 `operator new` 和 `operator delete` |
| `wifi.h` | 内部声明；线程和锁的说明 |

mac 层（`mac\`）：

| 文件 | 内容 |
|---|---|
| `mac.h` | 内部声明 |
| `hw.c` | `wiphy` 和 `ieee80211_hw` 的生命周期、wiphy 工作项、能力导出、`restart_hw` |
| `iface.c` | vif、信道上下文、接收过滤、启停、SER 后的恢复（`mac_restart_work`） |
| `scan.c` | 固件扫描（`hw_scan`） |
| `bss.c` | BSS 表和帧体导出 |
| `mlme.c` | 站点的认证、关联、连接、断开、信标丢失 |
| `key.c` | 密钥、豁免、CCMP 头和 PN 检查 |
| `pmf.c` | 管理帧保护：软件 CCMP 和 BIP、SA Query |
| `sae.c` | SAE 认证帧的收发 |
| `tx.c` | 发送、管理帧发送、发送状态 |
| `rx.c` | 接收、去除重复帧、接收 FIFO |
| `agg.c` | BA 会话和重排序 |
| `ps.c` | 站点的省电策略 |
| `reg.c`、`regdb.h` | 管制规则；`regdb.h` 由 `tools-dev\regdb-gen.ps1` 从 wireless-regdb 生成 |
| `wide.c`、`wide.h` | 信道宽度和 VHT 能力（纯函数，有主机测试） |
| `ap.c` | 组主：AP 类型的 vif、热点客户端、密钥、省电缓存、数据路径 |
| `apframe.c`、`apframe.h` | 组主的信标、探测响应、关联响应（纯函数，有主机测试） |
| `util.c` | 信道号、速率、TID 等无状态函数 |
| `stubs.c` | 未实现的部分：txq 调度、信道上下文模拟 |

术语见[术语表](glossary.md)。添加芯片和更换硬件平台见[移植指南](porting.md)。
