# Installation and use

[中文](../zh/install.md)

- **Readers**: Users. A user installs and uses this driver on the target device.
- **What you will do**: Install the Wi-Fi driver and the Bluetooth filter driver on the target device. Then connect to a network, use the mobile hotspot, change settings, and solve common problems.
- **Prerequisites**: You built the Wi-Fi driver package and the Bluetooth driver package (see [Build](build.md)). You have administrator rights on the target device. PowerShell 7 is installed on the target device.

<a id="summary"></a>
## Summary

1. Enable test-signing mode before the installation. Disable Secure Boot before you enable this mode.
2. The install script identifies a first installation or an in-place upgrade automatically. When you change the version, do not uninstall first. If you uninstall first, the WLAN profiles become invalid.
3. At installation, use `-Country` to set the country code of the country where the target device is. Without a country code, the driver can do only passive scans on 5 GHz before a connection.
4. For Bluetooth, install the Bluetooth filter driver separately. Without it, Bluetooth scans find no devices, and Bluetooth cannot pair.
5. If a problem occurs, run `rtwdiag.ps1 mode` first. Then use `rtwdiag.ps1 log -Saved` to read the saved log.

Unless this document states differently, run all commands on the target device. "Administrator PowerShell 7" means `pwsh` that runs as administrator. In this document, "the device" means the PCI device of the RTL8852BE (the Wi-Fi part of the module). "Restart the computer" means a restart of Windows on the target device.

<a id="before"></a>
## Before you start

On the target device (administrator PowerShell 7), examine these items:

