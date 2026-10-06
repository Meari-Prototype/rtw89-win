# Validation status

[中文](../zh/validation.md)

- **Readers**: Users and driver developers.
- **What you will learn**: The test conditions, results, and data for each feature. Also, the features that are not tested, and the reasons.
- **Prerequisites**: None.

<a id="summary"></a>
## Summary

- Current source version: Wi-Fi driver 0.0.11.2, Bluetooth filter driver 0.1.0.3 (the default versions of the build scripts).
- The latest versions tested on the target device are Wi-Fi driver 0.0.11.2 and Bluetooth filter driver 0.1.0.3. Most items had their last test with 0.0.11.1. For the differences between the two versions, see [Build](#build).
- All tests used one ROCK 5T. For the test environment, see [Test environment](#environment).
- Station features that pass: 2.4 GHz (HT20) and 5 GHz (up to VHT80), WPA2-Personal, WPA3-SAE and protected management frames, A-MPDU aggregation.
- Other features that pass: system error recovery (SER), country code, power save, mobile hotspot, Bluetooth patch download, BLE scan, and Bluetooth pairing.
- Main items that are not tested: open networks, hidden networks, key updates, long runs, WPA3-SAE with VHT80, two or more hotspot clients. HE (802.11ax) is not implemented. See [Items not tested](#untested).
- The in-place upgrade, the `-Country` parameter, and the default work directory ran on the target device. The first-installation path of the install script and the `-WorkDir` parameter had only a syntax check.

<a id="environment"></a>
## Test environment

All documents use these labels for the test conditions.

| Label | Description |
|---|---|
| Target device | ROCK 5T (RK3588, RTL8852BE module in the M.2 E-key slot), Windows 11 IoT Enterprise LTSC 2024 ARM64, version 26100.9457, test-signing mode |
| AP-1 | An 802.11ac dual-band router: 2.4 GHz HT20, 5 GHz VHT80 (channel 36), WPA2-Personal, no protected management frames, no HE support; signal −51 to −56 dBm |
| AP-2 | A mobile hotspot on a Windows 11 laptop: 2.4 GHz, WPA2-Personal |
| AP-3 | A hotspot on an Android tablet: 2.4 GHz, WPA3-SAE transition mode (protected management frames optional), no H2E support |
| Client-1 | An Android tablet: used as a hotspot client and as a Bluetooth peer |
| Country code | JP |
| Firmware | Wi-Fi `rtw8852b_fw-2.bin` 0.29.29.18; Bluetooth patch `0xdbc6b20f` |

Other conditions:

- During the tests, `wificx.sys` and `netadaptercx.sys` had version 10.0.26100.9278. The KMDF runtime was 1.35 (the driver needs 1.33).
- If the text gives no other condition, `Country` is `JP`. All other settings have their default values.

Throughput measurement method:

- `hosttools\rtw89win-thru.ps1` sends a 32 MiB or 48 MiB file over Wi-Fi. The result is valid only if the SHA256 of the file is the same on the two ends.
- The unit is Mbit/s. The data has the form "downlink / uplink". Downlink is from the peer to the target device. Uplink is from the target device to the peer.
- The peer is a computer on the same network for AP-1. For AP-2, the peer is the laptop that runs the hotspot. For AP-3 and Client-1, the peer is the Android tablet.
- The time includes the time to open the ssh connection. For this reason, the values are a little lower than the actual transfer speed.

How to read the tables:

- The matrix columns are always: Feature, Result, Version, Conditions, Data.
- The "Result" column uses only these terms:

  | Result | Definition |
  |---|---|
  | Pass | The feature operates as expected. |
  | Pass (with limits) | The feature operates, but it has the limits that the "Data" column gives. |
  | Fail | The feature does not operate as expected. The "Conditions" column gives the cause. |
  | Not supported | The driver or the hardware does not have this feature. |
  | Not implemented | The hardware or the upstream rtw89 has this feature. This driver does not implement it. |
  | Not tested | Nobody tested this feature. For the reason, see [Items not tested](#untested). |
  | Rejected by design | The driver rejects this operation by design. |

- "Version" is the driver version of the last test on the target device. If this version is older than the current source version, the column adds "(current version not tested again)".
- If an explanation is necessary, the "Conditions" or "Data" column links to an ID in [Observed behavior](observed-behavior.md).

<a id="station"></a>
## Station

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| Driver load and capability report | Pass | 0.0.11.2 | Target device, automatic load at startup | 2T2R; 5 PHY types |
| Scan (world rules) | Pass | 0.0.7.4 (current version not tested again) | `Country` not set; approximately 6–10 BSSs nearby | 39 channels: 14 on 2.4 GHz, 25 on 5 GHz. Approximately 4.1 s and 5–7 BSSs for each scan. Only passive scan on 5 GHz |
| Scan (country code JP) | Pass | 0.0.8.2 (current version not tested again) | `Country=JP` | 34 channels; active scan on 5 GHz channels 36–48 |
| Scan while connected | Pass | 0.0.11.1 (current version not tested again) | AP-1, 5 GHz VHT80 | The connection stays available |
| WPA2-Personal connection, 2.4 GHz HT20, no A-MPDU aggregation | Pass | 0.0.6.1 (current version not tested again) | AP-2, signal −26 dBm | Association 110 ms; `TASK_CONNECT` 285 ms; approximately 7 s from `netsh wlan connect` to a DHCP address; 17–19 / 12–17; 700 TCP connections, 0 failures |
| WPA2-Personal connection, 2.4 GHz HT20, no A-MPDU aggregation | Pass | 0.0.6.1 (current version not tested again) | AP-1 2.4 GHz, signal −53 dBm | 24.5 / 19.9 |
| Disconnect, reconnect, device restart while connected, access point stops (it sends deauth first) | Pass | 0.0.6.1 (current version not tested again) | AP-2 | The driver deletes the two keys; `rtwdiag.ps1 link` returns to idle ([CONN-8](observed-behavior.md#conn-8)) |
| A-MPDU aggregation (RX reorder, TX aggregation) | Pass | 0.0.7.4 (current version not tested again) | AP-1 2.4 GHz HT20, WPA2-Personal | 35–52 / 24.5–25.6; the block ack session starts approximately 1 s after the connection ([RTW-10](observed-behavior.md#rtw-10)) |
| 2.4 GHz station | Pass | 0.0.11.1 (current version not tested again) | AP-1 2.4 GHz | HT20 |
| 5 GHz station | Pass | 0.0.11.2 | AP-1 5 GHz channel 36 | See [Channel width](#width) |

The 2.4 GHz uplink throughput is 19.9–26.0 Mbit/s. This value is lower than 5 GHz HT20 (see [Channel width](#width)). The cause is not determined. A crowded 2.4 GHz band is a possible cause. No measurement confirms this.

<a id="security"></a>
## Security

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| WPA2-Personal (CCMP) | Pass | 0.0.11.1 (current version not tested again) | AP-1 2.4 GHz and 5 GHz | All station tests used this security type |
| WPA2-Personal + protected management frames | Pass | 0.0.8.7 (current version not tested again) | AP-3, `Pmf=1` | 62.1 / 71.5; after SER during a connection, the reconnection with association comeback is successful ([SEC-4](observed-behavior.md#sec-4)) |
| WPA3-SAE + protected management frames | Pass | 0.0.8.8 (current version not tested again) | AP-3 (transition mode), `Pmf=2` | `TASK_CONNECT` 326 ms; IGTK installed; 67.8 / 74.3 |
| Protected management frames: disassociation from the access point, deauth from the target device | Pass | 0.0.8.5 (current version not tested again) | AP-3, `Pmf=2` | `mgmt_dropped_unprot` is 0 |
| Automatic reconnection of a WPA3-SAE connection after SER | Fail | 0.0.8.7 (current version not tested again) | AP-3, `Pmf=2`; the cause is access point behavior: AP-3 discards the SAE state during the comeback time and then sends a disassociation ([SEC-4](observed-behavior.md#sec-4)) | Manual reconnection is correct |
| Crypto self-test (AES-CCM and AES-CMAC known answers) | Pass | 0.0.8.8 (current version not tested again) | At driver load | The self-test passes, and protected management frames are available; if the self-test fails, the driver disables protected management frames ([SEC-5](observed-behavior.md#sec-5)) |

<a id="power"></a>
## Power save

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| Default (`PowerSave=1`, follow Windows) | Pass (with limits) | 0.0.8.2 (current version not tested again) | AP-1 2.4 GHz; power plan "High performance", wireless adapter power saving mode "Maximum Performance" | Windows sends connection quality hint 4. The driver does not enter power save. Median round-trip time to open a TCP connection 1.8 ms ([PS-1](observed-behavior.md#ps-1)) |
| `PowerSave=2` (always power save) | Pass | 0.0.8.2 (current version not tested again) | AP-1 2.4 GHz | Power save entry and exit are correct; median round-trip time to open a TCP connection 95.7 ms; 50.1 / 26.0; the block ack session stays available |
| `PowerSave=2`, VHT80 | Pass | 0.0.11.1 (current version not tested again) | AP-1 5 GHz VHT80 | Power save entry and exit are correct |
| Coexistence diagnostics (`rtwdiag.ps1 btc`) | Pass | 0.0.8.2 (current version not tested again) | No paired Bluetooth device | The output of `btc` and `btc -Full` is correct |

<a id="regulatory"></a>
## Country code

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| World rules (`Country` not set) | Pass | 0.0.7.4 (current version not tested again) | No country code in the efuse | Only passive scan on 5 GHz |
| `Country=JP`: channels and power table | Pass | 0.0.8.2 (current version not tested again) | AP-1 2.4 GHz | Each rule agrees with wireless-regdb; the power table is MKK; active scan on 5 GHz channels 36–48 |
| `Country=JP`: throughput | Pass | 0.0.8.8 (current version not tested again) | AP-1 2.4 GHz | 47.8 / 24.9 (51.8 / 24.6 without a country code) |
| All station features with `Country=JP` | Pass | 0.0.11.2 | Test environment of the target device | The VHT80 connection is correct |

<a id="width"></a>
## Channel width

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| VHT80 (default, `Bandwidth` not set) | Pass | 0.0.11.2 | AP-1 5 GHz channel 36 (center channel 42), no HE | 117.8 / 127.8 (0.0.11.1: 133.7–133.9 / 127.7–130.2); Windows shows 802.11ac and a link rate of 866.6 Mbps; `rx_dropped_full` is 0 ([WIDE-11](observed-behavior.md#wide-11)) |
| HT40 (`Bandwidth=40`) | Pass | 0.0.11.1 (current version not tested again) | Same as above | The access point transmits with HT40; 64–67 / 85–88; link rate 300 Mbps |
| HT20 (`Bandwidth=20`) | Pass | 0.0.11.1 (current version not tested again) | Same as above | 53.2 / 60.2; link rate 144.4 Mbps |
| 2.4 GHz stays at HT20 | Pass | 0.0.11.1 (current version not tested again) | AP-1 2.4 GHz | HT20 |
| Regression tests with VHT80: SER, power save, scan while connected, mobile hotspot, computer restart | Pass | 0.0.11.1 (current version not tested again) | AP-1 5 GHz VHT80 | VHT80 is available again 0.6 s after SER |
| 160 MHz | Not supported | — | rtw89 does not declare 160 MHz for the 8852B ([WIDE-1](observed-behavior.md#wide-1)) | — |
| HE (802.11ax) | Not implemented | — | AP-1 does not advertise HE, so no verification is possible | — |
| 6 GHz | Not supported | — | The module does not support 6 GHz | — |

<a id="hotspot"></a>
## Mobile hotspot

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| Mobile hotspot, 2.4 GHz (WPA2-Personal, HT20) | Pass | 0.0.11.1 (current version not tested again) | Client-1 | Channel 1 or 11 |
| Mobile hotspot, 5 GHz | Pass | 0.0.10.12 (current version not tested again) | Client-1; country code JP | The driver selects channel 40 ([HS-11](observed-behavior.md#hs-11)) |
| Band "Any available" | Pass | 0.0.11.1 (current version not tested again) | Client-1; station disconnected and station connected | Station disconnected: 5 GHz channel 40; station connected: same channel as the station |
| Hotspot client connection and internet access | Pass | 0.0.10.12 (current version not tested again) | Client-1, 2.4 GHz HT20 | 60–70 ms from authentication to authorization; 45.8 / 34.6; downlink 53.5 in a second measurement; no A-MPDU aggregation in the group owner TX direction |
| Hotspot client power save | Pass | 0.0.10.11 (current version not tested again) | Client-1 with the screen off for 5 minutes | 99 frames kept, 99 frames released, 0 frames discarded ([HS-17](observed-behavior.md#hs-17)) |
| Hotspot client disconnection; stop and start the hotspot | Pass | 0.0.10.12 (current version not tested again) | Client-1 | Nothing remains; no bug check |
| Device restart or SER while the hotspot runs | Pass (with limits) | 0.0.10.12 (current version not tested again) | Client-1 | No bug check; Windows does not start the hotspot again automatically ([HS-13](observed-behavior.md#hs-13)) |
| Station and hotspot on the same channel | Pass | 0.0.11.1 (current version not tested again) | Station connected to AP-1 5 GHz channel 36; Client-1 | Operation on the same channel is correct |
| Station and hotspot on the same channel: station throughput | Pass | 0.0.10.12 (current version not tested again) | Station connected to AP-1 5 GHz HT20; Client-1 | Station 52.3 / 62.6; with the station only, 56.1 downlink and 59.2 uplink ([HS-12](observed-behavior.md#hs-12)) |
| Hotspot start while the station is on a different band | Rejected by design | 0.0.10.12 (current version not tested again) | Station on 5 GHz, hotspot band set to 2.4 GHz | Windows reports `BandInterference` ([HS-12](observed-behavior.md#hs-12)) |
| `WifiDirect=0` (hotspot function disabled) | Pass | 0.0.10.12 (current version not tested again) | AP-1 | Station functions are not affected; the driver does not report Wi-Fi Direct capabilities |
| Hotspot with WPA3-SAE | Not supported | — | WDI does not support SAE on the group owner side ([HS-13](observed-behavior.md#hs-13)) | — |

<a id="robustness"></a>
## Recovery and stability

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| SER (firmware CPU exception trigger), connected | Pass | 0.0.7.4 (current version not tested again) | AP-1 2.4 GHz HT20; `rtwdiag.ps1 ser` | Recovery in approximately 110 ms; INTx interrupt masked 1 time; Wlansvc reconnects immediately; address available again in 18 s ([RTW-9](observed-behavior.md#rtw-9)) |
| SER while the monitor runs in diagnostic mode | Pass | 0.0.7.4 (current version not tested again) | Diagnostic mode, `rtwdiag.ps1 monitor` | Scan can continue after recovery |
| SER, VHT80 | Pass | 0.0.11.1 (current version not tested again) | AP-1 5 GHz VHT80 | VHT80 is available again 0.6 s after SER |
| Device restart (idle, connected) | Pass | 0.0.11.1 (current version not tested again) | Restart the device after a change to `Bandwidth` | The setting becomes active; the reconnection is correct |
| Disable and enable the network adapter; device restart during a scan | Pass | 0.0.6.1 (current version not tested again) | Idle, scan, connected | The number of WDI messages received is equal to the number of completions; no minidump |
| Automatic load after a computer restart | Pass | 0.0.11.2 | Restart the computer | The network adapter and the connection recover correctly |
| Startup sequence | Pass | 0.0.9.0 (current version not tested again) | Restart the computer | Load at approximately 24.6 s; registry hive writable 0.53 s later; network adapter available at approximately 26.2 s ([INST-4](observed-behavior.md#inst-4)) |
| Crash loop guard (simulated) | Pass | 0.0.9.1 (current version not tested again) | Two runs that did not end correctly, simulated with device restarts and counter changes | Diagnostic mode at the second run; `rtwdiag.ps1 arm` recovers |
| The shutdown notification ends the current run | Pass | 0.0.11.2 | One computer restart | The driver log shows "ended in order (shutdown)" ([INST-5](observed-behavior.md#inst-5)) |
| Radio switch (`TASK_SET_RADIO_STATE`) | Pass | 0.0.11.2 | Windows sets the radio off; wlanapi sets it on | The driver log shows "radio on -> off" and "radio off -> on". After the radio is on, the station connects again correctly ([SCAN-1](observed-behavior.md#scan-1)) |
| Diagnostic mode: probe, start, monitor, scan, TXPROBE | Pass | 0.0.5.3 (current version not tested again) | 2.4 GHz channels 3 and 4 | TXPROBE 10 frames, each with a TX status; probe response 4–8 ms later |

<a id="install"></a>
## Installation and upgrade

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| In-place upgrade, interface GUID does not change | Pass | 0.0.11.2, 0.1.0.3 | Install script from the new driver package | The WLAN profiles stay usable ([INST-2](observed-behavior.md#inst-2)); the Bluetooth pairing records stay |
| `-Country`, default work directory, and test certificate file name `rtw89win-test.cer` during an in-place upgrade | Pass | 0.0.11.2, 0.1.0.3 | No `-WorkDir`; `-Country JP` for the Wi-Fi driver | The country code is active when the new driver starts; backups and logs in `%ProgramData%\rtw89win`; test certificate removed after the installation |
| In-place downgrade to an older version, interface GUID does not change | Pass | 0.0.7.4 (current version not tested again) | Install an older driver package, then upgrade again | The WLAN profiles stay usable |
| First-installation path of the install script, `-WorkDir` | Not tested | — | Syntax check only; nobody ran them on the target device | — |
| Partition parameter of the rollback script | Not tested | — | The tests checked only the message when the parameter is not given | — |

<a id="bluetooth"></a>
## Bluetooth

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| Patch download: startup, device arrival, device restart | Pass | 0.1.0.3 | Bluetooth part of the module (`USB\VID_13D3&PID_3572`) | Approximately 0.5 s; `lmpSubversion` is `0xb20f` |
| Patch download again after D3 → D0 | Pass | 0.1.0.2 (current version not tested again) | Disable and enable Bluetooth | The patch is active again ([BT-4](observed-behavior.md#bt-4)) |
| D2 (selective suspend) keeps the patch | Pass | 0.1.0.1 (current version not tested again) | D2 after approximately 1 s of idle time | The patch stays |
| BLE scan | Pass | 0.1.0.2 (current version not tested again) | BLE devices nearby | Approximately 355 advertisements from 24 devices |
| Classic Bluetooth discovery, pairing, SDP | Pass | 0.1.0.2 (current version not tested again) | Client-1 | Numeric comparison pairing takes 11 s |
| Shutdown notification | Pass | 0.1.0.2 (current version not tested again) | Restart the computer | The current run ends correctly |
| Reinstallation after uninstallation; crash loop guard test | Pass | 0.1.0.1 (current version not tested again) | — | Pairing records stay after uninstallation |
| Bluetooth and Wi-Fi transfer at the same time | Pass (with limits) | 0.1.0.1 (current version not tested again) | 32 MiB transfer over Wi-Fi during a BLE scan | The data is correct; throughput not measured ([BT-9](observed-behavior.md#bt-9)) |

<a id="build"></a>
## Build

In this table, "Version" is the version of the driver that the build made. The build used an x64 build PC. It used only the files of this repository, the WDK NuGet packages, and LLVM 23.1.2.

| Feature | Result | Version | Conditions | Data |
|---|---|---|---|---|
| Upstream rtw89, compatibility layer, and mac layer (`buildhal.ps1`) | Pass | 0.0.11.2 | x64 build PC | 53 object files, 0 warnings, 0 errors |
| Wi-Fi driver package (`build.ps1`) | Pass | 0.0.11.2 | Test signing and `-NoSign` | MSBuild 0 warnings, 0 errors |
| Driver package of the Bluetooth filter driver (`bt\rtkbtflt\build.ps1`) | Pass | 0.1.0.3 | Test signing and `-NoSign` | 0 warnings, 0 errors; the 723 host tests of `rtkbtfw` pass |
| Host tests `apframe-test.ps1`, `wide-test.ps1`, `crypto-test.ps1` | Pass | 0.0.11.2 | x64 build PC | All tests pass |
| `undef.ps1` (external symbols that rtw89 does not have) | Pass | 0.0.11.2 | x64 build PC | 45 |
| Release source compared with the tested drivers | Pass | 0.0.11.1, 0.1.0.2 | Comparison with the drivers tested on the target device | Wi-Fi driver: the machine code is different only in `__LINE__` immediate values (different line numbers). Bluetooth filter driver: different only in linker padding. `rtkbtfw.exe`: the code is the same |
| Current version compared with the tested version: Wi-Fi driver | Pass | 0.0.11.2 | Comparison with `rtw89win.sys` 0.0.11.1 | Different only in the version string and in bytes that change with each build. These bytes are the PE timestamp and checksum, debug directory, PDB GUID, and `fothk`. The code and data from `rtw89hal.lib` are the same |
| Build on an ARM64 computer | Not tested | — | The tests used only an x64 build PC | — |

<a id="untested"></a>
## Items not tested

| Item | Status | Reason |
|---|---|---|
| Connection to open (unencrypted) networks | Not tested | The test environment has no open network |
| Hidden networks | Not tested | The test environment has no hidden network |
| Beacon loss without deauth (Null probe path) | Not tested | The test access points always send deauth before they stop |
| Group key update, pairwise key update, installation of the same key again | Not tested | The access points did not update keys during the tests |
| Driver rejection path for TKIP networks, WPA3-SAE-only networks, and networks where protected management frames are mandatory (MFPR) | Not tested | The test environment has no such networks |
| SER during a scan, during a connection attempt, and from a firmware error | Not tested | `rtwdiag.ps1 ser` returns `-EBUSY` during a scan; the tests cannot catch the time of a connection attempt; the tests used only a triggered CPU exception |
| Crash loop guard: counter after a real bug check or a power loss, registry hive not writable for 60 s after startup, AddDevice failure | Not tested | The tests used only a simulation with device restarts |
| Long runs (60 minutes or more) | Not tested | The test plan did not include this type of test |
| Enterprise networks (802.1X) | Not supported | The driver does not support 802.1X |
| Roam between two or more access points | Not tested | The test environment has only one access point |
| Country code that the access point advertises (Country element) | Not tested | The tests set `Country`, and the driver ignores the Country element |
| 30 minutes in power save (group key update, DHCP renewal); SER during power save; power plan change | Not tested | Limited test time; power plan changes are outside the test scope |
| SA Query that the driver starts, multicast BIP, anti-clogging, H2E, incorrect password, WPA3-SAE-only access point (not transition mode) | Not tested | The test environment cannot make these conditions; AP-3 does not support H2E |
| WPA3-SAE and protected management frames with VHT80 | Not tested | AP-1 does not support protected management frames |
| Byte-by-byte comparison of the VHT80 association request | Not tested | The target device cannot capture frames on the air |
| TXPROBE limit of 32 frames for each load (`-EDQUOT`) | Not tested | This test needs approximately 30 frames in sequence on one channel; the tests sent only a small number of frames |
| Why the scan did not find two weak access points on channel 2 | Not tested | These two access points were not present at the second test |
| DFS, channel switch (CSA), operating mode notification (OMN) | Not tested | Host tests only. When the access point used DFS channel 52 for a short time, one VHT80 association was successful, but a timeout occurred in the 4-way handshake. The cause is not determined |
| HE (802.11ax) | Not implemented | AP-1 does not advertise HE, so no verification is possible |
| Two or more hotspot clients; traffic between hotspot clients | Not tested | Only one hotspot client (Client-1) |
| Group key update of the mobile hotspot | Not tested | Windows did not update the group key on the group owner during the tests |
| Coexistence result with Bluetooth load | Not tested | No paired headset-type device; the throughput script did not record the time during the BLE scan |
| Bluetooth OPP file transfer, audio, sleep | Not tested | Outside the test scope |
| First-installation path of the install script and the `-WorkDir` parameter; partition parameter of the rollback script | Not tested | Syntax check only |
| Other RTL8852BE modules, other RK3588 devices | Not tested | Only one module on one ROCK 5T |
| Build on an ARM64 computer; LLVM versions other than 23.1.2; WDK from a Visual Studio installation instead of the NuGet packages | Not tested | The tests used only an x64 build PC and LLVM 23.1.2 |

<a id="limits"></a>
## Known limits

- You can install the driver only on Windows 11 ARM64 26100 or later. Test-signing mode must be enabled.
- The driver supports only 2.4 GHz and 5 GHz. The module does not support 6 GHz.
- 2.4 GHz uses only HT20. The maximum on 5 GHz is VHT80. 160 MHz is not supported. HE is not implemented.
- Supported security types: open (not tested), WPA2-Personal (CCMP), WPA2-Personal with protected management frames, WPA3-SAE (AKM 8, CCMP-128, BIP-CMAC-128).
- Not supported: WEP, TKIP, GCMP, 802.1X (enterprise networks), OWE, SAE-EXT-KEY, PSK-SHA256 (AKM 6), beacon protection, BIP-GMAC, and BIP-CMAC-256.
- The driver does not cache PMKSA. Each connection does a full SAE.
- Protected management frames and WPA3-SAE are disabled by default (`Pmf=0`).
- The driver does not reassemble fragmented frames on the RX path. It discards fragmented frames and counts them.
- When `Country` is set, the driver ignores the country code that the access point advertises.
- If `Country` is not set and no connected access point advertises a country code, the driver uses the world rules. Then 5 GHz uses only passive scan, and you cannot start a 5 GHz hotspot.
- When location services are off, commands such as `netsh wlan show networks` report "Access is denied" ([SCAN-7](observed-behavior.md#scan-7)).
- The mobile hotspot supports only WPA2-Personal and HT20. The group owner TX direction has no A-MPDU aggregation.
- The capability report permits a maximum of 8 hotspot clients. The tests used only 1 hotspot client.
- The driver does not forward frames directly between hotspot clients. Traffic between hotspot clients through Windows is not tested.
- The mobile hotspot does not use HT protection or ERP protection.
- When the station is connected, the hotspot can only use the station channel ([HS-12](observed-behavior.md#hs-12)).
- When hotspot clients are connected, the station scans only the current channel.
- After SER or a device restart, you must start the hotspot again manually. If no hotspot client is connected for approximately 5 minutes, Windows stops the hotspot.
- With country code JP, a 5 GHz hotspot uses only channels 36–48.
- The default power save setting is equal to no power save. The driver cannot read the wireless adapter power save level from the power plan ([PS-2](observed-behavior.md#ps-2)).
- After two runs in sequence that do not end correctly (two power losses also count), the network adapter shows "Disabled" or disappears. This continues until you run `rtwdiag.ps1 arm`.
- After SER on a WPA3-SAE network, the tested access point (AP-3) cannot reconnect automatically. You must reconnect manually. Other access points are not tested ([SEC-4](observed-behavior.md#sec-4)).
- For Bluetooth, you must also install the Bluetooth filter driver. With the patch, the Bluetooth side still gives little coexistence data ([RTW-12](observed-behavior.md#rtw-12)).
- Only INTx interrupts are available. CPU0 processes all of them ([PLAT-1](observed-behavior.md#plat-1)).
- After re-enumeration of the device, the driver cannot download firmware to the chip until the chip loses power ([INST-6](observed-behavior.md#inst-6)). This driver and the scripts of this project do not cause re-enumeration.
- The diagnostic interface is available only to SYSTEM and administrators.

For limits in installation and use, see [Installation and use](install.md#limits). For terms, see the [Glossary](glossary.md).
