// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * rtw89win WiFiCx front end: device setup (the C entry points in rtw89win.h) and the capability report.
 * Order (WiFiCx sample driver.cpp, device.cpp, wifihal.cpp): NetDeviceInitConfig and WifiDeviceInitConfig before
 * WdfDeviceCreate, WifiDeviceInitialize after it; firmware download and the four capability calls in
 * EvtDevicePrepareHardware; the station adapter when WiFiCx asks for it (adapter.cpp).
 * Capabilities: what rtw89 registered after the world regulatory rules, up to VHT; 20 MHz channels on 2.4 GHz, on
 * 5 GHz the widths up to Parameters\Bandwidth (20/40/80 MHz). Security: open and WPA2-PSK/CCMP; with Parameters\Pmf 1
 * or 2 (default 0) and the crypto self-tests passed, also management frame protection (MFPCapable, BIP-CMAC-128 as
 * group management cipher) and with Pmf 2 WPA3-SAE (SAEAuthenticationSupported, AKM 00-0F-AC:8), like the WiFiCx
 * sample's wifihal.cpp.
 * With Parameters\WifiDirect on, also the Wi-Fi Direct capabilities (wfd.cpp); release and power down stop a GO.
 */
#include "wifi.h"

#define RTW_SCAN_SSIDS          4       // ScanSSIDListSize; rtw89 takes 8 (RTW89_SCANOFLD_MAX_SSID)
#define RTW_MAX_PHYS            5       // HR/DSSS, ERP, OFDM, HT, VHT

/* ---- capabilities ---- */
static VOID
FillPhy (
  _Out_ WIFI_PHY_INFO      *Info,
  _In_ WDI_PHY_TYPE        Type,
  _In_reads_ (Count) const UINT8  *Rates,
  _In_ ULONG               Count,
  _In_ BOOLEAN             CckOnly
  )
{
  ULONG  i;

  Info->PhyType               = Type;
  Info->NumberDataRateEntries = 0;
  for (i = 0; (i < Count) && (Info->NumberDataRateEntries < WDI_MAX_DATA_RATE_ENTRIES); i++) {
    // HR/DSSS: 1, 2, 5.5 and 11 Mbit/s (500 kbit/s units)
    if (CckOnly && (Rates[i] != 2) && (Rates[i] != 4) && (Rates[i] != 11) && (Rates[i] != 22)) {
      continue;
    }

    Info->DataRateList[Info->NumberDataRateEntries].DataRateFlag  = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
    Info->DataRateList[Info->NumberDataRateEntries].DataRateValue = Rates[i];
    Info->NumberDataRateEntries++;
  }
}

