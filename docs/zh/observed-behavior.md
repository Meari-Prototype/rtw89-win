# 实测结论总结

[English](../en/observed-behavior.md)

- **适用读者**：驱动开发者，包括编写 WiFiCx 驱动的人员和将 Linux Wi-Fi 驱动移植到 Windows 的人员。
- **本文内容**：WiFiCx、WDI 和 Windows 的实际行为中，官方文档未说明或与微软示例不同的部分。另有将上游 rtw89 接入 Windows ARM64 时须满足的要求。
- **前提**：了解 WDI 消息模型（任务、属性、指示）。

<a id="summary"></a>
## 要点

1. [CONN-2](#conn-2)：Wlansvc 每次连接都将 `AllowedBSSIDs` 设为广播地址，表示"任意 BSSID"。
2. [SCAN-4](#scan-4)：`ABORT_TASK` 的 `OriginalTaskOID` 是消息号，不是 OID 值。
3. [SEC-1](#sec-1)：`SET_SAE_AUTH_PARAMS` 携带 Windows 自行分配的事务号。按微软示例检查事务号时，WPA3-SAE 始终无法连接。
4. [HS-2](#hs-2)：Wi-Fi Direct 能力存在下限。低于下限时，连站点的网卡也会消失。
5. [HS-7](#hs-7)：角色端口须发送 `LINK_STATE_CHANGE`。不发送时，Windows 丢弃热点客户端的 EAPOL。
6. [HS-8](#hs-8)：不得调用 `WifiAdapterAddPeer`。未配置发送分流时，该调用会导致系统蓝屏。
7. [INST-2](#inst-2)：先卸载再安装会更换网卡 GUID，WLAN 配置文件随之失效。原地升级保持网卡 GUID 不变。
8. [INST-6](#inst-6)：重新枚举后，RTL8852BE 在断电前无法下载固件。
9. [RTW-9](#rtw-9)：只有 INTx 中断时，rtw89 在固件错误恢复（SER）期间不屏蔽芯片中断。兼容层须代为屏蔽，否则一次 SER 即导致蓝屏。
10. [REG-1](#reg-1)：Windows 不下发国家码。驱动须自带管制规则，并为用户提供设置项。

<a id="how-to-read"></a>
## 阅读说明

每条结论有一个固定的编号（例如 `SCAN-4`）和同名的小写锚点（`observed-behavior.md#scan-4`）。每条结论分为四项：

- **结论**：用一句话描述行为。需要时，后面附上细节。
- **依据**：先写可信度，然后写观察方式。
- **影响**：不按此处理时的后果。
- **代码**：本项目中对应的文件和函数。路径相对于仓库根目录。

可信度只使用三个词：

| 可信度 | 含义 |
|---|---|
| 板上实测 | 在目标设备上通过日志、计数或现象证实。只测过一次时注明"测过一次"。 |
| 代码推断 | 通过阅读 WDK 头文件、微软示例、上游源码或本项目源码得出。只由主机测试证实的结论也属于此类。 |
| 未确定 | 现有证据不足以判断。须写明缺少哪些证据。 |

其它约定：

- 测试环境和代号（AP-1、AP-2、AP-3、客户端-1）见[验证状态的测试环境](validation.md#environment)。
- WDI 消息写作 `TASK_SCAN`(4)。括号中为 WDK 头文件 `dot11wdi.h` 中的消息号。
- "请求完成"指驱动调用 `WifiRequestComplete`。"任务完成指示"指任务结束时驱动发送的指示，例如 `SCAN_COMPLETE`(60)。
- 各层的职责和调用上下文见[架构](architecture.md)。术语见[术语表](glossary.md)。

<a id="init"></a>
## 初始化与能力上报

<a id="init-1"></a>
### INIT-1 Windows 的 WDI 版本高于 WDK 头文件

- **结论**：Windows 11 26100 的 WDI 版本为 2.0.14，WDK 26100 的 `WDI_VERSION_LATEST` 为 2.0.13。驱动用 `WDI_VERSION_LATEST` 初始化。TLV 解析和生成的 `PeerVersion` 须使用 `WifiDeviceGetOsWdiVersion` 取得的版本。
- **依据**：板上实测。驱动日志记录两个版本号。`netsh wlan show wirelesscapabilities` 显示 WiFiCx OS 1.3、IHV 1.1。
- **影响**：`PeerVersion` 填写错误时的后果未确定（没有反例）。
- **代码**：`wificx\device.cpp` `RtwWifiPostDeviceCreate`

<a id="init-2"></a>
### INIT-2 能力结构须用 `*_INIT` 宏初始化，废弃字段保持为 0

- **结论**：能力上报有以下要求：
  1. 每个 `WIFI_*_CAPABILITIES` 结构都须用对应的 `*_INIT` 宏初始化（该宏填写 `Size`）。
  2. `*_Deprecated` 和 `DoNotUseOsReserved` 字段须保持为 0。
  3. WiFiCx 静默跳过它认为不兼容的认证和加密组合。
  4. 驱动在 `EvtDevicePrepareHardware` 中报告能力。WiFiCx 复制数组，调用返回后驱动即可释放数组。
- **依据**：代码推断：`wificx.sys` 中有检查 `Size`、已弃用字段和不兼容组合的错误字符串。板上实测：本驱动的能力上报每次都成功。
- **影响**：结构不正确时，能力上报失败。组合不兼容时，Windows 不列出对应的安全类型，也不报告错误。
- **代码**：`wificx\device.cpp` `ReportCapabilities`、`FillPhy`

<a id="init-3"></a>
### INIT-3 WDI 消息须单线程按序处理，每个请求恰好完成一次

- **结论**：WDI 消息须单线程按序处理：
  1. `EvtWifiDeviceSendCommand` 被调用时，IRQL 最高为 DISPATCH_LEVEL。驱动在此只将消息入队。
  2. 一个 PASSIVE_LEVEL 工作项按序处理每条消息：解析 → 请求完成 → 执行 → 指示 → 任务完成指示。
  3. KMDF 工作项在运行中再次入队时，会在另一个线程上并发运行。驱动须自行用标志防止并发。
  4. 每个 `WIFIREQUEST` 恰好完成一次。请求完成后，驱动不得再访问请求缓冲区。
- **依据**：代码推断（WDK 的 IRQL 约定和 KMDF 工作项的行为）。板上实测：每次运行中，收到的 WDI 消息数等于完成数（`rtwdiag.ps1 mode` 的计数）。
- **影响**：漏完成或重复完成时，WiFiCx 等待超时，或系统蓝屏。并发处理会打乱完成顺序。
- **代码**：`wificx\command.cpp` `RtwEvtWifiSendCommand`、`RtwWifiCmdWork`

<a id="init-4"></a>
### INIT-4 诊断接口不放在由 NetAdapterCx 管理的设备对象上

- **结论**：驱动另建一个设备对象来提供诊断接口：
  1. NetAdapterCx 接管功能设备对象。没有文档说明该对象上的 `IRP_MJ_DEVICE_CONTROL` 能否到达 KMDF 队列。
  2. 因此，驱动用 `WdfControlDeviceInitAllocate` 另建一个设备对象，提供诊断接口 `\\.\rtw89win`。其 SDDL 只允许 SYSTEM 和管理员。
  3. KMDF 不自动删除该对象。驱动须在功能设备对象的清理回调中删除它。
- **依据**：代码推断（设计选择）。板上实测：WiFiCx 模式和诊断模式下诊断接口均可用；反复重启设备后无残留。
- **影响**：不删除该对象时，设备停止后驱动映像仍留在内存中。更换驱动版本须重启电脑。
- **代码**：`driver\driver.c` `RtwCdoCreate`、`RtwEvtDeviceCleanup`

<a id="scan"></a>
## 扫描

<a id="scan-1"></a>
### SCAN-1 Wlansvc 启动后发送的 WDI 消息

- **结论**：Wlansvc 启动后按以下顺序发送消息：
  1. `SET_ADAPTER_CONFIGURATION`(85)，端口号 `0xFFFF`。
  2. `TASK_DOT11_RESET`(7)。
  3. `GET_STATISTICS`(34)。
  4. `TASK_SCAN`(4)。约 110 ms 后，`ABORT_TASK`(75) 中止该次扫描。
  5. `SET_LOCATION_PRIVACY`(138)。
  6. `TASK_SCAN`(4)。

  断开时，Wlansvc 每 60 s 发送一次 `TASK_SCAN`。连接后，它约每 40 s 发送一次 `GET_STATISTICS`。驱动对 34 和 138 返回 `STATUS_NOT_SUPPORTED`，扫描和连接不受影响。`TASK_SET_RADIO_STATE`(11) 不在该序列中，只在切换无线电开关时发送。
- **依据**：板上实测（驱动日志中的 WDI 消息序列和 `WdiDump` 输出）。
- **影响**：最小实现可以不支持 34 和 138。无线电开关的处理已在目标设备上验证：关闭再打开后，站点可重新连接。
- **代码**：`wificx\command.cpp` `Dispatch`、`SetAdapterConfiguration`、`TaskSetRadioState`

<a id="scan-2"></a>
### SCAN-2 Windows 自行维护 BSS 列表

- **结论**：能力 `BSSListCachemanagement = FALSE` 时，Windows 自行维护 BSS 列表。它从不发送 `GET_BSS_ENTRY_LIST`(47)，也从不发送 `SET_FLUSH_BSS_ENTRY`(97)。
- **依据**：板上实测。全部测试中，这两条消息均未出现。
- **影响**：驱动无需实现 BSS 缓存查询。每次扫描时，驱动上报本次收到的全部 BSS 即可。
- **代码**：`wificx\device.cpp` `ReportCapabilities`；`wificx\command.cpp` `Dispatch`

<a id="scan-3"></a>
### SCAN-3 `BSS_ENTRY_LIST` 的格式要求

- **结论**：`BSS_ENTRY_LIST`(62) 的每个条目须满足以下四条：
  1. 帧体从 8 字节 Timestamp 开始，不含 24 字节 MAC 头和 FCS。
  2. `ChannelInfo.BandId` 使用 WDI 的编号：2.4 GHz 为 1，5 GHz 为 2，6 GHz 为 5。不得使用 NL80211 的编号。
  3. 信道号必须在能力中报告过的信道表内。
  4. 指示的 `TransactionId` 为 0。
- **依据**：代码推断（微软示例的测试数据 `wifihaltestdata.h`）。板上实测：Windows 使用这些条目完成了全部连接；也曾在任务栏 Wi-Fi 列表中选择网络。
- **影响**：任一条不满足时，Windows 的网络列表为空，或缺少部分网络。
- **代码**：`wificx\scan.cpp` `RtwWifiScanEvents`；`mac\bss.c` `mac_bss_rx`、`rtw_linux_bss_export`

<a id="scan-4"></a>
### SCAN-4 `ABORT_TASK` 的 `OriginalTaskOID` 是消息号

- **结论**：WiFiCx 在 `ABORT_TASK`(75) 中填写的 `OriginalTaskOID` 是消息号（扫描为 4），而不是头文件 `WDI_DEFINE_OID(WDI_TASK_SCAN)` 计算出的值（`0xE4400004`）。驱动须同时识别两种写法。连接任务的中止按同样方式处理。
- **依据**：板上实测。驱动只识别 OID 值时，中止不生效，扫描照常执行完毕。两种写法都识别时，扫描在 10 ms 内结束。
- **影响**：中止不生效时，Wlansvc 须等待整个扫描结束。
- **代码**：`wificx\scan.cpp` `RtwWifiAbortTask`

<a id="scan-5"></a>
### SCAN-5 被中止的扫描也须发送完成指示

- **结论**：扫描被中止或被打断时，驱动先发送已收到的 `BSS_ENTRY_LIST`，然后发送 `SCAN_COMPLETE`(60)。状态使用 `NDIS_STATUS_REQUEST_ABORTED`（`0xC001000C`），不使用 `STATUS_REQUEST_ABORTED`。设备进入 D0Exit 时，若有未完成的扫描任务，驱动同样发送这两条指示。
- **依据**：板上实测。中止扫描和"扫描中重启设备"均已测试。
- **影响**：不发送任务完成指示时，WiFiCx 只能依靠超时判断该任务结束。
- **代码**：`wificx\scan.cpp` `RtwWifiScanStop`、`RtwWifiScanEvents`；`wificx\device.cpp` `RtwWifiD0Exit`

<a id="scan-6"></a>
### SCAN-6 扫描驻留时间

- **结论**：驱动将 WDI 的主动驻留时间传给 rtw89，并限制在 45～110 ms。
  1. Wlansvc 的扫描请求：通配 SSID、全部信道、类型 AUTO、驻留 80/110 ms、上限 4000 ms。
  2. rtw89 的固件扫描缺省主动驻留 45 ms；被动信道和 DFS 信道至少 105 ms。
  3. 全球通用规则下，一次扫描 39 个信道约需 4.1 s。
  4. 已连接时，一次扫描约需 5.4 s，连接不断开。
- **依据**：板上实测。不传递驻留时间时，同样 39 个信道约需 3.7 s。
- **影响**：驻留时间较短时，遗漏的弱信号接入点较多。`MaximumScanTime` 只用于驱动自身的超时。
- **代码**：`wificx\scan.cpp` `RtwWifiTaskScan`；`mac\scan.c` `scan_req_alloc`

<a id="scan-7"></a>
### SCAN-7 定位服务关闭时，命令行无法列出网络

- **结论**：Windows 定位服务关闭时，以下操作均报告"拒绝访问"（"Network shell commands need location permission"）：
  1. `netsh wlan show networks`。
  2. wlanapi 的 `WlanGetAvailableNetworkList`、`WlanGetNetworkBssList`、`WlanScan`。
  3. 已连接时的 `netsh wlan show interfaces`。

  只修改注册表中的定位同意值不生效。
- **依据**：板上实测。
- **影响**：验收和排障时，使用任务栏 Wi-Fi 列表、`Get-NetAdapter`、`Get-NetIPAddress`、WLAN-AutoConfig 事件日志，或临时打开定位服务。
- **代码**：无（Windows 的行为）。

<a id="connect"></a>
## 连接、密钥和豁免

<a id="conn-1"></a>
### CONN-1 WPA2-个人连接时 Windows 发送的 WDI 消息

- **结论**：一次 WPA2-个人连接的消息顺序如下：
  1. `TASK_DOT11_RESET`(7)，`SetDefaultMIB=1`。
  2. `GET_STATISTICS`(34)。
  3. `SET_PRIVACY_EXEMPTION_LIST`(28)，1 项。
  4. `TASK_CONNECT`(6)：1 个候选；认证只有 RSNA-PSK；单播 CCMP；组播 NONE 和 CCMP；无 AKM 表；`ExcludeUnencrypted=1`。
  5. 关联后约 0.43 s：`SET_ADD_CIPHER_KEYS`(29) 安装成对密钥。消息不带 key ID，驱动按 0 处理。
  6. 约 6 ms 后：`SET_ADD_CIPHER_KEYS`(29) 安装组密钥。key ID 由接入点决定（实测为 1 或 2），RSC 为 0。

  断开时：`TASK_DISCONNECT`(8，原因 3) → `TASK_DOT11_RESET`(7，`SetDefaultMIB=0`)。从 `netsh wlan connect` 到取得 DHCP 地址约需 7 s，其中 3～4 s 为 Wlansvc 连接前的扫描。
- **依据**：板上实测（AP-1、AP-2）。组密钥 RSC 的字节序未确定：Windows 只给出过 0。
- **影响**：驱动须在关联成功后先让 EAPOL 以明文通过，然后再接受密钥。四次握手由 Windows 完成，驱动不参与。
- **代码**：`wificx\connect.cpp` `RtwWifiTaskConnect`、`RtwWifiTaskDisconnect`；`wificx\security.cpp` `RtwWifiSetAddCipherKeys`

<a id="conn-2"></a>
### CONN-2 `TASK_CONNECT` 的 `AllowedBSSIDs` 是广播地址

- **结论**：Wlansvc 每次发送 `TASK_CONNECT`(6) 时都携带 `AllowedBSSIDs` = {`FF:FF:FF:FF:FF:FF`}，表示"任意 BSSID"。空表同样表示不限制。`DisallowedBSSIDs` 仍按 6 字节精确比较。
- **依据**：板上实测。
- **影响**：按 6 字节精确比较 `AllowedBSSIDs` 时，全部候选均被滤除。连接失败，驱动日志报告没有可用的候选。
- **代码**：`wificx\connect.cpp` `AddressListed`、`RtwWifiTaskConnect`

<a id="conn-3"></a>
### CONN-3 豁免表的格式

- **结论**：Windows 在 `TASK_DOT11_RESET` 之后、`TASK_CONNECT` 之前下发 1 项豁免：以太网类型 `0x888E`，动作 2（`ON_KEY_MAPPING_KEY_UNAVAILABLE`）。消息中的以太网类型为大端字节序。本驱动在能力中报告 `PrivacyExemptionListSize = 8`。
- **依据**：板上实测（驱动日志中的豁免表内容）。
- **影响**：字节序颠倒时，驱动丢弃 EAPOL，四次握手超时。
- **代码**：`wificx\security.cpp` `RtwWifiSetPrivacyExemptionList`；`mac\key.c` `exempt_lookup`、`mac_key_tx`、`mac_key_rx_clear`

<a id="conn-4"></a>
### CONN-4 每包豁免动作只在发送队列上启用

- **结论**：WiFiCx 只在发送队列上启用 `ms_packet_wifiexemptionaction` 扩展，不在接收队列上启用。
- **依据**：板上实测。驱动日志记录发送队列为"exemption action on"、接收队列为"exemption action off"。
- **影响**：接收方向不能依赖该扩展。发送方向上，扩展值为"不豁免"的帧仍须查询豁免表。
- **代码**：`wificx\adapter.cpp` `CreateTxQueue`、`CreateRxQueue`、`TxOne`

<a id="conn-5"></a>
### CONN-5 `ASSOCIATION_RESULT` 的必填内容

- **结论**：`ASSOCIATION_RESULT`(76) 须满足以下要求：
  1. `AssociationRequestFrame` 存放驱动发出的完整关联请求帧体。Windows 在四次握手消息 2 中使用其中的 RSN 元素。
  2. `BeaconProbeResponse` 存放候选 BSS 的帧体。Windows 用它核对四次握手消息 3。
  3. WPA2-个人时 `PortAuthorized = FALSE`。Windows 自行授权端口。
  4. `ActivePhyTypeList` 为必填项（WiFiCx 模型 1.1.4 起）。
  5. 成功时 `StatusCode` 必须为 0。
- **依据**：板上实测（WPA2-个人和 WPA3-SAE 连接成功）。`ActivePhyTypeList` 为必填项属于代码推断（WiFiCx 的模型文件）。
- **影响**：RSN 元素不一致时，四次握手失败。缺少必填字段时，指示生成失败。
- **代码**：`wificx\connect.cpp` `IndicateAssociation`；`mac\mlme.c` `mlme_send_assoc`

<a id="conn-6"></a>
### CONN-6 站点网卡的链路状态由 WiFiCx 管理

- **结论**：站点端口的网卡不需要驱动调用 `NetAdapterSetLinkState`。WiFiCx 根据关联指示和断开指示管理连接状态。角色端口的情况不同，见 [HS-7](#hs-7)。
- **依据**：板上实测。WPA2-个人、WPA3-SAE 和 5 GHz 连接全程正常。
- **影响**：在站点端口上自行设置链路状态是多余的。
- **代码**：`wificx\adapter.cpp` `RtwEvtWifiCreateAdapter`、`SetDataCapabilities`；`wificx\connect.cpp` `IndicateLinkState`

<a id="conn-7"></a>
### CONN-7 掉线后 Wlansvc 以漫游请求重连

- **结论**：驱动报告 `DISASSOCIATION`(51) 后（例如 SER 后的原因 61，即 `WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN`），Wlansvc 立即发送带 `RoamRequest=1` 的 `TASK_CONNECT`。端口空闲时，驱动须将其作为普通连接处理。
- **依据**：板上实测。驱动拒绝该请求时，Wlansvc 被拒绝三次后发送 `TASK_DOT11_RESET` 并放弃。驱动按普通连接处理时，SER 后立即重连。
- **影响**：拒绝漫游请求时，SER 后连接无法自动恢复。此后，"手动连接"的 WLAN 配置文件也不再自动连接。
- **代码**：`wificx\connect.cpp` `RtwWifiTaskConnect`

<a id="conn-8"></a>
### CONN-8 接入点主动断开时的表现

- **结论**：AP-2 关闭前先发送 deauth（原因 0）。驱动在 2.9 s 内断开，删除两把密钥，并上报 `DISASSOCIATION`(51)。AP-2 重新开启后更换了信道，重连正常。对端断开后，"手动连接"的 WLAN 配置文件不自动重连。
- **依据**：板上实测（AP-2）。
- **影响**：没有 deauth 的信标丢失路径未经测试，见[验证状态](validation.md#untested)。
- **代码**：`mac\mlme.c` `mac_mlme_peer_left`、`mlme_teardown`；`wificx\connect.cpp` `IndicateDisassociation`

<a id="security"></a>
## WPA3-SAE 和管理帧保护

<a id="sec-1"></a>
### SEC-1 `SET_SAE_AUTH_PARAMS` 的事务号由 Windows 自行分配

- **结论**：Windows 发送的 `SET_SAE_AUTH_PARAMS`(135) 携带其自身的 `TransactionId`，不等于连接任务的事务号。微软 WiFiCx 示例要求两者相等。驱动只检查"是否有连接任务正在运行"，BSSID 和 SAE 状态由 mac 层核对。
- **依据**：板上实测（AP-3）。检查事务号时，每一次 SAE 都被拒绝，空口上不发送任何 SAE 帧。
- **影响**：按示例实现时，WPA3-SAE 始终无法连接。
- **代码**：`wificx\sae.cpp` `RtwWifiSetSaeAuthParams`；`mac\sae.c` `sae_set_locked`

<a id="sec-2"></a>
### SEC-2 SAE 帧的上报字段

- **结论**：`SAE_AUTH_PARAMS_NEEDED`(134) 中上报的认证帧体从认证算法号开始。驱动将接入点的 confirm 帧同时放入 `SAEConfirmFrame` 和 `SAECommitFrame` 两个字段，Windows 能完成 SAE。Windows 选择的群为 19（scalar 32 字节，element 64 字节）。
- **依据**：板上实测（AP-3）。Windows 实际读取哪个字段未确定：未测试只填写一个字段的情况。
- **影响**：按现有写法，SAE 能够完成。
- **代码**：`wificx\sae.cpp` `RtwWifiSaeIndicate`

<a id="sec-3"></a>
### SEC-3 报告 SAE 能力后，Windows 改用 SAE 连接过渡模式网络

- **结论**：设置项 `Pmf=2` 时，驱动报告管理帧保护（PMF）和 SAE 能力。此时 Windows 连接过渡模式网络，自动将 WPA2-个人的 WLAN 配置文件改为使用 SAE（请求中为 `RSNA-PSK 0`、`WPA3-SAE 1`）。`Pmf=1` 时，连接才是 WPA2-个人加管理帧保护。
- **依据**：板上实测（AP-3）。
- **影响**：测试 WPA2-个人加管理帧保护时，须使用 `Pmf=1`。这也是管理帧保护和 WPA3-SAE 缺省关闭的原因之一。
- **代码**：`wificx\device.cpp` `ReportCapabilities`；`driver\probe.c` `RtwApplySettings`

<a id="sec-4"></a>
### SEC-4 管理帧保护下 SER 后的"稍后再来"

- **结论**：管理帧保护的连接在 SER 后重连。过程如下：
  1. 接入点先对旧关联执行 SA Query。
  2. 接入点随后返回关联状态码 30 和 comeback 时间（实测 1024 TU）。
  3. 驱动按 comeback 时间重发关联请求，连接成功。

  Windows 的定时器回调有时早于到期时间到达。此时驱动按剩余时间重新排定，不得丢弃该次重发。
- **依据**：板上实测（AP-3，`Pmf=1`）。定时器提前到达属于代码推断；按剩余时间重新排定时，日志中未出现重发丢失。WPA3-SAE 时，AP-3 在 comeback 期间丢弃 SAE 状态，随后发送 disassociation（接入点侧的行为）。
- **影响**：没有 comeback 处理时，管理帧保护的连接在 SER 后只能等待 Wlansvc 重新完成完整连接。SAE 连接遇到 AP-3 这类接入点时，须手动重连。
- **代码**：`mac\mlme.c` `mlme_rx`、`mlme_comeback_tu`、`mlme_timer`

<a id="sec-5"></a>
### SEC-5 8852B 不使用硬件加解密受保护的管理帧

- **结论**：启用管理帧保护时，受保护的管理帧须用软件加解密：
  1. 8852B 的 `hw_mgmt_tx_encrypt` 为假。受保护管理帧的硬件接收解密只在 WoWLAN 中启用。
  2. 受保护的单播管理帧（deauth、disassociation、块确认（BA）动作帧、SA Query 等）使用软件 CCMP。
  3. 组播管理帧使用软件 BIP-CMAC-128。
  4. 本驱动使用内核 CNG（`ksecdd.sys` 的 `BCrypt*`，AES-CCM 和 AES-CMAC），只在 PASSIVE_LEVEL 下调用。
  5. 驱动加载时自检 AES-CCM 和 AES-CMAC，首次设置 `Pmf` 时自检 CCMP 和 BIP 的帧处理。任一自检失败时，驱动均关闭管理帧保护。
- **依据**：板上实测（AP-3）：块确认的 ADDBA 双向受保护；受保护的 disassociation 被正确识别。组播 BIP 路径未经测试覆盖。
- **影响**：没有软件加解密时，管理帧保护连接上的块确认动作帧和断开帧均无法传递。驱动的导入表因此多出 `ksecdd.sys`。
- **代码**：`driver\crypto.c` `RtwCryptoInit`、`RtwCryptoSelfTest`、`rtwk_aes_ccm_encrypt`、`rtwk_aes_cmac`；`mac\pmf.c` `mac_pmf_rx`、`pmf_selftest`、`rtw_linux_set_pmf`

<a id="power"></a>
## 省电

<a id="ps-1"></a>
### PS-1 Windows 只发送连接质量提示

- **结论**：Windows 只用 `SET_CONNECTION_QUALITY`(32) 提示省电：
  1. WDK 头文件定义了 `SET_AUTO_POWER_SAVE`(77)，但 Windows 从不发送该消息。`GET_AUTO_POWER_SAVE`(78) 也从未收到。
  2. 站点能力 `AutoPowerSaveMode = TRUE` 时，Wlansvc 在每次 `TASK_DOT11_RESET` 后发送 32。该能力为 FALSE 时，Windows 从不发送 32。
  3. 电源计划中"无线适配器设置 → 节能模式"为"最高性能"时，32 的值为 4（不省电）。
- **依据**：板上实测（AP-1）。WiFiCx 的消息模型中没有 SET 消息，属于代码推断。
- **影响**：设置项 `PowerSave=1`（缺省，跟随 Windows）在该节能模式为"最高性能"时等同于不省电。如需省电，设置 `PowerSave=2`。修改电源计划的效果未测试。
- **代码**：`wificx\command.cpp` `SetConnectionQuality`、`GetAutoPowerSave`；`mac\ps.c` `ps_policy`、`mac_ps_recalc`

<a id="ps-2"></a>
### PS-2 驱动无法读取电源计划中无线网卡的省电档位

- **结论**：驱动用 `PoRegisterPowerSettingCallback` 注册电源计划中无线网卡的省电设置（GUID `12bbebe6-58d6-4636-95bb-3217ef867c1a`）。注册成功，回调立即到达。回调给出的是 16 字节数据，不是 0～3 的档位。同时注册的 `GUID_ACDC_POWER_SOURCE` 正常（4 字节）。
- **依据**：板上实测。16 字节数据的格式未确定：未查到文档。
- **影响**：驱动将该值作为"未知"处理，只记录一行日志。省电由 [PS-1](#ps-1) 的提示决定。
- **代码**：`driver\driver.c` `RtwPowerSettingCallback`、`RtwPowerSettingRegister`

<a id="ps-3"></a>
### PS-3 rtw89 省电的接入要点

- **结论**：rtw89 自行管理省电的进出和流量判断。mac 层按 mac80211 的语义只执行三项操作：
  1. 在"已授权并收到信标"后设置 `vif->cfg.ps`。
  2. 断开前先清除 `cfg.ps`。
  3. SER 后对新的 `vif` 发送一次 `BSS_CHANGED_PS`。原因是 rtw89 的恢复过程不清除 `lps_enabled`。

  固件 0.29.29.18 带有 `NO_LPS_PG`，深度省电只使用时钟门控。固件没有 `BEACON_TRACKING`。
- **依据**：板上实测（AP-1，`PowerSave=2`）。省电时，空闲状态下 TCP 建立连接的往返时间中位数约为 96 ms（DTIM 1，约一个信标间隔）。吞吐不下降。
- **影响**：省电明显增加交互延迟，因此缺省不省电。
- **代码**：`mac\ps.c` `mac_ps_recalc`、`mac_ps_leave`、`mac_ps_restart`

<a id="regulatory"></a>
## 国家码和管制规则

<a id="reg-1"></a>
### REG-1 Windows 不下发国家码

- **结论**：在 WiFiCx 和 WDI 中，国家（`WDI_TLV_COUNTRY_REGION_LIST`）只出现在驱动报告给 Windows 的能力中。没有设置国家码的 WDI 消息。`SET_LOCATION_PRIVACY`(138) 是隐私开关，不携带国家信息。
- **依据**：代码推断（WDK 头文件和 WiFiCx 模型）。板上实测：Windows 从未发送与国家有关的消息。
- **影响**：驱动须自带管制规则库，并提供设置项 `Country`。
- **代码**：`mac\reg.c`、`mac\regdb.h`；`driver\probe.c` `RtwApplySettings`

<a id="reg-2"></a>
### REG-2 注册前须先为 rtw89 提供一套管制规则

- **结论**：efuse 中没有国家码时，若不先调用一次 `reg_notifier`，rtw89 的 `regulatory.regd` 为空指针。`rtw89_core_start` 设置发射功率时会访问该指针。mac 层在注册时先按全球通用规则（`00`）调用一次。
- **依据**：代码推断（阅读上游源码得出，未在目标设备上触发）。
- **影响**：不调用时，rtw89 启动即因空指针而蓝屏。
- **代码**：`mac\reg.c` `mac_reg_register`、`reg_call_notifier`

<a id="reg-3"></a>
### REG-3 信道能否主动发射由 mac 层决定

- **结论**：信道的 `NO_IR`、`RADAR`、`DISABLED` 标志由 mac 层按 wireless-regdb 设置；在 Linux 上由 cfg80211 设置。rtw89 的管制规则不是 self-managed：其规则表只选择发射功率表（例如 JP 对应 MKK）和少数开关。另有三点：
  1. rtw89 的 `reg_notifier` 自行获取 wiphy 锁，须在锁外调用。
  2. `rtw89_regd_init_hint` 在 `ieee80211_register_hw` 之后才调用。
  3. FreeBSD 兼容头中 `enum nl80211_reg_initiator` 的数值与 Linux 不同（USER=0、DRIVER=1、CORE=2、COUNTRY_IE=3），须显式填写。
- **依据**：板上实测。国家码 JP 生效后，扫描信道从 39 个变为 34 个，与 wireless-regdb 原文逐条一致。
- **影响**：不实现这一层时，只能使用全球通用规则（5 GHz 全部被动扫描）。
- **代码**：`mac\reg.c` `reg_handle_channel`、`reg_apply`、`reg_work`；`mac\regdb.h`（由 `tools-dev\regdb-gen.ps1` 生成）

<a id="width"></a>
## 信道宽度

<a id="wide-1"></a>
### WIDE-1 rtw89 只为 8852B 声明 20、40、80 MHz

- **结论**：rtw89 v7.2 未为 8852B 声明 160 MHz。rtw89 的速率自适应只参考 `sta` 的字段和信道上下文（chanctx）的宽度，因此加宽信道的工作全部在 mac 层完成。
- **依据**：代码推断（上游 `rtw8852b.c`）。板上实测：VHT80 连接工作正常。
- **影响**：本驱动最宽支持 80 MHz。
- **代码**：`mac\wide.h`、`mac\wide.c`

<a id="wide-2"></a>
### WIDE-2 Windows 显示的链路速率来自 `LINK_STATE_CHANGE`

- **结论**：Windows 显示的链路速率取自驱动发送的 `LINK_STATE_CHANGE`(53)。VHT80 时显示 866.6 Mbps，有负载时随 rtw89 的发送速率变为 702 Mbps 或 780 Mbps。`Bandwidth=40` 时显示 300 Mbps，`Bandwidth=20` 时显示 144.4 Mbps。Windows 显示的"802.11ac"或"802.11n"来自 `ASSOCIATION_RESULT` 的 `ActivePhyTypeList`。
- **依据**：板上实测（AP-1）。
- **影响**：驱动须及时更新链路速率，Windows 才能显示正确的值。
- **代码**：`wificx\connect.cpp` `IndicateLinkState`、`IndicateAssociation`

<a id="wide-3"></a>
### WIDE-3 能力中的 `ChannelWidthList` 没有可见效果

- **结论**：驱动在能力中报告 `ChannelWidthList` = {20, 40, 80}，Windows 接受该值。`netsh` 的输出没有任何变化。WiFiCx 没有传递 VHT 或 HE 细节的 TLV。
- **依据**：板上实测。该字段的用途未确定。
- **影响**：信道宽度的选择完全由驱动决定。
- **代码**：`wificx\device.cpp` `ReportCapabilities`

<a id="wide-4"></a>
### WIDE-4 `TASK_CONNECT` 不携带 PHY 列表

- **结论**：Windows 发送的 `TASK_CONNECT`(6) 不携带 `ActivePhyTypeList`，也不携带 `AssociationRequestVendorIE`。因此，驱动按 PHY 列表关闭 VHT 的路径（`RTW_CONN_F_NO_VHT`）不会执行。
- **依据**：板上实测（AP-1）。
- **影响**：驱动不能依靠 Windows 限制 PHY 类型。
- **代码**：`wificx\connect.cpp` `RtwWifiTaskConnect`；`mac\mlme.c` `mlme_wide`

<a id="wide-5"></a>
### WIDE-5 只使用 40 或 20 MHz 时不宣告 VHT

- **结论**：VHT 站点必须能接收 80 MHz。BSS 为 VHT80 而驱动只使用 40 或 20 MHz 时，驱动降级为 HT。只有 BSS 本身为 VHT20 或 VHT40、且驱动使用其全部宽度时，才保留 VHT。
- **依据**：板上实测（AP-1）。`Bandwidth=40` 时，接入点以 HT40 发送。
- **影响**：宣告 VHT 却不接收 80 MHz 时，接入点会以驱动无法接收的宽度发送。
- **代码**：`mac\wide.c` `wide_choose`、`wide_ht_cap_ours`

<a id="wide-6"></a>
### WIDE-6 FreeBSD 头的 `ieee80211_hdrlen` 不计入 +HTC

- **结论**：FreeBSD 原版的 `ieee80211_hdrlen` 不计入 +HTC（HT Control 字段）。本项目的 `ieee80211_hdrlen` 按 Linux 的方法计入 +HTC（改动处标注 `rtw89win`）。驱动不宣告 `HTC_VHT`，AP-1 也不发送 +HTC 帧和 A-MSDU。
- **依据**：代码推断（与 Linux 头对照）。板上实测：`rx_htc` 和 `rx_amsdu` 计数均为 0，因此 +HTC 路径未在空口上触发。
- **影响**：长度计算错误时，rtw89 的发送描述符和 CCMP 头的位置都会出错。
- **代码**：`compat\include\linux\ieee80211.h` `ieee80211_hdrlen`

<a id="wide-7"></a>
### WIDE-7 VHT 能力的取舍

- **结论**：驱动不宣告 MU beamformee 和 +HTC。Max MPDU 宣告为 3895 字节（rtw89 给出的是 11454）。只有接入点是 SU beamformer 时，驱动才保留 SU beamformee。
- **依据**：板上实测（AP-1），测过一次。
- **影响**：宣告驱动不处理的能力时，接入点会发送驱动无法正确处理的帧。
- **代码**：`mac\wide.c` `wide_vht_cap_ours`、`wide_vht_sta`

<a id="wide-8"></a>
### WIDE-8 信道宽度的合法性由 mac 层检查

- **结论**：本项目的 cfg80211 chandef 检查函数为空函数。mac 层自行检查 40 MHz 配对、80 MHz 信道块和管制规则（`wide_usable`）。组合不合法时，按 80 → 40 → 20 MHz 降级。管制规则变化时，mac 层重新检查整条宽信道。
- **依据**：代码推断（主机测试 `wide-test.ps1`）。设置了 `Country` 时，规则变化的路径未在目标设备上触发。
- **影响**：不检查时，驱动会在不允许的信道组合上发射。
- **代码**：`mac\wide.c` `wide_usable`；`mac\mlme.c` `mac_mlme_chan_ok`；`mac\reg.c` `reg_work`

<a id="wide-9"></a>
### WIDE-9 `rx_chains_static` 保持为 1 也能两流接收

- **结论**：信道上下文的 `rx_chains_static` 和 `rx_chains_dynamic` 保持为 1 时，8852B 仍以两流接收。
- **依据**：板上实测（AP-1）：接入点以 2 流发送，驱动正常接收。
- **影响**：无需按流数修改这两个值。
- **代码**：`mac\iface.c` `mac_chanctx_use_def`

<a id="wide-10"></a>
### WIDE-10 站点和组主共用信道上下文

- **结论**：站点使用 VHT80 时，组主在同一信道上使用 20 MHz，工作正常。站点断开后，信道上下文保持 80 MHz，工作正常。组主先建立信道上下文时，站点只能使用 HT20。
- **依据**：板上实测（AP-1、客户端-1）。由此推出：SER 后用户先打开热点、然后连接站点时，站点只能使用 HT20。该推论属于代码推断，未经测试。
- **影响**：如需站点使用 VHT80，先连接站点，然后打开热点。
- **代码**：`mac\iface.c` `chanctx_share`

<a id="wide-11"></a>
### WIDE-11 VHT80 的吞吐

- **结论**：AP-1 上 VHT80 的吞吐为下行 133.7～133.9、上行 127.7～130.2 Mbit/s。`rx_dropped_full` 为 0。CPU0 中断最高约每秒 1.3 万次，未成为瓶颈。
- **依据**：板上实测。计时包含 ssh 建立连接的时间。`Bandwidth=40` 时下行只有 64～67 Mbit/s，原因未确定。
- **影响**：数值的条件和测量方法见[验证状态](validation.md#width)。
- **代码**：无（测量脚本 `hosttools\rtw89win-thru.ps1`）。

<a id="hotspot"></a>
## 移动热点（Wi-Fi Direct 组主）

<a id="hs-1"></a>
### HS-1 移动热点在 WiFiCx 下通过 Wi-Fi Direct 组主实现

- **结论**：在 WiFiCx 下，Windows 移动热点使用 Wi-Fi Direct 组主（GO）：
  1. 驱动报告 `WIFI_WIFIDIRECT_CAPABILITIES`。
  2. WiFiCx 回调驱动，创建 Wi-Fi Direct 设备端口（只有控制面）。
  3. 开启热点时，WiFiCx 使用同一个 `EvtWifiDeviceCreateAdapter` 创建角色端口（类型 `WIFI_ADAPTER_WIFI_DIRECT_ROLE`）。
  4. WiFiCx 在角色端口上发送 `TASK_START_AP`(14)。

  WiFiCx 不发送 `TASK_CREATE_PORT`(12)。`netsh` 显示的 "Soft AP: Not supported" 指旧的 ExtAP 方式，与移动热点无关。
- **依据**：板上实测（客户端-1）。
- **影响**：P2P 发现、协商和动作帧均可返回 NOT_SUPPORTED，移动热点仍能正常工作。
- **代码**：`wificx\wfd.cpp` `RtwEvtWifiCreateWifiDirectDevice`、`RtwWifiReportWfdCapabilities`；`wificx\adapter.cpp` `RtwEvtWifiCreateAdapter`；`wificx\ap.cpp` `RtwWifiApTaskStartAp`

<a id="hs-2"></a>
### HS-2 Wi-Fi Direct 能力存在下限

- **结论**：`ConcurrentClientCount = 0`，或 Wi-Fi Direct 能力的 `ScanSSIDListSize` 小于 4 时，`nwifi.sys` 挂接失败（`RareFilterPath … 0xC0000001`、`MissingMandatoryFilter`）。此时 Windows 没有任何 WLAN 网卡，站点的网卡也会消失。驱动须报告 `ConcurrentClientCount = 1` 和 `ScanSSIDListSize = 4`。
- **依据**：板上实测。`ScanSSIDListSize` 为 1 和 2 时失败，3 未测试。单独修改其它字段均不能解决问题。为何须为 4，未确定。
- **影响**：即使驱动不实现 P2P 客户端，也须报告 1 个 P2P 客户端端口。角色端口上的 `TASK_CONNECT` 仍返回 NOT_SUPPORTED。
- **代码**：`wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-3"></a>
### HS-3 INF 的 `NumberOfNetworkInterfaces` 须为 3

- **结论**：站点、Wi-Fi Direct 设备端口和 1 个角色端口共 3 个，因此 INF 中写 3。安装后多出两个网卡：WLAN 2（Wi-Fi Direct 设备端口）和 WLAN 3（角色端口）。热点关闭时，WLAN 3 显示为已禁用。
- **依据**：板上实测。值为 1 的情况未测试。原地升级后，网卡 GUID 和 WLAN 配置文件均保持不变。
- **影响**：该值过小时，Windows 无法创建热点的端口。
- **代码**：`driver\rtw89win.inf` `[Rtw89_Ndi_AddReg]`

<a id="hs-4"></a>
### HS-4 移动热点的 WDI 消息顺序

- **结论**：Windows 与驱动之间的消息按以下顺序出现：
  1. 设备启动：WiFiCx 创建 Wi-Fi Direct 设备端口 → `TASK_DOT11_RESET`(7) → `SET_LOCATION_PRIVACY`(138) → 7 → `TASK_CHANGE_OPERATION_MODE`(90，模式 `0x8`) → `SET_ADVERTISEMENT_INFORMATION`(89) 5 次。
  2. 角色端口创建后：7（`ResetMACAddress` 等于驱动报告的组主地址）→ `GET_STATISTICS`(34) → 138 → `SET_CONNECTION_QUALITY`(32)。角色端口有时在开启热点前创建，有时在开启热点时才创建。
  3. 开启热点：7 → 90（模式 `0x20`）→ 89 约 12 次 → `SET_PRIVACY_EXEMPTION_LIST`(28，1 项 `0x888E`) → `TASK_START_AP`(14) → 驱动发送 `P2P_GROUP_OPERATING_CHANNEL`(84) → 任务完成指示 → 89。开启热点时不安装组密钥。
  4. 热点客户端连接，从认证到授权约 60～70 ms：
     1. 驱动发送 `AP_ASSOCIATION_REQUEST_RECEIVED`(55)。
     2. Windows 发送 `TASK_SEND_AP_ASSOCIATION_RESPONSE`(17)。
     3. 关联响应收到 ACK 后，驱动发送任务完成指示。
     4. Windows 通过数据通路完成四次握手。
     5. `SET_ADD_CIPHER_KEYS`(29) 先安装成对密钥，然后安装组密钥（ID 2，第一个热点客户端完成握手后才安装）。
     6. `SET_DEFAULT_KEY_ID`(31)。
  5. 关闭热点：89 → 有热点客户端时，对每个热点客户端发送 `TASK_DISCONNECT`(8，原因 0) → 对广播地址发送 8（原因 8）→ `TASK_STOP_AP`(15) → 7 两次。
  6. Windows 发送 90、138、34。Windows 不发送 `SET_P2P_LISTEN_STATE`(26)、`SET_P2P_WPS_ENABLED`(123)、`SET_OPERATION_MODE`(24)。
- **依据**：板上实测（`WdiDump`，客户端-1）。
- **影响**：只实现移动热点时，驱动只需实现上述出现的消息。
- **代码**：`wificx\wfd.cpp` `RtwWifiWfdDispatch`；`wificx\ap.cpp`；`wificx\command.cpp` `Dispatch`

<a id="hs-5"></a>
### HS-5 须拦截发往 Wi-Fi Direct 端口的站点消息

- **结论**：Windows 也向 Wi-Fi Direct 设备端口和角色端口发送 `SET_CONNECTION_QUALITY`(32)、`SET_PRIVACY_EXEMPTION_LIST`(28)、`SET_DELETE_CIPHER_KEYS`(30) 等站点消息。站点的处理函数不检查端口号。驱动按端口拦截：
  1. 32：返回成功，不执行操作。
  2. Wi-Fi Direct 设备端口上的 28、30、31：返回成功，不执行操作。29 返回 NOT_SUPPORTED。
  3. `SET_SAE_AUTH_PARAMS`(135)：返回 NOT_SUPPORTED。
- **依据**：板上实测：角色端口上收到过 32。
- **影响**：不拦截时，这些消息会改变站点的省电策略，或删除站点的密钥。
- **代码**：`wificx\wfd.cpp` `RtwWifiWfdDispatch`

<a id="hs-6"></a>
### HS-6 关联完成指示的 `BeaconIEs` 须存放信标帧体

- **结论**：`TASK_SEND_AP_ASSOCIATION_RESPONSE`(17) 的完成指示中，`BeaconIEs` 须存放信标帧体：从 Timestamp 开始，包含 12 字节固定字段。WDI 示例给出的也是 `MMPDU_BODY(beaconframe)`。
- **依据**：板上实测。只存放信息元素时，Windows 在关联后约 40 ms 发送 `TASK_DISCONNECT`，未发送任何 EAPOL 帧。
- **影响**：存放内容错误时，热点客户端无法连接。
- **代码**：`wificx\ap.cpp` `AssocResponseComplete`；`mac\ap.c` `ap_resp_done`

<a id="hs-7"></a>
### HS-7 角色端口必须发送 `LINK_STATE_CHANGE`

- **结论**：`TASK_START_AP` 成功后、任务完成指示之前，驱动在角色端口上发送一次 `LINK_STATE_CHANGE`(53)。指示中的对端地址为组主自身，链路速率为 144400 kbit/s，信道宽度为 20 MHz。WDI 示例的组主不发送该指示。
- **依据**：板上实测。不发送时，角色端口的网卡在 NDIS 中始终处于断开状态：`TxAdvance` 从未被调用；pktmon 显示 EAPOL 在 Native WiFi 下层被丢弃（`Media Disconnected`）。
- **影响**：不发送时，热点客户端的四次握手无法完成。
- **代码**：`wificx\ap.cpp` `IndicateApLinkState`、`RtwWifiApTaskStartAp`

<a id="hs-8"></a>
### HS-8 不调用 `WifiAdapterAddPeer`，也无需发送分流

- **结论**：未配置发送分流（TX demux）时，调用 `WifiAdapterAddPeer` 会导致蓝屏（0x7E，`0xC0000420`，`WxPeerTable::Insert` 的容量为 0）。配置分流后不再蓝屏，但发送的帧仍无法到达驱动。帧能否到达取决于 [HS-7](#hs-7)。
- **依据**：板上实测（蓝屏和"帧无法到达驱动"）。根据 WiFiCx 的内部符号推测，分流按地址精确匹配，组播帧会被丢弃。这一点属于代码推断。
- **影响**：驱动不调用该函数，也不配置分流。移动热点仍能正常工作。
- **代码**：`wificx\` 中没有 `WifiAdapterAddPeer` 调用。

<a id="hs-9"></a>
### HS-9 `P2P_GROUP_OPERATING_CHANNEL` 的内容

- **结论**：`P2P_GROUP_OPERATING_CHANNEL`(84) 使用 `TASK_START_AP` 请求的事务号，Windows 接受。国家字符串为 `"<国家码>\x04"`。运营类别：2.4 GHz 使用 81，5 GHz 信道 36～48 使用 115。
- **依据**：板上实测（客户端-1）。事务号为 0 的情况未测试。
- **影响**：缺少该指示时的行为未测试。
- **代码**：`wificx\ap.cpp` `IndicateOperatingChannel`、`OperatingClass`

<a id="hs-10"></a>
### HS-10 角色端口的地址由驱动决定

- **结论**：角色端口使用驱动在能力 `InterfaceAddressList` 中报告的地址。`ResetMACAddress` 与该地址相同。热点客户端看到的 BSSID 也是该地址。驱动按以下方法取得地址：
  1. Wi-Fi Direct 设备地址为 efuse 地址加上本地管理位。
  2. 组主地址为设备地址的第 4 字节（从 0 起数）异或 `0x80`。
- **依据**：板上实测（客户端-1 看到的 BSSID）。
- **影响**：驱动须保证三个地址互不相同，且与站点地址不同。
- **代码**：`wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-11"></a>
### HS-11 热点的信道由驱动选择

- **结论**：Windows 设置中的"频段"与 `TASK_START_AP` 参数的对应关系如下：
  1. 设为 2.4 GHz 或 5 GHz：`APBandChannelList` 只有 1 项（只有频段号），`MustUse=TRUE`，没有 `AdvertisedOperatingChannel`。
  2. 设为"任何可用"：没有频段表，`MustUse=FALSE`。

  三种设置的共同参数为：`PreferOverStation=FALSE`、`DataRateSupport802_11b=TRUE`、`ExcludeUnencrypted=FALSE`、信标间隔 100、DTIM 1、认证 RSNA_PSK、加密 CCMP。因此信道由驱动选择：选择 BSS 表中最空闲的信道；2.4 GHz 只在 1、6、11 中选择；"任何可用"时优先选择 5 GHz。组主只使用管制规则允许主动发射、且无需雷达检测的信道。
- **依据**：板上实测（客户端-1，国家码 JP）。没有国家码时（全球通用规则），5 GHz 不允许主动发射，驱动不报告 5 GHz 组主能力。这一点属于代码推断。
- **影响**：将频段表的第一项当作指定信道时，5 GHz 热点总在信道 36 上，与接入点同信道。
- **代码**：`wificx\ap.cpp` `ChooseChannel`、`ChannelLoad`；`mac\ap.c` `rtw_linux_ap_channels`

<a id="hs-12"></a>
### HS-12 站点和组主只能同信道

- **结论**：站点已连接时，组主只能使用站点的信道。这是 rtw89 的组合表 `rtw89_iface_combs` 的限制。
  1. 站点在 5 GHz、热点频段设为 2.4 GHz 时，驱动返回 `BAND_CURRENTLY_NOT_AVAILABLE`。Windows 报告 `BandInterference`。
  2. 热点频段设为"任何可用"时，组主跟随站点的信道。
  3. 热点运行时站点发起连接：若候选中有与组主同信道的 BSS，驱动只使用这些候选。
  4. 没有任何同信道的候选时，驱动先发送 `STOP_AP`，然后连接。
- **依据**：板上实测（AP-1、客户端-1）。同信道运行时，站点吞吐为下行 52.3、上行 62.6 Mbit/s，与只有站点时相当。另有一次驱动成功执行 `TASK_START_AP`，Windows 仍报告 `BandInterference`，原因未确定。
- **影响**：站点已连接时，用户须选择"任何可用"或与站点相同的频段。
- **代码**：`wificx\ap.cpp` `ChooseChannel`、`RtwWifiApBeforeConnect`；`mac\iface.c` `chanctx_share`

<a id="hs-13"></a>
### HS-13 Windows 移动热点服务的行为

- **结论**：Windows 的移动热点服务（`icssvc`）有以下行为：
  1. `TetheringCapability` 只检查上游连接。没有 Wi-Fi Direct 能力的驱动也报告 Enabled。
  2. `IsBandSupported` 来自能力 `GOon5GHzBandSupported`。
  3. 认证方式只有 WPA2-个人：WDI 不支持组主侧的 SAE，`IsAuthenticationKindSupported(Wpa3)` 为 False。
  4. SER 后（驱动发送 `STOP_AP`(115) 和 `CAN_SUSTAIN_AP`(116)，原因 `0xFF000001`），或重启设备后，Windows 不自动重新打开热点。
  5. 约 5 分钟没有热点客户端时，Windows 自动关闭热点。AP-2 所用的笔记本电脑上也是如此。
- **依据**：板上实测（客户端-1）。`icssvc` 判断能否打开热点的完整条件未确定。
- **影响**：SER 或重启设备后，用户须手动重新打开热点。
- **代码**：`wificx\ap.cpp` `IndicateApReason`；`wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-14"></a>
### HS-14 组主侧的分工

- **结论**：组主侧的工作分配如下：
  1. 开放认证：驱动回应。
  2. 关联：Windows 决定是否接受；驱动发送关联响应并等待 ACK。
  3. 四次握手：Windows 通过数据通路完成。
  4. 探测响应：mac 层构造帧（rtw89 在 8852B 上不卸载探测响应），每秒最多 30 个。
  5. 信标：固件按 TBTT 发送。
  6. 热点客户端的省电跟踪、帧缓存和 TIM：8852B 的固件没有 `NOTIFY_AP_INFO`，因此全部由驱动完成。
- **依据**：代码推断（上游 rtw89 的固件功能表）。板上实测：客户端-1 的连接、上网和省电均正常。
- **影响**：移植到具有 `NOTIFY_AP_INFO` 的芯片时，须重新检查省电跟踪的分工。
- **代码**：`mac\ap.c` `ap_rx_auth`、`ap_rx_probe_req`、`linuxkpi_ieee80211_beacon_get_tim`

<a id="hs-15"></a>
### HS-15 驱动自行实现 802.1X 端口控制

- **结论**：Windows 在关联时即打开端口（`EXTAP_PORT_UP`），并给出 `ExcludeUnencrypted=FALSE`。驱动按 mac80211 的做法控制端口：
  1. 安装密钥前，只放行豁免的帧。没有密钥时拒绝发送。
  2. 以太网类型为 `0x888E` 的帧，目的地址只能是 BSSID 或 PAE 组地址（CVE-2020-26139）。
  3. A-MSDU 整帧校验：每个子帧的源地址等于 A2，目的地址不等于 RFC 1042 头（CVE-2020-24588）。
- **依据**：板上实测：`rx_dropped_unencrypted` 为 0。A-MSDU 路径属于代码推断：客户端-1 不发送 A-MSDU。
- **影响**：不实现端口控制时，热点会转发未授权热点客户端的数据帧。
- **代码**：`mac\ap.c` `ap_pae_da_ok`、`ap_amsdu_ok`

<a id="hs-16"></a>
### HS-16 组主数据帧的地址格式

- **结论**：Windows 交给驱动发送的是 FromDS 帧：A1 为目的地址，A2 为 BSSID，A3 为源地址。Windows 接受驱动交来的 ToDS 帧。
- **依据**：板上实测（客户端-1）。
- **影响**：驱动在收发两个方向上均无需改写地址格式。
- **代码**：`mac\ap.c` `rtw_linux_ap_tx_data`、`mac_ap_rx_deliver`

<a id="hs-17"></a>
### HS-17 热点客户端的省电

- **结论**：客户端-1 用 PM 位表示省电，不发送 PS-Poll。驱动的帧缓存、放出和 TIM 均工作正常。每个热点客户端最多缓存 64 帧，10 s 内未放出即丢弃。客户端-1 熄屏时 ping 不通：Android 自行丢弃 ICMP，但对发往未开放端口的 TCP 仍回复 RST。
- **依据**：板上实测：熄屏 5 分钟，缓存 99 帧、放出 99 帧、丢弃 0 帧。PS-Poll 路径（缓存为空时回复 Null）未经测试覆盖。rtw89 是否将 PS-Poll 交给驱动，未确定。
- **影响**：用 ping 判断热点客户端是否在线，结果不可靠。改用 TCP 判断。
- **代码**：`mac\ap.c` `ap_ps_set`、`ap_ps_run`

<a id="hs-18"></a>
### HS-18 TIM 偏移依赖 `sk_buff` 的布局

- **结论**：上游 rtw89 `fw.c` 将 skb 结构体本身当作帧头，读取帧控制字段。因此 `beacon_get_tim` 必须返回新分配的 skb（第一个成员 `next` 为 `NULL`）。
- **依据**：代码推断（上游源码）。板上实测：组主的信标和 TIM 正常。
- **影响**：返回的 skb 不是新分配的时，rtw89 算错帧头长度，TIM 偏移随之出错。
- **代码**：`mac\ap.c` `linuxkpi_ieee80211_beacon_get_tim`；`compat\include\rtw\skbuff.h`

<a id="hs-19"></a>
### HS-19 SER 时组主的处理顺序

- **结论**：热点运行时发生 SER，处理顺序如下：
  1. 驱动先丢弃组主的密钥。
  2. rtw89 恢复芯片，约需 110 ms；兼容层屏蔽 INTx 中断 1 次。
  3. 驱动发送 `DISASSOCIATION`(51，原因 60，即 `WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET`) → `STOP_AP`(115) → `CAN_SUSTAIN_AP`(116)。
  4. Windows 发送广播的 `TASK_DISCONNECT`(8) 和 `TASK_STOP_AP`(15)。

  rtw89 在 SER 时先清空 `rtwvifs_list`，因此停放的组主 `vif` 不影响站点之后进入省电。
- **依据**：板上实测（客户端-1）：无蓝屏；`tx_nokey` 为 3，未发出明文帧。
- **影响**：密钥丢弃后，驱动不得再用该密钥发送帧，否则会发出带 CCMP 头的明文。
- **代码**：`mac\iface.c` `mac_restart_work`；`mac\ap.c` `mac_ap_restart`

<a id="hs-20"></a>
### HS-20 组主的其它观察

- **结论**：
  1. 删除热点客户端时，rtw89 的 flush 每次都超时（约 200 ms），原因未确定。
  2. 热点客户端离开后，Windows 发送 `SET_DELETE_CIPHER_KEYS`(30)。此时驱动已删除密钥，mac 层返回 −2，WiFiCx 前端仍返回成功。
  3. 热点客户端重连时，Windows 不重新安装组密钥。
  4. 组密钥更新时 29 和 31 的先后顺序未确定。
  5. 2.4 GHz 的信标以 CCK 1 Mbit/s 发送，基本速率也为 CCK。客户端-1 能正常连接。
- **依据**：板上实测（客户端-1），只测试过一种热点客户端。
- **影响**：第 2 条的处理方式使 Windows 不报告错误。
- **代码**：`mac\ap.c` `ap_client_remove`

<a id="install"></a>
## 安装、PnP 和注册表

<a id="inst-1"></a>
### INST-1 Net 类安装器的副作用

- **结论**：Net 类的类安装器有两个副作用：
  1. 首次安装 Net 类驱动包时，它将 Wlansvc 的启动类型从"手动"改为"自动"，并启动该服务（System 事件 7040）。卸载后，Wlansvc 仍保持"自动"。
  2. 它按 INF 的 `NumberOfNetworkInterfaces` 在安装时预留网卡。该值只在安装时读取，运行时不读取。改小后重新安装，多余的网卡被删除；改大后重新安装，将新建网卡。
- **依据**：板上实测。"只在安装时读取"来自只读查看 `NetSetupSvc.dll`，属于代码推断。
- **影响**：按微软示例写 11 时，会多出 10 个不存在的 `WLAN n` 网卡。
- **代码**：`driver\rtw89win.inf` `[Rtw89_Ndi_AddReg]`

<a id="inst-2"></a>
### INST-2 先卸载再安装会使 WLAN 配置文件失效

- **结论**：`pnputil /delete-driver … /uninstall /force` 删除 Net 类软件键及其中的 `NetCfgInstanceId`。重新安装时网卡 GUID 改变，按网卡 GUID 保存的 WLAN 配置文件对新网卡不可见。原地升级保持网卡 GUID 不变：
  1. 不先删除旧驱动包，运行 `pnputil /add-driver /install`。新版本直接更新同一设备。
  2. 版本相同或更旧时，`pnputil` 返回 259（已是最新）。此时用 `UpdateDriverForPlugAndPlayDevicesW(INSTALLFLAG_FORCE)` 强制更新。
  3. 确认设备已换用新驱动后，删除旧驱动包（不带 `/uninstall`）。
- **依据**：板上实测。升级和回退两个方向上，网卡 GUID 均保持不变。
- **影响**：先卸载再安装时，连接报告 "There is no profile"，用户须重新输入每个网络的密码。
- **代码**：`tools\install-rtw89win.ps1`

<a id="inst-3"></a>
### INST-3 原地升级后，设备有时要求重启电脑

- **结论**：原地升级后，若设备的 `DEVPKEY_Device_IsRebootRequired` 为 True，`pnputil /restart-device` 失败（退出码 50）。此时只能重启电脑。
- **依据**：板上实测，测过一次。
- **影响**：排障时，先检查该设备属性。
- **代码**：无（Windows 的行为）。

<a id="inst-4"></a>
### INST-4 开机早期注册表配置单元无法写回磁盘

- **结论**：Wi-Fi 驱动在开机后约 22～24.6 s 加载。此时 `ZwFlushKey` 返回 `0xC000014D`（`STATUS_REGISTRY_IO_FAILED`）。约 0.5 s 后可以写回。蓝牙过滤驱动约在 26 s 启动，15 ms 内即可写回。
- **依据**：板上实测（驱动日志）。原因属于代码推断：按需启动的 PCI 驱动在会话管理器初始化注册表之前就已加载。
- **影响**：崩溃循环保护的计数必须在访问芯片之前写回磁盘。因此，驱动在 PrepareHardware 中等待注册表配置单元可写：每 500 ms 尝试一次，最长 60 s。等待超时时，本次启动失败，驱动不访问芯片。
- **代码**：`driver\driver.c` `RtwGuardBegin`、`RtwGuardWait`；`bt\rtkbtflt\driver.c` `FltGuardBegin`

<a id="inst-5"></a>
### INST-5 关机时须使用关机通知，不能依赖 D0Exit

- **结论**：系统关机时，驱动须依靠关机通知结束本次运行：
  1. 系统关机时，Windows 不调用 `ReleaseHardware`。
  2. D0Exit（目标状态 `WdfPowerDeviceD3Final`）晚于注册表配置单元最后一次写回。此时写入的注册表值无法保存到磁盘。
  3. Wi-Fi 驱动在诊断接口的设备对象上注册关机通知（`WdfControlDeviceInitSetShutdownNotification`，即 `IRP_MJ_SHUTDOWN`）。
  4. 蓝牙过滤驱动在一个单独的设备对象上注册关机通知。在过滤设备对象上调用 `IoRegisterShutdownNotification` 不生效。
- **依据**：板上实测：重启电脑后，驱动日志显示上次运行"ended in order (shutdown)"。过滤设备对象上的注册为何不生效，未确定。
- **影响**：只依靠 D0Exit 时，每次正常关机都被计为一次"未正常结束"。两次后，崩溃循环保护触发。
- **代码**：`driver\driver.c` `RtwCdoCreate`、`RtwEvtShutdown`、`RtwGuardEnd`；`bt\rtkbtflt\driver.c` `FltCdoCreate`、`FltEvtShutdown`

<a id="inst-6"></a>
### INST-6 不得使设备重新枚举

- **结论**：重新枚举后，RTL8852BE 在断电前无法下载固件（驱动日志 `[ERR]FWDL path ready`，WCPU 停在 ROM）。
  1. 会触发重新枚举的操作：驱动调用 `WdfDeviceSetFailed(WdfDeviceFailedAttemptRestart)`。PnP 重新枚举设备，并重新调用 AddDevice。即使芯片在此之前未被驱动访问过，结果也相同。
  2. 不会触发重新枚举的操作：`pnputil /restart-device`；`pnputil /remove-device` 加 `/scan-devices`；卸载脚本；设备管理器中的禁用再启用。
  3. 出现该故障后，`pnputil /restart-device` 无法恢复。只有重启电脑（芯片断电）才能恢复。
- **依据**：板上实测。原因属于代码推断：PCIe DevCap 中 FLR = 1，重新枚举时 Windows 对设备执行了 FLR。
- **影响**：驱动的恢复代码不得使用 `WdfDeviceFailedAttemptRestart`。因此，崩溃循环保护不能在同一次启动中切换到诊断模式。本驱动和本项目的脚本均不触发重新枚举。
- **代码**：`driver\hw.c` `LogPciConfig`（记录 DevCap）

<a id="inst-7"></a>
### INST-7 内存中的驱动日志随驱动卸载而丢失

- **结论**：设备停止后，驱动映像被卸载，内存中的驱动日志随之消失。因此，驱动在释放硬件、关机和启动失败时，将日志的最后 16 KB 存入设置项 `SavedLog`。`rtwdiag.ps1 log -Saved` 读取保存的日志。
- **依据**：板上实测。
- **影响**：没有保存的日志时，无法查明启动失败或断开的原因。
- **代码**：`driver\driver.c` `RtwSaveLog`

<a id="inst-8"></a>
### INST-8 新网络缺省为"公用"，防火墙丢弃 ICMP 回显

- **结论**：两端的新网络都被 Windows 归为"公用"。Windows 防火墙丢弃 ICMP 回显请求。`netstat -s -p icmp` 显示对端收到了回显请求，但未应答。
- **依据**：板上实测（AP-2）。
- **影响**：ping 不通不表示驱动有问题。改用 TCP 连接检查连通性。
- **代码**：无（Windows 的行为）。

<a id="inst-9"></a>
### INST-9 WLAN-AutoConfig 事件中的加密方式写法不一致

- **结论**：同一次 WPA2-个人连接，事件 11000 和 11010 写作 "AES-GCMP-128"，事件 8001 写作 "AES-CCMP"。实际使用的是 CCMP，连接不受影响。
- **依据**：板上实测（AP-2）。
- **影响**：排障时，不应据此怀疑加密协商有误。
- **代码**：无（Windows 的行为）。

<a id="rtw89"></a>
## 把 rtw89 接到 Windows

<a id="rtw-1"></a>
### RTW-1 rtw89 停止时遗留一个 C2H skb

- **结论**：rtw89 停止时遗留一个 C2H skb。兼容层在 remove 之后将其回收：
  1. rtw89 启动期间持有 wiphy 锁时，到达的 C2H 帧进入 `c2h_queue`。
  2. 随后进入 IPS 时，`rtw89_core_stop` 取消 C2H 工作项。上游没有释放该队列的代码，在 Linux 上同样会遗留。
  3. 该 skb 的数据位于 DMA 堆中，因此 remove 时 DMA 堆无法归还。
  4. 兼容层在 remove 之后回收所有仍存在的 skb。
- **依据**：板上实测（驱动日志 `rtw89: freed N skbs it left behind`）。
- **影响**：不回收时，此后每次 probe 都返回 −19，须重启设备才能恢复。
- **代码**：`compat\skbuff.c` `rtw_skb_reclaim`

<a id="rtw-2"></a>
### RTW-2 skb 的 `cb` 归 rtw89 使用

- **结论**：rtw89 将 `info->driver_data` 当作自己的 `rtw89_tx_skb_data`（与 `control` 位于同一个联合中）。发送状态回调时，它又清除从 `status` 开始的后半部分。mac 层不能将自己的记账信息放在 `cb` 中。兼容层在 `struct sk_buff` 中添加了 rtw89 不访问的字段 `mac_tag`。
- **依据**：代码推断（上游源码）。
- **影响**：记账信息被覆盖时，发送状态无法与帧对应。
- **代码**：`compat\include\rtw\skbuff.h`（`mac_tag`）；`mac\tx.c` `mac_tx_mgmt`、`linuxkpi_ieee80211_tx_status`

<a id="rtw-3"></a>
### RTW-3 发送状态回调中不得再发送帧

- **结论**：PCI 的发送完成在 DISPATCH_LEVEL、持有 `rtwpci->trx_lock` 时调用 `ieee80211_tx_status_ni` 和 `free_txskb`。回调中只能记账和排入工作项。回调中不得调用 `ops->tx`，也不得直接调用 NetAdapterCx：NetAdapterCx 会同步回调 `Advance`，进而调用 `ops->tx`。mac 层通过"设置标志、排入 KDPC"通知 WiFiCx 前端。
- **依据**：代码推断（上游源码的锁顺序）。
- **影响**：在回调中发送帧时，`trx_lock` 自锁，系统挂起。
- **代码**：`mac\tx.c` `linuxkpi_ieee80211_tx_status`、`linuxkpi_ieee80211_free_txskb`；`wificx\adapter.cpp` `DpTxReady`、`Kick`、`DpDpc`

<a id="rtw-4"></a>
### RTW-4 RCU 须真正等待读者

- **结论**：兼容层的 RCU 使用读者计数，真正等待读者退出：
  1. rtw89 在 NAPI 轮询中有 RCU 读者（PN 检查、`sta` 链路）。
  2. 兼容层的 NAPI 运行在工作队列线程上（PASSIVE_LEVEL），不在 DPC 中。因此，`synchronize_rcu` 只等待 DPC 是不够的。
  3. `synchronize_rcu` 等待读者计数归零。
  4. `call_rcu` 和 `kfree_rcu` 一律挂入链表，由工作项先同步、后释放。原因是调用者本身就在读者区内。
- **依据**：代码推断。板上实测：反复连接和断开均正常。
- **影响**：只等待 DPC 时，断开或删除 `sta` 会造成释放后使用。
- **代码**：`compat\rcu.c` `synchronize_rcu`；`mac\mac.h` `mac_read_lock`

<a id="rtw-5"></a>
### RTW-5 数据帧直接调用 `ops->tx`，不经过 txq

- **结论**：mac 层为 Windows 交来的帧补上 QoS 头和各 TID 的序号，然后直接调用 `ops->tx`。mac80211 对没有 txq 的帧也采用同样的做法。
  1. rtw89 的 txq 路径（`wake_tx_queue` → `rtw89_core_txq_work`）需要完整的 `ieee80211_txq` 调度，还会自行发起块确认会话。本驱动不使用该路径。
  2. 块确认会话的发起，以及"EAPOL 不参与 A-MPDU 聚合"，由 mac 层自行实现。
  3. 在途帧上限为 64。mac 层配合 rtw89 的停止和唤醒队列实现流控。
- **依据**：板上实测（AP-1，A-MPDU 聚合工作正常）。
- **影响**：使用 txq 路径须实现大量 mac80211 的调度逻辑。
- **代码**：`mac\tx.c` `rtw_linux_tx_data`；`mac\agg.c` `mac_agg_tx_check`

<a id="rtw-6"></a>
### RTW-6 发送管理帧前须有信道上下文

- **结论**：发送入口须先检查信道上下文。rtw89 发送管理帧时读取 `rtwvif_link->chanctx_idx`，而芯片处于 IPS（下电）时没有信道上下文。
  1. 管理帧的序号由硬件填写。
  2. 速率由 rtw89 决定：2.4 GHz 使用 CCK 1 Mbit/s，5 GHz 使用 OFDM 6 Mbit/s。有基本速率时，取最低的基本速率。
- **依据**：代码推断（上游源码）。
- **影响**：不检查时，驱动会向已下电的 MAC 写入 DMA 环。
- **代码**：`mac\tx.c` `mac_tx_mgmt`

<a id="rtw-7"></a>
### RTW-7 8852B 数据帧的加密分工

- **结论**：8852B 的 `hw_sec_hdr` 为假：
  1. 发送：驱动写入 CCMP 头和 PN（rtw89 为密钥设置 `GENERATE_IV`）。硬件加密并添加 MIC。
  2. 接收：硬件解密，但 CCMP 头和 8 字节 MIC 保留在帧中，硬件不检查 PN（只设置 `RX_FLAG_DECRYPTED`）。驱动去除头尾，并按 TID 严格检查 PN（17 个计数槽，其中非 QoS 帧占一个）。PN 检查位于 A-MPDU 重排序之后。
  3. 密钥：rtw89 保存 `key_conf` 指针，因此密钥结构须保留到 `DISABLE_KEY` 之后。同一 ID 的两把密钥不能同时存在，须先删除、后安装。同一把密钥重新安装时不得重置 PN（参照 mac80211 的 `ieee80211_key_identical`）。
- **依据**：板上实测（AP-1、AP-2）：约 23 万帧中，解密失败、PN 重放和缺少密钥的计数均为 0。同一把密钥重新安装的路径未经测试覆盖，属于代码推断。
- **影响**：不检查 PN 等同于没有重放保护。重新安装时重置 PN 会导致 nonce 重用。
- **代码**：`mac\key.c` `mac_key_ccmp_hdr`、`mac_key_rx`、`key_identical`、`rtw_linux_add_key`；`mac\rx.c` `mac_rx_deliver`

<a id="rtw-8"></a>
### RTW-8 SER 时 rtw89 不通知驱动即丢弃密钥

- **结论**：rtw89 的 SER L2 在 wiphy 锁下用 `ieee80211_iter_keys` 丢弃密钥，不通知固件，也不经过驱动的 `set_key`。mac 层须完成三项工作：
  1. 将密钥标记为丢失，此后不再用该密钥发送帧。
  2. 实现 `ieee80211_restart_hw`：上报掉线；将旧的 `vif` 和信道上下文暂存至 hw 释放时（rtw89 仍持有它们的指针）；重新 start；调用 `reconfig_complete(RESTART)`。
  3. 恢复后由 Wlansvc 重连（[CONN-7](#conn-7)）。
- **依据**：板上实测（AP-1）。恢复约需 110 ms，Wlansvc 立即重连。
- **影响**：不标记密钥时，驱动会发出"带 CCMP 头的明文"。不实现重启时，固件出错后设备停止工作，须手动重启设备。
- **代码**：`mac\key.c` `linuxkpi_ieee80211_iterate_keys`；`mac\hw.c` `linuxkpi_ieee80211_restart_hw`；`mac\iface.c` `mac_restart_work`；`mac\mlme.c` `mac_mlme_restart`

<a id="rtw-9"></a>
### RTW-9 SER 期间的 INTx 中断风暴

- **结论**：只有 INTx 中断时，SER 期间会出现中断风暴。兼容层须代替 rtw89 屏蔽芯片中断。
  1. 现象：在 SER L2 的 `rtw89_core_stop` 之后、重新启动的 `rtw89_hci_start` 之前，芯片持续拉起中断线。CPU0 上的 DPC 无法运行。
  2. 原因：在此期间，MAC 初始化已将中断屏蔽寄存器恢复为运行时的值，RXDMA 中断处于挂起状态。rtw89 的中断处理函数因 `running` 为假，只返回 `IRQ_HANDLED`，不屏蔽中断。INTx 是电平中断，因此中断持续存在。
  3. 兼容层的处理：中断处理函数返回 `IRQ_HANDLED` 后，兼容层检查并屏蔽芯片中断。然后再次查询 rtw89 的中断处理函数，避免覆盖另一个 CPU 上同时进行的 start。
- **依据**：板上实测：两次蓝屏 0x133（`DPC_QUEUE_EXECUTION_TIMEOUT_EXCEEDED`）的转储分析；加入屏蔽后不再出现风暴，每次 SER 恰好屏蔽一次。
- **影响**：Linux 上使用 MSI 时没有此问题。在只有 INTx 中断的平台上，不屏蔽时一次 SER 即导致蓝屏。
- **代码**：`compat\device.c` `rtw_linux_isr`、`intx_stopped`、`rtw_linux_irq_dpc`

<a id="rtw-10"></a>
### RTW-10 站点模式下 BAR 不交给驱动

- **结论**：A-MPDU 接收时，接入点的 BlockAckReq（BAR）在站点模式下不交给驱动（`rx_bar` 始终为 0）。
  1. 重排序窗口依靠 100 ms 超时推进。每次 48 MiB 传输只出现 1～2 次超时。
  2. `rx_dropped_old` 约占接收帧的 2～4%，为接入点重传的重复帧。
  3. 驱动拒绝 ADDBA 时，接入点在有流量时约每 2.5 s 重发一次 ADDBA 请求。
- **依据**：板上实测（AP-1、AP-2）。"不交给驱动"的原因属于代码推断：芯片的接收过滤。
- **影响**：`rx_bar` 为 0 或存在少量 `rx_dropped_old` 不属于故障。
- **代码**：`mac\agg.c` `mac_agg_rx_frame`、`ba_rx_addba`；`mac\rx.c` `linuxkpi_ieee80211_rx`

<a id="rtw-11"></a>
### RTW-11 rtw89 v7.2 没有 `mgd_prepare_tx`；`sta` 上传前不得被遍历

- **结论**：rtw89 v7.2 没有 `mgd_prepare_tx` 和 `mgd_complete_tx`。mac 层的连接流程按 mac80211 的顺序调用 ops 即可。`sta` 先挂入链表、然后调用 `sta_state(NOTEXIST → NONE)` 时，遍历函数须跳过仍处于 NOTEXIST 的 `sta`（mac80211 使用 `uploaded` 标志）。
- **依据**：代码推断（上游源码）。
- **影响**：不跳过时，rtw89 会取得 `drv_priv` 全为零的 `sta`，造成空指针访问或错误的配置。
- **代码**：`mac\iface.c` `sta_uploaded`、`__ieee80211_iterate_stations`；`mac\mlme.c`

<a id="rtw-12"></a>
### RTW-12 共存在 Wi-Fi 侧自动工作

- **结论**：rtw89 的共存逻辑（btc）全部位于 Wi-Fi 侧。
  1. mac 层按 mac80211 的顺序调用 ops，发送时填写 `skb->protocol` 以及 IP、UDP 头的偏移。这样，共存的各项通知都会发生。
  2. btc 的通知计数在每次 `rtw89_core_start`（离开 IPS）时清零。
  3. 蓝牙控制器只运行 ROM 固件时，蓝牙侧的共存版本为 0（不匹配），也不上报蓝牙信息。btc 只能按记分板使用 `BT_IDLE` 策略。
- **依据**：板上实测（`rtwdiag.ps1 btc` 的计数和报告）。有蓝牙负载时的共存效果未测试。
- **影响**：驱动无需单独实现共存。共存效果见 [BT-9](#bt-9)。
- **代码**：`mac\tx.c` `tx_set_protocol`；`compat\rtw89info.c`

<a id="platform"></a>
## ARM64 和 RK3588 平台

<a id="plat-1"></a>
### PLAT-1 只有 INTx 中断，全部由 CPU0 处理

- **结论**：ROCK 5T 上 RK3588 的 PCIe 没有 MSI，只能使用 INTx 中断。全部 INTx 中断均由 CPU0 处理。CPU0 的中断基线约为每秒 740 次。驱动在 WiFiCx 模式下空闲时，CPU0 约为每秒 784 次中断。
- **依据**：板上实测。
- **影响**：中断处理缓慢或出现中断风暴时，CPU0 的 DPC 被延误，系统蓝屏 0x133（见 [RTW-9](#rtw-9)）。
- **代码**：`driver\os.c` `RtwOsAttach`、`Isr`；`compat\device.c` `rtw_linux_isr`

<a id="plat-2"></a>
### PLAT-2 DMA 不做缓存一致

- **结论**：RK3588 的 PCIe 不做缓存一致（ACPI `_CCA` 为 0）。HAL 提供的公共缓冲为非缓存映射。
  1. `AllocateCommonBufferWithBounds` 不接受 `MmWriteCombined`。使用缺省缓存类型即可。
  2. 在非缓存的 Normal 内存上，非对齐访问不会出错。
  3. 逻辑地址在 4 GB 以下。
- **依据**：板上实测。
- **影响**：照搬 x64 上的写合并分配时，probe 返回 −12。
- **代码**：`driver\os.c` `AllocCommon`

<a id="plat-3"></a>
### PLAT-3 PCIe 链路和 BAR

- **结论**：ROCK 5T 上的 PCIe 链路和 BAR 如下：
  1. 链路为 Gen1 x1。ASPM、CLKREQ 和 L1SS 均关闭。
  2. BAR2 为 1 MB 内存 BAR；BAR0 为未分配的 I/O BAR。
  3. 芯片下电时，读取 MAC 区返回 `0xEAEAEAEA`，不会卡死。
  4. DevCap 中 FLR = 1（与 [INST-6](#inst-6) 有关）。
- **依据**：板上实测（驱动日志记录的 PCI 配置）。
- **影响**：寄存器读出 `0xEAEAEAEA` 时，先确认芯片是否处于 IPS。
- **代码**：`driver\hw.c` `RtwHwMap`、`LogPciConfig`

<a id="plat-4"></a>
### PLAT-4 UEFI 须先为 M.2 槽上电

- **结论**：为使 Windows 发现网卡，UEFI 须先为 M.2 E-key 槽上电，并将 W_DISABLE1# 置为高电平。ROCK 5T 上，此步骤由本项目之外的 UEFI 移植完成。模块位于 PCIe 段 2 上。
- **依据**：板上实测。UEFI 侧的实现细节不在本项目范围内。
- **影响**：移植到其它 RK3588 设备时，UEFI 和 ACPI 侧须先完成此步骤。
- **代码**：不在本仓库中。

<a id="plat-5"></a>
### PLAT-5 芯片和固件

- **结论**：ROCK 5T 上的芯片和固件如下：
  1. 芯片版本 1（CBV），RFE 5，2T2R。
  2. efuse 中没有国家码。efuse 登记了 2.4 GHz 的 14 个信道和 5 GHz 的 28 个信道；全球通用规则下启用其中 25 个 5 GHz 信道。
  3. 固件 `rtw8852b_fw-2.bin` 的版本为 0.29.29.18，经 DMA H2C 下载约需 330 ms。固件接受 CPU 异常触发（用于 SER 测试）。
  4. rtw89 启动约需 110 ms。
- **依据**：板上实测。只使用过 0.29.29.18 版固件。其它版本的省电和 SER 触发能力未确定。
- **影响**：更换固件版本时，须重新验证省电和 SER。
- **代码**：`driver\rtw89win.inf` `[Rtw89_CopyFiles]`

<a id="bluetooth"></a>
## 蓝牙补丁

<a id="bt-1"></a>
### BT-1 不打补丁时蓝牙不可用

- **结论**：模块的蓝牙部分通过 USB 连接（ROCK 5T 上的模块为 `USB\VID_13D3&PID_3572`）。Windows 自带的蓝牙栈（`bth.inf`、`BTHUSB`）能识别该设备，但不下载 Realtek 的蓝牙补丁。控制器只运行 ROM 固件（`lmpSubversion` 为 `0x8852`），扫描不到设备，配对失败。
- **依据**：板上实测：对照实验中，ROM 固件下无论 Wi-Fi 已连接还是已禁用，均扫描不到设备。打补丁后，两种情况均能扫描到。因此原因是缺少补丁，而非共存。系统中没有任何 HCI 错误。
- **影响**：使用蓝牙必须安装蓝牙过滤驱动。
- **代码**：`bt\rtkbtflt\`

<a id="bt-2"></a>
### BT-2 厂商的 Windows 蓝牙软件没有 ARM64 版

- **结论**：在 Microsoft Update Catalog 中，该蓝牙控制器的厂商软件只有 AMD64 版。
- **依据**：未确定。只查阅了 Microsoft Update Catalog，未查阅其它来源。
- **影响**：ARM64 上需要本项目的蓝牙过滤驱动。
- **代码**：无。

<a id="bt-3"></a>
### BT-3 用户态经 `bthport` 能下载补丁，但蓝牙栈随后无法工作

- **结论**：用户态程序能通过 `bthport` 的 IOCTL 下载补丁，但补丁生效后蓝牙栈无法工作：
  1. 该 IOCTL 发送厂商 HCI 命令：头为 9 字节，没有匹配模式，输出为原始事件。它只放行 OGF `0x3F`（`0x1001` 被拒绝，`0xFC20` 被放行）。
  2. 以此方式下载 231 段补丁约需 0.52 s。
  3. 补丁生效后，`bthport` 已设置的 SSP 和事件掩码失效：配对退化为 PIN 方式并失败，BLE 扫描结果为 0。
  4. 能使 `bthport` 重新初始化的方法都经过 D3，而 D3 会丢失补丁（[BT-4](#bt-4)）。
- **依据**：板上实测。
- **影响**：补丁必须在 `BTHUSB` 初始化控制器之前下载。因此本项目使用下层过滤驱动。用户态工具 `rtkbtfw` 只用于诊断。
- **代码**：`bt\rtkbtfw\rtkbtfw.c` `CmdLoad`、`HciCmd`

<a id="bt-4"></a>
### BT-4 D2 不丢失补丁，D3 会丢失补丁

- **结论**：`BTHUSB` 空闲约 1 s 后使控制器进入 D2（选择性挂起），补丁保留。关闭再打开蓝牙、重启设备都经过 D3，补丁丢失。USB 未重新枚举。禁用 Wi-Fi 网卡不影响补丁。
- **依据**：板上实测。睡眠未测试。
- **影响**：蓝牙过滤驱动在每次 D3 → D0 时重新下载补丁。
- **代码**：`bt\rtkbtflt\driver.c` `FltEvtD0Entry`

<a id="bt-5"></a>
### BT-5 蓝牙过滤驱动下载补丁的时机

- **结论**：蓝牙过滤驱动在 START 和 D0 的 IRP 向上返回的途中被调用。此时 `BTHUSB` 尚未收到该 IRP。
  1. 首次启动时，过滤驱动自行选择 USB 配置并下载补丁，不撤销配置。`BTHUSB` 之后再次选择配置，没有副作用。
  2. D3 → D0 时，过滤驱动使用 `BTHUSB` 记录的管道。
  3. D0Entry 始终返回成功。下载失败时，过滤驱动转为直通。
- **依据**：板上实测。截获 `BTHUSB` 读请求的路径未经测试覆盖（挂起的读请求始终为 0 个），属于代码推断。
- **影响**：补丁下载失败时，蓝牙仍以 ROM 固件运行，系统不受影响。
- **代码**：`bt\rtkbtflt\driver.c` `FltEvtD0Entry`；`bt\rtkbtflt\hci.c` `FltSession`、`FltSelectOwnConfig`；`bt\rtkbtflt\urb.c` `FltEvtWdmInternalIoctl`

<a id="bt-6"></a>
### BT-6 判断补丁是否生效不能依据 `bthport` 的缓存

- **结论**：`bthport` 缓存的版本信息不随补丁更新。判断补丁是否生效，须使用以下任一方法：
  1. Wi-Fi 侧的 `rtwdiag.ps1 btc`（ROM 为 `0xff0b8852`，打补丁后为 `0x1fc6b20f`）。
  2. 对端看到的版本。
  3. 实际功能。

  `bthport` 重新初始化后读到 `0xdbc6`/`0xb20f`。
- **依据**：板上实测。
- **影响**：只看 `bthport` 的缓存会误判补丁状态。
- **代码**：`bt\rtkbtfw\rtkbtfw.c` `CmdInfo`；`compat\rtw89info.c`

<a id="bt-7"></a>
### BT-7 用扩展 INF 和 `AddFilter` 安装过滤驱动

- **结论**：蓝牙过滤驱动通过扩展 INF 的 `AddFilter` 安装，基础驱动仍为 `bth.inf`。
  1. 删除驱动包后，设备恢复为 `BTHUSB` 和 `usbhub`，配对记录保留。
  2. 同一个 ExtensionId 的新驱动包可以原地升级。
  3. 不使用完整 INF，因为测试签名的完整 INF 排名低于 Windows 自带的驱动。
- **依据**：板上实测。
- **影响**：卸载蓝牙过滤驱动不会删除配对记录。
- **代码**：`bt\rtkbtflt\rtkbtflt.inf` `[RtkBtFlt_Device.NT.Filters]`

<a id="bt-8"></a>
### BT-8 蓝牙补丁文件和下载协议

- **结论**：测试过的补丁文件和下载协议如下：
  1. 补丁文件的版本为 `0xdbc6b20f`，project id 为 20。ROM 版本 1 对应 chip id 2 的补丁。
  2. 补丁和配置合计 57 997 + 6 = 58 003 字节，分 231 段下载。
  3. 段序号从 0 到 `0x7f`，然后回到 1。最后一段的序号与 `0x80` 按位或。key id 为 0。
  4. 补丁只在最后一段下载后生效。驱动不发送 `0xFC66`。
- **依据**：板上实测；代码推断（`epatch.c` 的 723 项主机测试）。
- **影响**：本项目只按容器格式读取补丁文件，不修改补丁内容。
- **代码**：`bt\rtkbtfw\epatch.c` `rtkbt_epatch_parse`、`rtkbt_frag_get`；`bt\rtkbtflt\hci.c` `FltCheckAndDownload`

<a id="bt-9"></a>
### BT-9 Wi-Fi 和蓝牙的共存

- **结论**：模块的 Wi-Fi 和蓝牙共用天线（RFE 5）。Wi-Fi 在 5 GHz 时，rtw89 使用 `WL_5G` 策略。BLE 扫描时经 Wi-Fi 传输 32 MiB，数据一致。
- **依据**：板上实测，测过一次。BLE 扫描时的吞吐未测得（测量脚本未计时）。OPP 文件传输未测试。
- **影响**：有蓝牙负载时 Wi-Fi 的吞吐变化未确定。
- **代码**：`compat\rtw89info.c`（共存诊断）

<a id="build"></a>
## 构建

<a id="build-1"></a>
### BUILD-1 上游 rtw89 用 clang 的 MinGW 目标原样编译

- **结论**：上游 rtw89 的源码不修改任何一行，使用 clang 的 MinGW 目标（`aarch64-w64-mingw32`）加 `-mno-ms-bitfields` 编译。
  1. clang-cl（MSVC 目标）能编译通过，但位域布局与 GCC 不同。
  2. 检查栈帧时，为 `buildhal.ps1` 添加 `-ExtraFlags '-Wframe-larger-than=1024'`（内核栈较小）。该选项不在缺省选项中。
  3. `mac\util.c` 与上游 `util.c` 同名。构建脚本按目录为目标文件添加前缀。
- **依据**：代码推断，并经编译验证。
- **影响**：位域布局不正确时，rtw89 读写的结构将错位。
- **代码**：`buildhal.ps1`

<a id="build-2"></a>
### BUILD-2 WiFiCx TLV 库的链接要求

- **结论**：`WificxTLVGenParse.lib` 有以下要求：
  1. 驱动须提供 `operator new(size_t, ULONG_PTR)` 和 `operator delete(void*)`。
  2. 它引用 `__security_push_cookie` 和 `__security_pop_cookie`，这两个符号只在 `libcntpr.lib` 中。
  3. C++ 文件须用 `/Zc:sizedDealloc-` 编译，否则 `arrayofelements.hpp` 的数组 placement new 将报告 C2956。
  4. 共享头须包在 `EXTERN_C_START` 和 `EXTERN_C_END` 之间。

  版本：WiFiCx 1.1、NetAdapterCx 2.4、KMDF 1.33。版本宏为 `NET_VERSION_*`，不是 `NETADAPTER_VERSION_*`。
- **依据**：代码推断，并经编译验证。
- **影响**：缺少任何一条时，链接或编译失败。
- **代码**：`wificx\alloc.cpp`；`driver\rtw89win.vcxproj`

<a id="build-3"></a>
### BUILD-3 Wi-Fi 驱动的导入表

- **结论**：Wi-Fi 驱动只导入 `ntoskrnl.exe`、`HAL.dll`、`WDFLDR.SYS` 和 `ksecdd.sys`（CNG，见 [SEC-5](#sec-5)）。NetAdapterCx 和 WiFiCx 通过 WdfLdr 的类扩展机制绑定，不在导入表中。
- **依据**：代码推断，并经编译验证（检查导入表）。
- **影响**：导入表中出现其它模块时，须检查是否使用了不应使用的内核接口。
- **代码**：`driver\rtw89win.vcxproj`

<a id="open-questions"></a>
## 未确定的问题

| 问题 | 所属结论 | 缺少的证据 |
|---|---|---|
| `PeerVersion` 填写错误时的后果 | [INIT-1](#init-1) | 没有反例 |
| 组密钥 RSC 的字节序 | [CONN-1](#conn-1) | Windows 只给出过 0 |
| Windows 读取 `SAEConfirmFrame` 还是 `SAECommitFrame` | [SEC-2](#sec-2) | 未测试只填写一个字段的情况 |
| 电源设置回调给出的 16 字节数据的格式 | [PS-2](#ps-2) | 未找到文档 |
| `ChannelWidthList` 的用途 | [WIDE-3](#wide-3) | Windows 的输出没有变化 |
| 先开启热点再连接站点时，站点是否只能使用 HT20 | [WIDE-10](#wide-10) | 未在目标设备上测试 |
| `Bandwidth=40` 时下行吞吐偏低的原因 | [WIDE-11](#wide-11) | 未分析 |
| `ScanSSIDListSize` 为何须为 4；3 是否可行 | [HS-2](#hs-2) | 3 未测试 |
| 发送分流是否丢弃组播帧 | [HS-8](#hs-8) | 只有 WiFiCx 内部符号作为依据 |
| 驱动成功执行 `TASK_START_AP` 而 Windows 仍报告 `BandInterference` 的原因 | [HS-12](#hs-12) | 只出现过一次 |
| 移动热点服务判断能否打开热点的完整条件 | [HS-13](#hs-13) | 没有文档 |
| rtw89 是否将 PS-Poll 交给驱动 | [HS-17](#hs-17) | 客户端-1 不发送 PS-Poll |
| 删除热点客户端时 flush 超时的原因；组密钥更新时 29 和 31 的先后顺序 | [HS-20](#hs-20) | 未分析；未观察到组密钥更新 |
| 重新枚举后无法下载固件的原因是否为 FLR | [INST-6](#inst-6) | 未直接观察到 FLR |
| 过滤设备对象上的 `IoRegisterShutdownNotification` 为何不生效 | [INST-5](#inst-5) | 未分析 |
| 其它固件版本的省电和 SER 触发能力 | [PLAT-5](#plat-5) | 只使用过 0.29.29.18 |
| 有蓝牙负载时 Wi-Fi 的吞吐 | [BT-9](#bt-9)、[RTW-12](#rtw-12) | 没有已配对的耳机类设备；脚本未计时 |
| 睡眠后蓝牙补丁是否保留 | [BT-4](#bt-4) | 睡眠未测试 |
| Microsoft Update Catalog 之外是否有 ARM64 版厂商蓝牙软件 | [BT-2](#bt-2) | 只查阅了一个来源 |
