# 术语表

[English](../en/glossary.md)

- **适用读者**：所有读者。
- **本文内容**：文档中每个术语的含义和对应的英文。
- **前提**：无。

<a id="summary"></a>
## 要点

- 全部中文文档只使用本表的中文词。同一个概念只使用一个词。
- 一篇文档首次使用某个术语时，在括号中注明英文或缩写，例如"组主（GO）"。此后只写中文词或本表给出的缩写。
- 代码、命令、WDI 消息名和注册表值名保留原文，并放在反引号中。
- 每个术语都有锚点。其它文档可链接到术语，例如 `glossary.md#group-owner`。
- 术语按主题分为五组：设备、安装和签名；Wi-Fi 概念；Windows 驱动框架；本项目的组成；可信度。

<a id="devices"></a>
## 设备、安装和签名

| 中文 | 英文 | 定义 |
|---|---|---|
| <a id="target-device"></a>目标设备 | target device | 运行 Windows 11 ARM64、装有 RTL8852BE 模块、需安装本驱动的设备。测试使用的是 ROCK 5T。 |
| <a id="build-pc"></a>构建电脑 | build PC | 运行构建脚本、生成驱动包的 x64 Windows 电脑。 |
| <a id="measuring-pc"></a>测量电脑 | measuring PC | 运行 `hosttools\` 脚本的 Windows 电脑：与目标设备位于同一网络，能通过 ssh 登录目标设备。可以是构建电脑。 |
| <a id="module"></a>模块 | module | 插在 M.2 E-key 槽上的 RTL8852BE 硬件，包含 Wi-Fi（PCIe）和蓝牙（USB）两部分。 |
| <a id="network-adapter"></a>网卡 | network adapter | Windows 中显示为"WLAN"的网络适配器。 |
| <a id="device"></a>设备 | device | 设备管理器中的 PnP 设备节点。单独写"设备"时，指 RTL8852BE 的 PCI 设备（模块的 Wi-Fi 部分）。 |
| <a id="bluetooth-usb-device"></a>蓝牙 USB 设备 | Bluetooth USB device | 模块的蓝牙部分在 Windows 中对应的 USB 设备。ROCK 5T 上的模块为 `USB\VID_13D3&PID_3572`。 |
| <a id="driver-package"></a>驱动包 | driver package | 构建脚本输出的一个目录，包含驱动文件、INF、目录文件（`.cat`）、固件、许可文件、安装脚本和 `SHA256SUMS`。 |
| <a id="wi-fi-driver"></a>Wi-Fi 驱动 | Wi-Fi driver | `rtw89win.sys`，本项目的 Wi-Fi 驱动。 |
| <a id="bluetooth-filter-driver"></a>蓝牙过滤驱动 | Bluetooth filter driver | `rtkbtflt.sys`：安装在 Windows 蓝牙 USB 驱动之下、负责下载蓝牙补丁的下层过滤驱动。 |
| <a id="lower-filter-driver"></a>下层过滤驱动 | lower filter driver | 位于设备的功能驱动之下、先于功能驱动处理该设备 I/O 的 KMDF 驱动。 |
| <a id="in-place-upgrade"></a>原地升级 | in-place upgrade | 不先卸载，直接用新驱动包更新同一设备。网卡 GUID 和 WLAN 配置文件保持不变。 |
| <a id="wlan-profile"></a>WLAN 配置文件 | WLAN profile | Windows 为一个网络保存的设置（名称、安全类型、密码），按网卡 GUID 存放。 |
| <a id="interface-guid"></a>网卡 GUID | interface GUID | Windows 为网卡分配的标识（`NetCfgInstanceId`）。WLAN 配置文件按该标识保存。 |
| <a id="test-signing"></a>测试签名 | test signing | 用自行创建的测试证书为驱动签名。Windows 只在测试签名模式下加载此类驱动。 |
| <a id="test-signing-mode"></a>测试签名模式 | test-signing mode | 通过 `bcdedit /set testsigning on` 打开的 Windows 启动选项。打开前须关闭安全启动。 |
| <a id="secure-boot"></a>安全启动 | Secure Boot | UEFI 功能：只启动具有受信任签名的系统组件。 |
| <a id="test-certificate"></a>测试证书 | test certificate | 构建电脑上用于测试签名的代码签名证书。公钥文件 `rtw89win-test.cer` 随驱动包提供。 |
| <a id="setting"></a>设置项 | setting | 驱动读取的注册表值。Wi-Fi 驱动的设置项位于 `HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters` 下，例如 `Country`；蓝牙过滤驱动的设置项位于 `Services\rtkbtflt\Parameters` 下，例如 `PassThrough`。其中 `ArmRunning`、`SavedLog` 等值由驱动自行写入。 |
| <a id="restart-the-device"></a>重启设备 | restart the device | `pnputil /restart-device`：停止并重新启动 RTL8852BE 的 PCI 设备，不重启电脑。 |
| <a id="restart-the-computer"></a>重启电脑 | restart the computer | 重新启动目标设备上的 Windows。芯片会断电。 |
| <a id="re-enumeration"></a>重新枚举 | re-enumeration | 驱动以 `WdfDeviceFailedAttemptRestart` 报告设备失败后，总线驱动重新枚举设备，并重新调用驱动的 AddDevice。重新枚举使 RTL8852BE 在下次断电前无法下载固件。本驱动和本项目的脚本不触发重新枚举。 |
| <a id="registry-hive"></a>注册表配置单元 | registry hive | 注册表的一个文件，例如 `SYSTEM`。 |

<a id="wifi"></a>
## Wi-Fi 概念

| 中文 | 英文 | 定义 |
|---|---|---|
| <a id="access-point"></a>接入点 | access point (AP) | 提供 Wi-Fi 网络的设备。描述具体的测试设备时可写作"路由器"。 |
| <a id="station"></a>站点 | station (STA) | 连接到接入点的一方。本驱动作为站点时，目标设备是接入点的客户。 |
| <a id="bss"></a>BSS | BSS | 基本服务集（basic service set）：一个接入点在一个信道上提供的网络，用 BSSID 标识。 |
| <a id="scan"></a>扫描 | scan | 在各信道上收集 BSS 信息。 |
| <a id="active-scan"></a>主动扫描 | active scan | 发送探测请求后等待应答的扫描。 |
| <a id="passive-scan"></a>被动扫描 | passive scan | 只接收信标、不发送帧的扫描。 |
| <a id="beacon"></a>信标 | beacon | 接入点定期发出、用于宣告 BSS 的帧。 |
| <a id="authentication"></a>认证 | authentication | 802.11 连接的第一步（开放认证或 SAE）。 |
| <a id="association"></a>关联 | association | 认证之后、站点加入 BSS 的 802.11 步骤。 |
| <a id="4-way-handshake"></a>四次握手 | 4-way handshake | 关联之后协商密钥的 EAPOL 交换。由 Windows 完成。 |
| <a id="eapol"></a>EAPOL | EAPOL | 承载四次握手的帧，以太网类型为 `0x888E`。 |
| <a id="pairwise-key"></a>成对密钥 | pairwise key (PTK) | 通过四次握手得到、只用于一个站点与接入点之间单播帧的密钥。 |
| <a id="group-key"></a>组密钥 | group key (GTK) | 接入点分发给全部站点、用于组播帧和广播帧的密钥。 |
| <a id="privacy-exemption"></a>豁免 | privacy exemption | 允许某类帧（EAPOL）在没有密钥时以明文收发的规则。 |
| <a id="wpa2-personal"></a>WPA2-个人 | WPA2-Personal | 使用预共享密钥（PSK）的 WPA2，加密方式为 CCMP。 |
| <a id="wpa3-personal-sae"></a>WPA3-SAE | WPA3-SAE | 使用 SAE 认证的 WPA3-个人，要求管理帧保护。 |
| <a id="transition-mode"></a>过渡模式 | transition mode | 同时接受 WPA2-个人和 WPA3-SAE 的网络。 |
| <a id="protected-management-frames"></a>管理帧保护 | protected management frames (PMF) | 对单播管理帧加密、对组播管理帧添加完整性校验的 802.11 功能。 |
| <a id="channel-width"></a>信道宽度 | channel width | 一次传输占用的带宽：20、40 或 80 MHz。 |
| <a id="channel-context"></a>信道上下文 | channel context (chanctx) | mac80211 和 rtw89 用于描述一个工作信道（频率和信道宽度）的对象。站点和组主可共用一个信道上下文。 |
| <a id="vif"></a>vif | vif | 虚拟接口（virtual interface）：mac80211 和 rtw89 用于表示一个站点接口或组主接口的对象（`ieee80211_vif`）。 |
| <a id="ht20-ht40-vht80"></a>HT20、HT40、VHT80 | HT20, HT40, VHT80 | 802.11n 的 20/40 MHz 和 802.11ac 的 80 MHz 工作方式。 |
| <a id="link-rate"></a>链路速率 | link rate | Windows 显示的当前发送速率。 |
| <a id="throughput"></a>吞吐 | throughput | 实测的数据传输速度，单位为 Mbit/s。 |
| <a id="a-mpdu-aggregation"></a>A-MPDU 聚合 | A-MPDU aggregation | 将多个数据帧合并为一次传输、由对方一次确认的方式。 |
| <a id="block-ack-session"></a>块确认会话 | block ack (BA) session | 双方通过 ADDBA 动作帧约定、对一个 TID 的帧采用 A-MPDU 聚合和一次性确认的协议状态。缩写为"BA 会话"。 |
| <a id="country-code"></a>国家码 | country code | 两个字母的代码，例如 `DE`，决定适用哪一套管制规则。对应设置项 `Country`。 |
| <a id="regulatory-rules"></a>管制规则 | regulatory rules | 一个国家允许的信道、每个信道能否主动发射，以及发射功率。 |
| <a id="world-rules"></a>全球通用规则 | world rules | 没有国家码时使用的最保守管制规则（国家码 `00`）：5 GHz 只能被动扫描。 |
| <a id="power-save"></a>省电 | power save | 站点空闲时关闭接收机、依靠信标唤醒的 802.11 省电方式（rtw89 的 LPS）。 |
| <a id="coexistence"></a>共存 | coexistence | Wi-Fi 和蓝牙共用天线时按时间分配的协调机制。由 rtw89 的 btc 模块负责。 |
| <a id="system-error-recovery"></a>固件错误恢复 | system error recovery (SER) | 芯片或固件出错时，rtw89 复位芯片并恢复运行的机制。连接会断开，随后自动重连。 |

<a id="framework"></a>
## Windows 驱动框架

| 中文 | 英文 | 定义 |
|---|---|---|
| <a id="wificx"></a>WiFiCx | WiFiCx (Wi-Fi WDF class extension) | Windows 的 Wi-Fi 驱动框架。驱动通过它与 Windows 交换 WDI 消息。 |
| <a id="netadaptercx"></a>NetAdapterCx | NetAdapterCx | Windows 的网卡驱动框架，提供收发队列。WiFiCx 基于 NetAdapterCx 构建。 |
| <a id="kmdf"></a>KMDF | KMDF | 内核模式驱动框架。 |
| <a id="wdi"></a>WDI | WDI (WLAN Device Driver Interface) | WiFiCx 与驱动之间的消息格式，采用 TLV 编码。 |
| <a id="wdi-message"></a>WDI 消息 | WDI message | WDI 的一条请求或通知，分为任务、属性和指示三类。写法：`TASK_SCAN`(4)，括号中为消息号。 |
| <a id="task"></a>任务 | task | 耗时较长、完成时由驱动发送完成指示的 WDI 消息，例如 `TASK_CONNECT`。 |
| <a id="property"></a>属性 | property | 立即完成的设置或查询消息，例如 `SET_ADD_CIPHER_KEYS`。 |
| <a id="indication"></a>指示 | indication | 驱动主动发送给 WiFiCx 的通知，例如 `LINK_STATE_CHANGE`。 |
| <a id="capabilities"></a>能力 | capabilities | 驱动启动时向 WiFiCx 报告的功能和参数。 |
| <a id="wlansvc"></a>Wlansvc | WLAN AutoConfig service | Windows 的 WLAN 自动配置服务，负责扫描调度、网络选择和四次握手。 |
| <a id="port"></a>端口 | port | WiFiCx 为一种角色创建的逻辑接口，分为站点端口、Wi-Fi Direct 设备端口、角色端口三种。 |
| <a id="station-port"></a>站点端口 | station port | 用于连接接入点的端口。 |
| <a id="wi-fi-direct-device-port"></a>Wi-Fi Direct 设备端口 | Wi-Fi Direct device port | 只有控制面、没有数据收发的 Wi-Fi Direct 端口。 |
| <a id="role-port"></a>角色端口 | role port | 开启移动热点时用于收发数据的端口。 |
| <a id="wi-fi-direct"></a>Wi-Fi Direct | Wi-Fi Direct | 设备之间直接组网的 Wi-Fi 标准。在 WiFiCx 下，Windows 移动热点基于该标准实现。 |
| <a id="mobile-hotspot"></a>移动热点 | mobile hotspot | Windows 的一项功能：使目标设备作为接入点，共享其网络连接。 |
| <a id="group-owner"></a>组主 | group owner (GO) | Wi-Fi Direct 组中起接入点作用的一方。移动热点运行时，目标设备是组主。 |
| <a id="hotspot-client"></a>热点客户端 | hotspot client | 连接到移动热点的设备。 |
| <a id="diagnostic-interface"></a>诊断接口 | diagnostic interface | 驱动提供的 `\\.\rtw89win` 设备，只对 SYSTEM 和管理员开放。`rtwdiag.ps1` 通过该接口工作。 |
| <a id="driver-log"></a>驱动日志 | driver log | Wi-Fi 驱动保存在内存中的日志（最后 64 KB），用 `rtwdiag.ps1 log` 读取。 |
| <a id="saved-log"></a>保存的日志 | saved log | 上一次运行结束时存入设置项 `SavedLog` 的驱动日志最后 16 KB。 |
| <a id="crash-loop-guard"></a>崩溃循环保护 | crash loop guard | 连续两次运行未正常结束时，下次启动只进入诊断模式、不访问芯片的保护机制。 |
| <a id="wificx-mode"></a>WiFiCx 模式 | WiFiCx mode | 驱动的正常模式：向 Windows 提供网卡。 |
| <a id="diagnostic-mode"></a>诊断模式 | diagnostic mode | 驱动不向 Windows 提供网卡、只提供诊断接口的模式。崩溃循环保护触发时进入该模式。 |
| <a id="intx-interrupt"></a>INTx 中断 | INTx interrupt | PCIe 的电平触发中断（不是 MSI）。在 RK3588 上全部由 CPU0 处理。 |
| <a id="shutdown-notification"></a>关机通知 | shutdown notification | 系统关机时 Windows 发送给驱动的 `IRP_MJ_SHUTDOWN`。 |

<a id="components"></a>
## 本项目的组成

| 中文 | 英文 | 定义 |
|---|---|---|
| <a id="upstream-rtw89"></a>上游 rtw89 | upstream rtw89 | Linux 内核 v7.2 中的 Realtek 802.11ax 驱动。本项目原样使用，放在 `third_party\rtw89\`。 |
| <a id="driver-layer"></a>驱动层 | driver layer | `driver\`：KMDF 驱动本体，负责 PnP、硬件资源、系统服务封装、诊断接口和崩溃循环保护。 |
| <a id="wificx-front-end"></a>WiFiCx 前端 | WiFiCx front end | `wificx\`：处理 WDI 消息和 NetAdapterCx 队列。 |
| <a id="mac-layer"></a>mac 层 | mac layer | `mac\`：代替 Linux mac80211 和 cfg80211 的部分，负责扫描、连接、密钥、组主等 802.11 逻辑。 |
| <a id="compatibility-layer"></a>兼容层 | compatibility layer | `compat\`：使上游 rtw89 能在 Windows 内核中编译和运行的 Linux API 子集。 |
| <a id="firmware"></a>固件 | firmware | Wi-Fi 芯片运行的程序文件 `rtw8852b_fw-2.bin`。驱动启动时将其下载到芯片。泛指时，"固件文件"也包括蓝牙补丁。 |
| <a id="bluetooth-patch"></a>蓝牙补丁 | Bluetooth patch | 蓝牙控制器在 ROM 固件之上加载的补丁文件 `rtl8852bu_fw.bin`，与配置文件 `rtl8852bu_config.bin` 一起使用。 |
| <a id="rom-firmware"></a>ROM 固件 | ROM firmware | 蓝牙控制器出厂自带、未打补丁时运行的固件。 |
| <a id="pass-through"></a>直通 | pass-through | 蓝牙过滤驱动不下载蓝牙补丁、只转发 I/O 的状态。此时控制器运行 ROM 固件。 |
| <a id="host-test"></a>主机测试 | host test | 在构建电脑上运行的测试程序：`tools-dev\*-test.ps1` 和 `bt\rtkbtfw\test-epatch.c`。 |
| <a id="developer-test-scripts"></a>开发者测试脚本 | developer test scripts | `tools-dev\board\` 下在目标设备上运行的测试脚本。 |

<a id="confidence"></a>
## 可信度

实测结论和验证状态两篇文档用以下三个词说明一条结论的依据强度。

| 中文 | 英文 | 定义 |
|---|---|---|
| <a id="verified-on-hardware"></a>板上实测 | verified on hardware | 在目标设备上通过日志、计数或现象证实。只测过一次时，"依据"中注明"测过一次"。 |
| <a id="inferred-from-code"></a>代码推断 | inferred from code | 通过阅读 WDK 头文件、微软示例、上游源码或本项目源码得出，没有目标设备上的直接证据。只由主机测试证实的结论也属于代码推断。 |
| <a id="not-determined"></a>未确定 | not determined | 现有证据不足以判断。须写明缺少哪些证据。 |