static NTSTATUS
ReportCapabilities (
  _In_ PRTW_WIFI  W
  )
{
  static const UINT32         Width20 = 20;
  static const UINT32         Width5[3] = { 20, 40, 80 };     // 5 GHz up to Parameters\Bandwidth
  const struct rtw_caps       *C;
  ULONG                       Wide;
  WIFI_DEVICE_CAPABILITIES    DevCaps;
  WIFI_STATION_CAPABILITIES   StaCaps;
  WIFI_BAND_CAPABILITIES      BandCaps;
  WIFI_PHY_CAPABILITIES       PhyCaps;
  WIFI_BAND_INFO              Bands[2];
  WDI_PHY_TYPE                BandPhys[2][3];
  DOT11_AUTH_CIPHER_PAIR      Unicast[3];
  DOT11_AUTH_CIPHER_PAIR      MgmtCast[3];
  RSNA_AKM_SUITE              Akms[2];
  WDI_CHANNEL_MAPPING_ENTRY   *Chans;
  WIFI_PHY_INFO               *Phys;
  struct rtw_radio_info       Radio;
  ULONG                       NumUnicast;
  ULONG                       NumMgmtCast;
  ULONG                       NumAkms;
  ULONG                       NumBands;
  ULONG                       NumPhys;
  ULONG                       Pmf;
  ULONG                       b;
  ULONG                       i;
  NTSTATUS                    Status;

  C     = W->Caps;
  Pmf   = rtw_linux_get_pmf ();   // Parameters\Pmf as it took effect (the crypto self-tests passed)
  Wide  = rtw_linux_get_wide ();  // Parameters\Bandwidth (20, 40 or 80)
  Chans = (WDI_CHANNEL_MAPPING_ENTRY *)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (*Chans) * 2 * 64, RTW_POOL_TAG);
  Phys  = (WIFI_PHY_INFO *)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (*Phys) * RTW_MAX_PHYS, RTW_POOL_TAG);
  if ((Chans == NULL) || (Phys == NULL)) {
    Status = STATUS_INSUFFICIENT_RESOURCES;
    goto Exit;
  }

  WIFI_DEVICE_CAPABILITIES_INIT (&DevCaps);
  DevCaps.HardwareRadioState = TRUE;            // no radio switch
  DevCaps.SoftwareRadioState = W->RadioOn;
  for (i = 0; (i < sizeof (C->fw_version) - 1) && (C->fw_version[i] != '\0'); i++) {
    DevCaps.FirmwareVersion[i] = (UCHAR)C->fw_version[i];
  }

  DevCaps.NumRxStreams                = (UINT8)max (C->rx_streams, 1);
  DevCaps.NumTxStreams                = (UINT8)max (C->tx_streams, 1);
  DevCaps.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;   // firmware coex
  // SAE with Parameters\Pmf = 2 (mac\sae.c does the frames, the OS the computations)
  DevCaps.SAEAuthenticationSupported  = (Pmf == RTW_PMF_SAE);
  // no action frames, no MAC randomization (so WiFiCx never hands over random addresses), no FT/MBO
  Status = WifiDeviceSetDeviceCapabilities (W->Device, &DevCaps);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceSetDeviceCapabilities failed 0x%08x", Status);
    goto Exit;
  }

  // open, WPA2-PSK and WPA3-SAE with CCMP; group management frames protected with BIP-CMAC-128 under MFP
  NumUnicast                          = 0;
  Unicast[NumUnicast].AuthAlgoId      = DOT11_AUTH_ALGO_80211_OPEN;
  Unicast[NumUnicast++].CipherAlgoId  = DOT11_CIPHER_ALGO_NONE;
  Unicast[NumUnicast].AuthAlgoId      = DOT11_AUTH_ALGO_RSNA_PSK;
  Unicast[NumUnicast++].CipherAlgoId  = DOT11_CIPHER_ALGO_CCMP;
  NumMgmtCast                         = 0;
  MgmtCast[NumMgmtCast].AuthAlgoId    = DOT11_AUTH_ALGO_80211_OPEN;
  MgmtCast[NumMgmtCast++].CipherAlgoId = DOT11_CIPHER_ALGO_NONE;
  NumAkms                             = 0;
  Akms[NumAkms++]                     = rsna_akm_psk;
  if (Pmf != RTW_PMF_OFF) {
    MgmtCast[NumMgmtCast].AuthAlgoId    = DOT11_AUTH_ALGO_RSNA_PSK;
    MgmtCast[NumMgmtCast++].CipherAlgoId = DOT11_CIPHER_ALGO_BIP;
  }

  if (Pmf == RTW_PMF_SAE) {
    Unicast[NumUnicast].AuthAlgoId      = DOT11_AUTH_ALGO_WPA3_SAE;
    Unicast[NumUnicast++].CipherAlgoId  = DOT11_CIPHER_ALGO_CCMP;
    MgmtCast[NumMgmtCast].AuthAlgoId    = DOT11_AUTH_ALGO_WPA3_SAE;
    MgmtCast[NumMgmtCast++].CipherAlgoId = DOT11_CIPHER_ALGO_BIP;
    Akms[NumAkms++]                     = rsna_akm_sae_pmk256;
  }

  WIFI_STATION_CAPABILITIES_INIT (&StaCaps);
  StaCaps.ScanSSIDListSize                    = (C->max_scan_ssids != 0) ? min (C->max_scan_ssids, RTW_SCAN_SSIDS) : RTW_SCAN_SSIDS;
  StaCaps.DesiredSSIDListSize                 = 1;
  StaCaps.PrivacyExemptionListSize            = RTW_EXEMPT_MAX;     // mac\key.c keeps this many
  StaCaps.KeyMappingTableSize                 = 32;
  StaCaps.DefaultKeyTableSize                 = 4;
  StaCaps.WEPKeyValueMaxLength                = 0;          // no WEP
  StaCaps.MaxNumPerSTA                        = 4;
  StaCaps.BSSListCachemanagement              = FALSE;      // the OS keeps the BSS list
  // 802.11 power save is ours to manage (mac\ps.c, rtw89 LPS), following the OS's hints and power plan; not
  // claimed when Parameters\PowerSave = 0 turns it off
  rtw_linux_radio_info (&Radio);
  StaCaps.AutoPowerSaveMode                   = (Radio.ps_reg != RTW_PS_REG_NEVER);
  // management frame protection (mac\pmf.c); the OS then sets MFPEnabled in connect requests
  StaCaps.MFPCapable                          = (Pmf != RTW_PMF_OFF);
  StaCaps.NumSupportedUnicastAlgorithms       = NumUnicast;
  StaCaps.UnicastAlgorithmsList               = Unicast;
  StaCaps.NumSupportedMulticastDataAlgorithms = NumUnicast;
  StaCaps.MulticastDataAlgorithmsList         = Unicast;
  StaCaps.NumSupportedMulticastMgmtAlgorithms = NumMgmtCast;
  StaCaps.MulticastMgmtAlgorithmsList         = MgmtCast;
  StaCaps.NumAkmsSupported                    = NumAkms;
  StaCaps.AkmsList                            = Akms;
  Status = WifiDeviceSetStationCapabilities (W->Device, &StaCaps);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceSetStationCapabilities failed 0x%08x", Status);
    goto Exit;
  }

  // bands: 2.4 GHz HR/DSSS, ERP (+HT), 20 MHz; 5 GHz OFDM (+HT, VHT), see the widths below; every channel not
  // disabled, the passive (NO_IR) ones included, so the OS keeps BSSs found on them
  RtlZeroMemory (Bands, sizeof (Bands));
  NumBands = 0;
  for (b = 0; b < 2; b++) {
    ULONG  n;

    if (C->n_chans[b] == 0) {
      continue;
    }

    n = 0;
    if (b == 0) {
      BandPhys[b][n++] = WDI_PHY_TYPE_HRDSSS;
      BandPhys[b][n++] = WDI_PHY_TYPE_ERP;
    } else {
      BandPhys[b][n++] = WDI_PHY_TYPE_OFDM;
    }

    if (C->ht[b]) {
      BandPhys[b][n++] = WDI_PHY_TYPE_HT;
    }

    if ((b == 1) && C->vht[b] && (n < 3)) {
      BandPhys[b][n++] = WDI_PHY_TYPE_VHT;
    }

    for (i = 0; i < C->n_chans[b]; i++) {
      Chans[b * 64 + i].ChannelNumber          = C->chans[b][i].number;
      Chans[b * 64 + i].ChannelCenterFrequency = C->chans[b][i].freq;
    }

    Bands[NumBands].BandID               = (b == 0) ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    Bands[NumBands].BandState            = TRUE;
    Bands[NumBands].NumValidPhyTypes     = n;
    Bands[NumBands].ValidPhyTypeList     = BandPhys[b];
    Bands[NumBands].NumValidChannelTypes = C->n_chans[b];
    Bands[NumBands].ValidChannelTypes    = &Chans[b * 64];
    Bands[NumBands].NumChannelWidths     = 1;
    Bands[NumBands].ChannelWidthList     = (UINT32 *)&Width20;
    if ((b == 1) && C->ht[b] && (Wide >= 40)) {
      // the widths the station may use on 5 GHz (MHz); 80 MHz with VHT only
      Bands[NumBands].NumChannelWidths = ((Wide >= 80) && C->vht[b]) ? 3 : 2;
      Bands[NumBands].ChannelWidthList = (UINT32 *)Width5;
    }

    NumBands++;
  }

  WIFI_BAND_CAPABILITIES_INIT (&BandCaps);
  BandCaps.NumBands     = NumBands;
  BandCaps.BandInfoList = Bands;
  Status = WifiDeviceSetBandCapabilities (W->Device, &BandCaps);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceSetBandCapabilities failed 0x%08x", Status);
    goto Exit;
  }

  // PHYs with their legacy rates (HT/VHT: the legacy table of their band; no MCS rates are listed)
  NumPhys = 0;
  if (C->n_chans[0] != 0) {
    FillPhy (&Phys[NumPhys++], WDI_PHY_TYPE_HRDSSS, C->rates[0], C->n_rates[0], TRUE);
    FillPhy (&Phys[NumPhys++], WDI_PHY_TYPE_ERP, C->rates[0], C->n_rates[0], FALSE);
  }

  if (C->n_chans[1] != 0) {
    FillPhy (&Phys[NumPhys++], WDI_PHY_TYPE_OFDM, C->rates[1], C->n_rates[1], FALSE);
  }

  if ((C->ht[0] && (C->n_chans[0] != 0)) || (C->ht[1] && (C->n_chans[1] != 0))) {
    b = (C->ht[0] && (C->n_chans[0] != 0)) ? 0 : 1;
    FillPhy (&Phys[NumPhys++], WDI_PHY_TYPE_HT, C->rates[b], C->n_rates[b], FALSE);
  }

  if (C->vht[1] && (C->n_chans[1] != 0)) {
    FillPhy (&Phys[NumPhys++], WDI_PHY_TYPE_VHT, C->rates[1], C->n_rates[1], FALSE);
  }

  WIFI_PHY_CAPABILITIES_INIT (&PhyCaps);
  PhyCaps.NumPhyTypes = NumPhys;
  PhyCaps.PhyInfoList = Phys;
  Status = WifiDeviceSetPhyCapabilities (W->Device, &PhyCaps);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceSetPhyCapabilities failed 0x%08x", Status);
    goto Exit;
  }

  RtwLog (
    "WiFiCx: capabilities reported: %uT%uR, fw %s, 2.4 GHz %u channels%s, 5 GHz %u channels%s%s up to %u MHz, %u PHYs%s%s",
    DevCaps.NumTxStreams, DevCaps.NumRxStreams, C->fw_version, C->n_chans[0], C->ht[0] ? " HT" : "",
    C->n_chans[1], C->ht[1] ? " HT" : "", C->vht[1] ? " VHT" : "",
    !C->ht[1] ? 20 : ((Wide >= 80) && C->vht[1]) ? 80 : (Wide >= 40) ? 40 : 20, NumPhys,
    StaCaps.AutoPowerSaveMode ? ", auto power save" : "",
    (Pmf == RTW_PMF_SAE) ? ", MFP, WPA3-SAE" : (Pmf == RTW_PMF_ON) ? ", MFP" : ""
    );

  // Wi-Fi Direct (the mobile hotspot) with Parameters\WifiDirect on; a failure there leaves the station alone
  RtwWifiReportWfdCapabilities (W);

