# 安装与使用

[English](../en/install.md)

- **适用读者**：使用者，即在目标设备上安装和使用本驱动的人员。
- **本文任务**：在目标设备上安装 Wi-Fi 驱动和蓝牙过滤驱动。然后连接网络、使用移动热点、修改设置项，并排除常见故障。
- **前提**：已自行构建 Wi-Fi 驱动包和蓝牙驱动包（见[构建](build.md)）；拥有目标设备的管理员权限；目标设备已安装 PowerShell 7。

<a id="summary"></a>
## 要点

1. 安装前须打开测试签名模式。打开该模式前须关闭安全启动。
2. 安装脚本自动判断首次安装或原地升级。更换版本时请勿先卸载，否则 WLAN 配置文件将失效。
3. 安装时用 `-Country` 设置目标设备所在国家的国家码。未设国家码时，连接前 5 GHz 只能被动扫描。
4. 使用蓝牙须另行安装蓝牙过滤驱动。未安装时，蓝牙无法扫描到设备，也无法配对。
5. 出现问题时，先运行 `rtwdiag.ps1 mode`，然后用 `rtwdiag.ps1 log -Saved` 查看保存的日志。

除非另有说明，本文的命令均在目标设备上运行。"管理员 PowerShell 7"指以管理员身份运行的 `pwsh`。本文的"设备"指 RTL8852BE 的 PCI 设备（模块的 Wi-Fi 部分），"重启电脑"指重新启动目标设备上的 Windows。

<a id="before"></a>
## 准备工作

在目标设备上（管理员 PowerShell 7）核对以下各项：

