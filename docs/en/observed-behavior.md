# Observed behavior

[中文](../zh/observed-behavior.md)

- **Readers**: Driver developers. This includes persons who write WiFiCx drivers and persons who port Linux Wi-Fi drivers to Windows.
- **What you will learn**: The actual behavior of WiFiCx, WDI, and Windows where the Microsoft documentation does not describe it, or where it is different from the Microsoft samples. Also, the requirements to connect the upstream rtw89 to Windows ARM64.
- **Prerequisites**: You know the WDI message model (tasks, properties, and indications).

<a id="summary"></a>
## Summary

1. [CONN-2](#conn-2): For each connection, Wlansvc sets `AllowedBSSIDs` to the broadcast address. This value means "any BSSID".
2. [SCAN-4](#scan-4): The `OriginalTaskOID` of `ABORT_TASK` is the message ID, not the OID value.
3. [SEC-1](#sec-1): `SET_SAE_AUTH_PARAMS` has a transaction ID that Windows assigns. If the driver checks the transaction ID as the Microsoft sample does, WPA3-SAE connections always fail.
4. [HS-2](#hs-2): The Wi-Fi Direct capabilities have minimum values. Below these values, the station network adapter also disappears.
5. [HS-7](#hs-7): The driver must send `LINK_STATE_CHANGE` on the role port. If it does not, Windows discards the EAPOL frames of hotspot clients.
6. [HS-8](#hs-8): Do not call `WifiAdapterAddPeer`. If TX demux is not configured, this call causes a bug check.
7. [INST-2](#inst-2): If you uninstall and then install again, the interface GUID changes. Windows then cannot use the WLAN profiles. An in-place upgrade keeps the interface GUID.
8. [INST-6](#inst-6): After re-enumeration, the driver cannot download firmware to the RTL8852BE until the chip loses power.
9. [RTW-9](#rtw-9): With only INTx interrupts, rtw89 does not mask the chip interrupts during system error recovery (SER). The compatibility layer must mask them. If it does not, one SER causes a bug check.
10. [REG-1](#reg-1): Windows does not send a country code. The driver must include its own regulatory rules and must give the user a setting.

<a id="how-to-read"></a>
## How to read this document

Each finding has a fixed ID (for example, `SCAN-4`) and an anchor with the same name in lowercase (`observed-behavior.md#scan-4`). Each finding has four items:

- **Finding**: One sentence that describes the behavior. Details follow if necessary.
- **Evidence**: The confidence level first, then the method of observation.
- **Effect**: The result if you do not obey the finding.
- **Code**: The related file and function in this project. Paths are relative to the root directory of the repository.

The confidence level uses only three terms:

| Confidence | Definition |
|---|---|
| Verified on hardware | Logs, counters, or symptoms on the target device show the behavior. If the test occurred only one time, the text says "tested one time". |
| Inferred from code | The result comes from the WDK headers, the Microsoft samples, the upstream source code, or the source code of this project. A finding that only host tests verify is also in this category. |
| Not determined | The available evidence is not enough. The text gives the evidence that is not available. |

Other conventions:

- For the test environment and the labels (AP-1, AP-2, AP-3, Client-1), see [Test environment](validation.md#environment) in the validation status.
- WDI messages have the form `TASK_SCAN`(4). The number in parentheses is the message ID in the WDK header `dot11wdi.h`.
- "Request completion" means that the driver calls `WifiRequestComplete`. "Task completion indication" means the indication that the driver sends at the end of a task, for example `SCAN_COMPLETE`(60).
- For the responsibilities and the call contexts of each layer, see [Architecture](architecture.md). For terms, see the [Glossary](glossary.md).

<a id="init"></a>
## Initialization and capability report

<a id="init-1"></a>
### INIT-1 Windows uses a later WDI version than the WDK headers

- **Finding**: In Windows 11 26100, the WDI version is 2.0.14. In WDK 26100, `WDI_VERSION_LATEST` is 2.0.13. The driver uses `WDI_VERSION_LATEST` for initialization. To parse and generate TLVs, set `PeerVersion` to the version that `WifiDeviceGetOsWdiVersion` returns.
- **Evidence**: Verified on hardware. The driver log records the two version numbers. `netsh wlan show wirelesscapabilities` shows WiFiCx OS 1.3 and IHV 1.1.
- **Effect**: The result of an incorrect `PeerVersion` is not determined (no counterexample is available).
- **Code**: `wificx\device.cpp` `RtwWifiPostDeviceCreate`

<a id="init-2"></a>
### INIT-2 Initialize capability structures with the `*_INIT` macros and keep deprecated fields at 0

- **Finding**: The capability report has these requirements:
  1. Initialize each `WIFI_*_CAPABILITIES` structure with its `*_INIT` macro. The macro sets `Size`.
  2. Keep the `*_Deprecated` and `DoNotUseOsReserved` fields at 0.
  3. WiFiCx skips authentication and cipher combinations that it does not accept. It does not report an error.
  4. The driver reports capabilities in `EvtDevicePrepareHardware`. WiFiCx copies the arrays. After the call returns, the driver can release the arrays.
- **Evidence**: Inferred from code: `wificx.sys` contains error strings for checks of `Size`, deprecated fields, and incompatible combinations. Verified on hardware: the capability report of this driver is successful each time.
- **Effect**: If a structure is not correct, the capability report fails. If a combination is not compatible, Windows does not show the related security type. Windows also does not report an error.
- **Code**: `wificx\device.cpp` `ReportCapabilities`, `FillPhy`

<a id="init-3"></a>
### INIT-3 Process WDI messages in sequence on one thread, and complete each request exactly one time

- **Finding**: The driver must process WDI messages in sequence on one thread:
  1. WiFiCx calls `EvtWifiDeviceSendCommand` at IRQL DISPATCH_LEVEL or lower. In this call, the driver only puts the message in a queue.
  2. One PASSIVE_LEVEL work item processes each message in sequence: parse → request completion → execution → indication → task completion indication.
  3. If a KMDF work item enters the queue again while it runs, a second instance runs at the same time on a different thread. The driver must use its own flag to prevent this.
  4. Complete each `WIFIREQUEST` exactly one time. After request completion, the driver must not access the request buffer.
- **Evidence**: Inferred from code (the IRQL rules of the WDK and the behavior of KMDF work items). Verified on hardware: in each run, the number of WDI messages received is equal to the number of completions (counters in `rtwdiag.ps1 mode`).
- **Effect**: If the driver does not complete a request, or completes it two times, WiFiCx waits until a timeout occurs. Alternatively, the system stops with a bug check. If two threads process messages at the same time, the completion order changes.
- **Code**: `wificx\command.cpp` `RtwEvtWifiSendCommand`, `RtwWifiCmdWork`

<a id="init-4"></a>
### INIT-4 The diagnostic interface is not on the device object that NetAdapterCx manages

- **Finding**: The driver creates a separate device object for the diagnostic interface:
  1. NetAdapterCx controls the functional device object. No documentation tells if `IRP_MJ_DEVICE_CONTROL` on this object arrives at the KMDF queue.
  2. For this reason, the driver uses `WdfControlDeviceInitAllocate` to create a separate device object for the diagnostic interface `\\.\rtw89win`. Its SDDL gives access only to SYSTEM and administrators.
  3. KMDF does not delete this object automatically. The driver must delete it in the cleanup callback of the functional device object.
- **Evidence**: Inferred from code (a design decision). Verified on hardware: the diagnostic interface operates in WiFiCx mode and in diagnostic mode. After many device restarts, no object remains.
- **Effect**: If the driver does not delete this object, the driver image stays in memory after the device stops. To change the driver version, you must then restart the computer.
- **Code**: `driver\driver.c` `RtwCdoCreate`, `RtwEvtDeviceCleanup`

<a id="scan"></a>
## Scan

<a id="scan-1"></a>
### SCAN-1 WDI messages that Wlansvc sends after it starts

- **Finding**: After Wlansvc starts, it sends messages in this sequence:
  1. `SET_ADAPTER_CONFIGURATION`(85), port number `0xFFFF`.
  2. `TASK_DOT11_RESET`(7).
  3. `GET_STATISTICS`(34).
  4. `TASK_SCAN`(4). Approximately 110 ms later, `ABORT_TASK`(75) stops this scan.
  5. `SET_LOCATION_PRIVACY`(138).
  6. `TASK_SCAN`(4).

  When the station is disconnected, Wlansvc sends `TASK_SCAN` every 60 s. When the station is connected, Wlansvc sends `GET_STATISTICS` approximately every 40 s. The driver returns `STATUS_NOT_SUPPORTED` for 34 and 138. Scan and connection are not affected. `TASK_SET_RADIO_STATE`(11) is not in this sequence. Windows sends it only when the radio switch changes.
- **Evidence**: Verified on hardware (the WDI message sequence in the driver log and the `WdiDump` output).
- **Effect**: A minimum implementation does not have to support 34 and 138. The driver code for the radio switch is verified on the target device: after the radio is set to off and then to on, the station can connect again.
- **Code**: `wificx\command.cpp` `Dispatch`, `SetAdapterConfiguration`, `TaskSetRadioState`

<a id="scan-2"></a>
### SCAN-2 Windows keeps its own BSS list

- **Finding**: If the capability `BSSListCachemanagement = FALSE`, Windows keeps its own BSS list. It never sends `GET_BSS_ENTRY_LIST`(47). It also never sends `SET_FLUSH_BSS_ENTRY`(97).
- **Evidence**: Verified on hardware. These two messages did not occur in any test.
- **Effect**: The driver does not have to implement BSS cache queries. For each scan, the driver reports all the BSSs that it received during that scan.
- **Code**: `wificx\device.cpp` `ReportCapabilities`; `wificx\command.cpp` `Dispatch`

<a id="scan-3"></a>
### SCAN-3 Format requirements for `BSS_ENTRY_LIST`

- **Finding**: Each entry in `BSS_ENTRY_LIST`(62) must obey these four rules:
  1. The frame body starts at the 8-byte Timestamp. It does not include the 24-byte MAC header or the FCS.
  2. `ChannelInfo.BandId` uses the WDI numbers: 1 for 2.4 GHz, 2 for 5 GHz, 5 for 6 GHz. Do not use the NL80211 numbers.
  3. The channel number must be in the channel table that the capabilities report.
  4. The `TransactionId` of the indication is 0.
- **Evidence**: Inferred from code (the test data `wifihaltestdata.h` in the Microsoft sample). Verified on hardware: Windows used these entries for all connections. Users also selected networks from the Wi-Fi list on the taskbar.
- **Effect**: If an entry does not obey one of the rules, the Windows network list is empty or does not show some networks.
- **Code**: `wificx\scan.cpp` `RtwWifiScanEvents`; `mac\bss.c` `mac_bss_rx`, `rtw_linux_bss_export`

<a id="scan-4"></a>
### SCAN-4 The `OriginalTaskOID` of `ABORT_TASK` is the message ID

- **Finding**: In `ABORT_TASK`(75), WiFiCx sets `OriginalTaskOID` to the message ID (4 for a scan). It does not use the value that `WDI_DEFINE_OID(WDI_TASK_SCAN)` in the header calculates (`0xE4400004`). The driver must accept the two forms. The driver processes an abort of a connect task in the same way.
- **Evidence**: Verified on hardware. If the driver accepts only the OID value, the abort has no effect, and the scan continues to the end. If the driver accepts the two forms, the scan stops in less than 10 ms.
- **Effect**: If the abort has no effect, Wlansvc must wait until the full scan is complete.
- **Code**: `wificx\scan.cpp` `RtwWifiAbortTask`

<a id="scan-5"></a>
### SCAN-5 Send a completion indication also for an aborted scan

- **Finding**: When a scan is aborted or interrupted, the driver first sends the `BSS_ENTRY_LIST` for the BSSs that it received. Then it sends `SCAN_COMPLETE`(60). The status is `NDIS_STATUS_REQUEST_ABORTED` (`0xC001000C`), not `STATUS_REQUEST_ABORTED`. If a scan task is not complete when the device enters D0Exit, the driver also sends these two indications.
- **Evidence**: Verified on hardware. The tests included an aborted scan and a device restart during a scan.
- **Effect**: If the driver does not send the task completion indication, WiFiCx can only use a timeout to find the end of the task.
- **Code**: `wificx\scan.cpp` `RtwWifiScanStop`, `RtwWifiScanEvents`; `wificx\device.cpp` `RtwWifiD0Exit`

<a id="scan-6"></a>
### SCAN-6 Scan dwell time

- **Finding**: The driver gives the WDI active dwell time to rtw89. It limits the value to 45–110 ms.
  1. The Wlansvc scan request: wildcard SSID, all channels, type AUTO, dwell time 80/110 ms, maximum 4000 ms.
  2. The rtw89 firmware scan uses an active dwell time of 45 ms by default. Passive channels and DFS channels use a minimum of 105 ms.
  3. With the world rules, one scan of 39 channels takes approximately 4.1 s.
  4. When the station is connected, one scan takes approximately 5.4 s. The connection stays available.
- **Evidence**: Verified on hardware. If the driver does not give the dwell time to rtw89, the same 39 channels take approximately 3.7 s.
- **Effect**: With a shorter dwell time, the scan misses more access points that have a weak signal. The driver uses `MaximumScanTime` only for its own timeout.
- **Code**: `wificx\scan.cpp` `RtwWifiTaskScan`; `mac\scan.c` `scan_req_alloc`

<a id="scan-7"></a>
### SCAN-7 When location services are off, command-line tools cannot show networks

- **Finding**: When Windows location services are off, these operations report "Access is denied" ("Network shell commands need location permission"):
  1. `netsh wlan show networks`.
  2. The wlanapi functions `WlanGetAvailableNetworkList`, `WlanGetNetworkBssList`, and `WlanScan`.
  3. `netsh wlan show interfaces` when the station is connected.

  A change to only the location consent value in the registry has no effect.
- **Evidence**: Verified on hardware.
- **Effect**: For acceptance tests and for fault isolation, use the Wi-Fi list on the taskbar, `Get-NetAdapter`, `Get-NetIPAddress`, or the WLAN-AutoConfig event log. Alternatively, enable location services for a short time.
- **Code**: None (Windows behavior).

<a id="connect"></a>
## Connection, keys, and privacy exemption

<a id="conn-1"></a>
### CONN-1 WDI messages that Windows sends for a WPA2-Personal connection

- **Finding**: For one WPA2-Personal connection, the message sequence is:
  1. `TASK_DOT11_RESET`(7), `SetDefaultMIB=1`.
  2. `GET_STATISTICS`(34).
  3. `SET_PRIVACY_EXEMPTION_LIST`(28), 1 entry.
  4. `TASK_CONNECT`(6): 1 candidate; authentication RSNA-PSK only; unicast CCMP; multicast NONE and CCMP; no AKM table; `ExcludeUnencrypted=1`.
  5. Approximately 0.43 s after association: `SET_ADD_CIPHER_KEYS`(29) installs the pairwise key. The message has no key ID. The driver uses 0.
  6. Approximately 6 ms later: `SET_ADD_CIPHER_KEYS`(29) installs the group key. The access point selects the key ID (1 or 2 in the tests). The RSC is 0.

  At disconnection, the sequence is `TASK_DISCONNECT`(8, reason 3) → `TASK_DOT11_RESET`(7, `SetDefaultMIB=0`). From `netsh wlan connect` to a DHCP address takes approximately 7 s. Of this time, 3–4 s is the Wlansvc scan before the connection.
- **Evidence**: Verified on hardware (AP-1, AP-2). The byte order of the group key RSC is not determined: Windows only gave the value 0.
- **Effect**: After a successful association, the driver must first let EAPOL frames pass without encryption. Then it accepts keys. Windows does the 4-way handshake. The driver does not do the 4-way handshake.
- **Code**: `wificx\connect.cpp` `RtwWifiTaskConnect`, `RtwWifiTaskDisconnect`; `wificx\security.cpp` `RtwWifiSetAddCipherKeys`

<a id="conn-2"></a>
### CONN-2 The `AllowedBSSIDs` of `TASK_CONNECT` is the broadcast address

- **Finding**: Each `TASK_CONNECT`(6) from Wlansvc has `AllowedBSSIDs` = {`FF:FF:FF:FF:FF:FF`}. This value means "any BSSID". An empty list also means no limit. The driver still compares `DisallowedBSSIDs` as exact 6-byte addresses.
- **Evidence**: Verified on hardware.
- **Effect**: If the driver compares `AllowedBSSIDs` as exact 6-byte addresses, it removes all candidates. The connection fails. The driver log reports that no usable candidate is available.
- **Code**: `wificx\connect.cpp` `AddressListed`, `RtwWifiTaskConnect`

<a id="conn-3"></a>
### CONN-3 Format of the privacy exemption list

- **Finding**: After `TASK_DOT11_RESET` and before `TASK_CONNECT`, Windows sends 1 privacy exemption: EtherType `0x888E`, action 2 (`ON_KEY_MAPPING_KEY_UNAVAILABLE`). The EtherType in the message is big-endian. This driver reports `PrivacyExemptionListSize = 8` in its capabilities.
- **Evidence**: Verified on hardware (the content of the privacy exemption list in the driver log).
- **Effect**: If the byte order is reversed, the driver discards EAPOL frames. A timeout of the 4-way handshake then occurs.
- **Code**: `wificx\security.cpp` `RtwWifiSetPrivacyExemptionList`; `mac\key.c` `exempt_lookup`, `mac_key_tx`, `mac_key_rx_clear`

<a id="conn-4"></a>
### CONN-4 WiFiCx enables the per-packet exemption action only on the TX queue

- **Finding**: WiFiCx enables the `ms_packet_wifiexemptionaction` extension only on the TX queue. It does not enable the extension on the RX queue.
- **Evidence**: Verified on hardware. The driver log records "exemption action on" for the TX queue and "exemption action off" for the RX queue.
- **Effect**: The RX path cannot use this extension. On the TX path, the driver must also examine the privacy exemption list for frames that have the extension value "no exemption".
- **Code**: `wificx\adapter.cpp` `CreateTxQueue`, `CreateRxQueue`, `TxOne`

<a id="conn-5"></a>
### CONN-5 Mandatory content of `ASSOCIATION_RESULT`

- **Finding**: `ASSOCIATION_RESULT`(76) must obey these requirements:
  1. `AssociationRequestFrame` contains the full body of the association request frame that the driver sent. Windows uses its RSN element in message 2 of the 4-way handshake.
  2. `BeaconProbeResponse` contains the frame body of the candidate BSS. Windows uses it to check message 3 of the 4-way handshake.
  3. For WPA2-Personal, `PortAuthorized = FALSE`. Windows authorizes the port.
  4. `ActivePhyTypeList` is mandatory (from WiFiCx model 1.1.4).
  5. On success, `StatusCode` must be 0.
- **Evidence**: Verified on hardware (WPA2-Personal and WPA3-SAE connections were successful). The mandatory status of `ActivePhyTypeList` is inferred from code (the WiFiCx model file).
- **Effect**: If the RSN elements are different, the 4-way handshake fails. If a mandatory field is not present, the driver cannot generate the indication.
- **Code**: `wificx\connect.cpp` `IndicateAssociation`; `mac\mlme.c` `mlme_send_assoc`

<a id="conn-6"></a>
### CONN-6 WiFiCx manages the link state of the station network adapter

- **Finding**: For the network adapter of the station port, the driver does not have to call `NetAdapterSetLinkState`. WiFiCx uses the association and disassociation indications to manage the connection state. The role port is different. See [HS-7](#hs-7).
- **Evidence**: Verified on hardware. WPA2-Personal, WPA3-SAE, and 5 GHz connections operated correctly from start to end.
- **Effect**: It is not necessary to set the link state on the station port.
- **Code**: `wificx\adapter.cpp` `RtwEvtWifiCreateAdapter`, `SetDataCapabilities`; `wificx\connect.cpp` `IndicateLinkState`

<a id="conn-7"></a>
### CONN-7 After a connection loss, Wlansvc reconnects with a roam request

- **Finding**: After the driver reports `DISASSOCIATION`(51), Wlansvc immediately sends `TASK_CONNECT` with `RoamRequest=1`. An example is reason 61 after SER (`WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN`). If the port is idle, the driver must process this request as a usual connection.
- **Evidence**: Verified on hardware. If the driver rejects the request, Wlansvc tries three times. Then it sends `TASK_DOT11_RESET` and stops. If the driver processes the request as a usual connection, the station reconnects immediately after SER.
- **Effect**: If the driver rejects roam requests, the connection does not recover automatically after SER. After that, WLAN profiles with "Connect manually" also do not connect automatically.
- **Code**: `wificx\connect.cpp` `RtwWifiTaskConnect`

<a id="conn-8"></a>
### CONN-8 Behavior when the access point disconnects

- **Finding**: Before AP-2 stops, it sends deauth (reason 0). The driver disconnects in 2.9 s. It deletes the two keys and reports `DISASSOCIATION`(51). After AP-2 starts again, it uses a different channel. The reconnection is correct. After the peer disconnects, WLAN profiles with "Connect manually" do not reconnect automatically.
- **Evidence**: Verified on hardware (AP-2).
- **Effect**: The beacon loss path without deauth is not tested. See [Validation status](validation.md#untested).
- **Code**: `mac\mlme.c` `mac_mlme_peer_left`, `mlme_teardown`; `wificx\connect.cpp` `IndicateDisassociation`

<a id="security"></a>
## WPA3-SAE and protected management frames

<a id="sec-1"></a>
### SEC-1 Windows assigns the transaction ID of `SET_SAE_AUTH_PARAMS`

- **Finding**: `SET_SAE_AUTH_PARAMS`(135) from Windows has its own `TransactionId`. This value is not equal to the transaction ID of the connect task. In the Microsoft WiFiCx sample, the two values must be equal. The driver only checks that a connect task is in progress. The mac layer checks the BSSID and the SAE state.
- **Evidence**: Verified on hardware (AP-3). With the transaction ID check, the driver rejected each SAE attempt. The driver sent no SAE frame on the air.
- **Effect**: If you implement the sample behavior, WPA3-SAE connections always fail.
- **Code**: `wificx\sae.cpp` `RtwWifiSetSaeAuthParams`; `mac\sae.c` `sae_set_locked`

<a id="sec-2"></a>
### SEC-2 Report fields for SAE frames

- **Finding**: In `SAE_AUTH_PARAMS_NEEDED`(134), the reported authentication frame body starts at the authentication algorithm number. The driver puts the confirm frame from the access point into the two fields `SAEConfirmFrame` and `SAECommitFrame`. With this method, Windows completes SAE. Windows selects group 19 (scalar 32 bytes, element 64 bytes).
- **Evidence**: Verified on hardware (AP-3). It is not determined which field Windows reads: nobody tested a report with only one field.
- **Effect**: With the current method, SAE is successful.
- **Code**: `wificx\sae.cpp` `RtwWifiSaeIndicate`

<a id="sec-3"></a>
### SEC-3 After the driver reports SAE capability, Windows uses SAE for transition mode networks

- **Finding**: With the setting `Pmf=2`, the driver reports the protected management frames (PMF) capability and the SAE capability. Windows then uses SAE to connect to transition mode networks. It changes WPA2-Personal WLAN profiles to SAE automatically (the request has `RSNA-PSK 0`, `WPA3-SAE 1`). The connection is WPA2-Personal with protected management frames only when `Pmf=1`.
- **Evidence**: Verified on hardware (AP-3).
- **Effect**: To test WPA2-Personal with protected management frames, use `Pmf=1`. This is also one reason why protected management frames and WPA3-SAE are disabled by default.
- **Code**: `wificx\device.cpp` `ReportCapabilities`; `driver\probe.c` `RtwApplySettings`

<a id="sec-4"></a>
### SEC-4 Association comeback after SER with protected management frames

- **Finding**: A connection with protected management frames reconnects after SER. The sequence is:
  1. The access point first sends an SA Query for the old association.
  2. Then the access point returns association status code 30 and a comeback time (1024 TU in the tests).
  3. The driver sends the association request again after the comeback time. The connection is successful.

  The Windows timer callback sometimes occurs before the timer expires. In this case, the driver sets the timer again for the time that remains. The driver must not discard this retry.
- **Evidence**: Verified on hardware (AP-3, `Pmf=1`). The early timer callback is inferred from code. With the new timer for the time that remains, the log shows no lost retries. For WPA3-SAE, AP-3 discards the SAE state during the comeback time and then sends a disassociation (access point behavior).
- **Effect**: Without comeback support, a connection with protected management frames must wait after SER until Wlansvc completes a full new connection. For SAE connections to access points such as AP-3, you must reconnect manually.
- **Code**: `mac\mlme.c` `mlme_rx`, `mlme_comeback_tu`, `mlme_timer`

<a id="sec-5"></a>
### SEC-5 The 8852B does not use hardware encryption for protected management frames

- **Finding**: With protected management frames, software must encrypt and decrypt the protected management frames:
  1. On the 8852B, `hw_mgmt_tx_encrypt` is false. Hardware receive decryption of protected management frames is enabled only in WoWLAN.
  2. Protected unicast management frames (deauth, disassociation, block ack (BA) action frames, SA Query, and others) use software CCMP.
  3. Multicast management frames use software BIP-CMAC-128.
  4. This driver uses kernel CNG (`BCrypt*` in `ksecdd.sys`, AES-CCM and AES-CMAC). It calls CNG only at PASSIVE_LEVEL.
  5. When the driver loads, it does a self-test of AES-CCM and AES-CMAC. When `Pmf` is set for the first time, the driver does a self-test of the CCMP and BIP frame operations. If a self-test fails, the driver disables protected management frames.
- **Evidence**: Verified on hardware (AP-3): ADDBA was protected in the two directions. The driver identified a protected disassociation correctly. The tests did not cover the multicast BIP path.
- **Effect**: Without software encryption, block ack action frames and disconnection frames cannot pass on a connection with protected management frames. For this reason, the import table of the driver also contains `ksecdd.sys`.
- **Code**: `driver\crypto.c` `RtwCryptoInit`, `RtwCryptoSelfTest`, `rtwk_aes_ccm_encrypt`, `rtwk_aes_cmac`; `mac\pmf.c` `mac_pmf_rx`, `pmf_selftest`, `rtw_linux_set_pmf`

<a id="power"></a>
## Power save

<a id="ps-1"></a>
### PS-1 Windows only sends connection quality hints

- **Finding**: Windows uses only `SET_CONNECTION_QUALITY`(32) as a power save hint:
  1. The WDK header defines `SET_AUTO_POWER_SAVE`(77), but Windows never sends this message. The driver also never received `GET_AUTO_POWER_SAVE`(78).
  2. If the station capability `AutoPowerSaveMode = TRUE`, Wlansvc sends 32 after each `TASK_DOT11_RESET`. If this capability is FALSE, Windows never sends 32.
  3. When "Wireless Adapter Settings > Power Saving Mode" in the power plan is "Maximum Performance", the value of 32 is 4 (no power save).
- **Evidence**: Verified on hardware (AP-1). The WiFiCx message model has no SET message. This part is inferred from code.
- **Effect**: With the setting `PowerSave=1` (default, follow Windows) and the power saving mode "Maximum Performance", the driver does not save power. To save power, set `PowerSave=2`. The effect of a power plan change is not tested.
- **Code**: `wificx\command.cpp` `SetConnectionQuality`, `GetAutoPowerSave`; `mac\ps.c` `ps_policy`, `mac_ps_recalc`

<a id="ps-2"></a>
### PS-2 The driver cannot read the wireless adapter power save level from the power plan

- **Finding**: The driver uses `PoRegisterPowerSettingCallback` to register for the wireless adapter power setting of the power plan (GUID `12bbebe6-58d6-4636-95bb-3217ef867c1a`). The registration is successful, and the callback occurs immediately. The callback gives 16 bytes of data, not a level from 0 to 3. The `GUID_ACDC_POWER_SOURCE` registration at the same time operates correctly (4 bytes).
- **Evidence**: Verified on hardware. The format of the 16 bytes is not determined: the tests found no documentation.
- **Effect**: The driver records the value as "unknown" and writes one line to the log. The hints in [PS-1](#ps-1) control power save.
- **Code**: `driver\driver.c` `RtwPowerSettingCallback`, `RtwPowerSettingRegister`

<a id="ps-3"></a>
### PS-3 Requirements to connect rtw89 power save

- **Finding**: rtw89 itself controls power save entry, power save exit, and traffic decisions. The mac layer follows the mac80211 rules and does only three operations:
  1. It sets `vif->cfg.ps` after the station is authorized and has received a beacon.
  2. It clears `cfg.ps` before disconnection.
  3. After SER, it sends `BSS_CHANGED_PS` one time for the new `vif`. The reason: the rtw89 recovery does not clear `lps_enabled`.

  Firmware 0.29.29.18 has `NO_LPS_PG`. Deep power save uses only clock gating. The firmware does not have `BEACON_TRACKING`.
- **Evidence**: Verified on hardware (AP-1, `PowerSave=2`). With power save and no traffic, a TCP connection opens with a median round-trip time of approximately 96 ms. This is approximately one beacon interval (DTIM 1). Throughput does not decrease.
- **Effect**: Power save makes the interactive delay much longer. For this reason, power save is off by default.
- **Code**: `mac\ps.c` `mac_ps_recalc`, `mac_ps_leave`, `mac_ps_restart`

<a id="regulatory"></a>
## Country code and regulatory rules

<a id="reg-1"></a>
### REG-1 Windows does not send a country code

- **Finding**: In WiFiCx and WDI, the country (`WDI_TLV_COUNTRY_REGION_LIST`) occurs only in the capabilities that the driver reports to Windows. No WDI message sets the country code. `SET_LOCATION_PRIVACY`(138) is a privacy switch. It does not contain country data.
- **Evidence**: Inferred from code (the WDK headers and the WiFiCx model). Verified on hardware: Windows never sent a message about the country.
- **Effect**: The driver must include its own regulatory rules database. It must also supply the setting `Country`.
- **Code**: `mac\reg.c`, `mac\regdb.h`; `driver\probe.c` `RtwApplySettings`

<a id="reg-2"></a>
### REG-2 Give rtw89 a set of regulatory rules before registration

- **Finding**: If the efuse has no country code and nothing calls `reg_notifier` first, the rtw89 `regulatory.regd` is a null pointer. `rtw89_core_start` uses this pointer when it sets the transmit power. During registration, the mac layer first calls `reg_notifier` one time with the world rules (`00`).
- **Evidence**: Inferred from code (the upstream source code; this condition did not occur on the target device).
- **Effect**: Without this call, rtw89 causes a bug check at start because of the null pointer.
- **Code**: `mac\reg.c` `mac_reg_register`, `reg_call_notifier`

<a id="reg-3"></a>
### REG-3 The mac layer sets the transmit permission of each channel

- **Finding**: The mac layer sets the `NO_IR`, `RADAR`, and `DISABLED` flags of each channel from wireless-regdb. On Linux, cfg80211 sets them. The regulatory rules of rtw89 are not self-managed. The rtw89 rule table only selects the transmit power table (for example, MKK for JP) and some switches. There are three more points:
  1. The rtw89 `reg_notifier` itself takes the wiphy lock. Call it without the lock.
  2. rtw89 calls `rtw89_regd_init_hint` only after `ieee80211_register_hw`.
  3. In the FreeBSD compatibility header, the values of `enum nl80211_reg_initiator` are different from Linux (USER=0, DRIVER=1, CORE=2, COUNTRY_IE=3). Set the values explicitly.
- **Evidence**: Verified on hardware. With country code JP, the number of scanned channels changes from 39 to 34. Each rule agrees with the wireless-regdb source.
- **Effect**: Without this layer, the driver can only use the world rules (passive scan on all 5 GHz channels).
- **Code**: `mac\reg.c` `reg_handle_channel`, `reg_apply`, `reg_work`; `mac\regdb.h` (generated by `tools-dev\regdb-gen.ps1`)

<a id="width"></a>
## Channel width

<a id="wide-1"></a>
### WIDE-1 rtw89 declares only 20, 40, and 80 MHz for the 8852B

- **Finding**: rtw89 v7.2 does not declare 160 MHz for the 8852B. The rtw89 rate adaptation uses only the `sta` fields and the width of the channel context (chanctx). For this reason, the mac layer does all the work for wider channels.
- **Evidence**: Inferred from code (upstream `rtw8852b.c`). Verified on hardware: VHT80 connections operate correctly.
- **Effect**: The maximum width of this driver is 80 MHz.
- **Code**: `mac\wide.h`, `mac\wide.c`

<a id="wide-2"></a>
### WIDE-2 The link rate in Windows comes from `LINK_STATE_CHANGE`

- **Finding**: Windows shows the link rate from the `LINK_STATE_CHANGE`(53) that the driver sends. With VHT80, Windows shows 866.6 Mbps. Under load, the value changes to 702 Mbps or 780 Mbps with the rtw89 transmit rate. With `Bandwidth=40`, Windows shows 300 Mbps. With `Bandwidth=20`, Windows shows 144.4 Mbps. The "802.11ac" or "802.11n" text in Windows comes from `ActivePhyTypeList` in `ASSOCIATION_RESULT`.
- **Evidence**: Verified on hardware (AP-1).
- **Effect**: The driver must update the link rate when the rate changes. Then Windows shows the correct value.
- **Code**: `wificx\connect.cpp` `IndicateLinkState`, `IndicateAssociation`

<a id="wide-3"></a>
### WIDE-3 `ChannelWidthList` in the capabilities has no visible effect

- **Finding**: The driver reports `ChannelWidthList` = {20, 40, 80} in its capabilities. Windows accepts this value. The `netsh` output does not change. WiFiCx has no TLV for VHT or HE details.
- **Evidence**: Verified on hardware. The purpose of this field is not determined.
- **Effect**: Only the driver selects the channel width.
- **Code**: `wificx\device.cpp` `ReportCapabilities`

<a id="wide-4"></a>
### WIDE-4 `TASK_CONNECT` does not contain a PHY list

- **Finding**: `TASK_CONNECT`(6) from Windows does not contain `ActivePhyTypeList` or `AssociationRequestVendorIE`. For this reason, the driver path that disables VHT from the PHY list (`RTW_CONN_F_NO_VHT`) never runs.
- **Evidence**: Verified on hardware (AP-1).
- **Effect**: The driver cannot use Windows to limit the PHY type.
- **Code**: `wificx\connect.cpp` `RtwWifiTaskConnect`; `mac\mlme.c` `mlme_wide`

<a id="wide-5"></a>
### WIDE-5 Do not advertise VHT when the driver uses only 40 or 20 MHz

- **Finding**: A VHT station must be able to receive 80 MHz. If the BSS is VHT80 and the driver uses only 40 or 20 MHz, the driver changes to HT. The driver keeps VHT only when the BSS itself is VHT20 or VHT40 and the driver uses its full width.
- **Evidence**: Verified on hardware (AP-1). With `Bandwidth=40`, the access point transmits with HT40.
- **Effect**: If the driver advertises VHT but does not receive 80 MHz, the access point transmits with a width that the driver cannot receive.
- **Code**: `mac\wide.c` `wide_choose`, `wide_ht_cap_ours`

<a id="wide-6"></a>
### WIDE-6 `ieee80211_hdrlen` in the FreeBSD header does not include +HTC

- **Finding**: The original FreeBSD `ieee80211_hdrlen` does not include +HTC (the HT Control field). The `ieee80211_hdrlen` of this project includes +HTC as Linux does. The change has the mark `rtw89win`. The driver does not advertise `HTC_VHT`. AP-1 does not send +HTC frames or A-MSDUs.
- **Evidence**: Inferred from code (comparison with the Linux header). Verified on hardware: the `rx_htc` and `rx_amsdu` counters are 0. For this reason, the +HTC path did not occur on the air.
- **Effect**: If the length calculation is incorrect, the rtw89 transmit descriptor and the CCMP header position are incorrect.
- **Code**: `compat\include\linux\ieee80211.h` `ieee80211_hdrlen`

<a id="wide-7"></a>
### WIDE-7 VHT capability decisions

- **Finding**: The driver does not advertise MU beamformee or +HTC. It advertises a Max MPDU of 3895 bytes (rtw89 gives 11454). The driver keeps SU beamformee only when the access point is an SU beamformer.
- **Evidence**: Verified on hardware (AP-1), tested one time.
- **Effect**: If the driver advertises capabilities that it does not support, the access point sends frames that the driver cannot process correctly.
- **Code**: `mac\wide.c` `wide_vht_cap_ours`, `wide_vht_sta`

<a id="wide-8"></a>
### WIDE-8 The mac layer checks if the channel width is valid

- **Finding**: In this project, the cfg80211 chandef check function is empty. The mac layer itself checks 40 MHz pairs, 80 MHz channel blocks, and the regulatory rules (`wide_usable`). If a combination is not valid, the width decreases in the sequence 80 → 40 → 20 MHz. When the regulatory rules change, the mac layer checks the full wide channel again.
- **Evidence**: Inferred from code (host test `wide-test.ps1`). With `Country` set, the rule change path did not occur on the target device.
- **Effect**: Without these checks, the driver transmits on channel combinations that are not permitted.
- **Code**: `mac\wide.c` `wide_usable`; `mac\mlme.c` `mac_mlme_chan_ok`; `mac\reg.c` `reg_work`

<a id="wide-9"></a>
### WIDE-9 Two-stream receive operates with `rx_chains_static` at 1

- **Finding**: When `rx_chains_static` and `rx_chains_dynamic` of the channel context stay at 1, the 8852B still receives two streams.
- **Evidence**: Verified on hardware (AP-1): the access point transmits 2 streams, and the driver receives them correctly.
- **Effect**: It is not necessary to change these two values for the number of streams.
- **Code**: `mac\iface.c` `mac_chanctx_use_def`

<a id="wide-10"></a>
### WIDE-10 The station and the group owner share a channel context

- **Finding**: When the station uses VHT80, the group owner uses 20 MHz on the same channel. This operates correctly. After the station disconnects, the channel context stays at 80 MHz. This also operates correctly. If the group owner creates the channel context first, the station can only use HT20.
- **Evidence**: Verified on hardware (AP-1, Client-1). From this, a conclusion follows: after SER, if the user starts the hotspot first and then connects the station, the station can only use HT20. This conclusion is inferred from code and is not tested.
- **Effect**: For VHT80 on the station, connect the station first. Then start the hotspot.
- **Code**: `mac\iface.c` `chanctx_share`

<a id="wide-11"></a>
### WIDE-11 VHT80 throughput

- **Finding**: With VHT80 on AP-1, the throughput is 133.7–133.9 Mbit/s downlink and 127.7–130.2 Mbit/s uplink. `rx_dropped_full` is 0. CPU0 has a maximum of approximately 13 000 interrupts per second. The interrupts do not limit the throughput.
- **Evidence**: Verified on hardware. The time measurement includes the time to open the ssh connection. With `Bandwidth=40`, the downlink is only 64–67 Mbit/s. The cause is not determined.
- **Effect**: For the conditions and the measurement method of these values, see [Validation status](validation.md#width).
- **Code**: None (measurement script `hosttools\rtw89win-thru.ps1`).

<a id="hotspot"></a>
## Mobile hotspot (Wi-Fi Direct group owner)

<a id="hs-1"></a>
### HS-1 With WiFiCx, the mobile hotspot uses a Wi-Fi Direct group owner

- **Finding**: With WiFiCx, the Windows mobile hotspot uses a Wi-Fi Direct group owner (GO):
  1. The driver reports `WIFI_WIFIDIRECT_CAPABILITIES`.
  2. WiFiCx calls the driver to create the Wi-Fi Direct device port (control plane only).
  3. When the hotspot starts, WiFiCx uses the same `EvtWifiDeviceCreateAdapter` to create the role port (type `WIFI_ADAPTER_WIFI_DIRECT_ROLE`).
  4. WiFiCx sends `TASK_START_AP`(14) on the role port.

  WiFiCx does not send `TASK_CREATE_PORT`(12). The text "Soft AP: Not supported" in the `netsh` output applies to the old ExtAP method. It is not related to the mobile hotspot.
- **Evidence**: Verified on hardware (Client-1).
- **Effect**: The driver can return NOT_SUPPORTED for P2P discovery, P2P negotiation, and action frames. The mobile hotspot still operates correctly.
- **Code**: `wificx\wfd.cpp` `RtwEvtWifiCreateWifiDirectDevice`, `RtwWifiReportWfdCapabilities`; `wificx\adapter.cpp` `RtwEvtWifiCreateAdapter`; `wificx\ap.cpp` `RtwWifiApTaskStartAp`

<a id="hs-2"></a>
### HS-2 The Wi-Fi Direct capabilities have minimum values

- **Finding**: In some conditions, `nwifi.sys` cannot attach (`RareFilterPath … 0xC0000001`, `MissingMandatoryFilter`). These conditions are `ConcurrentClientCount = 0`, or a `ScanSSIDListSize` of less than 4 in the Wi-Fi Direct capabilities. Windows then has no WLAN network adapter. The station network adapter also disappears. The driver must report `ConcurrentClientCount = 1` and `ScanSSIDListSize = 4`.
- **Evidence**: Verified on hardware. The `ScanSSIDListSize` values 1 and 2 fail. The value 3 is not tested. A change to other fields alone does not solve the problem. The reason for the value 4 is not determined.
- **Effect**: The driver must report 1 P2P client port, also when it does not implement a P2P client. `TASK_CONNECT` on the role port still returns NOT_SUPPORTED.
- **Code**: `wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-3"></a>
### HS-3 `NumberOfNetworkInterfaces` in the INF must be 3

- **Finding**: The station, the Wi-Fi Direct device port, and 1 role port make 3 interfaces. For this reason, the INF has the value 3. After installation, two more network adapters are available: WLAN 2 (Wi-Fi Direct device port) and WLAN 3 (role port). When the hotspot is off, WLAN 3 shows as disabled.
- **Evidence**: Verified on hardware. The value 1 is not tested. After an in-place upgrade, the interface GUID and the WLAN profiles do not change.
- **Effect**: If this value is too small, Windows cannot create the hotspot ports.
- **Code**: `driver\rtw89win.inf` `[Rtw89_Ndi_AddReg]`

<a id="hs-4"></a>
### HS-4 WDI message sequence for the mobile hotspot

- **Finding**: Messages between Windows and the driver occur in this sequence:
  1. Device start: WiFiCx creates the Wi-Fi Direct device port. Then the sequence is `TASK_DOT11_RESET`(7) → `SET_LOCATION_PRIVACY`(138) → 7 → `TASK_CHANGE_OPERATION_MODE`(90, mode `0x8`) → `SET_ADVERTISEMENT_INFORMATION`(89) 5 times.
  2. After Windows creates the role port: 7 → `GET_STATISTICS`(34) → 138 → `SET_CONNECTION_QUALITY`(32). In this 7, `ResetMACAddress` is equal to the group owner address that the driver reports. Windows sometimes creates the role port before the hotspot starts. At other times, Windows creates it when the hotspot starts.
  3. Hotspot start: 7 → 90 (mode `0x20`) → 89 approximately 12 times → `SET_PRIVACY_EXEMPTION_LIST`(28, 1 entry `0x888E`) → `TASK_START_AP`(14). Then the driver sends `P2P_GROUP_OPERATING_CHANNEL`(84) → task completion indication → 89. No group key is installed at hotspot start.
  4. Hotspot client connection, approximately 60–70 ms from authentication to authorization:
     1. The driver sends `AP_ASSOCIATION_REQUEST_RECEIVED`(55).
     2. Windows sends `TASK_SEND_AP_ASSOCIATION_RESPONSE`(17).
     3. After the association response receives an ACK, the driver sends the task completion indication.
     4. Windows does the 4-way handshake through the data path.
     5. `SET_ADD_CIPHER_KEYS`(29) installs the pairwise key first. Then it installs the group key (ID 2). Windows installs the group key only after the first hotspot client completes the handshake.
     6. `SET_DEFAULT_KEY_ID`(31).
  5. Hotspot stop: 89 → `TASK_DISCONNECT`(8, reason 0) for each connected hotspot client → 8 for the broadcast address (reason 8) → `TASK_STOP_AP`(15) → 7 two times.
  6. Windows sends 90, 138, and 34. Windows does not send `SET_P2P_LISTEN_STATE`(26), `SET_P2P_WPS_ENABLED`(123), or `SET_OPERATION_MODE`(24).
- **Evidence**: Verified on hardware (`WdiDump`, Client-1).
- **Effect**: For a mobile hotspot only, the driver must implement only the messages above.
- **Code**: `wificx\wfd.cpp` `RtwWifiWfdDispatch`; `wificx\ap.cpp`; `wificx\command.cpp` `Dispatch`

<a id="hs-5"></a>
### HS-5 Stop station messages to the Wi-Fi Direct ports

- **Finding**: Windows also sends station messages to the Wi-Fi Direct device port and to the role port. Examples are `SET_CONNECTION_QUALITY`(32), `SET_PRIVACY_EXEMPTION_LIST`(28), and `SET_DELETE_CIPHER_KEYS`(30). The station handlers do not check the port number. For this reason, the driver intercepts these messages by port:
  1. 32: return success and do nothing.
  2. 28, 30, and 31 on the Wi-Fi Direct device port: return success and do nothing. For 29, return NOT_SUPPORTED.
  3. `SET_SAE_AUTH_PARAMS`(135): return NOT_SUPPORTED.
- **Evidence**: Verified on hardware: the role port received 32.
- **Effect**: If the driver does not intercept these messages, they change the station power save policy or delete the station keys.
- **Code**: `wificx\wfd.cpp` `RtwWifiWfdDispatch`

<a id="hs-6"></a>
### HS-6 `BeaconIEs` in the association completion indication must contain the beacon frame body

- **Finding**: In the completion indication of `TASK_SEND_AP_ASSOCIATION_RESPONSE`(17), `BeaconIEs` must contain the beacon frame body. The body starts at Timestamp and includes the 12 bytes of fixed fields. The WDI sample also gives `MMPDU_BODY(beaconframe)`.
- **Evidence**: Verified on hardware. When the field contained only the information elements, Windows sent `TASK_DISCONNECT` approximately 40 ms after association. Windows sent no EAPOL frame.
- **Effect**: If the content is incorrect, hotspot clients cannot connect.
- **Code**: `wificx\ap.cpp` `AssocResponseComplete`; `mac\ap.c` `ap_resp_done`

<a id="hs-7"></a>
### HS-7 The driver must send `LINK_STATE_CHANGE` on the role port

- **Finding**: After `TASK_START_AP` is successful and before the task completion indication, the driver sends `LINK_STATE_CHANGE`(53) one time on the role port. In this indication, the peer address is the group owner itself. The link rate is 144400 kbit/s, and the channel width is 20 MHz. The group owner in the WDI sample does not send this indication.
- **Evidence**: Verified on hardware. Without this indication, the network adapter of the role port always stays disconnected in NDIS. WiFiCx never calls `TxAdvance`. pktmon shows that EAPOL frames are discarded below Native WiFi (`Media Disconnected`).
- **Effect**: Without this indication, the 4-way handshake of hotspot clients cannot complete.
- **Code**: `wificx\ap.cpp` `IndicateApLinkState`, `RtwWifiApTaskStartAp`

<a id="hs-8"></a>
### HS-8 Do not call `WifiAdapterAddPeer`; TX demux is not necessary

- **Finding**: If TX demux is not configured, a call to `WifiAdapterAddPeer` causes a bug check (0x7E, `0xC0000420`; the capacity of `WxPeerTable::Insert` is 0). With demux configured, the bug check does not occur. But the transmitted frames still do not arrive at the driver. [HS-7](#hs-7) controls if the frames arrive at the driver.
- **Evidence**: Verified on hardware (the bug check and the frames that do not arrive at the driver). The WiFiCx internal symbols show that demux uses an exact address match and discards multicast frames. This part is inferred from code.
- **Effect**: The driver does not call this function and does not configure demux. The mobile hotspot still operates correctly.
- **Code**: `wificx\` has no `WifiAdapterAddPeer` call.

<a id="hs-9"></a>
### HS-9 Content of `P2P_GROUP_OPERATING_CHANNEL`

- **Finding**: `P2P_GROUP_OPERATING_CHANNEL`(84) uses the transaction ID of the `TASK_START_AP` request. Windows accepts this value. The country string is `"<country code>\x04"`. The operating class is 81 for 2.4 GHz and 115 for 5 GHz channels 36–48.
- **Evidence**: Verified on hardware (Client-1). The transaction ID 0 is not tested.
- **Effect**: The behavior without this indication is not tested.
- **Code**: `wificx\ap.cpp` `IndicateOperatingChannel`, `OperatingClass`

<a id="hs-10"></a>
### HS-10 The driver sets the role port address

- **Finding**: The role port uses the address that the driver reports in the capability `InterfaceAddressList`. `ResetMACAddress` is equal to this address. Hotspot clients also see this address as the BSSID. The driver calculates the addresses as follows:
  1. The Wi-Fi Direct device address is the efuse address with the locally administered bit set.
  2. The group owner address is the device address with byte 4 (count from 0) XOR `0x80`.
- **Evidence**: Verified on hardware (the BSSID that Client-1 sees).
- **Effect**: The driver must make sure that the three addresses are all different. They must also be different from the station address.
- **Code**: `wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-11"></a>
### HS-11 The driver selects the hotspot channel

- **Finding**: The "Band" setting in Windows Settings sets these `TASK_START_AP` parameters:
  1. 2.4 GHz or 5 GHz: `APBandChannelList` has 1 entry (band number only), `MustUse=TRUE`, and no `AdvertisedOperatingChannel`.
  2. "Any available": no band list, `MustUse=FALSE`.

  The three settings have these common parameters: `PreferOverStation=FALSE`, `DataRateSupport802_11b=TRUE`, `ExcludeUnencrypted=FALSE`, beacon interval 100, DTIM 1, authentication RSNA_PSK, cipher CCMP. For this reason, the driver selects the channel. It selects the channel with the lowest load in the BSS table. On 2.4 GHz, it selects only from 1, 6, and 11. For "Any available", it selects 5 GHz first. The group owner uses only channels where the regulatory rules permit active transmission and no radar detection is necessary.
- **Evidence**: Verified on hardware (Client-1, country code JP). Without a country code (world rules), active transmission on 5 GHz is not permitted. The driver then does not report the 5 GHz group owner capability. This part is inferred from code.
- **Effect**: If the driver uses the first entry of the band list as the channel, a 5 GHz hotspot is always on channel 36. This is the same channel as the access point.
- **Code**: `wificx\ap.cpp` `ChooseChannel`, `ChannelLoad`; `mac\ap.c` `rtw_linux_ap_channels`

<a id="hs-12"></a>
### HS-12 The station and the group owner can only use the same channel

- **Finding**: When the station is connected, the group owner can only use the station channel. This limit comes from the rtw89 combination table `rtw89_iface_combs`.
  1. If the station is on 5 GHz and the hotspot band is 2.4 GHz, the driver returns `BAND_CURRENTLY_NOT_AVAILABLE`. Windows reports `BandInterference`.
  2. If the hotspot band is "Any available", the group owner uses the station channel.
  3. If the station starts a connection while the hotspot runs, the driver examines the candidates. If some candidates are on the group owner channel, the driver uses only these candidates.
  4. If no candidate is on the same channel, the driver first sends `STOP_AP`. Then it connects.
- **Evidence**: Verified on hardware (AP-1, Client-1). On the same channel, the station throughput is 52.3 Mbit/s downlink and 62.6 Mbit/s uplink. This is almost the same as with the station only. One time, the driver completed `TASK_START_AP` successfully, but Windows still reported `BandInterference`. The cause is not determined.
- **Effect**: When the station is connected, the user must select "Any available" or the band of the station.
- **Code**: `wificx\ap.cpp` `ChooseChannel`, `RtwWifiApBeforeConnect`; `mac\iface.c` `chanctx_share`

<a id="hs-13"></a>
### HS-13 Behavior of the Windows mobile hotspot service

- **Finding**: The Windows mobile hotspot service (`icssvc`) has this behavior:
  1. `TetheringCapability` checks only the upstream connection. It also reports Enabled for drivers that have no Wi-Fi Direct capabilities.
  2. `IsBandSupported` comes from the capability `GOon5GHzBandSupported`.
  3. The only authentication type is WPA2-Personal. WDI does not support SAE on the group owner side. `IsAuthenticationKindSupported(Wpa3)` is False.
  4. After SER or a device restart, Windows does not start the hotspot again automatically. At SER, the driver sends `STOP_AP`(115) and `CAN_SUSTAIN_AP`(116) with the reason `0xFF000001`.
  5. If no hotspot client is connected for approximately 5 minutes, Windows stops the hotspot. The laptop of AP-2 has the same behavior.
- **Evidence**: Verified on hardware (Client-1). The full conditions that `icssvc` uses to permit the hotspot are not determined.
- **Effect**: After SER or a device restart, the user must start the hotspot again manually.
- **Code**: `wificx\ap.cpp` `IndicateApReason`; `wificx\wfd.cpp` `RtwWifiReportWfdCapabilities`

<a id="hs-14"></a>
### HS-14 Division of work on the group owner side

- **Finding**: The work on the group owner side has this division:
  1. Open authentication: the driver replies.
  2. Association: Windows decides to accept or reject. The driver sends the association response and waits for the ACK.
  3. 4-way handshake: Windows does it through the data path.
  4. Probe responses: the mac layer builds the frames (rtw89 does not offload probe responses on the 8852B). The maximum is 30 for each second.
  5. Beacons: the firmware sends them at each TBTT.
  6. Power save state, frame buffers, and TIM for hotspot clients: the 8852B firmware does not have `NOTIFY_AP_INFO`. For this reason, the driver does all of this work.
- **Evidence**: Inferred from code (the firmware feature table of the upstream rtw89). Verified on hardware: the connection, the internet access, and the power save of Client-1 operate correctly.
- **Effect**: For a port to a chip that has `NOTIFY_AP_INFO`, examine the division of the power save work again.
- **Code**: `mac\ap.c` `ap_rx_auth`, `ap_rx_probe_req`, `linuxkpi_ieee80211_beacon_get_tim`

<a id="hs-15"></a>
### HS-15 The driver implements 802.1X port control itself

- **Finding**: Windows opens the port at association (`EXTAP_PORT_UP`) and gives `ExcludeUnencrypted=FALSE`. The driver controls the port as mac80211 does:
  1. Before keys are installed, the driver lets only exempted frames pass. Without a key, it rejects transmission.
  2. A frame with EtherType `0x888E` must have the BSSID or the PAE group address as its destination address (CVE-2020-26139).
  3. The driver checks the full A-MSDU. The source address of each subframe must be equal to A2. The destination address must not be equal to an RFC 1042 header (CVE-2020-24588).
- **Evidence**: Verified on hardware: `rx_dropped_unencrypted` is 0. The A-MSDU path is inferred from code: Client-1 does not send A-MSDUs.
- **Effect**: Without port control, the hotspot forwards data frames of hotspot clients that are not authorized.
- **Code**: `mac\ap.c` `ap_pae_da_ok`, `ap_amsdu_ok`

<a id="hs-16"></a>
### HS-16 Address format of group owner data frames

- **Finding**: Windows gives FromDS frames to the driver to transmit. A1 is the destination address, A2 is the BSSID, and A3 is the source address. Windows accepts ToDS frames from the driver.
- **Evidence**: Verified on hardware (Client-1).
- **Effect**: The driver does not have to change the address format in either direction.
- **Code**: `mac\ap.c` `rtw_linux_ap_tx_data`, `mac_ap_rx_deliver`

<a id="hs-17"></a>
### HS-17 Power save of hotspot clients

- **Finding**: Client-1 uses the PM bit for power save. It does not send PS-Poll. The frame buffers, the frame release, and the TIM of the driver operate correctly. The driver keeps a maximum of 64 frames for each hotspot client. It discards frames that it cannot release in 10 s.

  When the screen of Client-1 is off, ping fails. Android discards ICMP, but it still replies with RST to TCP packets for closed ports.
- **Evidence**: Verified on hardware: 5 minutes with the screen off, 99 frames kept, 99 frames released, 0 frames discarded. The tests did not cover the PS-Poll path (a Null reply when no frame is kept). It is not determined if rtw89 gives PS-Poll frames to the driver.
- **Effect**: Ping is not a reliable method to find if a hotspot client is available. Use TCP.
- **Code**: `mac\ap.c` `ap_ps_set`, `ap_ps_run`

<a id="hs-18"></a>
### HS-18 The TIM offset depends on the `sk_buff` layout

- **Finding**: The upstream rtw89 `fw.c` uses the skb structure itself as the frame header and reads the frame control field from it. For this reason, `beacon_get_tim` must return a newly allocated skb (the first member `next` is `NULL`).
- **Evidence**: Inferred from code (the upstream source code). Verified on hardware: the beacon and the TIM of the group owner are correct.
- **Effect**: If the returned skb is not newly allocated, rtw89 calculates an incorrect frame header length. The TIM offset is then incorrect.
- **Code**: `mac\ap.c` `linuxkpi_ieee80211_beacon_get_tim`; `compat\include\rtw\skbuff.h`

<a id="hs-19"></a>
### HS-19 Group owner sequence at SER

- **Finding**: When SER occurs while the hotspot runs, the sequence is:
  1. The driver first discards the group owner keys.
  2. rtw89 recovers the chip in approximately 110 ms. The compatibility layer masks the INTx interrupt one time.
  3. The driver sends `DISASSOCIATION`(51, reason 60, `WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET`) → `STOP_AP`(115) → `CAN_SUSTAIN_AP`(116).
  4. Windows sends a broadcast `TASK_DISCONNECT`(8) and `TASK_STOP_AP`(15).

  At SER, rtw89 first clears `rtwvifs_list`. For this reason, the parked group owner `vif` does not prevent station power save after that.
- **Evidence**: Verified on hardware (Client-1): no bug check. `tx_nokey` is 3. The driver sent no unencrypted frame.
- **Effect**: After the keys are discarded, the driver must not use these keys to send frames. If it does, it sends unencrypted data with a CCMP header.
- **Code**: `mac\iface.c` `mac_restart_work`; `mac\ap.c` `mac_ap_restart`

<a id="hs-20"></a>
### HS-20 Other group owner observations

- **Finding**:
  1. When the driver removes a hotspot client, a timeout of the rtw89 flush always occurs (approximately 200 ms). The cause is not determined.
  2. After a hotspot client leaves, Windows sends `SET_DELETE_CIPHER_KEYS`(30). At this time, the driver has already deleted the keys. The mac layer returns −2, but the WiFiCx front end still returns success.
  3. When a hotspot client reconnects, Windows does not install the group key again.
  4. The order of 29 and 31 during a group key update is not determined.
  5. On 2.4 GHz, the beacon uses CCK 1 Mbit/s. The basic rates are also CCK. Client-1 connects correctly.
- **Evidence**: Verified on hardware (Client-1). The tests used only one type of hotspot client.
- **Effect**: With the method in item 2, Windows does not report an error.
- **Code**: `mac\ap.c` `ap_client_remove`

<a id="install"></a>
## Installation, PnP, and registry

<a id="inst-1"></a>
### INST-1 Side effects of the Net class installer

- **Finding**: The class installer of the Net class has two side effects:
  1. At the first installation of a Net class driver package, the class installer changes the Wlansvc startup type from "Manual" to "Automatic". It also starts the service (System event 7040). After uninstallation, Wlansvc stays "Automatic".
  2. The class installer reserves network adapters at installation from `NumberOfNetworkInterfaces` in the INF. It reads this value only at installation, not at run time. If you decrease the value and install again, it deletes the extra network adapters. If you increase the value and install again, it creates new network adapters.
- **Evidence**: Verified on hardware. "Only at installation" comes from a read-only examination of `NetSetupSvc.dll`. This part is inferred from code.
- **Effect**: If you use the value 11 from the Microsoft sample, Windows shows 10 extra `WLAN n` network adapters that do not exist.
- **Code**: `driver\rtw89win.inf` `[Rtw89_Ndi_AddReg]`

<a id="inst-2"></a>
### INST-2 If you uninstall and then install again, Windows cannot use the WLAN profiles

- **Finding**: `pnputil /delete-driver … /uninstall /force` deletes the Net class software key and the `NetCfgInstanceId` in it. At the next installation, the interface GUID changes. Windows stores WLAN profiles by interface GUID. For this reason, the new network adapter cannot see the WLAN profiles. An in-place upgrade keeps the interface GUID:
  1. Do not delete the old driver package first. Run `pnputil /add-driver /install`. The new version updates the same device directly.
  2. If the version is the same or older, `pnputil` returns 259 (already up to date). Then use `UpdateDriverForPlugAndPlayDevicesW(INSTALLFLAG_FORCE)` to force the update.
  3. Make sure that the device uses the new driver. Then delete the old driver package (without `/uninstall`).
- **Evidence**: Verified on hardware. The interface GUID did not change for upgrades or for downgrades.
- **Effect**: If you uninstall and then install again, connections report "There is no profile". The user must type the password of each network again.
- **Code**: `tools\install-rtw89win.ps1`

<a id="inst-3"></a>
### INST-3 After an in-place upgrade, the device sometimes needs a computer restart

- **Finding**: After an in-place upgrade, `DEVPKEY_Device_IsRebootRequired` of the device can be True. Then `pnputil /restart-device` fails (exit code 50). The only solution is to restart the computer.
- **Evidence**: Verified on hardware, tested one time.
- **Effect**: For fault isolation, examine this device property first.
- **Code**: None (Windows behavior).

<a id="inst-4"></a>
### INST-4 Early in startup, the registry hive cannot be written to disk

- **Finding**: The Wi-Fi driver loads approximately 22–24.6 s after startup. At that time, `ZwFlushKey` returns `0xC000014D` (`STATUS_REGISTRY_IO_FAILED`). Approximately 0.5 s later, the flush is successful. The Bluetooth filter driver starts at approximately 26 s. Its flush is successful in 15 ms.
- **Evidence**: Verified on hardware (driver log). The cause is inferred from code: the demand-start PCI driver loads before the session manager initializes the registry.
- **Effect**: The crash loop guard must write its counter to disk before the driver accesses the chip. For this reason, in PrepareHardware, the driver waits until the registry hive is writable. It tries every 500 ms for a maximum of 60 s. If a timeout occurs, this start fails. The driver then does not access the chip.
- **Code**: `driver\driver.c` `RtwGuardBegin`, `RtwGuardWait`; `bt\rtkbtflt\driver.c` `FltGuardBegin`

<a id="inst-5"></a>
### INST-5 Use the shutdown notification at shutdown; do not use D0Exit

- **Finding**: At system shutdown, the driver must use the shutdown notification to end the current run:
  1. At system shutdown, Windows does not call `ReleaseHardware`.
  2. D0Exit (target state `WdfPowerDeviceD3Final`) occurs after the last write of the registry hive. Registry values that the driver writes at this time are not saved to disk.
  3. The Wi-Fi driver registers for the shutdown notification on the device object of the diagnostic interface (`WdfControlDeviceInitSetShutdownNotification`, that is, `IRP_MJ_SHUTDOWN`).
  4. The Bluetooth filter driver registers for the shutdown notification on a separate device object. `IoRegisterShutdownNotification` on the filter device object has no effect.
- **Evidence**: Verified on hardware: after a computer restart, the driver log shows that the last run "ended in order (shutdown)". It is not determined why the registration on the filter device object has no effect.
- **Effect**: If the driver uses only D0Exit, each normal shutdown counts as a run that did not end correctly. After two shutdowns, the crash loop guard becomes active.
- **Code**: `driver\driver.c` `RtwCdoCreate`, `RtwEvtShutdown`, `RtwGuardEnd`; `bt\rtkbtflt\driver.c` `FltCdoCreate`, `FltEvtShutdown`

<a id="inst-6"></a>
### INST-6 Do not cause re-enumeration of the device

- **Finding**: After re-enumeration, the driver cannot download firmware to the RTL8852BE until the chip loses power. The driver log shows `[ERR]FWDL path ready`, and the WCPU stays in ROM.
  1. Operation that causes re-enumeration: the driver calls `WdfDeviceSetFailed(WdfDeviceFailedAttemptRestart)`. PnP re-enumerates the device and calls AddDevice again. The result is the same if the driver did not access the chip before.
  2. Operations that do not cause re-enumeration: `pnputil /restart-device`; `pnputil /remove-device` with `/scan-devices`; the uninstall scripts; disable and enable in Device Manager.
  3. After this fault, `pnputil /restart-device` cannot recover the device. Only a computer restart (the chip loses power) recovers it.
- **Evidence**: Verified on hardware. The cause is inferred from code: FLR = 1 in the PCIe DevCap, so Windows does an FLR on the device at re-enumeration.
- **Effect**: The recovery code of the driver must not use `WdfDeviceFailedAttemptRestart`. For this reason, the crash loop guard cannot change to diagnostic mode during the same start. This driver and the scripts of this project do not cause re-enumeration.
- **Code**: `driver\hw.c` `LogPciConfig` (records DevCap)

<a id="inst-7"></a>
### INST-7 The driver log in memory is lost when the driver unloads

- **Finding**: After the device stops, the driver image unloads. The driver log in memory is then lost. For this reason, the driver saves the last 16 KB of the log in the setting `SavedLog`. It does this at hardware release, at shutdown, and at start failure. `rtwdiag.ps1 log -Saved` reads the saved log.
- **Evidence**: Verified on hardware.
- **Effect**: Without the saved log, you cannot find the cause of a start failure or a disconnection.
- **Code**: `driver\driver.c` `RtwSaveLog`

<a id="inst-8"></a>
### INST-8 New networks are "Public" by default, and the firewall discards ICMP echo

- **Finding**: Windows sets each new network to "Public" on the two ends. Windows Firewall discards ICMP echo requests. `netstat -s -p icmp` shows that the peer received echo requests but did not reply.
- **Evidence**: Verified on hardware (AP-2).
- **Effect**: A ping failure does not show a driver problem. Use a TCP connection to check connectivity.
- **Code**: None (Windows behavior).

<a id="inst-9"></a>
### INST-9 WLAN-AutoConfig events show different cipher names

- **Finding**: For the same WPA2-Personal connection, events 11000 and 11010 show "AES-GCMP-128". Event 8001 shows "AES-CCMP". The connection actually uses CCMP and is not affected.
- **Evidence**: Verified on hardware (AP-2).
- **Effect**: During fault isolation, do not use this difference as evidence of a cipher negotiation problem.
- **Code**: None (Windows behavior).

<a id="rtw89"></a>
## Connect rtw89 to Windows

<a id="rtw-1"></a>
### RTW-1 rtw89 leaves one C2H skb when it stops

- **Finding**: rtw89 leaves one C2H skb when it stops. The compatibility layer reclaims this skb after remove:
  1. While rtw89 holds the wiphy lock during start, rtw89 adds the C2H frames that arrive to `c2h_queue`.
  2. When the chip then enters IPS, `rtw89_core_stop` cancels the C2H work item. The upstream code does not free this queue. The same leak occurs on Linux.
  3. The data of this skb is in the DMA heap. For this reason, the driver cannot return the DMA heap at remove.
  4. After remove, the compatibility layer reclaims all skbs that remain.
- **Evidence**: Verified on hardware (driver log `rtw89: freed N skbs it left behind`).
- **Effect**: Without reclaim, each later probe returns −19. You must then restart the device.
- **Code**: `compat\skbuff.c` `rtw_skb_reclaim`

<a id="rtw-2"></a>
### RTW-2 The skb `cb` belongs to rtw89

- **Finding**: rtw89 uses `info->driver_data` as its own `rtw89_tx_skb_data` (in the same union as `control`). In the TX status callback, rtw89 also clears the second half, from `status`. The mac layer cannot keep its own record data in `cb`. The compatibility layer adds the field `mac_tag` to `struct sk_buff`. rtw89 does not access this field.
- **Evidence**: Inferred from code (the upstream source code).
- **Effect**: If rtw89 overwrites the record data, the driver cannot match the TX status to the frame.
- **Code**: `compat\include\rtw\skbuff.h` (`mac_tag`); `mac\tx.c` `mac_tx_mgmt`, `linuxkpi_ieee80211_tx_status`

<a id="rtw-3"></a>
### RTW-3 Do not send frames in the TX status callback

- **Finding**: On PCI, TX completion calls `ieee80211_tx_status_ni` and `free_txskb` at DISPATCH_LEVEL while it holds `rtwpci->trx_lock`. In the callback, the driver can only update records and put work items in a queue. The callback must not call `ops->tx`. The callback must also not call NetAdapterCx directly. NetAdapterCx calls `Advance` synchronously, and `Advance` calls `ops->tx`. The mac layer uses a flag and a KDPC to notify the WiFiCx front end.
- **Evidence**: Inferred from code (the lock order in the upstream source code).
- **Effect**: If the driver sends a frame in the callback, `trx_lock` deadlocks on itself. The system then stops.
- **Code**: `mac\tx.c` `linuxkpi_ieee80211_tx_status`, `linuxkpi_ieee80211_free_txskb`; `wificx\adapter.cpp` `DpTxReady`, `Kick`, `DpDpc`

<a id="rtw-4"></a>
### RTW-4 RCU must actually wait for readers

- **Finding**: The RCU of the compatibility layer uses a reader count. It actually waits until all readers exit:
  1. rtw89 has RCU readers in NAPI poll (PN check, `sta` links).
  2. The NAPI of the compatibility layer runs on a work queue thread (PASSIVE_LEVEL), not in a DPC. For this reason, it is not enough that `synchronize_rcu` waits only for DPCs.
  3. `synchronize_rcu` waits until the reader count is zero.
  4. `call_rcu` and `kfree_rcu` always add the object to a list. A work item first synchronizes and then frees the objects. The reason: the caller itself is in a reader section.
- **Evidence**: Inferred from code. Verified on hardware: many connections and disconnections operate correctly.
- **Effect**: If the driver waits only for DPCs, a disconnection or a `sta` removal causes a use-after-free.
- **Code**: `compat\rcu.c` `synchronize_rcu`; `mac\mac.h` `mac_read_lock`

<a id="rtw-5"></a>
### RTW-5 Data frames go directly to `ops->tx`, without txq

- **Finding**: The mac layer adds the QoS header and the sequence numbers of each TID to frames from Windows. Then it calls `ops->tx` directly. mac80211 does the same for frames that have no txq.
  1. The rtw89 txq path (`wake_tx_queue` → `rtw89_core_txq_work`) needs full `ieee80211_txq` scheduler support. It also starts block ack sessions itself. This driver does not use this path.
  2. The mac layer itself starts block ack sessions. It also does not put EAPOL frames into A-MPDU aggregation.
  3. The maximum number of frames in transit is 64. For flow control, the mac layer uses the rtw89 stop queue and wake queue calls.
- **Evidence**: Verified on hardware (AP-1, A-MPDU aggregation operates correctly).
- **Effect**: The txq path needs much of the mac80211 scheduler logic.
- **Code**: `mac\tx.c` `rtw_linux_tx_data`; `mac\agg.c` `mac_agg_tx_check`

<a id="rtw-6"></a>
### RTW-6 A channel context is necessary before the driver sends management frames

- **Finding**: The transmit entry point must first check the channel context. rtw89 reads `rtwvif_link->chanctx_idx` when it sends management frames. When the chip is in IPS (no power), no channel context exists.
  1. The hardware sets the sequence numbers of management frames.
  2. rtw89 selects the rate: CCK 1 Mbit/s on 2.4 GHz, OFDM 6 Mbit/s on 5 GHz. If basic rates are available, rtw89 uses the lowest basic rate.
- **Evidence**: Inferred from code (the upstream source code).
- **Effect**: Without this check, the driver writes to the DMA ring of a MAC that has no power.
- **Code**: `mac\tx.c` `mac_tx_mgmt`

<a id="rtw-7"></a>
### RTW-7 Encryption tasks for 8852B data frames

- **Finding**: On the 8852B, `hw_sec_hdr` is false:
  1. Transmit: the driver writes the CCMP header and the PN (rtw89 sets `GENERATE_IV` for the key). The hardware encrypts and adds the MIC.
  2. Receive: the hardware decrypts, but the CCMP header and the 8-byte MIC stay in the frame. The hardware does not check the PN (it sets only `RX_FLAG_DECRYPTED`). The driver removes the header and the MIC. It checks the PN strictly for each TID (17 counters, one of them for non-QoS frames). The PN check occurs after A-MPDU reorder.
  3. Keys: rtw89 keeps a pointer to `key_conf`. For this reason, the key structure must stay available until after `DISABLE_KEY`. Two keys with the same ID cannot exist at the same time. Delete the old key first, then install the new key. If the same key is installed again, do not reset the PN (see `ieee80211_key_identical` in mac80211).
- **Evidence**: Verified on hardware (AP-1, AP-2): in approximately 230 000 frames, the counters for decryption failure, PN replay, and no key are all 0. The tests did not cover a second installation of the same key. That path is inferred from code.
- **Effect**: Without a PN check, there is no replay protection. A PN reset at a second installation of the same key causes nonce reuse.
- **Code**: `mac\key.c` `mac_key_ccmp_hdr`, `mac_key_rx`, `key_identical`, `rtw_linux_add_key`; `mac\rx.c` `mac_rx_deliver`

<a id="rtw-8"></a>
### RTW-8 At SER, rtw89 discards keys and does not notify the driver

- **Finding**: The rtw89 SER L2 uses `ieee80211_iter_keys` under the wiphy lock to discard keys. It does not notify the firmware. It also does not call the `set_key` of the driver. The mac layer must do three tasks:
  1. Mark the keys as lost. After that, do not use these keys to send frames.
  2. Implement `ieee80211_restart_hw`. This function reports the connection loss. It keeps the old `vif` and channel context until the hw is released, because rtw89 still has pointers to them. Then it starts again and calls `reconfig_complete(RESTART)`.
  3. After recovery, Wlansvc reconnects ([CONN-7](#conn-7)).
- **Evidence**: Verified on hardware (AP-1). Recovery takes approximately 110 ms. Wlansvc reconnects immediately.
- **Effect**: If the keys are not marked, the driver sends unencrypted data with a CCMP header. Without restart support, the device stops after a firmware error. You must then restart the device manually.
- **Code**: `mac\key.c` `linuxkpi_ieee80211_iterate_keys`; `mac\hw.c` `linuxkpi_ieee80211_restart_hw`; `mac\iface.c` `mac_restart_work`; `mac\mlme.c` `mac_mlme_restart`

<a id="rtw-9"></a>
### RTW-9 INTx interrupt storm during SER

- **Finding**: With only INTx interrupts, an interrupt storm occurs during SER. The compatibility layer must mask the chip interrupts for rtw89.
  1. Symptom: the storm starts after `rtw89_core_stop` of SER L2 and stops at `rtw89_hci_start` of the restart. During this time, the chip keeps the interrupt line asserted. DPCs on CPU0 cannot run.
  2. Cause: during this time, MAC initialization has already set the interrupt mask register back to its run-time value. An RXDMA interrupt is set and not cleared. Because `running` is false, the rtw89 interrupt handler only returns `IRQ_HANDLED`. It does not mask the interrupt. INTx is level-triggered, so the interrupt stays active.
  3. Solution in the compatibility layer: after the interrupt handler returns `IRQ_HANDLED`, the compatibility layer checks and masks the chip interrupts. Then it queries the rtw89 interrupt handler again. This prevents a conflict with a start that runs at the same time on a different CPU.
- **Evidence**: Verified on hardware: dump analysis of two bug checks 0x133 (`DPC_QUEUE_EXECUTION_TIMEOUT_EXCEEDED`). With the mask, no storm occurs. Each SER masks the interrupts exactly one time.
- **Effect**: On Linux with MSI, this problem does not occur. On a platform with only INTx interrupts, one SER without the mask causes a bug check.
- **Code**: `compat\device.c` `rtw_linux_isr`, `intx_stopped`, `rtw_linux_irq_dpc`

<a id="rtw-10"></a>
### RTW-10 In station mode, BAR frames do not arrive at the driver

- **Finding**: During A-MPDU receive in station mode, BlockAckReq (BAR) frames from the access point do not arrive at the driver (`rx_bar` is always 0).
  1. The reorder window advances only after a 100 ms timeout. Each 48 MiB transfer has only 1–2 timeouts.
  2. `rx_dropped_old` is approximately 2–4% of the received frames. These frames are duplicates that the access point transmits again.
  3. If the driver rejects ADDBA, the access point sends the ADDBA request again approximately every 2.5 s while traffic is present.
- **Evidence**: Verified on hardware (AP-1, AP-2). The cause is inferred from code: the receive filter of the chip.
- **Effect**: `rx_bar` at 0 and a small `rx_dropped_old` value are not faults.
- **Code**: `mac\agg.c` `mac_agg_rx_frame`, `ba_rx_addba`; `mac\rx.c` `linuxkpi_ieee80211_rx`

<a id="rtw-11"></a>
### RTW-11 rtw89 v7.2 has no `mgd_prepare_tx`; do not iterate a `sta` before it is uploaded

- **Finding**: rtw89 v7.2 does not have `mgd_prepare_tx` or `mgd_complete_tx`. The connection procedure of the mac layer can call the ops in the mac80211 sequence. The mac layer adds the `sta` to the list before it calls `sta_state(NOTEXIST → NONE)`. For this reason, the iteration functions must skip a `sta` that is still in NOTEXIST (mac80211 uses the `uploaded` flag).
- **Evidence**: Inferred from code (the upstream source code).
- **Effect**: If the functions do not skip it, rtw89 receives a `sta` with `drv_priv` all zero. This causes a null pointer access or an incorrect configuration.
- **Code**: `mac\iface.c` `sta_uploaded`, `__ieee80211_iterate_stations`; `mac\mlme.c`

<a id="rtw-12"></a>
### RTW-12 Coexistence operates automatically on the Wi-Fi side

- **Finding**: All of the rtw89 coexistence logic (btc) is on the Wi-Fi side.
  1. The mac layer calls the ops in the mac80211 sequence. At transmit, it sets `skb->protocol` and the IP and UDP header offsets. For this reason, all coexistence notifications occur.
  2. Each `rtw89_core_start` (exit from IPS) sets the btc notification counters to zero.
  3. When the Bluetooth controller runs only the ROM firmware, the Bluetooth coexistence version is 0 (not a match). The controller also does not report Bluetooth data. btc can then use only the `BT_IDLE` policy from the scoreboard.
- **Evidence**: Verified on hardware (the counters and the report of `rtwdiag.ps1 btc`). The coexistence result with Bluetooth load is not tested.
- **Effect**: The driver does not have to implement coexistence separately. For the coexistence result, see [BT-9](#bt-9).
- **Code**: `mac\tx.c` `tx_set_protocol`; `compat\rtw89info.c`

<a id="platform"></a>
## ARM64 and RK3588 platform

<a id="plat-1"></a>
### PLAT-1 Only INTx interrupts, all on CPU0

- **Finding**: On the ROCK 5T, the RK3588 PCIe has no MSI. Only INTx interrupts are available. CPU0 processes all INTx interrupts. The CPU0 interrupt baseline is approximately 740 per second. When the driver is idle in WiFiCx mode, CPU0 has approximately 784 interrupts per second.
- **Evidence**: Verified on hardware.
- **Effect**: If the interrupt handler is slow or an interrupt storm occurs, DPCs on CPU0 are late. The system then stops with bug check 0x133 (see [RTW-9](#rtw-9)).
- **Code**: `driver\os.c` `RtwOsAttach`, `Isr`; `compat\device.c` `rtw_linux_isr`

<a id="plat-2"></a>
### PLAT-2 DMA is not cache-coherent

- **Finding**: The RK3588 PCIe is not cache-coherent (ACPI `_CCA` is 0). The HAL maps common buffers as non-cached memory.
  1. `AllocateCommonBufferWithBounds` does not accept `MmWriteCombined`. Use the default cache type.
  2. On non-cached Normal memory, unaligned access does not cause a fault.
  3. The logical addresses are below 4 GB.
- **Evidence**: Verified on hardware.
- **Effect**: If you copy the write-combined allocation from x64, probe returns −12.
- **Code**: `driver\os.c` `AllocCommon`

<a id="plat-3"></a>
### PLAT-3 PCIe link and BARs

- **Finding**: The PCIe link and the BARs on the ROCK 5T are:
  1. The link is Gen1 x1. ASPM, CLKREQ, and L1SS are all off.
  2. BAR2 is a 1 MB memory BAR. BAR0 is an I/O BAR that has no assignment.
  3. When the chip has no power, a read from the MAC area returns `0xEAEAEAEA`. The system does not stop.
  4. FLR = 1 in DevCap (related to [INST-6](#inst-6)).
- **Evidence**: Verified on hardware (the PCI configuration that the driver log records).
- **Effect**: If a register read returns `0xEAEAEAEA`, first check if the chip is in IPS.
- **Code**: `driver\hw.c` `RtwHwMap`, `LogPciConfig`

<a id="plat-4"></a>
### PLAT-4 UEFI must first supply power to the M.2 slot

- **Finding**: Before Windows can find the network adapter, UEFI must supply power to the M.2 E-key slot. UEFI must also set W_DISABLE1# high. On the ROCK 5T, a UEFI port outside this project does this step. The module is on PCIe segment 2.
- **Evidence**: Verified on hardware. The UEFI implementation details are outside the scope of this project.
- **Effect**: For a port to other RK3588 devices, the UEFI and ACPI side must do this step first.
- **Code**: Not in this repository.

<a id="plat-5"></a>
### PLAT-5 Chip and firmware

- **Finding**: The chip and the firmware on the ROCK 5T are:
  1. Chip version 1 (CBV), RFE 5, 2T2R.
  2. The efuse has no country code. The efuse lists 14 channels on 2.4 GHz and 28 channels on 5 GHz. The world rules enable 25 of these 5 GHz channels.
  3. The firmware `rtw8852b_fw-2.bin` has version 0.29.29.18. The download through DMA H2C takes approximately 330 ms. The firmware accepts a CPU exception trigger (for SER tests).
  4. The rtw89 start takes approximately 110 ms.
- **Evidence**: Verified on hardware. The tests used only firmware 0.29.29.18. The power save and SER trigger support of other versions is not determined.
- **Effect**: If you change the firmware version, verify power save and SER again.
- **Code**: `driver\rtw89win.inf` `[Rtw89_CopyFiles]`

<a id="bluetooth"></a>
## Bluetooth patch

<a id="bt-1"></a>
### BT-1 Without the patch, Bluetooth is not usable

- **Finding**: The Bluetooth part of the module connects through USB (on the ROCK 5T, the module is `USB\VID_13D3&PID_3572`). The Windows Bluetooth stack (`bth.inf`, `BTHUSB`) identifies the device. But it does not download the Realtek Bluetooth patch. The controller runs only the ROM firmware (`lmpSubversion` is `0x8852`). The scan finds no devices, and pairing fails.
- **Evidence**: Verified on hardware: a controlled test used the ROM firmware with Wi-Fi connected and with Wi-Fi disabled. In both conditions, the scan found no devices. With the patch, the scan found devices in both conditions. For this reason, the cause is the absence of the patch, not coexistence. The system showed no HCI errors.
- **Effect**: To use Bluetooth, you must install the Bluetooth filter driver.
- **Code**: `bt\rtkbtflt\`

<a id="bt-2"></a>
### BT-2 The vendor Windows Bluetooth software has no ARM64 version

- **Finding**: In the Microsoft Update Catalog, the vendor software for this Bluetooth controller is available only for AMD64.
- **Evidence**: Not determined. The examination included only the Microsoft Update Catalog. It did not include other sources.
- **Effect**: On ARM64, the Bluetooth filter driver of this project is necessary.
- **Code**: None.

<a id="bt-3"></a>
### BT-3 A user-mode download through `bthport` is possible, but the Bluetooth stack then does not operate

- **Finding**: A user-mode program can download the patch through a `bthport` IOCTL. But after the patch becomes active, the Bluetooth stack does not operate:
  1. This IOCTL sends vendor HCI commands. The header is 9 bytes, there is no match mode, and the output is the raw event. The IOCTL lets only OGF `0x3F` pass (it rejects `0x1001` and accepts `0xFC20`).
  2. With this method, the download of 231 patch fragments takes approximately 0.52 s.
  3. After the patch becomes active, the SSP and event mask that `bthport` set are lost. Pairing changes to the PIN method and fails. The BLE scan finds 0 devices.
  4. All methods that make `bthport` initialize again pass through D3. D3 removes the patch ([BT-4](#bt-4)).
- **Evidence**: Verified on hardware.
- **Effect**: The patch download must occur before `BTHUSB` initializes the controller. For this reason, this project uses a lower filter driver. The user-mode tool `rtkbtfw` is only for diagnostics.
- **Code**: `bt\rtkbtfw\rtkbtfw.c` `CmdLoad`, `HciCmd`

<a id="bt-4"></a>
### BT-4 D2 keeps the patch, and D3 removes the patch

- **Finding**: After approximately 1 s of idle time, `BTHUSB` puts the controller into D2 (selective suspend). The patch stays. When you disable and enable Bluetooth, the controller goes through D3. A device restart also goes through D3. In both cases, the patch is lost.

  USB does not re-enumerate the device. When you disable the Wi-Fi network adapter, the patch is not affected.
- **Evidence**: Verified on hardware. Sleep is not tested.
- **Effect**: The Bluetooth filter driver downloads the patch again at each D3 → D0 transition.
- **Code**: `bt\rtkbtflt\driver.c` `FltEvtD0Entry`

<a id="bt-5"></a>
### BT-5 When the Bluetooth filter driver downloads the patch

- **Finding**: The Bluetooth filter driver runs while the START IRP and the D0 IRP return up the stack. At that time, `BTHUSB` has not received the IRP.
  1. At the first start, the filter driver selects a USB configuration itself and downloads the patch. It does not cancel the configuration. `BTHUSB` then selects the configuration again. This has no side effects.
  2. At D3 → D0, the filter driver uses the pipes that `BTHUSB` recorded.
  3. D0Entry always returns success. If the download fails, the filter driver changes to pass-through.
- **Evidence**: Verified on hardware. The tests did not cover the path that intercepts `BTHUSB` read requests (the number of read requests in the queue was always 0). This path is inferred from code.
- **Effect**: If the patch download fails, Bluetooth continues to run with the ROM firmware. The system is not affected.
- **Code**: `bt\rtkbtflt\driver.c` `FltEvtD0Entry`; `bt\rtkbtflt\hci.c` `FltSession`, `FltSelectOwnConfig`; `bt\rtkbtflt\urb.c` `FltEvtWdmInternalIoctl`

<a id="bt-6"></a>
### BT-6 Do not use the `bthport` cache to check if the patch is active

- **Finding**: The version data that `bthport` caches does not change after the patch. To check if the patch is active, use one of these:
  1. `rtwdiag.ps1 btc` on the Wi-Fi side (ROM `0xff0b8852`, with the patch `0x1fc6b20f`).
  2. The version that the peer sees.
  3. The actual functions.

  After `bthport` initializes again, it reads `0xdbc6`/`0xb20f`.
- **Evidence**: Verified on hardware.
- **Effect**: If you use only the `bthport` cache, you see an incorrect patch state.
- **Code**: `bt\rtkbtfw\rtkbtfw.c` `CmdInfo`; `compat\rtw89info.c`

<a id="bt-7"></a>
### BT-7 Install the filter driver with an extension INF and `AddFilter`

- **Finding**: The Bluetooth filter driver installs through `AddFilter` in an extension INF. The base driver stays `bth.inf`.
  1. After you delete the driver package, the device returns to `BTHUSB` and `usbhub`. The pairing records stay.
  2. A new driver package with the same ExtensionId can do an in-place upgrade.
  3. This project does not use a full INF. The reason: a test-signed full INF has a lower rank than the driver in Windows.
- **Evidence**: Verified on hardware.
- **Effect**: When you uninstall the Bluetooth filter driver, the pairing records stay.
- **Code**: `bt\rtkbtflt\rtkbtflt.inf` `[RtkBtFlt_Device.NT.Filters]`

<a id="bt-8"></a>
### BT-8 Bluetooth patch file and download protocol

- **Finding**: The tested patch file and download protocol are:
  1. The patch file has version `0xdbc6b20f` and project id 20. ROM version 1 uses the patch for chip id 2.
  2. The patch and the configuration together are 57 997 + 6 = 58 003 bytes. The download uses 231 fragments.
  3. The fragment sequence numbers go from 0 to `0x7f`, then start again at 1. The sequence number of the last fragment has `0x80` added with a bitwise OR. The key id is 0.
  4. The patch becomes active only after the download of the last fragment. The driver does not send `0xFC66`.
- **Evidence**: Verified on hardware. Inferred from code (723 host tests for `epatch.c`).
- **Effect**: This project reads the patch file only at the container format level. It does not change the patch content.
- **Code**: `bt\rtkbtfw\epatch.c` `rtkbt_epatch_parse`, `rtkbt_frag_get`; `bt\rtkbtflt\hci.c` `FltCheckAndDownload`

<a id="bt-9"></a>
### BT-9 Wi-Fi and Bluetooth coexistence

- **Finding**: The Wi-Fi and the Bluetooth of the module share the antenna (RFE 5). When Wi-Fi is on 5 GHz, rtw89 uses the `WL_5G` policy. During a BLE scan, a 32 MiB transfer over Wi-Fi gives correct data.
- **Evidence**: Verified on hardware, tested one time. The throughput during the BLE scan is not measured (the measurement script did not record the time). OPP file transfer is not tested.
- **Effect**: The change of Wi-Fi throughput with Bluetooth load is not determined.
- **Code**: `compat\rtw89info.c` (coexistence diagnostics)

<a id="build"></a>
## Build

<a id="build-1"></a>
### BUILD-1 clang compiles the upstream rtw89 without changes for the MinGW target

- **Finding**: This project does not change one line of the upstream rtw89 source code. clang compiles it for the MinGW target (`aarch64-w64-mingw32`) with `-mno-ms-bitfields`.
  1. clang-cl (MSVC target) can compile the code, but its bit-field layout is different from GCC.
  2. To check stack frames, add `-ExtraFlags '-Wframe-larger-than=1024'` to `buildhal.ps1` (the kernel stack is small). This option is not in the default options.
  3. `mac\util.c` has the same name as the upstream `util.c`. For this reason, the build script adds a directory prefix to the object file names.
- **Evidence**: Inferred from code, and verified by compilation.
- **Effect**: If the bit-field layout is not correct, the fields of the structures that rtw89 reads and writes are at incorrect positions.
- **Code**: `buildhal.ps1`

<a id="build-2"></a>
### BUILD-2 Link requirements of the WiFiCx TLV library

- **Finding**: `WificxTLVGenParse.lib` has these requirements:
  1. The driver must supply `operator new(size_t, ULONG_PTR)` and `operator delete(void*)`.
  2. The library refers to `__security_push_cookie` and `__security_pop_cookie`. These two symbols are only in `libcntpr.lib`.
  3. Compile the C++ files with `/Zc:sizedDealloc-`. If you do not, the array placement new in `arrayofelements.hpp` causes error C2956.
  4. Put the shared headers between `EXTERN_C_START` and `EXTERN_C_END`.

  Versions: WiFiCx 1.1, NetAdapterCx 2.4, KMDF 1.33. The version macros are `NET_VERSION_*`, not `NETADAPTER_VERSION_*`.
- **Evidence**: Inferred from code, and verified by compilation.
- **Effect**: If one of these items is not present, the link or the compilation fails.
- **Code**: `wificx\alloc.cpp`; `driver\rtw89win.vcxproj`

<a id="build-3"></a>
### BUILD-3 Import table of the Wi-Fi driver

- **Finding**: The Wi-Fi driver imports only `ntoskrnl.exe`, `HAL.dll`, `WDFLDR.SYS`, and `ksecdd.sys` (CNG, see [SEC-5](#sec-5)). NetAdapterCx and WiFiCx bind through the class extension mechanism of WdfLdr. They are not in the import table.
- **Evidence**: Inferred from code, and verified by compilation (an examination of the import table).
- **Effect**: If other modules occur in the import table, make sure that the driver does not use prohibited kernel interfaces.
- **Code**: `driver\rtw89win.vcxproj`

<a id="open-questions"></a>
## Open questions

| Question | Related finding | Evidence that is not available |
|---|---|---|
| The result of an incorrect `PeerVersion` | [INIT-1](#init-1) | No counterexample |
| The byte order of the group key RSC | [CONN-1](#conn-1) | Windows only gave the value 0 |
| Which field Windows reads: `SAEConfirmFrame` or `SAECommitFrame` | [SEC-2](#sec-2) | No test with only one field |
| The format of the 16 bytes from the power setting callback | [PS-2](#ps-2) | The tests found no documentation |
| The purpose of `ChannelWidthList` | [WIDE-3](#wide-3) | The Windows output does not change |
| If the station can only use HT20 when the hotspot starts before the station connects | [WIDE-10](#wide-10) | Not tested on the target device |
| The cause of the low downlink throughput with `Bandwidth=40` | [WIDE-11](#wide-11) | No analysis |
| Why `ScanSSIDListSize` must be 4; if 3 is possible | [HS-2](#hs-2) | The value 3 is not tested |
| If TX demux discards multicast frames | [HS-8](#hs-8) | The only evidence is the WiFiCx internal symbols |
| Why Windows reports `BandInterference` after the driver completes `TASK_START_AP` successfully | [HS-12](#hs-12) | It occurred only one time |
| The full conditions that the mobile hotspot service uses to permit the hotspot | [HS-13](#hs-13) | No documentation |
| If rtw89 gives PS-Poll frames to the driver | [HS-17](#hs-17) | Client-1 does not send PS-Poll |
| The cause of the flush timeout when the driver removes a hotspot client; the order of 29 and 31 during a group key update | [HS-20](#hs-20) | No analysis; no group key update occurred |
| If FLR is the cause of the firmware download failure after re-enumeration | [INST-6](#inst-6) | Nobody saw the FLR directly |
| Why `IoRegisterShutdownNotification` on the filter device object has no effect | [INST-5](#inst-5) | No analysis |
| The power save and SER trigger support of other firmware versions | [PLAT-5](#plat-5) | The tests used only 0.29.29.18 |
| The Wi-Fi throughput with Bluetooth load | [BT-9](#bt-9), [RTW-12](#rtw-12) | No paired headset-type device; the script did not record the time |
| If the Bluetooth patch stays after sleep | [BT-4](#bt-4) | Sleep is not tested |
| If a source other than the Microsoft Update Catalog has an ARM64 version of the vendor Bluetooth software | [BT-2](#bt-2) | The examination included only one source |