Exit:
  if (Chans != NULL) {
    ExFreePoolWithTag (Chans, RTW_POOL_TAG);
  }

  if (Phys != NULL) {
    ExFreePoolWithTag (Phys, RTW_POOL_TAG);
  }

  return Status;
}

/* ---- entry points for driver.c ---- */
NTSTATUS
RtwWifiPreDeviceCreate (
  _Inout_ PWDFDEVICE_INIT  DeviceInit
  )
{
  NTSTATUS  Status;

  Status = NetDeviceInitConfig (DeviceInit);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: NetDeviceInitConfig failed 0x%08x", Status);
    return Status;
  }

  Status = WifiDeviceInitConfig (DeviceInit);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceInitConfig failed 0x%08x", Status);
  }

  return Status;
}

NTSTATUS
RtwWifiPostDeviceCreate (
  _In_ PRTW_DEVICE  Dev
  )
{
  DECLARE_CONST_UNICODE_STRING (DumpName, L"WdiDump");
  WIFI_DEVICE_CONFIG     Config;
  WDF_OBJECT_ATTRIBUTES  Attributes;
  WDF_WORKITEM_CONFIG    WorkConfig;
  WDF_TIMER_CONFIG       TimerConfig;
  WDFMEMORY              Memory;
  WDFKEY                 Key;
  PRTW_WIFI              W;
  PVOID                  Context;
  ULONG                  Dump;
  NTSTATUS               Status;

  // the context first: WiFiCx may call back (commands, adapter) as soon as it is initialized
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, RTW_WIFI);
  Status = WdfObjectAllocateContext (Dev->Device, &Attributes, &Context);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: context not allocated (0x%08x)", Status);
    return Status;
  }

  W          = (PRTW_WIFI)Context;          // zeroed
  W->Dev     = Dev;
  W->Device  = Dev->Device;
  W->RadioOn = TRUE;
  KeInitializeSpinLock (&W->Lock);
  RtwWifiDpInit (W);

  WDF_OBJECT_ATTRIBUTES_INIT (&Attributes);
  Attributes.ParentObject = Dev->Device;
  WDF_WORKITEM_CONFIG_INIT (&WorkConfig, RtwWifiCmdWork);
  WorkConfig.AutomaticSerialization = FALSE;
  Status = WdfWorkItemCreate (&WorkConfig, &Attributes, &W->CmdWork);
  if (NT_SUCCESS (Status)) {
    WDF_TIMER_CONFIG_INIT (&TimerConfig, RtwWifiScanTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    Status = WdfTimerCreate (&TimerConfig, &Attributes, &W->ScanTimer);
  }

  if (NT_SUCCESS (Status)) {
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, RTW_POOL_TAG, sizeof (*W->Caps), &Memory,
                              (PVOID *)&W->Caps);
  }

  if (NT_SUCCESS (Status)) {
    RtlZeroMemory (W->Caps, sizeof (*W->Caps));
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, RTW_POOL_TAG, RTW_WIFI_EXPORT_SIZE, &Memory,
                              (PVOID *)&W->ExportBuf);
  }

  if (NT_SUCCESS (Status)) {
    WDF_TIMER_CONFIG_INIT (&TimerConfig, RtwWifiConnTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    Status = WdfTimerCreate (&TimerConfig, &Attributes, &W->ConnTimer);
  }

  if (NT_SUCCESS (Status)) {
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, RTW_POOL_TAG, RTW_WIFI_CONN_BUF, &Memory,
                              (PVOID *)&W->ConnBuf);
  }

  if (NT_SUCCESS (Status)) {
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, RTW_POOL_TAG, RTW_WIFI_SAE_BUF, &Memory,
                              (PVOID *)&W->SaeBuf);
  }

  if (NT_SUCCESS (Status)) {
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, RTW_POOL_TAG, RTW_WIFI_AP_BUF, &Memory,
                              (PVOID *)&W->ApBuf);
  }

  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: setup failed 0x%08x", Status);
    return Status;
  }

  WIFI_DEVICE_CONFIG_INIT (
    &Config,
    WDI_VERSION_LATEST,
    RtwEvtWifiSendCommand,
    RtwEvtWifiCreateAdapter,
    RtwEvtWifiCreateWifiDirectDevice
    );
  Status = WifiDeviceInitialize (Dev->Device, &Config);
  if (!NT_SUCCESS (Status)) {
    RtwLog ("WiFiCx: WifiDeviceInitialize failed 0x%08x", Status);
    return Status;
  }

  // TLV messages are parsed and generated in the OS's WDI version (sample driver.cpp)
  W->OsWdiVersion          = WifiDeviceGetOsWdiVersion (Dev->Device);
  W->Tlv.AllocationContext = 0;
  W->Tlv.PeerVersion       = W->OsWdiVersion;

  Dump = 0;
  if (NT_SUCCESS (WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
    if (!NT_SUCCESS (WdfRegistryQueryULong (Key, &DumpName, &Dump))) {
      Dump = 0;
    }

    WdfRegistryClose (Key);
  }

  W->WdiDump = (Dump == 1);
  Dev->Wifi  = W;
  RtwLog ("WiFiCx: device initialized, driver WDI version %#x, OS WDI version %#x%s", WDI_VERSION_LATEST,
          W->OsWdiVersion, W->WdiDump ? ", WDI dump on" : "");
  return STATUS_SUCCESS;
}

