# Architecture

[中文](../zh/architecture.md)

- **Readers**: Driver developers. This includes persons who change this driver, write other WiFiCx drivers, or connect other rtw89 chips to Windows.
- **What you will learn**: What each layer does and does not do. The interfaces between the layers. The main control flow and data flow. The design decisions and their reasons.
- **Prerequisites**: Basic knowledge of KMDF and 802.11.

<a id="summary"></a>
## Summary

1. The Wi-Fi driver `rtw89win.sys` has five parts: the driver layer, the WiFiCx front end, the mac layer, upstream rtw89, and the compatibility layer. The Bluetooth filter driver `rtkbtflt.sys` is a separate driver.
2. This project does not change one line of upstream rtw89 (Linux v7.2). The mac layer calls upstream rtw89 through the Linux mac80211 interface `ieee80211_ops`.
3. The mac layer replaces Linux mac80211 and cfg80211. The mac layer contains the 802.11 logic, for example scan, connection, keys, and group owner.
4. The WiFiCx front end only converts WDI messages and NetAdapterCx queues into calls to the mac layer. It does not make 802.11 frames, and it does not call rtw89 directly.
5. clang compiles the Linux side (rtw89, compatibility layer, mac layer). MSVC compiles the Windows side (driver layer, WiFiCx front end). The linker puts the two sides into one `.sys` file.
6. One PASSIVE work item processes all WDI messages in sequence. The callbacks from the mac layer only record the state. Then they queue a DPC or a work item.
7. The driver uses only INTx interrupts. When rtw89 stops, the compatibility layer masks the chip interrupts.

<a id="overview"></a>
## Overall structure

The Wi-Fi driver has five layers. In the diagram, the items on the left of each line are calls down. The items on the right are callbacks up.

```text
 Windows              Wlansvc, icssvc (mobile hotspot)
                          | WDI messages (tasks, properties, indications)
                      WiFiCx + NetAdapterCx (TX and RX queues)
                          |
 =========================== rtw89win.sys ===========================
 WiFiCx front end     wificx\              C++, MSVC
                          | rtw_linux_* (glue.h)      ^ callbacks (glue.h)
 mac layer            mac\                 C, clang
                          | ieee80211_ops              ^ linuxkpi_ieee80211_*
 upstream rtw89       third_party\rtw89\   C, clang, not changed
                          | Linux kernel API
 compatibility layer  compat\              C, clang
                          | rtwk_* (os.h)              ^ rtw_linux_isr and others (glue.h)
 driver layer         driver\              C, MSVC, KMDF
 ====================================================================
                          | PCIe: BAR2, INTx interrupt, DMA
                      Wi-Fi part of the module (RTL8852BE)
```

The driver layer has two roles. First, it is the KMDF entry point. PnP callbacks go to the driver layer first, and the driver layer then calls the WiFiCx front end and the compatibility layer. Second, it supplies the system services. The code on the Linux side uses these services through `rtwk_*`.

The Bluetooth filter driver operates independently. It has no calls to or from the Wi-Fi driver:

```text
 bthport, BTHUSB (part of Windows, base driver bth.inf)
        | A START or D0 request goes through the filter driver first when it returns up
 rtkbtflt.sys (lower filter driver)     <- rtl8852bu_fw.bin, rtl8852bu_config.bin
        | URBs: HCI commands on the control endpoint, events on the interrupt endpoint
 USB stack
        |
 Bluetooth part of the module (USB)
```

