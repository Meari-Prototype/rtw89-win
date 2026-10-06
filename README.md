# rtw89-win

[English](README.en.md)

本项目为 Windows 11 ARM64 提供 Realtek RTL8852BE（Wi-Fi 6、蓝牙）的驱动，基于 Linux 内核中的 rtw89 驱动。

> **注意**：本项目并非 Realtek 或微软的官方驱动，与这两家公司无关。驱动采用测试签名，本项目不提供编译好的驱动包。

<a id="features"></a>
## 功能

| 功能 | 状态（条件和数据见[验证状态](docs/zh/validation.md)） |
|---|---|
| 扫描和连接：2.4 GHz（HT20），5 GHz（最宽 VHT80） | 通过 |
| WPA2-个人；WPA3-SAE 和管理帧保护（缺省关闭，设置项 `Pmf`） | 通过 |
| A-MPDU 聚合 | 通过 |
| Windows 移动热点（WPA2-个人，HT20） | 通过（有限制） |
| 固件错误恢复（SER） | 通过 |
| 国家码（设置项 `Country`） | 通过 |
| 省电（设置项 `PowerSave`） | 通过（有限制） |
| 蓝牙：补丁自动下载、BLE 扫描、配对 | 通过 |
| 802.11ax（HE） | 未实现 |
| 160 MHz、6 GHz、企业网络（802.1X） | 不支持 |

<a id="requirements"></a>
## 使用条件

- **硬件**：RTL8852BE 模块（PCI 硬件 ID `PCI\VEN_10EC&DEV_B852`）。仅在 ROCK 5T（RK3588）上测试过。UEFI 须为模块所在的 M.2 槽上电。蓝牙部分的 USB 硬件 ID 须为 `USB\VID_13D3&PID_3572`；若不是，请先按[移植指南](docs/zh/porting.md#new-board)修改 INF，然后构建。
- **系统**：Windows 11 ARM64，版本 26100 或更高。测试使用的是 Windows 11 IoT Enterprise LTSC 2024。
- **测试签名模式**：Windows 仅在该模式下加载本驱动。打开该模式前须关闭安全启动。
- **固件**：本仓库不含固件。用户须从 linux-firmware 取得 3 个固件文件。
- **自行构建**：用户须在一台 x64 Windows 电脑上构建驱动包。

<a id="start"></a>
## 开始使用

1. 取得固件文件：见[固件获取说明](firmware/README.md)。
2. 构建 Wi-Fi 驱动包和蓝牙驱动包：见[构建](docs/zh/build.md)。
3. 在目标设备上安装并使用驱动：见[安装与使用](docs/zh/install.md)。

<a id="docs"></a>
## 文档

| 文档 | 读者 | 内容 |
|---|---|---|
| [安装与使用](docs/zh/install.md) | 使用者 | 测试签名模式、安装、连接、移动热点、设置项、排障、恢复 |
| [构建](docs/zh/build.md) | 构建者 | 工具、固件、测试证书、构建驱动包、主机测试 |
| [固件获取说明](firmware/README.md) | 使用者、构建者 | 固件文件、来源、SHA256、许可 |
| [架构](docs/zh/architecture.md) | 驱动开发者 | 各层边界、控制流、数据流、设计决策 |
| [实测结论](docs/zh/observed-behavior.md) | 驱动开发者 | WiFiCx/WDI 文档未记载、经实测得到的行为 |
| [移植指南](docs/zh/porting.md) | 驱动开发者 | 添加 rtw89 芯片、更换硬件平台、更换驱动家族 |
| [验证状态](docs/zh/validation.md) | 使用者、驱动开发者 | 各项功能的测试条件、结果和数据 |
| [第三方许可说明](docs/zh/licenses.md) | 许可审阅者 | 各部分的许可和义务 |
| [术语表](docs/zh/glossary.md) | 所有读者 | 文档用词的定义 |

<a id="license"></a>
## 许可

- 本项目的代码采用 BSD-3-Clause 许可，见 [`LICENSE`](LICENSE)。上游 rtw89 原样存放在 `third_party\rtw89\`，本项目按 BSD-3-Clause 使用。
- 其它第三方部分见 [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES)，中文说明见[第三方许可说明](docs/zh/licenses.md)。
- 固件不随本仓库提供。固件采用 Realtek 的许可，见[固件获取说明](firmware/README.md)。