1. Make sure that Windows identifies the Wi-Fi part of the module.

   ```powershell
   Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*'
   ```

   **Result**: The output has one line. If there is no output, the UEFI does not supply power to the M.2 slot. See [PLAT-4](observed-behavior.md#plat-4).
2. Make sure that the Windows build number is 26100 or higher.

   ```powershell
   [Environment]::OSVersion.Version.Build
   ```

   **Result**: The number is not less than `26100`. The INF of the driver installs only on these versions. The tests used Windows 11 IoT Enterprise LTSC 2024.
3. Make sure that PowerShell 7 is available.

   ```powershell
   $PSVersionTable.PSVersion
   ```

   **Result**: `Major` is `7`. If it is not `7`, install PowerShell. See [Install PowerShell on Windows](https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-windows) in the Microsoft documentation.
4. Make sure that you have the two driver package directories. They are two directories in `out\` on the build PC:

   | Driver package | Directory name | Necessary files |
   |---|---|---|
   | Wi-Fi driver package | `out\<time>\` | `rtw89win.sys`, `rtw89win.inf`, `rtw89win.cat`, `rtw8852b_fw-2.bin`, `install-rtw89win.ps1`, `rtwdiag.ps1`, `LICENSE`, `THIRD_PARTY_NOTICES`, `SHA256SUMS` |
   | Bluetooth driver package | `out\rtkbtflt-<time>\` | `rtkbtflt.sys`, `rtkbtflt.inf`, `rtkbtflt.cat`, `rtl8852bu_fw.bin`, `rtl8852bu_config.bin`, `rtkbtfw.exe`, `install-rtkbtflt.ps1`, `LICENSE`, `THIRD_PARTY_NOTICES`, `SHA256SUMS` |

   The driver packages contain the firmware. You do not have to add the firmware at installation.

<a id="test-signing"></a>
## Enable test-signing mode

This driver has a test signature. Windows loads this driver only in test-signing mode. Disable Secure Boot before you enable test-signing mode.

> **Caution**: Do these steps only on a computer that you own. When you disable Secure Boot and enable test-signing mode, the security protection of Windows decreases.

> **Caution**: Save a copy of the BitLocker recovery key first. If the Windows partition uses BitLocker, Windows asks for the recovery key after a change to the Secure Boot setting.

1. Restart the computer into the UEFI setup of the target device.
2. If Secure Boot is enabled, disable it. The menu location is different for each UEFI.
3. Save the UEFI settings. Then start Windows.
4. Open administrator PowerShell 7.
5. Enable test-signing mode.

   ```powershell
   bcdedit /set testsigning on
   ```

   **Result**: `bcdedit` reports that the operation completed successfully. If it shows an error about Secure Boot, do step 1 again.
6. Restart the computer.
7. Make sure that test-signing mode is enabled.

   ```powershell
   bcdedit /enum '{current}'
   ```

   **Result**: In the output, the `testsigning` line has the value `Yes`. The desktop shows the Windows "Test Mode" watermark in the lower-right corner.

<a id="install-wifi"></a>
## Install the Wi-Fi driver

The install script `install-rtw89win.ps1` does these operations in sequence:

1. It compares each file in the driver package with `SHA256SUMS`. If one file is different, the script stops.
2. It uses `reg save` to save a backup of the `SYSTEM` registry hive to `<work directory>\backup\rtw89win-<time>\SYSTEM.hiv`.
3. If you give `-Country`, it writes the value to the setting `Country`.
4. During the installation, it trusts the test certificate `rtw89win-test.cer` from the driver package temporarily. After the installation, it removes the certificate.
5. It does a first installation, or an in-place upgrade (when the device already uses this driver).
6. It shows the device status and the interface GUID (before and after the installation). It also shows the output of `rtwdiag.ps1 mode` and the driver log.

| Script parameter | Value | Default |
|---|---|---|
| `-Country` | A two-letter country code, or `00` (no country code) | Not written; the current `Country` value does not change |
| `-WorkDir` | A directory for the backups and for a copy of the script output | `%ProgramData%\rtw89win` |

The script saves a copy of its output in `<work directory>\logs\rtw89win-install.log`. The copy does not contain the output of `rtwdiag.ps1 mode` and the driver log at the end.

> **Caution**: Set the country code of the country where the target device is. Do not set the country code of a different country. The country code controls which channels allow active transmission. It also controls the transmit power.

> **Note**: The tests did not run the first installation or the parameter `-WorkDir` on the target device (see [Validation status](validation.md#install)). If the output is different from this document, keep `<work directory>\logs\rtw89win-install.log`. Then go to [Problems and solutions](#troubleshooting).

For a first installation with `-Country`, the country code becomes effective at the next start of the device. For this reason, you must restart the device in step 5.

1. On the build PC: copy the full Wi-Fi driver package directory to the target device. You can use a USB flash drive or a network share.
2. On the target device: open administrator PowerShell 7.
3. Go to the driver package directory.

   ```powershell
   cd <driver package directory>
   ```

   `<driver package directory>`: the Wi-Fi driver package directory that you copied to the target device.
4. Run the install script.

   ```powershell
   pwsh -NoProfile -File .\install-rtw89win.ps1 -Country <country code>
   ```

   `<country code>`: two letters, for example `DE`. If you do not set a country code, remove `-Country <country code>`.

   **Result**: The line after `=== verify package` is `ok`. After `=== result`, there is a line `mode: WiFiCx`.
5. If the output contains `applies from the next start of the device`, restart the device (see [Restart the device](#restart-device)).
6. Make sure that Windows shows the network adapter of this driver.

   ```powershell
   Get-NetAdapter | Where-Object InterfaceDescription -like '*rtw89win*'
   ```

   **Result**: One line has the `InterfaceDescription` `Realtek RTL8852BE Wi-Fi (rtw89win)` and a `Status` that is not `Disabled`. Until the next computer restart, the output can also contain a network adapter with the `Status` `Disabled`. This adapter is the role port of the mobile hotspot. This is normal ([HS-3](observed-behavior.md#hs-3)).
7. If you gave `-Country`, make sure that the country code is correct.

   ```powershell
   .\rtwdiag.ps1 country
   ```

   **Result**: The `in effect:` line shows your country code and `from user (Parameters\Country)`.

> **Note**: Keep the driver package directory on the target device. It contains the diagnostic script `rtwdiag.ps1` and the uninstall script.

<a id="connect"></a>
## Connect to a network

The driver supports WPA2-Personal and WPA3-SAE. WPA3-SAE and protected management frames (PMF) are disabled by default. To enable them, see [Change settings](#settings).

On the target device (for step 4, use administrator PowerShell 7 in the Wi-Fi driver package directory):

1. Click the network icon in the lower-right corner of the taskbar. The Wi-Fi list opens.
2. Select the network. Click **Connect**.
3. Type the password.
4. Make sure that the connection is active.

   ```powershell
   .\rtwdiag.ps1 link
   ```

   **Result**: The first line is `state: connected`.

For a saved network, you can also use the command line to connect:

```powershell
netsh wlan connect name=<profile name>
```

`<profile name>`: the name of the WLAN profile. By default, Windows uses the network name as the profile name.

- When the Windows location service is off, `netsh wlan show networks` reports "Access is denied". When the station is connected, `netsh wlan show interfaces` also reports it ([SCAN-7](observed-behavior.md#scan-7)). In this condition, use the Wi-Fi list in the taskbar or `Get-NetAdapter`.
- Some WLAN profiles do not have **Connect automatically**. These profiles do not connect again automatically after you restart the device or the computer. They also do not connect again after the access point disconnects ([CONN-8](observed-behavior.md#conn-8)).
- A new network is a "Public network". Windows Firewall drops ping echo requests ([INST-8](observed-behavior.md#inst-8)).

<a id="install-bt"></a>
## Install the Bluetooth filter driver

The Bluetooth part of the module is a USB device. This document calls it the "Bluetooth USB device". The Bluetooth USB driver of Windows (`BTHUSB`) identifies this device. But the controller runs only the ROM firmware. In this condition, Bluetooth scans find no devices, and Bluetooth cannot pair ([BT-1](observed-behavior.md#bt-1)).

The Bluetooth filter driver downloads the Bluetooth patch each time the Bluetooth USB device starts or resumes from the D3 power state. Each download takes approximately 0.5 s.

The install script `install-rtkbtflt.ps1` does these operations in sequence:

1. It compares each file in the driver package with `SHA256SUMS`.
2. It saves a backup of the `SYSTEM` registry hive to `<work directory>\backup\rtkbtflt-<time>\SYSTEM.hiv`.
3. It trusts the test certificate temporarily and installs the driver package. After the installation, it removes the certificate.
4. It makes sure that the Bluetooth filter driver is loaded. If the driver is not loaded, the script restarts the Bluetooth USB device.
5. It shows the output of `rtkbtflt.ps1 status` and `rtkbtfw info`.

The Bluetooth USB device continues to use `BTHUSB` from Windows. The Bluetooth filter driver installs below `BTHUSB`.

| Script parameter | Value | Default |
|---|---|---|
| `-HardwareId` | The hardware ID of the Bluetooth USB device | `USB\VID_13D3&PID_3572` (the module on the ROCK 5T) |
| `-WorkDir` | A directory for the backups and for a copy of the script output | `%ProgramData%\rtw89win` |

1. On the build PC: copy the full Bluetooth driver package directory to the target device.
2. On the target device: open administrator PowerShell 7.
3. Find the hardware ID of the Bluetooth USB device.

   ```powershell
   Get-PnpDevice -PresentOnly -Class Bluetooth | Where-Object InstanceId -like 'USB\*' | Format-List FriendlyName, InstanceId
   ```

   **Result**: `InstanceId` starts with `USB\VID_xxxx&PID_xxxx\`. The first two parts are the hardware ID.
4. If the hardware ID is not `USB\VID_13D3&PID_3572`, change the INF and build the Bluetooth driver package again. See the [Porting guide](porting.md#new-board). Then start again at step 1.
5. Go to the Bluetooth driver package directory.

   ```powershell
   cd <driver package directory>
   ```

   `<driver package directory>`: the Bluetooth driver package directory that you copied to the target device.
6. Run the install script. If the hardware ID is not the default, add `-HardwareId '<hardware ID>'` at the end of the command.

   ```powershell
   pwsh -NoProfile -File .\install-rtkbtflt.ps1
   ```

   **Result**: The output of `rtkbtfw info` contains `lmpSubversion 0xb20f`.
7. If the output does not contain `0xb20f`, run `.\rtkbtflt.ps1 status`. Then use the [Problems and solutions](#troubleshooting) table.
8. Open **Settings > Bluetooth & devices**. Add a device.

   **Result**: The list shows the Bluetooth devices near the target device.

<a id="settings"></a>
## Change settings

The settings of the Wi-Fi driver are in the registry key `HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters`. The driver reads the settings one time at each start of the device. For this reason, restart the device after you change a setting.

| Setting | Type | Values | Default (no value) | Function |
|---|---|---|---|---|
| `Country` | REG_SZ | Two letters; `00` means no country code | No country code | Country code |
| `Bandwidth` | REG_DWORD | `20`, `40`, `80` | `80` | Maximum channel width of the station on 5 GHz |
| `PowerSave` | REG_DWORD | `0` no power save; `1` follow Windows; `2` always power save | `1` | Power save |
| `Pmf` | REG_DWORD | `0` disabled; `1` PMF; `2` PMF and WPA3-SAE | `0` | PMF and WPA3-SAE |
| `WifiDirect` | REG_DWORD | `0` disabled; `1` enabled | `1` | Mobile hotspot |

- `Country`: This setting controls which channels allow active transmission. It also controls the transmit power. Without this value, the driver tries three sources in sequence. These sources are the country code from the connected access point, the country code in the efuse, and the world rules. The module on the ROCK 5T has no country code in the efuse. A country code that is not in wireless-regdb has no effect, and the driver log records this condition.
- `Bandwidth`: `20`, `40`, and `80` select HT20, HT40, and VHT80. This setting has no effect on 2.4 GHz and on the mobile hotspot. The driver uses `80` for other values.
- `PowerSave`: With "follow Windows", the measured result is the same as no power save ([PS-1](observed-behavior.md#ps-1)). The driver uses `1` for other values.
- `Pmf`: This setting has an effect only when the encryption self-test of the driver passes. The driver uses `0` for other values.
- `WifiDirect`: When the value is `0`, the driver does not report the Wi-Fi Direct capability to Windows. The mobile hotspot is then not available.

These settings are only for debug. If you do not debug the driver, do not set these values.

| Setting | Type | Values | Default | Function |
|---|---|---|---|---|
| `AllowConnect` (debug only) | REG_DWORD | `0` refuses all connections; other values allow them | Allow | Connection gate |
| `Allow\AllowSsid` (debug only) | REG_SZ, in the subkey `Allow` | One network name | No limit | Allows connections only to the network with this name |
| `WdiDump` (debug only) | REG_DWORD | `1` enabled | Disabled | Writes the first message of each WDI message type to the driver log |

- The driver reads `AllowConnect` and `AllowSsid` at each connection. After a change, you do not have to restart the device. `AllowConnect` in the subkey `Allow` has priority over the value with the same name in `Parameters`.
- `WdiDump` does not record messages that contain keys. When you finish, delete this value.

The driver writes `ArmRunning`, `BootAttempts`, and `SavedLog`. Do not change these values. The first two values are part of the crash loop guard. `SavedLog` is the saved log.

The settings of the Bluetooth filter driver are in `HKLM\SYSTEM\CurrentControlSet\Services\rtkbtflt\Parameters`:

| Setting | Type | Values | Default | Function |
|---|---|---|---|---|
| `PassThrough` | REG_DWORD | `0` normal; `1` pass-through | `0` | When the value is `1`, the driver does not download the Bluetooth patch and only forwards I/O (pass-through) |

`PassThrough` becomes effective at the next start of the Bluetooth USB device. An upgrade keeps this value. The Bluetooth filter driver writes `SessionRunning` and `BadSessions`. These values are part of its crash loop guard.

### Change a setting

On the target device, open administrator PowerShell 7 in the Wi-Fi driver package directory:

1. To change the country code, run `rtwdiag.ps1 country`.

   ```powershell
   .\rtwdiag.ps1 country <country code>
   ```

2. To change a different setting, run `reg add`.

   ```powershell
   reg add HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters /v <setting> /t REG_DWORD /d <value> /f
   ```

   `<setting>`: a name from the table above, for example `Bandwidth`. `<value>`: a number from the "Values" column of the table.
3. Restart the device (see [Restart the device](#restart-device)).
4. Make sure that the new value is effective.

   ```powershell
   .\rtwdiag.ps1 settings
   ```

   **Result**: The output shows the value of each setting. The `in effect:` line shows the effective country code. The `Bandwidth in effect:` line shows the effective channel width.
5. If the network does not connect again automatically, connect to the network again.

To use the default value again, delete the value. Then restart the device:

```powershell
reg delete HKLM\SYSTEM\CurrentControlSet\Services\rtw89win\Parameters /v <setting> /f
```

<a id="restart-device"></a>
### Restart the device

This procedure stops the PCI device of the RTL8852BE and starts it again. It does not restart the computer. The network connection stops.

1. Restart the device.

   ```powershell
   $id = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'PCI\VEN_10EC&DEV_B852*').InstanceId
   pnputil /restart-device $id
   ```

   **Result**: `pnputil` reports that the device restarted. If `pnputil` reports that a computer restart is necessary, restart the computer ([INST-3](observed-behavior.md#inst-3)).

`.\rtwdiag.ps1 arm` also restarts the device. This command also clears the counters of the crash loop guard. Then it waits until the network adapter completes its start.

<a id="hotspot"></a>
## Use the mobile hotspot

The mobile hotspot makes the target device operate as an access point. The target device then shares its network connection. With a WiFiCx driver, Windows uses Wi-Fi Direct for the mobile hotspot. When the hotspot operates, the target device is the group owner (GO).

On the target device (for step 5, use administrator PowerShell 7 in the Wi-Fi driver package directory):

1. Open **Settings > Network & internet > Mobile hotspot**.
2. Select the connection to share.
3. Click **Edit**. Set the network name, the password, and the band.
4. Set the **Mobile hotspot** switch to **On**.
5. Make sure that the group owner operates.

   ```powershell
   .\rtwdiag.ps1 ap
   ```

   **Result**: The first line is `GO: running`. The band and the channel are on the lines after it.
6. On the hotspot client, connect to this network.

Limits:

- The only security type is WPA2-Personal. The only channel width is HT20.
- When the station is connected, the hotspot must use the same channel as the station. Select **Any available** or the band of the station. If you select a different band, Windows reports `BandInterference` ([HS-12](observed-behavior.md#hs-12)).
- A 5 GHz hotspot uses only channels where the regulatory rules allow active transmission and where radar detection is not necessary. With the world rules, the hotspot can use only 2.4 GHz.
- If a system error recovery (SER) occurs while the hotspot is on, the hotspot stops. A device restart also stops the hotspot. Windows does not start the hotspot again automatically. Start it again manually ([HS-13](observed-behavior.md#hs-13)).
- If the hotspot has no hotspot clients for approximately 5 minutes, Windows stops the hotspot ([HS-13](observed-behavior.md#hs-13)).
- The driver reports to Windows that it supports a maximum of 8 hotspot clients. The tests used only 1 hotspot client. The tests did not include traffic between hotspot clients.
- When hotspot clients are connected, the station scans only the current channel.
- The entries `WLAN 2` and `WLAN 3` in the network adapter list are normal. They are the Wi-Fi Direct device port and the role port ([HS-3](observed-behavior.md#hs-3)).

If you do not use the mobile hotspot, you can set `WifiDirect` to `0`. Then restart the device.

<a id="upgrade"></a>
## Upgrade the driver

Use the install script in the new driver package for the upgrade. The install script does an in-place upgrade. The device, the interface GUID, and the WLAN profiles do not change.

> **Caution**: Do not uninstall the driver and then install it again. If you do this, the interface GUID changes, and all current WLAN profiles become invalid ([INST-2](observed-behavior.md#inst-2)).

1. On the build PC: copy the new Wi-Fi driver package to the target device.
2. On the target device: open administrator PowerShell 7 in the new driver package directory.
3. Run the install script of the new driver package.

   ```powershell
   pwsh -NoProfile -File .\install-rtw89win.ps1
   ```

   The country code does not change. To change the country code at the same time, add `-Country <country code>` at the end of the command. In an in-place upgrade, the new country code becomes active when the new driver starts.

   **Result**: The output contains `in-place upgrade`. The `NetCfgInstanceId after:` line ends with `kept`.
4. If the output contains `a reboot is needed`, restart the computer.
5. Make sure that the new driver runs.

   ```powershell
   pwsh -NoProfile -File .\rtwdiag.ps1 log | Select-String 'rtw89win .* loaded'
   ```

   **Result**: The last line is `rtw89win <version> loaded`. `<version>` is the version of the new driver package. In the output of step 3, the version after `driver running:` is also this version.

To upgrade the Bluetooth filter driver, use the same method: run `install-rtkbtflt.ps1` from the new Bluetooth driver package. The two install scripts also accept a driver package with the same version or an older version. In this condition, the script forces an update of the driver on the device.

<a id="uninstall"></a>
## Uninstall the drivers

`uninstall-rtw89win.ps1` deletes all Wi-Fi driver packages. Then it removes the device and tells Windows to find the hardware again. After this, the RTL8852BE shows as a device without a driver. The script keeps the `SYSTEM` backups from the installation.

`uninstall-rtkbtflt.ps1` deletes all Bluetooth driver packages. The Bluetooth USB device then uses only `BTHUSB` again, and the controller runs the ROM firmware again. The script does not remove the device, so the Bluetooth pairing records stay. The script keeps the `SYSTEM` backups from the installation.

> **Caution**: To change only the version, use the method in [Upgrade the driver](#upgrade). If you uninstall the Wi-Fi driver and install it again, the interface GUID changes. The current WLAN profiles then become invalid.

1. On the target device: open administrator PowerShell 7 in the driver package directory.
2. Uninstall the Wi-Fi driver.

   ```powershell
   pwsh -NoProfile -File .\uninstall-rtw89win.ps1
   ```

   **Result**: The output does not contain `failed`. The last table shows the RTL8852BE device, and its `Status` is not `OK` (no driver).
3. To uninstall the Bluetooth filter driver, run the uninstall script in the Bluetooth driver package directory.

   ```powershell
   pwsh -NoProfile -File .\uninstall-rtkbtflt.ps1
   ```

   If the hardware ID is not the default, add `-HardwareId '<hardware ID>'`.

   **Result**: The `stack:` value in the last line does not contain `\Driver\rtkbtflt`.
4. If the output contains `reboot`, restart the computer.

<a id="diagnostics"></a>
## Diagnostic commands

`rtwdiag.ps1` is in the Wi-Fi driver package. Run `.\rtwdiag.ps1 <command>` as administrator in the driver package directory. The script communicates with the driver through the diagnostic interface `\\.\rtw89win`. For this reason, the device must be started. `settings` and `country` also operate when the device is not started.

| Command | Purpose |
|---|---|
| `mode` | The current mode, the counters of the crash loop guard, and the counts of received WDI messages. The mode is WiFiCx mode, diagnostic mode, or diagnostic mode because the crash loop guard triggered. |
| `log` | The driver log (the last 64 KB in memory). Add `-Follow` to show new entries continuously. Push Ctrl+C to stop. |
| `log -Saved` | The saved log: the last 16 KB of the driver log at the end of the previous run |
| `settings` | The values of the settings, the effective country code and channel width, and the current mode |
| `country [<country code>]` | Shows or writes the setting `Country` |
| `link` | The connection state, the channel and the channel width, the security type, and the data counters |
| `bss`, `bss -Ies` | The BSS list of the driver. `-Ies` decodes the elements in the beacons. |
| `radio` | The regulatory rules, the power save state, and a coexistence summary |
| `btc`, `btc -Full` | A coexistence summary. `-Full` shows the full rtw89 report. This option is available only when the chip has power. |
| `ap` | The group owner state of the mobile hotspot, the hotspot clients, and the counters |
| `arm` | Clears the counters of the crash loop guard, restarts the device, and waits until the network adapter completes its start |

These commands are for development:

| Command | Purpose |
|---|---|
| `ser` (development only) | Starts one SER. The connection stops and then connects again automatically. |
| `read <offset>` (development only) | Reads a chip register |
| `probe`, `remove`, `start`, `stop`, `monitor`, `scan`, `txprobe` (development only) | Available only in diagnostic mode |

For the Bluetooth filter driver, use the two tools in the Bluetooth driver package:

| Command | Purpose |
|---|---|
| `.\rtkbtflt.ps1 status` | The state of the Bluetooth USB device, the settings of the Bluetooth filter driver, and the result and the log of the last download |
| `.\rtkbtflt.ps1 status -Download` | Also shows the log of the last patch download |
| `.\rtkbtflt.ps1 clear` | Clears the crash loop guard of the Bluetooth filter driver. Then restarts the Bluetooth USB device. Administrator rights are necessary. |
| `.\rtkbtfw.exe info` | The controller version that the Windows Bluetooth stack identifies. With the patch, `lmpSubversion` is `0xb20f`. |

> **Note**: Before you send output to other persons, hide this data. The driver log and the output of `rtwdiag.ps1 link`, `bss`, and `ap` contain network names (SSID) and MAC addresses. The output of `rtkbtfw.exe info` contains the Bluetooth address.

<a id="troubleshooting"></a>
## Problems and solutions

First, run `.\rtwdiag.ps1 mode` and `.\rtwdiag.ps1 log -Saved`. Then find the symptom in the table below.

| Symptom | Cause | Solution | Details |
|---|---|---|---|
| When you run a script, PowerShell reports "cannot be loaded because running scripts is disabled on this system". Or it reports that the file "is not digitally signed". | The execution policy does not allow scripts. Or the driver package comes from a downloaded archive, and the files have the "from the Internet" mark. | In the driver package directory, run `Get-ChildItem \| Unblock-File`. Or run the script with `pwsh -NoProfile -ExecutionPolicy Bypass -File .\<script>`. | [Install the Wi-Fi driver](#install-wifi) |
| The install script reports `<file name> hash mismatch` and stops | A file in the driver package does not agree with `SHA256SUMS`. The copy is not complete, or the file has changes. | Copy the full driver package directory from the build PC again | [Install the Wi-Fi driver](#install-wifi) |
| The install script reports `RTL8852BE (PCI\VEN_10EC&DEV_B852) not present` | The UEFI does not supply power to the M.2 slot. Windows cannot identify the module. | Use a UEFI that supplies power to the M.2 slot | [PLAT-4](observed-behavior.md#plat-4) |
| After the installation, the RTL8852BE in Device Manager has a yellow exclamation mark. There is no network adapter. | Test-signing mode is not enabled. Windows does not load drivers with a test signature. | Do the steps in [Enable test-signing mode](#test-signing). Then restart the computer. | [Enable test-signing mode](#test-signing) |
| The network adapter shows "Disabled" or is not visible. `rtwdiag.ps1 mode` shows `diagnostic, because the crash loop guard tripped`. | The crash loop guard triggered. Two runs in sequence did not end correctly. The cause was a bug check (the Windows blue screen) or power loss. | First, use `.\rtwdiag.ps1 log -Saved` to read the log of the previous run. Then run `.\rtwdiag.ps1 arm`. | [INST-4](observed-behavior.md#inst-4) |
| After the computer starts, the network adapter does not start. The log contains `guard pending` and then `registry still not writable`. | For 60 s after the start, Windows could not write the registry hive to the disk. By design, the driver then does not operate the chip. | Restart the computer. Use `.\rtwdiag.ps1 log -Saved` to read the log. | [INST-4](observed-behavior.md#inst-4) |
| `netsh wlan show networks` reports "Access is denied". When the station is connected, `netsh wlan show interfaces` also reports it. | The Windows location service is off | Enable the location service in **Settings > Privacy & security > Location**. Or use the Wi-Fi list in the taskbar or `Get-NetAdapter`. | [SCAN-7](observed-behavior.md#scan-7) |
| After a version change, the connection reports `There is no profile` | You uninstalled the driver and then installed it again. The interface GUID changed. | In the future, use the method in [Upgrade the driver](#upgrade). This time, connect again from the Wi-Fi list and type the password. | [INST-2](observed-behavior.md#inst-2) |
| The connection is active, but ping gets no reply | A new network is a "Public network". Windows Firewall drops echo requests. | Use `Test-NetConnection <address> -Port <port>` to test a TCP connection. To use ping, allow ICMPv4 echo in the firewall. | [INST-8](observed-behavior.md#inst-8) |
| After you restart the computer or the device, the network does not connect automatically | The WLAN profile does not have **Connect automatically** | Select **Connect automatically** in the Wi-Fi settings. Or run `netsh wlan connect name=<profile name>`. | [CONN-8](observed-behavior.md#conn-8) |
| A change to a setting has no effect | The driver reads the settings only when the device starts | Restart the device or the computer. Then connect again. | [Change settings](#settings) |
| `pnputil /restart-device` fails and asks for a computer restart | After the upgrade, Windows marks the device as "computer restart necessary" | Restart the computer | [INST-3](observed-behavior.md#inst-3) |
| The driver log contains `[ERR]FWDL path ready`, and the device does not start | The device had a re-enumeration. The chip cannot download firmware until it loses power. | Restart the computer (the chip loses power) | [INST-6](observed-behavior.md#inst-6) |
| After a device restart or a SER, the mobile hotspot is off | Windows does not start the hotspot again automatically | Start the hotspot again in **Settings > Network & internet > Mobile hotspot** | [HS-13](observed-behavior.md#hs-13) |
| When you set the hotspot band, Windows reports `BandInterference` | When the station is connected, the hotspot must use the same channel as the station | Select **Any available** or the band of the station | [HS-12](observed-behavior.md#hs-12) |
| The mobile hotspot stops automatically | The hotspot had no hotspot clients for approximately 5 minutes. Windows then stops the hotspot. | Start the hotspot again | [HS-13](observed-behavior.md#hs-13) |
| The network adapter list contains `WLAN 2` and `WLAN 3` | They are the Wi-Fi Direct device port and the role port | No action is necessary. If you do not use the mobile hotspot, you can set `WifiDirect` to `0` and restart the device. | [HS-3](observed-behavior.md#hs-3) |
| The connection to a WPA3-SAE network fails | `Pmf` is `0` by default. The driver then does not report the SAE capability. | Set `Pmf` to `2`. Then restart the device. | [Change settings](#settings) |
| When `Pmf` is `2`, a network in transition mode always connects with WPA3-SAE | Windows upgrades the WPA2-Personal profile to SAE | If you need only WPA2-Personal with PMF, set `Pmf` to `1` | [SEC-3](observed-behavior.md#sec-3) |
| A WPA3-SAE network disconnects after a SER and does not connect again automatically | The tested access point (AP-3) loses the SAE state during the association comeback time. The tests did not use other access points. | Connect again manually | [SEC-4](observed-behavior.md#sec-4) |
| The interactive latency is approximately 100 ms | Power save is on | Set `PowerSave` to `0`, or delete the value. Then restart the device. | [PS-3](observed-behavior.md#ps-3) |
| The throughput on 2.4 GHz is low | The cause is not determined. A crowded 2.4 GHz band is a possible cause. No measurement confirms this. | Use 5 GHz (default maximum VHT80) | [Validation status](validation.md#station) |
| The 5 GHz connection is not VHT80 | `Bandwidth` is `20` or `40`. Or the access point or the regulatory rules do not allow 80 MHz. | Run `.\rtwdiag.ps1 settings` and read `Bandwidth in effect:`. Find `limited by:` in the driver log. | [Change settings](#settings) |
| Some 5 GHz networks are not visible, or the driver finds them only with passive scans | Without a country code, the driver uses the world rules. On 5 GHz, these rules allow only passive scans. | Set a country code with `.\rtwdiag.ps1 country <country code>`. Then restart the device. | [REG-3](observed-behavior.md#reg-3) |
| Bluetooth scans find no devices, and Bluetooth cannot pair | The Bluetooth filter driver is not installed. The controller runs only the ROM firmware. | Install the Bluetooth filter driver | [BT-1](observed-behavior.md#bt-1) |
| The Bluetooth filter driver is installed, but Bluetooth does not operate. `rtkbtflt.ps1 status` shows `BadSessions 2`. | The crash loop guard of the Bluetooth filter driver triggered. The driver only forwards I/O and does not download the Bluetooth patch. | First, read the log in the output of `rtkbtflt.ps1 status`. Then run `.\rtkbtflt.ps1 clear`. | [Diagnostic commands](#diagnostics) |
| `rtkbtfw info` shows an `lmpSubversion` that is not `0xb20f`. The `stack:` value in `rtkbtflt.ps1 status` does not contain `\Driver\rtkbtflt`. | The Bluetooth filter driver is not installed on this device. The hardware ID does not agree with the INF, or the installation did not complete. | Do steps 3 and 4 of [Install the Bluetooth filter driver](#install-bt) to examine the hardware ID. Then install again. | [Install the Bluetooth filter driver](#install-bt) |
| `lmpSubversion` is not `0xb20f`. `rtkbtflt.ps1 status` shows `PassThrough 1`. | The setting `PassThrough` is `1`. The Bluetooth filter driver operates in pass-through. | Set `PassThrough` to `0`, or delete the value. Then restart the computer. | [Change settings](#settings) |
| `rtkbtfw info` reports `no Bluetooth radio present` | Bluetooth is disabled in Windows. The Bluetooth USB device is in D3. | Enable Bluetooth in **Settings > Bluetooth & devices**. Then run `.\rtkbtfw.exe info` again. When `rtkbtflt.ps1 status` shows `LastResult 0`, the patch download was successful. | [Diagnostic commands](#diagnostics) |
| A bug check occurs at startup, and Windows does not show the desktop | The Wi-Fi driver or the Bluetooth filter driver has an error at startup | Do the steps in [Recover when Windows does not start](#recovery) | [Recover when Windows does not start](#recovery) |

<a id="recovery"></a>
## Recover when Windows does not start

If a bug check occurs at startup after the installation, try method 0 first. If method 0 does not help, use method 1 or method 2. Method 1 only disables the driver. Method 2 restores the full `SYSTEM` registry hive to the state before the installation.

To find which driver causes the bug check, read the "What failed" line on the bug check screen. If this line shows `rtw89win.sys`, the cause is the Wi-Fi driver. If this line shows `rtkbtflt.sys`, the cause is the Bluetooth filter driver. If there is no such line, do the procedure for the two drivers.

### Method 0: Let the crash loop guard disable the driver

The two drivers have a crash loop guard. After two runs in sequence that did not end correctly, the Wi-Fi driver starts only in diagnostic mode. It does not operate the chip. The Bluetooth filter driver then operates only in pass-through. The tests used only a simulation of this method, not a real bug check (see [Validation status](validation.md#untested)).

1. Restart the computer. If a bug check occurs again, restart the computer one more time.

   **Result**: After a maximum of two more restarts, Windows shows the desktop. If the bug checks continue, use method 1 or method 2.
2. Open administrator PowerShell 7 in the Wi-Fi driver package directory.
3. Save the log of the previous run.

   ```powershell
   .\rtwdiag.ps1 log -Saved > saved-log.txt
   ```

4. Run the applicable uninstall script (see [Uninstall the drivers](#uninstall)).

### Method 1: Disable the driver from Linux on the target device

For this method, the target device must also have a Linux system. Its kernel must include the `ntfs3` driver. The rollback scripts are in the driver packages: `rollback-rtw89win.sh` for Wi-Fi and `rollback-rtkbtflt.sh` for Bluetooth. The script renames the `.sys` file in the Windows driver store (`DriverStore`). Windows then does not load the file.

Method 1 and method 2 must mount the Windows partition with read and write access. If Windows is in hibernation, or the last shutdown used Fast Startup, `ntfs3` can mount the partition only as read-only. The rename and the overwrite then fail.

1. Start the Linux system on the target device.
2. Copy the rollback script to the Linux system.
3. Show the list of NTFS partitions.

   ```sh
   sudo sh rollback-rtw89win.sh
   ```

   **Result**: The script shows its usage and a list of the NTFS partitions.
4. Run the rollback script with the Windows partition as the parameter.

   ```sh
   sudo sh rollback-rtw89win.sh <Windows partition>
   ```

   `<Windows partition>`: the device file of the Windows partition from the list in step 3, in the format `/dev/<partition name>`.

   **Result**: The output shows `disabled <path>/rtw89win.sys`.
5. If the bug check comes from the Bluetooth filter driver, do steps 3 and 4 again with `rollback-rtkbtflt.sh`.
6. Start Windows.
7. Run the applicable uninstall script (see [Uninstall the drivers](#uninstall)).

### Method 2: Restore the registry hive from the backup

> **Caution**: Save a copy of the current `SYSTEM` file first. This method restores the full `SYSTEM` registry hive to the state at installation. All changes to `SYSTEM` after that time are lost.

1. Open the Windows partition from a different system (for example, the Linux system on the target device).
2. Find the backup of the install script: `ProgramData\rtw89win\backup\rtw89win-<time>\SYSTEM.hiv` (for Bluetooth: `rtkbtflt-<time>`). If you gave `-WorkDir` at installation, look in that directory.
3. Save a copy of the current `Windows\System32\config\SYSTEM`.
4. Replace `Windows\System32\config\SYSTEM` with `SYSTEM.hiv`.
5. Start Windows.

<a id="limits"></a>
## Known limits

- The driver supports only 2.4 GHz and 5 GHz. On 2.4 GHz, it supports only HT20. On 5 GHz, the maximum is VHT80. The driver does not supply 802.11ax (HE) or 160 MHz.
- The supported security types are WPA2-Personal, WPA3-SAE (disabled by default), and PMF (disabled by default). The driver does not support WEP, TKIP, or enterprise networks (802.1X). The tests did not include open networks.
- The mobile hotspot supports only WPA2-Personal and HT20. When the station is connected, the hotspot must use the same channel as the station.
- By default, power save follows Windows. The measured result is the same as no power save.
- After the crash loop guard triggers, the network adapter is not available until you run `rtwdiag.ps1 arm`.
- For Bluetooth, you must install the Bluetooth filter driver separately.
- The tests used only the ROCK 5T.
- After a re-enumeration of the device, the chip cannot download firmware until it loses power ([INST-6](observed-behavior.md#inst-6)). These operations do not cause a re-enumeration: `pnputil /restart-device`, disable and enable, and the uninstall scripts in this document.

For all limits and the items without tests, see [Validation status](validation.md#limits). For the definitions of the terms, see the [Glossary](glossary.md).