Wi-Fi and Bluetooth have only one connection: coexistence. The rtw89 btc module does the coexistence on the Wi-Fi side ([RTW-12](observed-behavior.md#rtw-12)).

<a id="layers"></a>
## Function of each layer

| Part | Directory | Language and compiler | Does | Does not do |
|---|---|---|---|---|
| Driver layer | `driver\` | C, MSVC | `DriverEntry`, PnP and power callbacks; map BAR2; connect the INTx interrupt; DMA adapter and DMA heap; implement `rtwk_*`; read the settings; diagnostic interface; crash loop guard; driver log; AES with CNG | No 802.11 logic; does not parse WDI messages |
| WiFiCx front end | `wificx\` | C++, MSVC | WiFiCx device and network adapter objects; capability report; dispatch, parse, and generate WDI messages (`WificxTLVGenParse.lib`); NetAdapterCx TX and RX queues; port state | Does not make 802.11 frames; does not call rtw89 directly |
| mac layer | `mac\` | C, clang | Call rtw89 with the sequence and lock rules of mac80211; scan, authentication, association; keys, privacy exemptions, CCMP header and PN; protected management frames (PMF); SAE frame TX and RX; block ack sessions and reorder; regulatory rules, power save, channel width; group owner; state recovery after system error recovery (SER) | Does not use Windows APIs directly (only through `os.h`); does not do the SAE calculations or the 4-way handshake (Windows does them) |
| Upstream rtw89 | `third_party\rtw89\` | C, clang | Chip initialization, firmware download, DMA rings, H2C/C2H, firmware scan, rate control, coexistence, power save (rtw89 LPS and IPS), SER detection and recovery, security CAM | This project does not change any file in it |
| Compatibility layer | `compat\` | C, clang | The subset of the Linux API that rtw89 uses; FreeBSD LinuxKPI 802.11 headers; entry points such as `rtw_linux_init` and `rtw_linux_probe`; INTx mask | No 802.11 logic |
| Bluetooth filter driver | `bt\rtkbtflt\` | C, MSVC | Download the Bluetooth patch to the Bluetooth controller before BTHUSB starts | Does not process Wi-Fi; does not change other parts of the Windows Bluetooth stack |

The Linux API subset in the compatibility layer contains memory, locks, work queues, timers, RCU, NAPI, and `sk_buff`. It also contains PCI configuration space, interrupts, DMA map functions, firmware load, and `printk`.

<a id="interfaces"></a>
## Interfaces between layers

The structures that both compilers use contain only fixed-width fields. Both sides use static assertions to check the structure sizes (for example `driver\diag.c`, `wificx\wifi.h`, `mac\hw.c`).

| Direction | Interface | Defined in | Call context |
|---|---|---|---|
| Driver layer → WiFiCx front end | `RtwWifiPreDeviceCreate`, `RtwWifiPostDeviceCreate`, `RtwWifiPrepareHardware`, `RtwWifiReleaseHardware`, `RtwWifiD0Entry`, `RtwWifiD0Exit` | `driver\rtw89win.h`; implemented in `wificx\device.cpp` | KMDF PnP and power callbacks, PASSIVE |
| WiFiCx front end → driver layer | `RtwProbe`, `RtwRemove` | `driver\rtw89win.h`; implemented in `driver\probe.c` | PASSIVE; the caller holds `Dev->CtlLock` |
| Driver layer → compatibility layer | `rtw_linux_init`, `rtw_linux_probe`, `rtw_linux_remove`, `rtw_linux_exit` | `compat\include\rtw\glue.h` | PASSIVE; the caller holds `Dev->CtlLock` |
| Driver layer → compatibility layer (interrupts) | `rtw_linux_isr`, `rtw_linux_irq_dpc` | `glue.h` | DIRQL; DISPATCH |
| Driver layer → mac layer (settings) | `rtw_linux_set_pmf`, `rtw_linux_set_wfd`, `rtw_linux_set_wide`, `rtw_linux_set_ps_policy`, `rtw_linux_reg_set_user` | `glue.h`; called from `driver\probe.c` `RtwApplySettings` | PASSIVE; after the probe and before the capability report |
| Linux side → driver layer | `rtwk_*`: memory, spin locks, KMUTEX, KEVENT, threads, timers, time, log, bug check, DMA, PCI configuration space, firmware files, interrupt synchronization, AES-CCM and AES-CMAC | `compat\include\rtw\os.h`; implemented in `driver\os.c`, `driver\crypto.c` | The comment of each function gives the IRQL |
| WiFiCx front end → mac layer (station) | `rtw_linux_start`, `rtw_linux_scan_start`, `rtw_linux_bss_export`, `rtw_linux_connect`, `rtw_linux_add_key`, `rtw_linux_set_exemptions`, `rtw_linux_sae_set`, `rtw_linux_tx_data`, `rtw_linux_rx_data`, and others | `glue.h`; implemented in `mac\` | Control plane at PASSIVE; data plane at ≤ DISPATCH |
| WiFiCx front end → mac layer (group owner) | `rtw_linux_ap_*` | `glue.h`; implemented in `mac\ap.c` | Same as above |
| mac layer → WiFiCx front end | Callbacks `rtw_scan_done_fn`, `rtw_conn_event_fn`, `rtw_ap_event_fn`; `tx_ready` and `rx_ready` in `struct rtw_dp_ops` | `glue.h` | Called in a mac layer lock or an rtw89 lock, at ≤ DISPATCH; only record the state and send a signal |
| mac layer → rtw89 | `struct ieee80211_ops`: `start`, `add_interface`, `hw_scan`, `sta_state`, `set_key`, `tx`, `ampdu_action`, and others | `compat\include\net\mac80211.h` | The mac layer holds `wiphy->mtx` (KMUTEX, PASSIVE); it calls `tx` in an RCU reader section |
| rtw89 → mac layer | `linuxkpi_ieee80211_*`: `rx`, `tx_status`, `scan_completed`, `restart_hw`, `iterate_keys`, and others | `mac80211.h`; implemented in `mac\` | `rx` runs in the NAPI work queue thread (PASSIVE); `tx_status` runs at DISPATCH and holds the rtw89 `trx_lock` |
| rtw89 ↔ compatibility layer | Linux kernel headers (`linux\*.h`, `net\*.h`) | `compat\include\` | Same definitions as in Linux |
| User mode → driver layer | Diagnostic IOCTLs `IOCTL_RTW_*`; `tools\rtwdiag.ps1` uses them | `driver\rtw89win_ioctl.h`; data structures in `glue.h` | `\\.\rtw89win`, open only to SYSTEM and administrators |
| User → driver layer | Settings (`Services\rtw89win\Parameters`) | `driver\probe.c` `RtwApplySettings`; `wificx\connect.cpp` `AllowConnect` | Read at each device start; `AllowConnect` is read at each connection |
| Bluetooth filter driver → Bluetooth controller | URBs: HCI commands on the control endpoint, events on the interrupt endpoint | `bt\rtkbtflt\hci.c`, `bt\rtkbtflt\urb.c` | Only before BTHUSB receives START or D0 |

<a id="contexts"></a>
## Run contexts and locks

Conclusion: All control plane work runs in series at PASSIVE. The data plane and the callbacks do not call code that tries to get a lock that is already held.

- **WDI messages**: `RtwEvtWifiSendCommand` runs at ≤ DISPATCH and only puts the request in a queue. The work item `RtwWifiCmdWork` processes the requests one at a time at PASSIVE. It holds `Dev->CtlLock` during this work. A KMDF work item can start again on a different thread at the same time. For this reason, the front end uses the `WorkRunning` flag to stop a second instance. Each request completes exactly one time ([INIT-3](observed-behavior.md#init-3)).
- **Lock order**: Get `Dev->CtlLock` first, and then the mac layer `wiphy->mtx`. The mac layer calls the callbacks while it holds `wiphy->mtx`. The callbacks do not get `Dev->CtlLock`.
- **mac layer callbacks**: The scan, connection, and group owner callbacks only set a flag and queue the WDI work item. The work item then sends the indication. The data plane callbacks `tx_ready` and `rx_ready` only queue the KDPC `DpDpc`, and `DpDpc` calls NetAdapterCx. Reason: the `tx_status` callback runs in the rtw89 `trx_lock`. A direct call to NetAdapterCx causes a self-deadlock ([RTW-3](observed-behavior.md#rtw-3)).
- **rtw89 calls**: The mac layer calls `ieee80211_ops` while it holds `wiphy->mtx`, the same as mac80211. Only one wiphy work item runs at a time. It holds `wiphy->mtx` while it runs.
- **RX**: The rtw89 NAPI poll runs on a work queue thread (PASSIVE), not in a DPC. For this reason, `linuxkpi_ieee80211_rx` runs at PASSIVE.
- **RCU**: One global reader count implements the reader sections. `synchronize_rcu` waits until the count is zero. `call_rcu` adds its callback to a list, and a work item runs the callbacks after the synchronization (`compat\rcu.c`, [RTW-4](observed-behavior.md#rtw-4)).
- **Work queues**: Each work queue has one PASSIVE kernel thread. The thread runs the work items of the queue in sequence (`compat\workqueue.c`).
- **Spin locks**: `rtwk_spin_lock_irq` first raises the IRQL to the device interrupt level. For this reason, it also excludes the ISR.
- **Diagnostic interface**: The IOCTLs that change the state also get `Dev->CtlLock`. For this reason, they cannot run at the same time as the WDI work item or the hardware release.

<a id="control-flow"></a>
## Control flow

For the sequence and the time of the WDI messages in each flow, see [Observed behavior](observed-behavior.md).

### Start

1. `RtwEvtDeviceAdd` calls `RtwGuardDecide`. `RtwGuardDecide` uses the crash loop guard count to select WiFiCx mode or diagnostic mode.
2. In WiFiCx mode, `RtwWifiPreDeviceCreate` configures NetAdapterCx and WiFiCx. `RtwWifiPostDeviceCreate` creates the WDI work item.
3. `RtwCdoCreate` creates the diagnostic interface `\\.\rtw89win`.
4. `RtwEvtPrepareHardware` calls `RtwHwMap` to map BAR2. It also calls `RtwHwLogChipInfo` to record the chip version.
5. If Windows cannot write to the registry hive yet, `RtwGuardWait` tries again every 500 ms. The maximum time is 60 s. The driver does not access the chip before `ArmRunning = 1` is on the disk.
6. `RtwWifiPrepareHardware` calls `RtwProbe`. At the first call, `rtw_linux_init` creates the work queues, RCU, and the mac layer.
7. `RtwOsAttach` creates the DMA adapter (32-bit addresses) and connects the INTx interrupt.
8. `rtw_linux_probe` runs the rtw89 PCI probe. The probe supplies power to the chip, downloads the firmware, reads the efuse, and removes the power. Then it registers with the mac layer. This step does not transmit frames.
9. `RtwApplySettings` sends the settings to the mac layer. `rtw_linux_get_caps` and `ReportCapabilities` report the capabilities. When Wi-Fi Direct is enabled, `RtwWifiReportWfdCapabilities` also reports the Wi-Fi Direct capabilities.
10. WiFiCx calls `RtwEvtWifiCreateAdapter` to create the station network adapter. The first message that needs the radio calls `rtw_linux_start` through `RtwWifiEnsureStarted`. This message is `TASK_DOT11_RESET`, `TASK_SCAN`, `TASK_CONNECT`, or `TASK_START_AP`. rtw89 starts and adds a station vif (the mac80211 virtual interface `ieee80211_vif`). Then it enters IPS (the chip has no power).

In diagnostic mode, the driver does only step 4. After that, you start rtw89 manually with the diagnostic IOCTLs (`rtwdiag.ps1 probe`, `start`, `monitor`, `scan`).

### Scan

1. Wlansvc sends `TASK_SCAN`. `RtwWifiTaskScan` completes the request first. Then it calls `rtw_linux_scan_start`.
2. The mac layer function `scan_start_locked` makes the scan request and calls the rtw89 `hw_scan`. The firmware scans the channels one by one.
3. Received beacons and probe responses move through `linuxkpi_ieee80211_rx` and `mac_bss_rx` to the BSS table (`mac\bss.c`). The BSS table keeps the last beacon frame body and the last probe response frame body of each BSS.
4. The firmware reports the end of the scan. rtw89 calls `linuxkpi_ieee80211_scan_completed`. `scan_done_work` releases the request while it holds `wiphy->mtx`. Then it calls `rtw_scan_done_fn`.
5. The front end callback `ScanDone` only sets a flag and queues the WDI work item. In the work item, `RtwWifiScanEvents` gets the BSSs of this scan through `rtw_linux_bss_export`. It sends the `BSS_ENTRY_LIST` indication and then `SCAN_COMPLETE`.
6. `RtwWifiAbortTask` processes `ABORT_TASK`. It calls `rtw_linux_scan_cancel`. A canceled scan also sends these two indications ([SCAN-4](observed-behavior.md#scan-4), [SCAN-5](observed-behavior.md#scan-5)).

### Connection (WPA2-Personal and WPA3-SAE)

1. Windows sends `TASK_DOT11_RESET` and `SET_PRIVACY_EXEMPTION_LIST`. `rtw_linux_set_exemptions` sends the exemption list to `mac\key.c`.
2. Windows sends `TASK_CONNECT`. `RtwWifiTaskConnect` filters the candidate BSSs ([CONN-2](observed-behavior.md#conn-2)). Then it calls `rtw_linux_connect`.
3. `mac\mlme.c` uses the mac80211 sequence: `wide_choose` selects the channel width → create the channel context (chanctx) → station state NONE → authentication → AUTH → association (`mlme_send_assoc`) → ASSOC.
4. A work item converts the mac layer connection events into indications. The sequence is: `ASSOCIATION_RESULT` (with the association request frame body and the BSS frame body) → `LINK_STATE_CHANGE` → the completion indication of the connect task ([CONN-5](observed-behavior.md#conn-5)).
5. Windows does the 4-way handshake through the data plane. EAPOL frames go without encryption, as the exemption list specifies.
6. Windows sends `SET_ADD_CIPHER_KEYS`. `RtwWifiSetAddCipherKeys` calls `rtw_linux_add_key`, and the mac layer then calls the rtw89 `set_key`. The first pairwise key moves the station entry of the access point to AUTHORIZED.

WPA3-SAE adds only one more authentication part. Windows does the SAE calculations. `mac\sae.c` sends and receives the authentication frames. These two parts exchange two types of messages: the indication `SAE_AUTH_PARAMS_NEEDED` (`RtwWifiSaeIndicate`) and the property `SET_SAE_AUTH_PARAMS` (`RtwWifiSetSaeAuthParams` → `rtw_linux_sae_set`). See [SEC-1](observed-behavior.md#sec-1).

### Mobile hotspot

1. When the setting `WifiDirect` is enabled, the driver reports the Wi-Fi Direct capabilities. WiFiCx calls `RtwEvtWifiCreateWifiDirectDevice` to create the Wi-Fi Direct device port. This port has only a control plane. The mac layer does not create a vif for it.
2. WiFiCx creates the network adapter of the role port (`RtwWifiCreateRoleAdapter`). Then it sends `TASK_START_AP` on the role port ([HS-1](observed-behavior.md#hs-1)).
3. `RtwWifiApTaskStartAp` calls `ChooseChannel` to select the channel. Then it calls `rtw_linux_ap_start`. The mac layer adds an AP type vif, a channel context, and a beacon template. Then it calls the rtw89 `start_ap`.
4. The front end sends the `P2P_GROUP_OPERATING_CHANNEL` indication. Then it sends `LINK_STATE_CHANGE` through `IndicateApLinkState` ([HS-7](observed-behavior.md#hs-7)). Last, it sends `START_AP_COMPLETE`.
5. When a hotspot client connects, the mac layer replies to the open authentication and checks the association request. Then it reports `AP_ASSOCIATION_REQUEST_RECEIVED`.
6. If Windows accepts the hotspot client, it sends `TASK_SEND_AP_ASSOCIATION_RESPONSE`. `rtw_linux_ap_assoc_resp` sends the association response.
7. After the hotspot client acknowledges the association response, the front end sends `SEND_AP_ASSOCIATION_RESPONSE_COMPLETE`. This indication contains the association response frame body and the beacon frame body ([HS-6](observed-behavior.md#hs-6)).
8. Windows does the 4-way handshake through the data plane of the role port. Then it installs the keys.

The firmware sends the beacons at each TBTT. The mac layer makes the probe responses, with a maximum of 30 each second ([HS-14](observed-behavior.md#hs-14)).

### System error recovery (SER)

1. The firmware or the MAC has an error. rtw89 receives a C2H and enters SER level 2. It stops and resets the chip, and it discards the keys while it holds `wiphy->mtx` ([RTW-8](observed-behavior.md#rtw-8)).
2. During the reset, the chip keeps the interrupt line asserted. The compatibility layer function `intx_stopped` masks the chip interrupts ([RTW-9](observed-behavior.md#rtw-9)).
3. rtw89 calls `linuxkpi_ieee80211_restart_hw`. The mac layer sets `restarting` and queues `mac_restart_work`.
4. `mac_restart_work` stops the scan and parks the group owner (`mac_ap_restart`). It disconnects and reports the connection loss (`mac_mlme_restart`).
5. `mac_restart_work` parks the old vif and channel context until the hw is released. Then it starts rtw89 again through `start_locked`.
6. `mac_restart_work` calls `mac_ps_restart` and `reconfig_complete`. This ends the SER.
7. The front end sends `DISASSOCIATION`. Wlansvc sends `TASK_CONNECT` with `RoamRequest=1` and connects again automatically ([CONN-7](observed-behavior.md#conn-7)).

When the group owner operates, the front end sends `DISASSOCIATION` for each hotspot client. Then it sends `STOP_AP` and `CAN_SUSTAIN_AP`. Windows does not start the hotspot again automatically ([HS-13](observed-behavior.md#hs-13), [HS-19](observed-behavior.md#hs-19)).

### Stop and shutdown

When Windows releases the hardware, the driver does these steps in sequence. Windows releases the hardware when you restart the device, disable it, uninstall the driver, or update the driver.

1. `RtwEvtReleaseHardware` calls `RtwWifiReleaseHardware`. This function stops the group owner, disconnects, and stops the scan. Then it calls `rtw_linux_stop`.
2. `RtwRemove` calls `rtw_linux_remove`. This function runs the rtw89 PCI remove and releases the devm memory. Then `rtw_skb_reclaim` reclaims the `sk_buff` buffers that are left.
3. `RtwHwUnmap` unmaps BAR2.
4. `RtwGuardEnd` ends the run (`ArmRunning = 0`).
5. `RtwSaveLog` writes the last 16 KB of the driver log to the setting `SavedLog`.

At shutdown and when you restart the computer, Windows does not release the device. D0 exit occurs after the last registry write to the disk. For this reason, the diagnostic interface registers for the shutdown notification. `RtwEvtShutdown` calls `RtwGuardEnd` and `RtwSaveLog` ([INST-5](observed-behavior.md#inst-5)).

<a id="data-flow"></a>
## Data flow

Conclusion: The driver sends and receives native 802.11 data frames. Windows converts between 802.3 and 802.11. The mac layer processes the encryption headers, the sequence numbers, the reorder, and the PN check.

| Step | Direction | Layer | Function |
|---|---|---|---|
| Copy the frame and return the Windows buffer immediately | TX | mac layer | `mac\tx.c` `rtw_linux_tx_data` |
| Add the QoS Control field and the sequence number of each TID | TX | mac layer | `rtw_linux_tx_data` |
| Use the exemption list to decide if encryption is necessary | TX, RX | mac layer | `mac\key.c` `mac_key_tx`, `mac_key_rx_clear` |
| Write the CCMP header and the PN | TX | mac layer | `mac\key.c` `mac_key_ccmp_hdr` |
| Encrypt the data and add the MIC | TX | Chip (rtw89 configures it) | `ops->tx` |
| A-MPDU aggregation | TX | Chip (the mac layer starts the BA session) | `mac\agg.c` `mac_agg_tx_check` |
| Decrypt the data | RX | Chip | — |
| Remove the FCS | RX | mac layer | `mac\rx.c` `linuxkpi_ieee80211_rx` |
| Remove duplicate frames | RX | mac layer | `mac\rx.c` `rx_data` |
| Reorder the frames (64 slots, 100 ms timeout) | RX | mac layer | `mac\agg.c` `mac_agg_rx_frame` |
| Check the PN; remove the CCMP header and the MIC | RX | mac layer | `mac\key.c` `mac_key_rx`; `mac\rx.c` `mac_rx_deliver` |
| Check for A-MSDU; make a new 24-byte data frame header | RX | mac layer | `mac\rx.c` `mac_rx_deliver`, `rx_fifo_put` |

### TX

```text
NetAdapterCx TX queue
  -> TxAdvance, TxOne (wificx\adapter.cpp): get the packet and the exemption action of each packet
  -> rtw_linux_tx_data (mac\tx.c)
       Not connected: returns -ENOTCONN; the front end discards the packet and counts it
       64 or more frames in transit, or rtw89 stopped the queue: returns -EBUSY; the frame stays in the queue
       EAPOL uses TID 7; other frames use the priority from Windows
  -> ops->tx (rtw89) -> chip
  -> tx_status (DISPATCH, trx_lock held): only updates the counters
  -> tx_ready callback -> DpDpc (KDPC) -> NetAdapterCx calls Advance again
```

### RX

```text
INTx interrupt -> ISR in driver\os.c -> rtw_linux_isr (DIRQL, calls the rtw89 interrupt handler)
  -> DPC: rtw_linux_irq_dpc -> rtw89 interrupt thread (work queue) -> NAPI work queue thread (PASSIVE)
  -> linuxkpi_ieee80211_rx (mac\rx.c)
       Beacons and probe responses -> mac_bss_rx (BSS table)
       Frames to the group owner -> mac_ap_rx
       Management frames -> mac_mlme_rx; BlockAckReq -> mac_agg_rx_bar
       Data frames -> rx_data: discard fragments, remove duplicate frames -> reorder -> mac_rx_deliver
  -> RX FIFO (maximum 256 frames; when full, discard the frame and count it)
  -> rx_ready callback -> DpDpc -> NetAdapterCx -> RxAdvance -> rtw_linux_rx_data (one MSDU per buffer)
```

### Group owner data path

The group owner has its own set of entry points: `rtw_linux_ap_tx_data`, `rtw_linux_ap_rx_data`, and others. The group owner also has its own count of frames in transit (maximum 64 frames), RX FIFO (maximum 256 frames), and callbacks.

- TX: The driver uses address 1 of a unicast frame to find the hotspot client. If the hotspot client is in power save, the driver puts the frame in the buffer of that hotspot client. Each buffer holds a maximum of 64 frames, and the frames expire after 10 s. Multicast frames use the group key.
- RX: The driver accepts only ToDS frames. It uses address 2 to find the hotspot client.
- With the setting `WifiDirect=0`, the group owner code does not run. The station data path does not change.

<a id="protection"></a>
## Protection functions

### Crash loop guard

Conclusion: If two runs in sequence do not end correctly, the next start enters diagnostic mode. The driver then does not access the chip.

- The count is in `ArmRunning` and `BootAttempts` under `Parameters` (`driver\driver.c`). The driver writes these two values itself.
- Before the driver accesses the chip, it writes `ArmRunning = 1` to the disk (`RtwGuardBegin`). If Windows cannot write to the registry hive, `RtwGuardWait` tries again every 500 ms, for a maximum of 60 s. After this time, the start fails and the driver does not access the chip ([INST-4](observed-behavior.md#inst-4)).
- A run ends correctly in one of two ways: hardware release or a shutdown notification. Both ways call `RtwGuardEnd`, which sets the two values to zero.
- When `BootAttempts` is 2, `RtwGuardDecide` selects diagnostic mode. `rtwdiag.ps1 arm` sets the count to zero and restarts the device.
- The driver cannot change to diagnostic mode during the same start, because the change needs re-enumeration (`WdfDeviceFailedAttemptRestart`). After re-enumeration, the chip cannot download firmware until it loses power ([INST-6](observed-behavior.md#inst-6)).

### Diagnostic interface and logs

- The diagnostic interface `\\.\rtw89win` is a KMDF control device object (CDO). Its SDDL gives access only to SYSTEM and administrators. The interface is available in both modes. Reason: NetAdapterCx owns the FDO ([INIT-4](observed-behavior.md#init-4)).
- `RtwEvtDeviceCleanup` deletes the CDO. If the CDO stays, the driver cannot unload.
- The driver log is a 64 KB ring buffer in memory (`driver\log.c`). Each line also goes to `DbgPrintEx`.
- The driver image unloads when the device stops. `RtwSaveLog` writes the saved log at hardware release, at shutdown, and when the start fails ([INST-7](observed-behavior.md#inst-7)).
- When the driver loads, it does known-answer self-tests for AES-CCM and AES-CMAC (`driver\crypto.c` `RtwCryptoSelfTest`).
- The frame-level self-tests (CCMP, BIP) run at the first call to `rtw_linux_set_pmf` (`mac\pmf.c` `pmf_selftest`). This call occurs when the device starts for the first time and reads the settings.
- If one of the self-tests fails, `rtw_linux_set_pmf` disables PMF.

<a id="bluetooth"></a>
## Bluetooth filter driver

Conclusion: The Bluetooth filter driver downloads the Bluetooth patch to the controller before BTHUSB starts. If an error occurs, the driver changes to pass-through, and BTHUSB starts as usual with the ROM firmware.

- **Position**: The extension INF (`bt\rtkbtflt\rtkbtflt.inf`) uses `AddFilter` and `FilterPosition = Lower` to install the filter driver below BTHUSB. The base driver is still the Windows `bth.inf` ([BT-7](observed-behavior.md#bt-7)).
- **Time**: It is a KMDF filter driver, not the power policy owner. Its `PrepareHardware` and `D0Entry` run when the START or D0 request returns up. At that time, BTHUSB has not received the request ([BT-5](observed-behavior.md#bt-5)).
- **Session**: At each change from D3 to D0 (also at start), `hci.c` reads the version. If the controller runs the ROM firmware, the driver downloads the patch. A return from D1 or D2 does nothing, because the patch is still valid ([BT-4](observed-behavior.md#bt-4)).
- **USB access**: At the first start, the driver selects the USB configuration and gets the interrupt pipe itself. At D3 → D0, the driver uses the interrupt pipe that it recorded when BTHUSB selected the configuration (`urb.c`).
- **Capture**: `urb.c` is a WDM preprocess routine. A BTHUSB event read request can get the command complete event that the session waits for. In that case, `urb.c` keeps the event for the session, and BTHUSB does not receive it.
- **Errors**: `D0Entry` always returns success. The driver changes to pass-through when the patch file is not correct, a command fails, or the controller is not an 8852B ROM.
- **Crash loop guard**: The count is in `SessionRunning` and `BadSessions` under `Services\rtkbtflt\Parameters`. The driver writes these values itself. After 2 sessions in sequence that do not end correctly, the driver uses pass-through until you run `rtkbtflt.ps1 clear`. If a session changed the device, `SessionRunning = 1` stays for 60 s more, or until the session ends correctly. The setting `PassThrough=1` forces pass-through.
- **Shutdown notification**: The driver receives it through a separate CDO ([INST-5](observed-behavior.md#inst-5)).

`bt\rtkbtfw\` has two parts:

- `epatch.c`: It parses the patch file and calculates the segments. The Bluetooth filter driver and the tool both use this file. It has a host test (`test-epatch.c`).
- `rtkbtfw.exe`: A user-mode tool. `rtkbtfw info` shows the controller version and the result of the Bluetooth filter driver. Use `load` only for diagnosis ([BT-3](observed-behavior.md#bt-3)).

<a id="build-structure"></a>
## Build structure

Conclusion: `buildhal.ps1` uses clang to compile the Linux side into `rtw89hal.lib`. Then `build.ps1` uses MSBuild to compile the Windows side and link the driver. For the build steps, see [Build](build.md).

```text
third_party\rtw89\*.c (26 files) + compat\*.c + mac\*.c
   --clang, --target=aarch64-w64-mingw32--> build\ARM64\Release\rtw89hal.lib
driver\*.c + wificx\*.cpp + rtw89hal.lib + WificxTLVGenParse.lib + libcntpr.lib + ksecdd.lib
   --MSBuild (MSVC)--> rtw89win.sys
```

- **clang target**: The build uses the MinGW target. Only this target supports `-mno-ms-bitfields`, which arranges bit fields with the GCC rules. rtw89 puts bit-field structures directly on firmware and efuse data ([BUILD-1](observed-behavior.md#build-1)).
- **ABI**: On ARM64, the two targets have the same call convention, LLP64 types, COFF relocations, and x18 reservation rule. For this reason, MSVC can link the object files from clang.
- **clang options**: The options are the same as for the arm64 Linux kernel: `-mgeneral-regs-only` (no FP/SIMD registers), `-fno-strict-aliasing`, `-fwrapv`, `-ffreestanding`. The macros are `-D__KERNEL__`, `-DCONFIG_RTW89_8852BE=1`, and `-DCONFIG_RTW89_PCI=1`.
- **Object file names**: The upstream files keep their names. The files from `compat\` and `mac\` get a directory prefix (for example `mac_util`), because upstream also has a `util.c`.
- **`IS_ENABLED`**: `compat\include\rtw\base.h` uses an explicit table. An option that is not in the table causes a compile error.
- **MSVC**: `/W4 /WX`; C++ files use `/Zc:sizedDealloc-`; KMDF 1.33, WiFiCx 1.1, NetAdapterCx 2.4 ([BUILD-2](observed-behavior.md#build-2)).
- **Import table**: Only `ntoskrnl.exe`, `HAL.dll`, `WDFLDR.SYS`, and `ksecdd.sys`. NetAdapterCx and WiFiCx bind through `WDFLDR.SYS` ([BUILD-3](observed-behavior.md#build-3)).
- **INF**: The `[Manufacturer]` section of `driver\rtw89win.inf` targets only `NTARM64.10.0...26100`. This is Windows 11 ARM64 build 26100 and later.
- **Bluetooth**: `bt\rtkbtflt\` and `bt\rtkbtfw\` use only MSVC, not clang.

<a id="decisions"></a>
## Design decisions

| Decision | Reason | Cost or limit |
|---|---|---|
| Do not change one line of upstream rtw89 | You can replace the full directory at an upgrade. The interface between the layers is the mac80211 `ieee80211_ops`. | The mac layer and the compatibility layer must copy Linux details, for example the `sk_buff` layout ([HS-18](observed-behavior.md#hs-18)). |
| Write a custom mac layer | Windows has no mac80211. With WiFiCx, the driver must do authentication and association. mac80211 uses the GPL. FreeBSD LinuxKPI shows that this method works. | This project maintains all 802.11 logic. This project implements only the necessary parts (see [Porting guide](porting.md#mac-assumptions)). |
| Use the BSD headers from FreeBSD LinuxKPI, not the GPL headers from Linux. Mark the changes with `rtw89win:`. | The full project can use BSD-3-Clause. | Some definitions in the FreeBSD headers are different from the Linux headers, for example `ieee80211_hdrlen` ([WIDE-6](observed-behavior.md#wide-6)). |
| Use the clang MinGW target with `-mno-ms-bitfields` | rtw89 uses GNU extensions. Bit fields must use the GCC layout. The ABI is the same as MSVC. | The build needs two toolchains. |
| Use MSVC for the Windows side | The WiFiCx TLV library is a C++ library that MSVC compiled. | The build must meet the link requirements of the TLV library ([BUILD-2](observed-behavior.md#build-2)). |
| Use the compile options of the arm64 kernel | They agree with the assumptions of rtw89 on Linux. | — |
| Run NAPI in a work queue thread. Each work queue has one thread and runs in sequence. RCU uses a reader count. | The rtw89 poll function can run at PASSIVE. An RCU writer must wait until all readers exit. | All work queues run in series. You cannot call `synchronize_rcu` at DISPATCH. |
| Create a custom DMA adapter and DMA heap | The driver must control the addresses below 4 GB and the cache type. The RK3588 PCIe is not cache coherent ([PLAT-2](observed-behavior.md#plat-2)). | DMA uses only 32-bit addresses. Data in memory outside the heap moves through bounce buffers. |
| The compatibility layer masks INTx for rtw89 | rtw89 expects MSI. While rtw89 is stopped, a level-triggered INTx causes an interrupt storm ([RTW-9](observed-behavior.md#rtw-9)). | The mask registers are for RTL8852BE (AX generation). Check them again for other chips. |
| Reclaim the `sk_buff` buffers after the rtw89 remove | At stop, upstream leaves one C2H frame. The driver then cannot return the DMA heap, and the next probe fails ([RTW-1](observed-behavior.md#rtw-1)). | — |
| Use a crash loop guard. If Windows cannot write to the hive, the start fails. | This prevents a bug check at the same location at each boot. If the driver accesses the chip before it records the count, the count does not work. | Early in the boot, the driver can wait for a maximum of 60 s. After two abnormal ends in sequence (also power loss), you must recover manually. |
| The group owner uses an AP type vif. The Wi-Fi Direct device port has no vif. The group owner vif exists only while the hotspot operates. | This prevents MCC and NoA. There is no effect on the station power save. | No P2P discovery or negotiation. |
| A separate data plane for the group owner | With `WifiDirect=0`, the station path is the same as without the hotspot function. | Two sets of similar code. |
| No MCC | For the mobile hotspot, one channel context for the station and the group owner is enough. | While the station is connected, the hotspot can use only the same channel ([HS-12](observed-behavior.md#hs-12)). |
| Keep HT20 on 2.4 GHz. Use no wider channel for the group owner. Use no HE. | The default path does not contain functions that the tests cannot examine. | Lower rates on 2.4 GHz and for the hotspot. No 802.11ax. |
| With `Bandwidth=20`, use only HT20 and send no VHT in the association request | This gives a known fallback for wide channel problems. | Lower throughput. |
| Use only CCMP-128. PMF uses software and CNG. | RTL8852B does not encrypt management frames ([SEC-5](observed-behavior.md#sec-5)). CNG is the AES implementation in the kernel. | No TKIP or GCMP. The import table also has `ksecdd.sys`. |
| Regulatory rules use the full wireless-regdb. The priority is `Country` > country element of the access point > efuse > world rules. At registration, apply the world rules first. | WDI does not send the country code ([REG-1](observed-behavior.md#reg-1)). Without regulatory rules, rtw89 accesses a NULL pointer ([REG-2](observed-behavior.md#reg-2)). | The user must set the country code. |
| Bluetooth uses an extension INF and `AddFilter`, a WDM preprocess routine, and raw URBs (no `WDFUSBDEVICE`). | With test signing, a full INF has a lower rank than the Windows driver. The base driver stays `bth.inf`, so an uninstall restores the Windows Bluetooth stack. | The filter driver must select the USB configuration and capture the events itself. |
| Use pass-through when Bluetooth fails. The guard count stays for 60 s. A `PassThrough` setting is available. | Windows can start Bluetooth when the patch fails. Bug checks from other causes do not count against the filter driver. | In pass-through, the controller runs the ROM firmware and cannot find devices in a scan ([BT-1](observed-behavior.md#bt-1)). |

<a id="source-tree"></a>
## Source directories

| Directory or file | Content |
|---|---|
| `build.ps1` | Builds the Wi-Fi driver package: calls `buildhal.ps1`, then compiles with MSBuild, signs, and makes the driver package |
| `buildhal.ps1` | Uses clang to compile `rtw89hal.lib` |
| `packages.config` | NuGet packages for the WDK and the SDK |
| `local.settings.example.psd1` | Example of the build settings |
| `LICENSE`, `THIRD_PARTY_NOTICES` | The license of this project and the third-party notices (the build scripts put them into the driver packages) |
| `driver\` | Driver layer: `driver.c` (entry, PnP, crash loop guard, diagnostic interface); `probe.c` (start and stop rtw89, read the settings); `os.c` (`rtwk_*`, DMA, interrupts); `hw.c` (BAR2, PCI configuration); `diag.c` (diagnostic IOCTLs); `log.c` (driver log); `crypto.c` (AES with CNG); `rtw89win.inf` |
| `wificx\` | WiFiCx front end (see the table below) |
| `mac\` | mac layer (see the table below) |
| `compat\` | Compatibility layer: `glue.c` (entry points); `device.c` (PCI, interrupts, DMA, firmware); `skbuff.c`; `netdev.c` (NAPI); `workqueue.c`; `rcu.c`; `printk.c`; `lib.c`; `rtw89info.c` (reads the internal rtw89 state for diagnosis) |
| `compat\include\rtw\` | Interface headers of this project: `glue.h`, `os.h`, `base.h`, `bus.h`, `skbuff.h`, and others |
| `compat\include\linux\`, `asm\`, `sys\`, `net\`, `net80211\` | Linux API headers; 6 of the 802.11 headers come from FreeBSD LinuxKPI and 2 from FreeBSD net80211 (commit in `compat\FREEBSD_REV`) |
| `third_party\rtw89\` | The 49 upstream rtw89 files (not changed), two licenses, `README.md` (source and the blob id of each file) |
| `bt\rtkbtflt\` | Bluetooth filter driver, INF, and scripts for build, installation, uninstallation, status, and rollback |
| `bt\rtkbtfw\` | Source code and host test of `epatch.c` and `rtkbtfw.exe` |
| `tools\` | Scripts in the driver package: `install-rtw89win.ps1`, `uninstall-rtw89win.ps1`, `rtwdiag.ps1`, `rollback-rtw89win.sh` |
| `tools-dev\` | Build helper scripts and host tests: `buildenv.ps1`, `new-testcert.ps1`, `undef.ps1`, `errsum.ps1`, `regdb-gen.ps1`, `apframe-test`, `wide-test`, `crypto-test` |
| `tools-dev\board\` | Developer test scripts (they run on the target device; see `tools-dev\board\README.txt`) |
| `hosttools\` | Scripts on the measuring PC that test the target device: `rtw89win-thru.ps1` (throughput), `rtw89win-tcprtt.ps1` (TCP round-trip time) |
| `firmware\` | Firmware license, WHENCE excerpt, `SHA256SUMS`, instructions to get the firmware; the firmware files are not in the repository |
| `docs\` | This document set |

WiFiCx front end (`wificx\`):

| File | Content |
|---|---|
| `device.cpp` | Device initialization, capability report (`ReportCapabilities`), `PrepareHardware`, D0 entry and exit |
| `command.cpp` | Queue and dispatch of WDI messages (`RtwEvtWifiSendCommand`, `RtwWifiCmdWork`), `TASK_DOT11_RESET`, radio state, power save hints, `WdiDump` |
| `scan.cpp` | `TASK_SCAN`, `ABORT_TASK`, `BSS_ENTRY_LIST` |
| `connect.cpp` | `TASK_CONNECT`, `TASK_DISCONNECT`, association result, link state, connection loss |
| `security.cpp` | Exemption list, installation and removal of keys |
| `sae.cpp` | WPA3-SAE indications and parameters |
| `adapter.cpp` | Network adapters of the station and the role port, TX and RX queues, data path |
| `wfd.cpp` | Wi-Fi Direct capabilities, Wi-Fi Direct device port, port dispatch |
| `ap.cpp` | Group owner: `TASK_START_AP`, channel selection, association of hotspot clients, `TASK_STOP_AP` |
| `alloc.cpp` | `operator new` and `operator delete` for the TLV library |
| `wifi.h` | Internal declarations; notes about threads and locks |

mac layer (`mac\`):

| File | Content |
|---|---|
| `mac.h` | Internal declarations |
| `hw.c` | Life cycle of `wiphy` and `ieee80211_hw`, wiphy work items, capability export, `restart_hw` |
| `iface.c` | vif, channel context, RX filter, start and stop, recovery after SER (`mac_restart_work`) |
| `scan.c` | Firmware scan (`hw_scan`) |
| `bss.c` | BSS table and frame body export |
| `mlme.c` | Station authentication, association, connection, disconnection, beacon loss |
| `key.c` | Keys, exemptions, CCMP header and PN check |
| `pmf.c` | PMF: software CCMP and BIP, SA Query |
| `sae.c` | TX and RX of SAE authentication frames |
| `tx.c` | TX, management frame TX, TX status |
| `rx.c` | RX, removal of duplicate frames, RX FIFO |
| `agg.c` | BA sessions and reorder |
| `ps.c` | Station power save policy |
| `reg.c`, `regdb.h` | Regulatory rules; `tools-dev\regdb-gen.ps1` generates `regdb.h` from wireless-regdb |
| `wide.c`, `wide.h` | Channel width and VHT capabilities (pure functions, with host tests) |
| `ap.c` | Group owner: AP type vif, hotspot clients, keys, power save buffer, data path |
| `apframe.c`, `apframe.h` | Group owner beacon, probe response, association response (pure functions, with host tests) |
| `util.c` | Functions without state for channel numbers, rates, TIDs, and other items |
| `stubs.c` | Parts that are not implemented: txq scheduler, channel context emulation |

For the terms, see the [Glossary](glossary.md). To add a chip or to change the hardware platform, see the [Porting guide](porting.md).