NTSTATUS
RtwWifiPrepareHardware (
  _In_ PRTW_DEVICE  Dev
  )
{
  PRTW_WIFI  W;
  NTSTATUS   Status;
  LONG       Ret;
  KIRQL      Irql;

  W = (PRTW_WIFI)Dev->Wifi;
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->Stopping = FALSE;            // again after a stop (resource rebalance)
  KeReleaseSpinLock (&W->Lock, Irql);

  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  RtwLog ("WiFiCx: probe");
  W->Wfd = FALSE;                 // set again by the capability report
  Ret = RtwProbe (Dev);
  if (Ret != 0) {
    RtwLog ("WiFiCx: probe failed: %d", Ret);
    Status = (Ret == -12) ? STATUS_INSUFFICIENT_RESOURCES : STATUS_ADAPTER_HARDWARE_ERROR;    // -ENOMEM
  } else {
    Ret    = rtw_linux_get_caps (W->Caps);
    Status = (Ret == 0) ? ReportCapabilities (W) : STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS (Status)) {
      W->HwReady     = TRUE;
      W->Started     = FALSE;
      W->PoweredDown = FALSE;
      W->ReadyNs     = rtwk_time_ns ();
    } else {
      RtwRemove (Dev);
    }
  }

  WdfWaitLockRelease (Dev->CtlLock);
  return Status;
}

