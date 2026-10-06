# Glossary

[中文](../zh/glossary.md)

- **Readers**: All readers.
- **What you will learn**: What each term in the documents means, and the Chinese term for it.
- **Prerequisites**: None.

<a id="summary"></a>
## Summary

- All English documents use only the English terms in these tables. One concept has only one term.
- When a document uses a term for the first time, it can give the abbreviation in parentheses, for example "group owner (GO)". After that, the document uses only the term or the abbreviation in these tables.
- Code, commands, WDI message names, and registry value names are not translated. They are in backticks.
- Each term has an anchor. Other documents can link to a term, for example `glossary.md#group-owner`.
- The terms are in five groups: devices, installation, and signatures; Wi-Fi concepts; Windows driver frameworks; components of this project; confidence.

<a id="devices"></a>
## Devices, installation, and signatures

| Term | Chinese | Definition |
|---|---|---|
| <a id="target-device"></a>target device | 目标设备 | The device that runs Windows 11 ARM64, has the RTL8852BE module, and gets this driver. The tests used a ROCK 5T. |
| <a id="build-pc"></a>build PC | 构建电脑 | The x64 Windows computer that runs the build scripts and makes the driver packages. |
| <a id="measuring-pc"></a>measuring PC | 测量电脑 | The Windows computer that runs the `hosttools\` scripts. It is on the same network as the target device and can connect to the target device through ssh. It can be the build PC. |
| <a id="module"></a>module | 模块 | The RTL8852BE hardware in the M.2 E-key slot. It has a Wi-Fi part (PCIe) and a Bluetooth part (USB). |
| <a id="network-adapter"></a>network adapter | 网卡 | The network adapter that Windows shows as "WLAN". |
| <a id="device"></a>device | 设备 | A PnP device node in Device Manager. When a document uses "device" alone, it is the RTL8852BE PCI device (the Wi-Fi part of the module). |
| <a id="bluetooth-usb-device"></a>Bluetooth USB device | 蓝牙 USB 设备 | The USB device in Windows for the Bluetooth part of the module. On ROCK 5T, the module is `USB\VID_13D3&PID_3572`. |
| <a id="driver-package"></a>driver package | 驱动包 | A directory that a build script makes. It contains the driver files, the INF, the catalog file (`.cat`), the firmware, the license files, the install scripts, and `SHA256SUMS`. |
| <a id="wi-fi-driver"></a>Wi-Fi driver | Wi-Fi 驱动 | `rtw89win.sys`, the Wi-Fi driver of this project. |
| <a id="bluetooth-filter-driver"></a>Bluetooth filter driver | 蓝牙过滤驱动 | `rtkbtflt.sys`: the lower filter driver below the Windows Bluetooth USB driver. It downloads the Bluetooth patch. |
| <a id="lower-filter-driver"></a>lower filter driver | 下层过滤驱动 | A KMDF driver below the function driver of a device. It processes the I/O of that device before the function driver. |
| <a id="in-place-upgrade"></a>in-place upgrade | 原地升级 | An update of the same device with a new driver package, without an uninstallation first. The interface GUID and the WLAN profiles do not change. |
| <a id="wlan-profile"></a>WLAN profile | WLAN 配置文件 | The settings that Windows keeps for one network (name, security type, password). Windows keeps them for each interface GUID. |
| <a id="interface-guid"></a>interface GUID | 网卡 GUID | The identifier that Windows gives to the network adapter (`NetCfgInstanceId`). Windows keeps the WLAN profiles by this identifier. |
| <a id="test-signing"></a>test signing | 测试签名 | A signature on a driver with a test certificate that you make yourself. Windows loads such drivers only in test-signing mode. |
| <a id="test-signing-mode"></a>test-signing mode | 测试签名模式 | A Windows boot option that `bcdedit /set testsigning on` enables. Before you enable it, you must disable Secure Boot. |
| <a id="secure-boot"></a>Secure Boot | 安全启动 | A UEFI function that starts only system components with a trusted signature. |
| <a id="test-certificate"></a>test certificate | 测试证书 | The code signing certificate on the build PC for test signing. The driver package contains its public key file `rtw89win-test.cer`. |
| <a id="setting"></a>setting | 设置项 | A registry value that the driver reads. The Wi-Fi driver settings are under `HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters`, for example `Country`. The Bluetooth filter driver settings are under `Services\rtkbtflt\Parameters`, for example `PassThrough`. The driver itself writes some of these values, for example `ArmRunning` and `SavedLog`. |
| <a id="restart-the-device"></a>restart the device | 重启设备 | `pnputil /restart-device`: stop the RTL8852BE PCI device and start it again. The computer does not restart. |
| <a id="restart-the-computer"></a>restart the computer | 重启电脑 | Restart Windows on the target device. The chip loses power. |
| <a id="re-enumeration"></a>re-enumeration | 重新枚举 | After the driver reports a device failure with `WdfDeviceFailedAttemptRestart`, the bus driver enumerates the device again and calls AddDevice of the driver again. After re-enumeration, the RTL8852BE cannot download firmware until the next power loss. This driver and the scripts of this project do not cause re-enumeration. |
| <a id="registry-hive"></a>registry hive | 注册表配置单元 | One file of the registry, for example `SYSTEM`. |

<a id="wifi"></a>
## Wi-Fi concepts

| Term | Chinese | Definition |
|---|---|---|
| <a id="access-point"></a>access point (AP) | 接入点 | A device that supplies a Wi-Fi network. For a specific test device, a document can use "router". |
| <a id="station"></a>station (STA) | 站点 | The side that connects to an access point. When this driver operates as a station, the target device is a client of the access point. |
| <a id="bss"></a>BSS | BSS | Basic service set: the network that one access point supplies on one channel. A BSSID identifies it. |
| <a id="scan"></a>scan | 扫描 | The collection of BSS information on the channels. |
| <a id="active-scan"></a>active scan | 主动扫描 | A scan that sends probe requests and waits for responses. |
| <a id="passive-scan"></a>passive scan | 被动扫描 | A scan that only receives beacons and does not transmit frames. |
| <a id="beacon"></a>beacon | 信标 | A frame that an access point transmits at regular intervals to announce the BSS. |
| <a id="authentication"></a>authentication | 认证 | The first step of an 802.11 connection (open authentication or SAE). |
| <a id="association"></a>association | 关联 | The 802.11 step after authentication. In this step, the station joins the BSS. |
| <a id="4-way-handshake"></a>4-way handshake | 四次握手 | The EAPOL exchange after association that sets the keys. Windows does it. |
| <a id="eapol"></a>EAPOL | EAPOL | The frames that carry the 4-way handshake. The Ethernet type is `0x888E`. |
| <a id="pairwise-key"></a>pairwise key (PTK) | 成对密钥 | The key from the 4-way handshake. Only the unicast frames between one station and the access point use it. |
| <a id="group-key"></a>group key (GTK) | 组密钥 | The key that the access point sends to all stations. Multicast frames and broadcast frames use it. |
| <a id="privacy-exemption"></a>privacy exemption | 豁免 | A rule that lets one type of frame (EAPOL) go without encryption when there is no key. |
| <a id="wpa2-personal"></a>WPA2-Personal | WPA2-个人 | WPA2 with a pre-shared key (PSK). The encryption is CCMP. |
| <a id="wpa3-personal-sae"></a>WPA3-SAE | WPA3-SAE | WPA3-Personal with SAE authentication. PMF is mandatory for it. |
| <a id="transition-mode"></a>transition mode | 过渡模式 | A network that accepts both WPA2-Personal and WPA3-SAE. |
| <a id="protected-management-frames"></a>protected management frames (PMF) | 管理帧保护 | An 802.11 function that encrypts unicast management frames and adds an integrity check to multicast management frames. |
| <a id="channel-width"></a>channel width | 信道宽度 | The bandwidth of one transmission: 20, 40, or 80 MHz. |
| <a id="channel-context"></a>channel context (chanctx) | 信道上下文 | The object that mac80211 and rtw89 use to describe one operating channel (frequency and channel width). The station and the group owner can use the same channel context. |
| <a id="vif"></a>vif | vif | Virtual interface: the object that mac80211 and rtw89 use for one station interface or one group owner interface (`ieee80211_vif`). |
| <a id="ht20-ht40-vht80"></a>HT20, HT40, VHT80 | HT20、HT40、VHT80 | The 20 MHz and 40 MHz modes of 802.11n, and the 80 MHz mode of 802.11ac. |
| <a id="link-rate"></a>link rate | 链路速率 | The current transmit rate that Windows shows. |
| <a id="throughput"></a>throughput | 吞吐 | The measured data transfer speed, in Mbit/s. |
| <a id="a-mpdu-aggregation"></a>A-MPDU aggregation | A-MPDU 聚合 | A method that puts many data frames into one transmission. The receiver acknowledges them with one response. |
| <a id="block-ack-session"></a>block ack (BA) session | 块确认会话 | A protocol state that two sides agree on with ADDBA action frames. In this state, the frames of one TID use A-MPDU aggregation and one acknowledgment. The short form is "BA session". |
| <a id="country-code"></a>country code | 国家码 | A code of two letters, for example `DE`. It selects the set of regulatory rules. The setting is `Country`. |
| <a id="regulatory-rules"></a>regulatory rules | 管制规则 | The channels that a country permits, the permission for active transmission on each channel, and the transmit power. |
| <a id="world-rules"></a>world rules | 全球通用规则 | The most conservative regulatory rules, for use when there is no country code (country code `00`). On 5 GHz, only passive scan is permitted. |
| <a id="power-save"></a>power save | 省电 | The 802.11 power save method: an idle station stops its receiver and starts it again for beacons (rtw89 LPS). |
| <a id="coexistence"></a>coexistence | 共存 | The coordination that divides the antenna time between Wi-Fi and Bluetooth when they use the same antenna. The rtw89 btc module does it. |
| <a id="system-error-recovery"></a>system error recovery (SER) | 固件错误恢复 | The rtw89 function that resets the chip and starts the operation again when the chip or the firmware has an error. The connection stops, and then it connects again automatically. |

<a id="framework"></a>
## Windows driver frameworks

| Term | Chinese | Definition |
|---|---|---|
| <a id="wificx"></a>WiFiCx (Wi-Fi WDF class extension) | WiFiCx | The Windows Wi-Fi driver framework. A driver uses it to exchange WDI messages with Windows. |
| <a id="netadaptercx"></a>NetAdapterCx | NetAdapterCx | The Windows network adapter driver framework. It supplies the TX and RX queues. WiFiCx uses NetAdapterCx as its base. |
| <a id="kmdf"></a>KMDF | KMDF | Kernel-Mode Driver Framework. |
| <a id="wdi"></a>WDI (WLAN Device Driver Interface) | WDI | The message format between WiFiCx and the driver. It uses the TLV format to encode the data. |
| <a id="wdi-message"></a>WDI message | WDI 消息 | One WDI request or notification. There are three types: tasks, properties, and indications. Format: `TASK_SCAN`(4), where the number in parentheses is the message number. |
| <a id="task"></a>task | 任务 | A WDI message that takes a long time. When it completes, the driver sends a completion indication. Example: `TASK_CONNECT`. |
| <a id="property"></a>property | 属性 | A set message or query message that completes immediately, for example `SET_ADD_CIPHER_KEYS`. |
| <a id="indication"></a>indication | 指示 | A notification that the driver sends to WiFiCx without a request, for example `LINK_STATE_CHANGE`. |
| <a id="capabilities"></a>capabilities | 能力 | The functions and parameters that the driver reports to WiFiCx when it starts. |
| <a id="wlansvc"></a>Wlansvc (WLAN AutoConfig service) | Wlansvc | The Windows WLAN AutoConfig service. It controls the scan schedule, selects the network, and does the 4-way handshake. |
| <a id="port"></a>port | 端口 | A logical interface that WiFiCx creates for one role. There are three types: station port, Wi-Fi Direct device port, and role port. |
| <a id="station-port"></a>station port | 站点端口 | The port that connects to an access point. |
| <a id="wi-fi-direct-device-port"></a>Wi-Fi Direct device port | Wi-Fi Direct 设备端口 | A Wi-Fi Direct port that has only a control plane and no data TX or RX. |
| <a id="role-port"></a>role port | 角色端口 | The port that sends and receives data while the mobile hotspot operates. |
| <a id="wi-fi-direct"></a>Wi-Fi Direct | Wi-Fi Direct | The Wi-Fi standard for direct networks between devices. With WiFiCx, the Windows mobile hotspot uses this standard. |
| <a id="mobile-hotspot"></a>mobile hotspot | 移动热点 | A Windows function that makes the target device operate as an access point and share its network connection. |
| <a id="group-owner"></a>group owner (GO) | 组主 | The side in a Wi-Fi Direct group that operates as the access point. While the mobile hotspot operates, the target device is the group owner. |
| <a id="hotspot-client"></a>hotspot client | 热点客户端 | A device that connects to the mobile hotspot. |
| <a id="diagnostic-interface"></a>diagnostic interface | 诊断接口 | The device `\\.\rtw89win` that the driver supplies. Only SYSTEM and administrators can open it. `rtwdiag.ps1` uses this interface. |
| <a id="driver-log"></a>driver log | 驱动日志 | The log of the Wi-Fi driver in memory (the last 64 KB). Read it with `rtwdiag.ps1 log`. |
| <a id="saved-log"></a>saved log | 保存的日志 | The last 16 KB of the driver log. The driver writes it to the setting `SavedLog` when a run ends. |
| <a id="crash-loop-guard"></a>crash loop guard | 崩溃循环保护 | A protection function. If two runs in sequence do not end correctly, the next start enters only diagnostic mode and does not access the chip. |
| <a id="wificx-mode"></a>WiFiCx mode | WiFiCx 模式 | The normal mode of the driver: it supplies a network adapter to Windows. |
| <a id="diagnostic-mode"></a>diagnostic mode | 诊断模式 | A mode where the driver supplies only the diagnostic interface and no network adapter to Windows. The driver enters this mode when the crash loop guard triggers. |
| <a id="intx-interrupt"></a>INTx interrupt | INTx 中断 | The level-triggered PCIe interrupt (not MSI). On RK3588, CPU0 handles all of these interrupts. |
| <a id="shutdown-notification"></a>shutdown notification | 关机通知 | The `IRP_MJ_SHUTDOWN` that Windows sends to a driver at system shutdown. |

<a id="components"></a>
## Components of this project

| Term | Chinese | Definition |
|---|---|---|
| <a id="upstream-rtw89"></a>upstream rtw89 | 上游 rtw89 | The Realtek 802.11ax driver in Linux kernel v7.2. This project uses it without changes, in `third_party\rtw89\`. |
| <a id="driver-layer"></a>driver layer | 驱动层 | `driver\`: the main part of the KMDF driver. It controls PnP, hardware resources, system service wrappers, the diagnostic interface, and the crash loop guard. |
| <a id="wificx-front-end"></a>WiFiCx front end | WiFiCx 前端 | `wificx\`: the part that processes WDI messages and NetAdapterCx queues. |
| <a id="mac-layer"></a>mac layer | mac 层 | `mac\`: the part that replaces Linux mac80211 and cfg80211. It contains the 802.11 logic, for example scan, connection, keys, and group owner. |
| <a id="compatibility-layer"></a>compatibility layer | 兼容层 | `compat\`: the subset of the Linux API that lets upstream rtw89 compile and run in the Windows kernel. |
| <a id="firmware"></a>firmware | 固件 | The program file `rtw8852b_fw-2.bin` that the Wi-Fi chip runs. The driver downloads it to the chip at start. In general use, "firmware files" also includes the Bluetooth patch. |
| <a id="bluetooth-patch"></a>Bluetooth patch | 蓝牙补丁 | The patch file `rtl8852bu_fw.bin` that the Bluetooth controller loads on top of the ROM firmware. The controller uses it with the configuration file `rtl8852bu_config.bin`. |
| <a id="rom-firmware"></a>ROM firmware | ROM 固件 | The factory firmware in the Bluetooth controller. The controller runs it when no patch is loaded. |
| <a id="pass-through"></a>pass-through | 直通 | A state of the Bluetooth filter driver: it does not download the Bluetooth patch and only forwards I/O. The controller then runs the ROM firmware. |
| <a id="host-test"></a>host test | 主机测试 | A test program that runs on the build PC: `tools-dev\*-test.ps1` and `bt\rtkbtfw\test-epatch.c`. |
| <a id="developer-test-scripts"></a>developer test scripts | 开发者测试脚本 | The test scripts in `tools-dev\board\` that run on the target device. |

<a id="confidence"></a>
## Confidence

The documents Observed behavior and Validation status use these three terms. The terms show how strong the evidence for a finding is.

| Term | Chinese | Definition |
|---|---|---|
| <a id="verified-on-hardware"></a>verified on hardware | 板上实测 | Confirmed on the target device with logs, counters, or symptoms. If the test occurred only one time, the Evidence field of the finding says "tested one time". |
| <a id="inferred-from-code"></a>inferred from code | 代码推断 | Found from WDK header files, Microsoft samples, upstream source code, or the source code of this project. There is no direct evidence on the target device. A finding that only host tests confirm is also inferred from code. |
| <a id="not-determined"></a>not determined | 未确定 | The available evidence is not enough for a decision. The document must state which evidence is necessary. |
