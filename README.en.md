# rtw89-win

[中文](README.md)

This project supplies a driver for the Realtek RTL8852BE (Wi-Fi 6 and Bluetooth) on Windows 11 ARM64. The driver is based on the rtw89 driver from the Linux kernel.

> **Caution**: This project is not an official Realtek or Microsoft driver. It has no relation to these two companies. The driver has a test signature. This project does not supply compiled driver packages.

<a id="features"></a>
## Features

| Feature | Status (for conditions and data, see [Validation status](docs/en/validation.md)) |
|---|---|
| Scan and connect: 2.4 GHz (HT20), 5 GHz (maximum VHT80) | Pass |
| WPA2-Personal; WPA3-SAE and protected management frames (disabled by default, setting `Pmf`) | Pass |
| A-MPDU aggregation | Pass |
| Windows mobile hotspot (WPA2-Personal, HT20) | Pass (with limits) |
| System error recovery (SER) | Pass |
| Country code (setting `Country`) | Pass |
| Power save (setting `PowerSave`) | Pass (with limits) |
| Bluetooth: automatic patch download, BLE scan, pairing | Pass |
| 802.11ax (HE) | Not implemented |
| 160 MHz, 6 GHz, enterprise networks (802.1X) | Not supported |

<a id="requirements"></a>
## Requirements

- **Hardware**: An RTL8852BE module (PCI hardware ID `PCI\VEN_10EC&DEV_B852`). The tests used only the ROCK 5T (RK3588). The UEFI must supply power to the M.2 slot of the module. The USB hardware ID of the Bluetooth part must be `USB\VID_13D3&PID_3572`. If it is different, change the INF before you build. See the [Porting guide](docs/en/porting.md#new-board).
- **System**: Windows 11 ARM64, version 26100 or later. The tests used Windows 11 IoT Enterprise LTSC 2024.
- **Test-signing mode**: Windows loads this driver only in this mode. Disable Secure Boot before you enable this mode.
- **Firmware**: This repository does not contain firmware. Get the 3 firmware files from linux-firmware.
- **Your own build**: Build the driver packages on an x64 Windows computer.

<a id="start"></a>
## First steps

1. Get the firmware files. See [Firmware files](firmware/README.en.md).
2. Build the Wi-Fi driver package and the Bluetooth driver package. See [Build](docs/en/build.md).
3. Install and use the drivers on the target device. See [Installation and use](docs/en/install.md).

<a id="docs"></a>
## Documents

| Document | Readers | Contents |
|---|---|---|
| [Installation and use](docs/en/install.md) | Users | Test-signing mode, installation, connection, mobile hotspot, settings, problems and solutions, recovery |
| [Build](docs/en/build.md) | Builders | Tools, firmware, test certificate, driver package build, host tests |
| [Firmware files](firmware/README.en.md) | Users, builders | Firmware files, sources, SHA256 values, license |
| [Architecture](docs/en/architecture.md) | Driver developers | Layer boundaries, control flow, data flow, design decisions |
| [Observed behavior](docs/en/observed-behavior.md) | Driver developers | Behavior that the WiFiCx and WDI documentation does not describe, found by tests |
| [Porting guide](docs/en/porting.md) | Driver developers | Add an rtw89 chip, change the hardware platform, change the driver family |
| [Validation status](docs/en/validation.md) | Users, driver developers | Test conditions, results, and data for each feature |
| [Third-party licenses](docs/en/licenses.md) | License reviewers | Licenses and obligations of each part |
| [Glossary](docs/en/glossary.md) | All readers | Definitions of the terms in the documents |

<a id="license"></a>
## License

- The code of this project uses the BSD-3-Clause license. See [`LICENSE`](LICENSE). The upstream rtw89 is in `third_party\rtw89\` without changes. This project uses it under the BSD-3-Clause license.
- For the other third-party parts, see [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES). For an explanation, see [Third-party licenses](docs/en/licenses.md).
- This repository does not supply the firmware. The firmware uses a Realtek license. See [Firmware files](firmware/README.en.md).