1. 确认 Windows 能识别模块的 Wi-Fi 部分。

   ```powershell
   Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*'
   ```

   **结果**：输出一行。若无输出，说明 UEFI 未给 M.2 槽上电，见 [PLAT-4](observed-behavior.md#plat-4)。
2. 确认 Windows 的版本号不低于 26100。

   ```powershell
   [Environment]::OSVersion.Version.Build
   ```

   **结果**：数字不小于 `26100`。驱动的 INF 只能安装在这些版本上。测试使用的是 Windows 11 IoT Enterprise LTSC 2024。
3. 确认 PowerShell 7 可用。

   ```powershell
   $PSVersionTable.PSVersion
   ```

   **结果**：`Major` 为 `7`。若不是，请按微软文档[安装 PowerShell](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows)。
4. 确认已有两个驱动包目录。它们是构建电脑上 `out\` 下的两个目录：

   | 驱动包 | 目录名 | 必须有的文件 |
   |---|---|---|
   | Wi-Fi 驱动包 | `out\<时间>\` | `rtw89win.sys`、`rtw89win.inf`、`rtw89win.cat`、`rtw8852b_fw-2.bin`、`install-rtw89win.ps1`、`rtwdiag.ps1`、`LICENSE`、`THIRD_PARTY_NOTICES`、`SHA256SUMS` |
   | 蓝牙驱动包 | `out\rtkbtflt-<时间>\` | `rtkbtflt.sys`、`rtkbtflt.inf`、`rtkbtflt.cat`、`rtl8852bu_fw.bin`、`rtl8852bu_config.bin`、`rtkbtfw.exe`、`install-rtkbtflt.ps1`、`LICENSE`、`THIRD_PARTY_NOTICES`、`SHA256SUMS` |

   固件已包含在驱动包中，安装时无需另行放置固件。

<a id="test-signing"></a>
## 打开测试签名模式

本驱动采用测试签名。Windows 仅在测试签名模式下加载本驱动。打开测试签名模式前须关闭安全启动。

> **注意**：关闭安全启动并打开测试签名模式，会降低 Windows 的安全防护。请仅在自己的设备上执行此操作。

> **注意**：若 Windows 分区启用了 BitLocker，更改安全启动设置后，Windows 将要求输入恢复密钥。请先备份恢复密钥。

1. 重启电脑，进入目标设备的 UEFI 设置界面。
2. 若安全启动已打开，将其关闭。各种 UEFI 的菜单位置不同。
3. 保存 UEFI 设置，然后启动 Windows。
4. 打开管理员 PowerShell 7。
5. 打开测试签名模式。

   ```powershell
   bcdedit /set testsigning on
   ```

   **结果**：`bcdedit` 报告操作成功。若报错并提及安全启动，请返回第 1 步。
6. 重启电脑。
7. 确认测试签名模式已打开。

   ```powershell
   bcdedit /enum '{current}'
   ```

   **结果**：输出中 `testsigning` 一行的值为 `Yes`。桌面右下角显示 Windows 的"测试模式"水印。

<a id="install-wifi"></a>
## 安装 Wi-Fi 驱动

安装脚本 `install-rtw89win.ps1` 依次执行以下操作：

1. 按 `SHA256SUMS` 核对驱动包中的每个文件。任一文件不符即停止。
2. 用 `reg save` 将 `SYSTEM` 注册表配置单元备份到 `<工作目录>\backup\rtw89win-<时间>\SYSTEM.hiv`。
3. 若给出 `-Country`，将其写入设置项 `Country`。
4. 安装期间临时信任驱动包中的测试证书 `rtw89win-test.cer`，安装完成后将其删除。
5. 执行首次安装，或执行原地升级（设备已在使用本驱动时）。
6. 显示设备状态、网卡 GUID（安装前后对比）、`rtwdiag.ps1 mode` 的输出和驱动日志。

| 脚本参数 | 取值 | 缺省 |
|---|---|---|
| `-Country` | 两个字母的国家码，或 `00`（不设国家码） | 不写入；已有的 `Country` 值保持不变 |
| `-WorkDir` | 一个目录，用于存放备份和脚本输出的副本 | `%ProgramData%\rtw89win` |

脚本输出的副本保存在 `<工作目录>\logs\rtw89win-install.log`。副本不含最后显示的 `rtwdiag.ps1 mode` 输出和驱动日志。

> **注意**：国家码决定哪些信道可以主动发射，也决定发射功率。请设为目标设备所在国家的国家码，切勿设为其它国家的国家码。

> **提示**：首次安装的流程和 `-WorkDir` 参数尚未在目标设备上运行过（见[验证状态](validation.md#install)）。若输出与本文不同，请保存 `<工作目录>\logs\rtw89win-install.log`，然后按[排障](#troubleshooting)处理。

首次安装并给出 `-Country` 时，国家码从设备下次启动时起生效，因此第 5 步须重启设备。

1. 在构建电脑上：将 Wi-Fi 驱动包目录整个复制到目标设备。可使用 U 盘或网络共享。
2. 在目标设备上：打开管理员 PowerShell 7。
3. 进入驱动包目录。

   ```powershell
   cd <驱动包目录>
   ```

   `<驱动包目录>`：复制到目标设备上的 Wi-Fi 驱动包目录。
4. 运行安装脚本。

   ```powershell
   pwsh -NoProfile -File .\install-rtw89win.ps1 -Country <国家码>
   ```

   `<国家码>`：两个字母，例如 `DE`。若不设国家码，请删去 `-Country <国家码>`。

   **结果**：`=== verify package` 的下一行为 `ok`；`=== result` 之后有一行 `mode: WiFiCx`。
5. 若输出中有 `applies from the next start of the device`，重启设备（见[重启设备](#restart-device)）。
6. 确认 Windows 中已出现本驱动的网卡。

   ```powershell
   Get-NetAdapter | Where-Object InterfaceDescription -like '*rtw89win*'
   ```

   **结果**：有一行的 `InterfaceDescription` 为 `Realtek RTL8852BE Wi-Fi (rtw89win)`，且 `Status` 不是 `Disabled`。下次重启电脑前，有时另有一行 `Status` 为 `Disabled` 的网卡。它是移动热点的角色端口，属于正常现象（[HS-3](observed-behavior.md#hs-3)）。
7. 若给出了 `-Country`，确认国家码。

   ```powershell
   .\rtwdiag.ps1 country
   ```

   **结果**：`in effect:` 一行显示所设的国家码和 `from user (Parameters\Country)`。

> **提示**：请保留目标设备上的驱动包目录。诊断脚本 `rtwdiag.ps1` 和卸载脚本均在其中。

<a id="connect"></a>
## 连接网络

驱动支持 WPA2-个人和 WPA3-SAE。WPA3-SAE 和管理帧保护缺省关闭，打开方法见[修改设置](#settings)。

在目标设备上（第 4 步使用管理员 PowerShell 7，当前目录为 Wi-Fi 驱动包目录）：

1. 单击任务栏右下角的网络图标，打开 Wi-Fi 列表。
2. 选择要连接的网络，单击"连接"。
3. 输入密码。
4. 确认已连接。

   ```powershell
   .\rtwdiag.ps1 link
   ```

   **结果**：第一行为 `state: connected`。

对已保存的网络，也可以用命令行连接：

```powershell
netsh wlan connect name=<配置文件名>
```

`<配置文件名>`：WLAN 配置文件的名称。Windows 以网络名称作为其缺省名称。

- Windows 的定位服务关闭时，`netsh wlan show networks` 和已连接时的 `netsh wlan show interfaces` 会报告"拒绝访问"（[SCAN-7](observed-behavior.md#scan-7)）。此时请使用任务栏的 Wi-Fi 列表或 `Get-NetAdapter` 查看。
- 未勾选"自动连接"的 WLAN 配置文件，在重启设备、重启电脑或接入点断开后不会自动重连（[CONN-8](observed-behavior.md#conn-8)）。
- 新连接的网络为"公用网络"。Windows 防火墙会丢弃 ping 的回显请求（[INST-8](observed-behavior.md#inst-8)）。

<a id="install-bt"></a>
## 安装蓝牙过滤驱动

模块的蓝牙部分是一个 USB 设备，下文称"蓝牙 USB 设备"。Windows 自带的蓝牙 USB 驱动（`BTHUSB`）能识别该设备，但控制器只运行 ROM 固件。此时蓝牙无法扫描到设备，也无法配对（[BT-1](observed-behavior.md#bt-1)）。蓝牙 USB 设备每次启动、每次从 D3 电源状态恢复时，蓝牙过滤驱动都会下载蓝牙补丁，每次约 0.5 s。

安装脚本 `install-rtkbtflt.ps1` 依次执行以下操作：

1. 按 `SHA256SUMS` 核对驱动包中的每个文件。
2. 将 `SYSTEM` 注册表配置单元备份到 `<工作目录>\backup\rtkbtflt-<时间>\SYSTEM.hiv`。
3. 临时信任测试证书并安装驱动包，安装完成后删除该证书。
4. 确认蓝牙过滤驱动已加载。若未加载，则重启蓝牙 USB 设备。
5. 显示 `rtkbtflt.ps1 status` 和 `rtkbtfw info` 的输出。

蓝牙 USB 设备继续使用 Windows 自带的 `BTHUSB`。蓝牙过滤驱动安装在 `BTHUSB` 下层。

| 脚本参数 | 取值 | 缺省 |
|---|---|---|
| `-HardwareId` | 蓝牙 USB 设备的硬件 ID | `USB\VID_13D3&PID_3572`（ROCK 5T 上的模块） |
| `-WorkDir` | 一个目录，用于存放备份和脚本输出的副本 | `%ProgramData%\rtw89win` |

1. 在构建电脑上：将蓝牙驱动包目录整个复制到目标设备。
2. 在目标设备上：打开管理员 PowerShell 7。
3. 查看蓝牙 USB 设备的硬件 ID。

   ```powershell
   Get-PnpDevice -PresentOnly -Class Bluetooth | Where-Object InstanceId -like 'USB\*' | Format-List FriendlyName, InstanceId
   ```

   **结果**：`InstanceId` 以 `USB\VID_xxxx&PID_xxxx\` 开头。前两段即为硬件 ID。
4. 若硬件 ID 不是 `USB\VID_13D3&PID_3572`，按[移植指南](porting.md#new-board)修改 INF 并重新构建蓝牙驱动包，然后从第 1 步重新开始。
5. 进入蓝牙驱动包目录。

   ```powershell
   cd <驱动包目录>
   ```

   `<驱动包目录>`：复制到目标设备上的蓝牙驱动包目录。
6. 运行安装脚本。若硬件 ID 不是缺省值，在命令末尾加上 `-HardwareId '<硬件 ID>'`。

   ```powershell
   pwsh -NoProfile -File .\install-rtkbtflt.ps1
   ```

   **结果**：`rtkbtfw info` 的输出中有 `lmpSubversion 0xb20f`。
7. 若没有 `0xb20f`，运行 `.\rtkbtflt.ps1 status`，并按[排障](#troubleshooting)表处理。
8. 打开"设置 → 蓝牙和其他设备"，添加设备。

   **结果**：列表中出现附近的蓝牙设备。

<a id="settings"></a>
## 修改设置

Wi-Fi 驱动的设置项位于注册表 `HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters` 下。设备每次启动时，驱动读取一次设置项。因此修改后须重启设备。

| 设置项 | 类型 | 取值 | 缺省（无此值时） | 作用 |
|---|---|---|---|---|
| `Country` | REG_SZ | 两个字母；`00` 表示不设 | 不设 | 国家码 |
| `Bandwidth` | REG_DWORD | `20`、`40`、`80` | `80` | 站点在 5 GHz 的最大信道宽度 |
| `PowerSave` | REG_DWORD | `0` 不省电；`1` 跟随 Windows；`2` 始终省电 | `1` | 省电 |
| `Pmf` | REG_DWORD | `0` 关闭；`1` 管理帧保护；`2` 管理帧保护和 WPA3-SAE | `0` | 管理帧保护和 WPA3-SAE |
| `WifiDirect` | REG_DWORD | `0` 关闭；`1` 打开 | `1` | 移动热点 |

- `Country`：决定哪些信道可以主动发射，也决定发射功率。未设置时，驱动依次使用已连接接入点宣告的国家码、efuse 中的国家码（ROCK 5T 上的模块没有）、全球通用规则。wireless-regdb 中没有的国家码不生效，驱动日志中会记录该情况。
- `Bandwidth`：`20`、`40`、`80` 分别对应 HT20、HT40、VHT80。2.4 GHz 和移动热点不受其影响。其它值按 `80` 处理。
- `PowerSave`：跟随 Windows 时，实测效果等同于不省电（[PS-1](observed-behavior.md#ps-1)）。其它值按 `1` 处理。
- `Pmf`：仅在驱动的加密自检通过时生效。其它值按 `0` 处理。
- `WifiDirect`：为 `0` 时，驱动不向 Windows 报告 Wi-Fi Direct 能力，移动热点不可用。

下列设置项仅用于调试。不调试时，请勿设置这些值。

| 设置项 | 类型 | 取值 | 缺省 | 作用 |
|---|---|---|---|---|
| `AllowConnect`（调试用） | REG_DWORD | `0` 拒绝所有连接；其它值允许 | 允许 | 连接闸门 |
| `Allow\AllowSsid`（调试用） | REG_SZ，位于子键 `Allow` 下 | 一个网络名称 | 不限制 | 仅允许连接该名称的网络 |
| `WdiDump`（调试用） | REG_DWORD | `1` 打开 | 关闭 | 将每种 WDI 消息的第一条写入驱动日志 |

- `AllowConnect` 和 `AllowSsid` 在每次连接时读取，修改后无需重启设备。子键 `Allow` 下的 `AllowConnect` 优先于 `Parameters` 下的同名值。
- `WdiDump` 不记录含密钥的消息。使用完毕后，请删除该值。

`ArmRunning`、`BootAttempts`、`SavedLog` 由驱动自行写入，请勿修改。前两个属于崩溃循环保护，`SavedLog` 是保存的日志。

蓝牙过滤驱动的设置项位于 `HKLM\SYSTEM\CurrentControlSet\Services\rtkbtflt\Parameters` 下：

| 设置项 | 类型 | 取值 | 缺省 | 作用 |
|---|---|---|---|---|
| `PassThrough` | REG_DWORD | `0` 正常；`1` 直通 | `0` | 为 `1` 时不下载蓝牙补丁，仅转发 I/O（直通） |

`PassThrough` 从蓝牙 USB 设备下次启动时起生效，升级时保留。`SessionRunning`、`BadSessions` 由蓝牙过滤驱动自行写入，属于其崩溃循环保护。

### 修改设置项

在目标设备上，在 Wi-Fi 驱动包目录中打开管理员 PowerShell 7：

1. 若要修改国家码，运行 `rtwdiag.ps1 country`。

   ```powershell
   .\rtwdiag.ps1 country <国家码>
   ```

2. 若要修改其它设置项，运行 `reg add`。

   ```powershell
   reg add HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters /v <设置项> /t REG_DWORD /d <值> /f
   ```

   `<设置项>`：上表中的名称，例如 `Bandwidth`。`<值>`：上表"取值"列中的一个数。
3. 重启设备（见[重启设备](#restart-device)）。
4. 确认新值已生效。

   ```powershell
   .\rtwdiag.ps1 settings
   ```

   **结果**：输出列出每个设置项的值。`in effect:` 一行为生效的国家码，`Bandwidth in effect:` 一行为生效的信道宽度。
5. 若网络未自动重连，请重新连接网络。

恢复缺省值时，删除该值，然后重启设备：

```powershell
reg delete HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters /v <设置项> /f
```

<a id="restart-device"></a>
### 重启设备

重启设备即停止并重新启动 RTL8852BE 的 PCI 设备，不重启电脑。网络连接将断开。

1. 重启设备。

   ```powershell
   $id = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*').InstanceId
   pnputil /restart-device $id
   ```

   **结果**：`pnputil` 报告设备已重新启动。若 `pnputil` 报告需要重启电脑，则重启电脑（[INST-3](observed-behavior.md#inst-3)）。

`.\rtwdiag.ps1 arm` 也会重启设备。该命令还会清除崩溃循环保护的计数，并等待网卡启动完成。

<a id="hotspot"></a>
## 使用移动热点

移动热点使目标设备作为接入点，共享其网络连接。在 WiFiCx 驱动上，Windows 通过 Wi-Fi Direct 实现移动热点。热点运行时，目标设备为组主（GO）。

在目标设备上（第 5 步使用管理员 PowerShell 7，当前目录为 Wi-Fi 驱动包目录）：

1. 打开"设置 → 网络和 Internet → 移动热点"。
2. 选择要共享的连接。
3. 单击"编辑"，设置网络名称、密码和频段。
4. 打开"移动热点"开关。
5. 确认组主正在运行。

   ```powershell
   .\rtwdiag.ps1 ap
   ```

   **结果**：第一行为 `GO: running`，其后为频段和信道。
6. 在热点客户端上连接该网络。

限制：

- 安全类型仅支持 WPA2-个人。信道宽度仅支持 HT20。
- 站点已连接时，热点只能与站点使用同一信道。频段须选择"任何可用"或站点所在的频段。选择其它频段时，Windows 报告 `BandInterference`（[HS-12](observed-behavior.md#hs-12)）。
- 5 GHz 热点只使用管制规则允许主动发射、且不要求雷达检测的信道。使用全球通用规则时，热点只能使用 2.4 GHz。
- 热点开启期间，若发生固件错误恢复（SER）或重启设备，热点将关闭。Windows 不会自动重新打开热点，须手动打开（[HS-13](observed-behavior.md#hs-13)）。
- 约 5 分钟内没有热点客户端时，Windows 自动关闭热点（[HS-13](observed-behavior.md#hs-13)）。
- 驱动向 Windows 报告最多支持 8 个热点客户端。实测只使用过 1 个。热点客户端之间的互访未经测试。
- 有热点客户端时，站点只扫描当前信道。
- 网卡列表中多出的 `WLAN 2`、`WLAN 3` 属于正常现象。它们是 Wi-Fi Direct 设备端口和角色端口（[HS-3](observed-behavior.md#hs-3)）。

不使用移动热点时，可将 `WifiDirect` 设为 `0`，然后重启设备。

<a id="upgrade"></a>
## 升级驱动

使用新驱动包中的安装脚本升级。安装脚本执行原地升级：设备、网卡 GUID 和 WLAN 配置文件均保持不变。

> **注意**：请勿先卸载再安装。卸载后重新安装时，网卡 GUID 将改变，原有的 WLAN 配置文件全部失效（[INST-2](observed-behavior.md#inst-2)）。

1. 在构建电脑上：将新的 Wi-Fi 驱动包复制到目标设备。
2. 在目标设备上：在新驱动包目录中打开管理员 PowerShell 7。
3. 运行新驱动包中的安装脚本。

   ```powershell
   pwsh -NoProfile -File .\install-rtw89win.ps1
   ```

   国家码保持升级前的值。若要同时修改国家码，在命令末尾加上 `-Country <国家码>`。原地升级时，新的国家码在新驱动启动时即生效。

   **结果**：输出中有 `in-place upgrade`，`NetCfgInstanceId after:` 一行末尾为 `kept`。
4. 若输出中有 `a reboot is needed`，重启电脑。
5. 确认新驱动正在运行。

   ```powershell
   pwsh -NoProfile -File .\rtwdiag.ps1 log | Select-String 'rtw89win .* loaded'
   ```

   **结果**：最后一行为 `rtw89win <版本> loaded`，`<版本>` 为新驱动包的版本。第 3 步输出中 `driver running:` 后的版本也是该版本。

升级蓝牙过滤驱动的方法相同：运行新蓝牙驱动包中的 `install-rtkbtflt.ps1`。两个安装脚本也接受同版本或旧版本的驱动包。此时脚本强制更新设备的驱动。

<a id="uninstall"></a>
## 卸载驱动

`uninstall-rtw89win.ps1` 删除所有 Wi-Fi 驱动包，然后删除设备并重新扫描硬件。此后 RTL8852BE 显示为没有驱动的设备。该脚本保留安装时备份的 `SYSTEM`。

`uninstall-rtkbtflt.ps1` 删除所有蓝牙驱动包。蓝牙 USB 设备恢复为只使用 `BTHUSB`，控制器恢复运行 ROM 固件。设备本身保留，因此蓝牙配对记录也保留。该脚本保留安装时备份的 `SYSTEM`。

> **注意**：卸载 Wi-Fi 驱动后重新安装，网卡 GUID 将改变，原有的 WLAN 配置文件将失效。仅需更换版本时，请使用[升级驱动](#upgrade)的方法。

1. 在目标设备上：在驱动包目录中打开管理员 PowerShell 7。
2. 卸载 Wi-Fi 驱动。

   ```powershell
   pwsh -NoProfile -File .\uninstall-rtw89win.ps1
   ```

   **结果**：输出中没有 `failed`；最后的表格列出 RTL8852BE 设备，其 `Status` 不是 `OK`（没有驱动）。
3. 若要卸载蓝牙过滤驱动，在蓝牙驱动包目录中运行卸载脚本。

   ```powershell
   pwsh -NoProfile -File .\uninstall-rtkbtflt.ps1
   ```

   若硬件 ID 不是缺省值，加上 `-HardwareId '<硬件 ID>'`。

   **结果**：最后一行的 `stack:` 中没有 `\Driver\rtkbtflt`。
4. 若输出中有 `reboot`，重启电脑。

<a id="diagnostics"></a>
## 诊断命令

`rtwdiag.ps1` 位于 Wi-Fi 驱动包中。在驱动包目录中以管理员身份运行 `.\rtwdiag.ps1 <命令>`。该脚本通过诊断接口 `\\.\rtw89win` 与驱动通信，因此设备须已启动。`settings` 和 `country` 在设备未启动时也可使用。

| 命令 | 用途 |
|---|---|
| `mode` | 当前模式（WiFiCx 模式、诊断模式，或崩溃循环保护触发后的诊断模式），崩溃循环保护的计数，收到的 WDI 消息计数 |
| `log` | 驱动日志（内存中的最后 64 KB）；加 `-Follow` 持续显示新内容，按 Ctrl+C 停止 |
| `log -Saved` | 保存的日志：上一次运行结束时存下的驱动日志最后 16 KB |
| `settings` | 设置项的值、生效的国家码和信道宽度、当前模式 |
| `country [<国家码>]` | 查看或写入设置项 `Country` |
| `link` | 连接状态、信道和信道宽度、安全方式、数据收发计数 |
| `bss`、`bss -Ies` | 驱动保存的 BSS 列表；`-Ies` 解码信标中的元素 |
| `radio` | 管制规则、省电状态、共存摘要 |
| `btc`、`btc -Full` | 共存摘要；`-Full` 显示 rtw89 的完整报告，仅在芯片上电时可用 |
| `ap` | 移动热点的组主状态、热点客户端和计数 |
| `arm` | 清除崩溃循环保护的计数，重启设备，等待网卡启动完成 |

下列命令供开发使用：

| 命令 | 用途 |
|---|---|
| `ser`（开发用） | 触发一次固件错误恢复（SER）；连接将断开，然后自动重连 |
| `read <偏移>`（开发用） | 读取芯片的寄存器 |
| `probe`、`remove`、`start`、`stop`、`monitor`、`scan`、`txprobe`（开发用） | 仅在诊断模式下可用 |

蓝牙过滤驱动的诊断使用蓝牙驱动包中的两个工具：

| 命令 | 用途 |
|---|---|
| `.\rtkbtflt.ps1 status` | 蓝牙 USB 设备的状态、蓝牙过滤驱动的设置项、上一次下载的结果和日志 |
| `.\rtkbtflt.ps1 status -Download` | 另外显示上一次下载补丁的日志 |
| `.\rtkbtflt.ps1 clear` | 清除蓝牙过滤驱动的崩溃循环保护，然后重启蓝牙 USB 设备（需管理员权限） |
| `.\rtkbtfw.exe info` | Windows 蓝牙栈识别到的控制器版本；打过补丁时 `lmpSubversion` 为 `0xb20f` |

> **提示**：驱动日志以及 `rtwdiag.ps1 link`、`bss`、`ap` 的输出含网络名称（SSID）和 MAC 地址，`rtkbtfw.exe info` 的输出含蓝牙地址。将输出发给他人之前，请先遮盖这些信息。

<a id="troubleshooting"></a>
## 排障

先运行 `.\rtwdiag.ps1 mode` 和 `.\rtwdiag.ps1 log -Saved`，然后按现象查阅下表。

| 现象 | 原因 | 处理 | 详见 |
|---|---|---|---|
| 运行脚本时，PowerShell 报告"无法加载文件……因为在此系统上禁止运行脚本"，或报告文件"未进行数字签名" | 执行策略不允许运行脚本；或驱动包解压自下载的压缩包，文件带有"来自 Internet"的标记 | 在驱动包目录中运行 `Get-ChildItem \| Unblock-File`；或用 `pwsh -NoProfile -ExecutionPolicy Bypass -File .\<脚本>` 运行 | [安装 Wi-Fi 驱动](#install-wifi) |
| 安装脚本报告 `<文件名> hash mismatch` 并停止 | 驱动包中的文件与 `SHA256SUMS` 不符：复制不完整或文件被修改 | 从构建电脑重新复制整个驱动包目录 | [安装 Wi-Fi 驱动](#install-wifi) |
| 安装脚本报告 `RTL8852BE (PCI\VEN_10EC&DEV_B852) not present` | UEFI 未给 M.2 槽上电，Windows 无法识别模块 | 改用会给 M.2 槽上电的 UEFI | [PLAT-4](observed-behavior.md#plat-4) |
| 安装后，设备管理器中 RTL8852BE 带有黄色感叹号，没有网卡 | 测试签名模式未打开，Windows 不加载测试签名的驱动 | 按[打开测试签名模式](#test-signing)操作，然后重启电脑 | [打开测试签名模式](#test-signing) |
| 网卡显示"已禁用"或消失；`rtwdiag.ps1 mode` 显示 `diagnostic, because the crash loop guard tripped` | 崩溃循环保护已触发：连续两次运行未正常结束（蓝屏或断电） | 先用 `.\rtwdiag.ps1 log -Saved` 查看上一次的日志，然后运行 `.\rtwdiag.ps1 arm` | [INST-4](observed-behavior.md#inst-4) |
| 开机后网卡未启动；日志中有 `guard pending`，随后有 `registry still not writable` | 开机后 60 s 内注册表配置单元始终无法写盘，驱动按设计不操作芯片 | 重启电脑；用 `.\rtwdiag.ps1 log -Saved` 查看日志 | [INST-4](observed-behavior.md#inst-4) |
| `netsh wlan show networks`，或已连接时 `netsh wlan show interfaces` 报告"拒绝访问" | Windows 的定位服务已关闭 | 打开"设置 → 隐私和安全性 → 位置"；或用任务栏 Wi-Fi 列表、`Get-NetAdapter` 查看 | [SCAN-7](observed-behavior.md#scan-7) |
| 更换版本后，连接时报告 `There is no profile` | 先卸载后安装，网卡 GUID 已改变 | 今后使用[升级驱动](#upgrade)的方法；本次在 Wi-Fi 列表中重新连接并输入密码 | [INST-2](observed-behavior.md#inst-2) |
| 已连接，但 ping 不通 | 新网络为"公用网络"，Windows 防火墙丢弃回显请求 | 用 `Test-NetConnection <地址> -Port <端口>` 测试 TCP 连接；如需使用 ping，在防火墙中放行 ICMPv4 回显 | [INST-8](observed-behavior.md#inst-8) |
| 重启电脑或重启设备后未自动连接 | WLAN 配置文件未勾选"自动连接" | 在 Wi-Fi 设置中勾选"自动连接"；或运行 `netsh wlan connect name=<配置文件名>` | [CONN-8](observed-behavior.md#conn-8) |
| 修改设置项后没有效果 | 驱动仅在设备启动时读取设置项 | 重启设备或重启电脑，然后重新连接 | [修改设置](#settings) |
| `pnputil /restart-device` 失败，要求重启电脑 | 升级后设备被标记为"需要重启电脑" | 重启电脑 | [INST-3](observed-behavior.md#inst-3) |
| 驱动日志中有 `[ERR]FWDL path ready`，设备启动失败 | 设备曾被重新枚举，芯片在断电前无法下载固件 | 重启电脑（芯片断电） | [INST-6](observed-behavior.md#inst-6) |
| 重启设备或 SER 后，移动热点已关闭 | Windows 不会自动重新打开热点 | 在"设置 → 移动热点"中重新打开 | [HS-13](observed-behavior.md#hs-13) |
| 设置热点频段时报告 `BandInterference` | 站点已连接时，热点须与站点使用同一信道 | 频段选择"任何可用"或站点所在的频段 | [HS-12](observed-behavior.md#hs-12) |
| 移动热点自动关闭 | 约 5 分钟内没有热点客户端，Windows 自动关闭热点 | 重新打开 | [HS-13](observed-behavior.md#hs-13) |
| 网卡列表中有 `WLAN 2`、`WLAN 3` | 它们是 Wi-Fi Direct 设备端口和角色端口 | 无需处理；不使用移动热点时，可将 `WifiDirect` 设为 `0` 并重启设备 | [HS-3](observed-behavior.md#hs-3) |
| 无法连接 WPA3-SAE 网络 | `Pmf` 缺省为 `0`，驱动不报告 SAE 能力 | 将 `Pmf` 设为 `2`，然后重启设备 | [修改设置](#settings) |
| `Pmf` 为 `2` 时，过渡模式网络总是以 WPA3-SAE 连接 | Windows 将 WPA2-个人的配置文件升级为 SAE | 仅需 WPA2-个人加管理帧保护时，将 `Pmf` 设为 `1` | [SEC-3](observed-behavior.md#sec-3) |
| WPA3-SAE 网络在 SER 后断开，且不自动重连 | 测过的接入点（AP-3）在"稍后再来"期间丢失 SAE 状态；其它接入点未测 | 手动重新连接 | [SEC-4](observed-behavior.md#sec-4) |
| 交互延迟约 100 ms | 省电已打开 | 将 `PowerSave` 设为 `0` 或删除该值，然后重启设备 | [PS-3](observed-behavior.md#ps-3) |
| 2.4 GHz 吞吐低 | 原因未确定（推测为频段拥挤，未经测量） | 改用 5 GHz（缺省最宽 VHT80） | [验证状态](validation.md#station) |
| 5 GHz 连接不是 VHT80 | `Bandwidth` 设为 `20` 或 `40`；或接入点、管制规则不允许 80 MHz | 运行 `.\rtwdiag.ps1 settings`，查看 `Bandwidth in effect:`；在驱动日志中查找 `limited by:` | [修改设置](#settings) |
| 5 GHz 的部分网络扫描不到，或只能被动发现 | 没有国家码时使用全球通用规则，5 GHz 只能被动扫描 | 设置国家码：`.\rtwdiag.ps1 country <国家码>`，然后重启设备 | [REG-3](observed-behavior.md#reg-3) |
| 蓝牙无法扫描到设备，也无法配对 | 未安装蓝牙过滤驱动，控制器只运行 ROM 固件 | 安装蓝牙过滤驱动 | [BT-1](observed-behavior.md#bt-1) |
| 已安装蓝牙过滤驱动，蓝牙仍不工作；`rtkbtflt.ps1 status` 显示 `BadSessions 2` | 蓝牙过滤驱动的崩溃循环保护已触发，该驱动只转发 I/O，不下载蓝牙补丁 | 先查看 `rtkbtflt.ps1 status` 输出的日志，然后运行 `.\rtkbtflt.ps1 clear` | [诊断命令](#diagnostics) |
| `rtkbtfw info` 显示的 `lmpSubversion` 不是 `0xb20f`；`rtkbtflt.ps1 status` 的 `stack:` 中没有 `\Driver\rtkbtflt` | 蓝牙过滤驱动未安装在该设备上：硬件 ID 与 INF 不符，或安装未完成 | 按[安装蓝牙过滤驱动](#install-bt)第 3、4 步核对硬件 ID，然后重新安装 | [安装蓝牙过滤驱动](#install-bt) |
| `lmpSubversion` 不是 `0xb20f`；`rtkbtflt.ps1 status` 显示 `PassThrough 1` | 设置项 `PassThrough` 为 `1`，蓝牙过滤驱动处于直通状态 | 将 `PassThrough` 设为 `0` 或删除该值，然后重启电脑 | [修改设置](#settings) |
| `rtkbtfw info` 报告 `no Bluetooth radio present` | Windows 中的蓝牙已关闭，蓝牙 USB 设备处于 D3 | 在"设置 → 蓝牙和其他设备"中打开蓝牙，然后重新运行 `.\rtkbtfw.exe info`。`rtkbtflt.ps1 status` 显示 `LastResult 0` 时，表示补丁下载成功 | [诊断命令](#diagnostics) |
| Windows 启动时蓝屏，无法进入桌面 | Wi-Fi 驱动或蓝牙过滤驱动在启动时出错 | 按[Windows 无法启动时恢复](#recovery)操作 | [Windows 无法启动时恢复](#recovery) |

<a id="recovery"></a>
## Windows 无法启动时恢复

若安装后 Windows 启动时蓝屏，请先尝试方法零。方法零无效时，使用方法一或方法二。方法一仅停用驱动；方法二将整个 `SYSTEM` 注册表配置单元恢复到安装前的状态。

判断蓝屏与哪个驱动有关：查看蓝屏画面上"失败的操作"一行。该行为 `rtw89win.sys` 时，与 Wi-Fi 驱动有关；为 `rtkbtflt.sys` 时，与蓝牙过滤驱动有关。没有该行时，两个驱动均按下述方法处理。

### 方法零：由崩溃循环保护停用驱动

两个驱动均具有崩溃循环保护。连续两次运行未正常结束后，Wi-Fi 驱动在下次启动时只进入诊断模式，不操作芯片。蓝牙过滤驱动则只直通。该方法仅经过模拟测试，未在真实蓝屏下测试（见[验证状态](validation.md#untested)）。

1. 重启电脑。若再次蓝屏，再重启电脑一次。

   **结果**：最多再重启两次后，Windows 进入桌面。若仍然蓝屏，改用方法一或方法二。
2. 在 Wi-Fi 驱动包目录中打开管理员 PowerShell 7。
3. 保存上一次运行的日志。

   ```powershell
   .\rtwdiag.ps1 log -Saved > saved-log.txt
   ```

4. 运行对应的卸载脚本（见[卸载驱动](#uninstall)）。

### 方法一：在目标设备的 Linux 系统中停用驱动

该方法要求目标设备上另装有 Linux 系统，且其内核带有 `ntfs3` 驱动。回滚脚本位于驱动包中：Wi-Fi 为 `rollback-rtw89win.sh`，蓝牙为 `rollback-rtkbtflt.sh`。脚本将 Windows 驱动存储区（`DriverStore`）中的 `.sys` 文件改名，Windows 便不再加载该文件。

方法一和方法二均须以读写方式挂载 Windows 分区。若 Windows 处于休眠状态，或上次关机使用了快速启动，`ntfs3` 只能以只读方式挂载，改名和覆盖都将失败。

1. 启动目标设备上的 Linux 系统。
2. 将回滚脚本复制到 Linux 系统中。
3. 列出 NTFS 分区。

   ```sh
   sudo sh rollback-rtw89win.sh
   ```

   **结果**：脚本打印用法和 NTFS 分区列表。
4. 以 Windows 分区为参数运行回滚脚本。

   ```sh
   sudo sh rollback-rtw89win.sh <Windows 分区>
   ```

   `<Windows 分区>`：第 3 步列表中 Windows 所在分区的设备文件，形如 `/dev/<分区名>`。

   **结果**：输出 `disabled <路径>/rtw89win.sys`。
5. 若蓝屏与蓝牙过滤驱动有关，用 `rollback-rtkbtflt.sh` 重复第 3、4 步。
6. 启动 Windows。
7. 运行对应的卸载脚本（见[卸载驱动](#uninstall)）。

### 方法二：恢复安装前备份的注册表配置单元

> **注意**：该方法将整个 `SYSTEM` 注册表配置单元恢复到安装时的状态，此后对 `SYSTEM` 的所有修改都将丢失。请先保存一份当前的 `SYSTEM` 文件。

1. 从另一个系统（例如目标设备上的 Linux 系统）打开 Windows 分区。
2. 找到安装脚本的备份：`ProgramData\rtw89win\backup\rtw89win-<时间>\SYSTEM.hiv`（蓝牙为 `rtkbtflt-<时间>`）。若安装时给出了 `-WorkDir`，请在该目录下查找。
3. 将当前的 `Windows\System32\config\SYSTEM` 复制一份保存。
4. 用 `SYSTEM.hiv` 覆盖 `Windows\System32\config\SYSTEM`。
5. 启动 Windows。

<a id="limits"></a>
## 已知限制

- 仅支持 2.4 GHz 和 5 GHz。2.4 GHz 仅支持 HT20；5 GHz 最宽为 VHT80。不提供 802.11ax（HE）和 160 MHz。
- 支持的安全类型：WPA2-个人、WPA3-SAE（缺省关闭）、管理帧保护（缺省关闭）。不支持 WEP、TKIP 和企业网络（802.1X）。开放网络未经测试。
- 移动热点仅支持 WPA2-个人和 HT20。站点已连接时，热点只能与站点使用同一信道。
- 省电缺省跟随 Windows，实测效果等同于不省电。
- 崩溃循环保护触发后，网卡将消失，直至运行 `rtwdiag.ps1 arm`。
- 使用蓝牙须另行安装蓝牙过滤驱动。
- 仅在 ROCK 5T 上测试过。
- 设备被重新枚举后，芯片在断电前无法下载固件（[INST-6](observed-behavior.md#inst-6)）。`pnputil /restart-device`、禁用后再启用、本文的卸载脚本均不会触发重新枚举。

完整的限制和未测项目见[验证状态](validation.md#limits)。术语的定义见[术语表](glossary.md)。