VOID
RtwWifiReleaseHardware (
  _In_ PRTW_DEVICE  Dev
  )
{
  PRTW_WIFI  W;
  KIRQL      Irql;

  W = (PRTW_WIFI)Dev->Wifi;

  // no new work and no more scan events; then let the work item finish (it fails what is still queued)
  KeAcquireSpinLock (&W->Lock, &Irql);
  W->Stopping = TRUE;
  KeReleaseSpinLock (&W->Lock, Irql);
  WdfTimerStop (W->ScanTimer, TRUE);
  WdfTimerStop (W->ConnTimer, TRUE);
  WdfWorkItemFlush (W->CmdWork);

  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  RtwWifiApStop (W, FALSE, 0);    // the GO first (nothing without one); the device is going: no indications
  RtwWifiConnStop (W, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST, FALSE);   // the device is going: no indications
  RtwWifiScanStop (W, FALSE);
  RtwWifiStopMac (W);
  W->HwReady = FALSE;
  WdfWaitLockRelease (Dev->CtlLock);

  // The callbacks point at this device's W, but the mac layer's table lives as long as the driver: detach here
  // as well (AdapterCleanup does it only at PASSIVE_LEVEL), so a later diagnostic session of the same driver load
  // never calls into a freed W. Then let a DPC they queued finish.
  rtw_linux_dp_attach (NULL);
  rtw_linux_ap_dp_attach (NULL);  // the GO port's callbacks likewise
  KeFlushQueuedDpcs ();
  RtwLog ("WiFiCx: hardware released (%ld commands received, %ld completed)", W->Received, W->Completed);
}

VOID
RtwWifiD0Entry (
  _In_ PRTW_DEVICE  Dev
  )
{
  PRTW_WIFI  W;

  W = (PRTW_WIFI)Dev->Wifi;
  if (W == NULL) {
    return;
  }

  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  W->PoweredDown = FALSE;
  WdfWaitLockRelease (Dev->CtlLock);
}

/* Leaving D0 (also for shutdown, where ReleaseHardware does not run): no DMA may go on. A running scan task is
   completed as aborted while the port still exists; the next command that needs the radio starts it again. */
VOID
RtwWifiD0Exit (
  _In_ PRTW_DEVICE  Dev
  )
{
  PRTW_WIFI  W;

  W = (PRTW_WIFI)Dev->Wifi;
  if (W == NULL) {
    return;
  }

  WdfWaitLockAcquire (Dev->CtlLock, NULL);
  RtwWifiApStop (W, FALSE, 0);    // the GO goes without indications (the OS resets the role port in D0)
  RtwWifiConnStop (W, WDI_ASSOC_STATUS_RADIO_OFF, TRUE);
  RtwWifiScanStop (W, TRUE);
  RtwWifiStopMac (W);
  W->PoweredDown = TRUE;
  WdfWaitLockRelease (Dev->CtlLock);
}

VOID
RtwWifiGetMode (
  _In_ PRTW_DEVICE                    Dev,
  _Out_ struct _RTW_MODE_OUT          *Out,
  _Out_opt_ struct _RTW_WDI_COUNTS    *Counts
  )
{
  PRTW_WIFI  W;
  KIRQL      Irql;
  ULONG      i;

  W = (PRTW_WIFI)Dev->Wifi;
  RtlZeroMemory (Out, sizeof (*Out));
  KeAcquireSpinLock (&W->Lock, &Irql);
  Out->AdapterPort = W->AdapterUp ? W->PortId : MAXULONG;
  KeReleaseSpinLock (&W->Lock, Irql);
  // the rest is owned under CtlLock; a snapshot without it is good enough for diagnostics
  Out->HwReady      = W->HwReady;
  Out->RadioOn      = W->RadioOn;
  Out->Started      = W->Started;
  Out->ScanPending  = W->Scan.Active;
  Out->OsWdiVersion = W->OsWdiVersion;
  Out->Received     = (ULONG)W->Received;
  Out->Completed    = (ULONG)W->Completed;
  Out->Unsupported  = (ULONG)W->Unsupported;
  Out->Scans        = (ULONG)W->Scans;
  Out->LastScanBss  = (ULONG)W->LastScanBss;
  if (Counts != NULL) {
    for (i = 0; i < RTW_WDI_IDS; i++) {
      Counts->Received[i]    = W->MsgCount[i];
      Counts->Unsupported[i] = W->MsgUnsupported[i];
    }
  }
}

/* IOCTL_RTW_AP: the front end's side of Wi-Fi Direct (a snapshot, like RtwWifiGetMode) */
VOID
RtwWifiGetWfd (
  _In_ PRTW_DEVICE              Dev,
  _Out_ struct _RTW_WFD_OUT     *Out
  )
{
  PRTW_WIFI  W;
  KIRQL      Irql;

  RtlZeroMemory (Out, sizeof (*Out));
  W = (PRTW_WIFI)Dev->Wifi;
  Out->WfdPort  = MAXULONG;
  Out->RolePort = MAXULONG;
  if (W == NULL) {
    return;
  }

  KeAcquireSpinLock (&W->Lock, &Irql);
  Out->WfdPort  = W->WfdUp ? W->WfdPortId : MAXULONG;
  Out->RolePort = W->RoleUp ? W->RolePortId : MAXULONG;
  KeReleaseSpinLock (&W->Lock, Irql);
  Out->Wfd       = W->Wfd;
  Out->ApRunning = W->Ap.Running;
  Out->ApPeers   = W->Ap.NumPeers;
  Out->ApStarts  = W->Ap.Seq;
  Out->Band      = W->Ap.Band;
  Out->Channel   = W->Ap.Channel;
}
