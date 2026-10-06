// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Starts and stops rtw89 on the device, from the WiFiCx front end's PrepareHardware or, in the diagnostic mode, by
 * IOCTL (tools\rtwdiag.ps1 probe/remove). Probe powers the chip on, downloads the firmware, reads the efuse and powers
 * it off again, then registers with the mac layer; nothing is transmitted. Remove stops the mac layer first (start,
 * scan, listening), then removes rtw89. Callers hold Dev->CtlLock.
 * After the probe, the settings under Parameters (below) go to the mac layer.
 */
#include "rtw89win.h"
#include "../compat/include/rtw/os.h"
#include "../compat/include/rtw/glue.h"

// Linux errno values
#define ENOMEM  12
#define EBUSY   16
#define ENODEV  19
#define EINVAL  22

static BOOLEAN  mLinuxUp;     // rtw_linux_init done (system work queues and their threads exist)

/*
 * This driver's settings under Parameters, read at every probe (so a change takes a restart of the device):
 *  Country    REG_SZ, two letters: the regulatory domain to use (wireless-regdb); absent or "00" = none (the
 *             Country element of the connected BSS, else the world rules)
 *  PowerSave  REG_DWORD: 0 never save power, 1 follow the OS (default), 2 always (tests)
 *  Pmf        REG_DWORD: 0 no management frame protection and no WPA3-SAE (the default, also for a value other than
 *             0-2), 1 MFP, 2 MFP and SAE; either needs the crypto self-tests to pass
 *  WifiDirect REG_DWORD: 0 no Wi-Fi Direct (so no mobile hotspot): no capabilities reported, no ports; 1 on; absent or
 *             another value: RTW_WIFI_DIRECT_DEFAULT
 *  Bandwidth  REG_DWORD: the widest 5 GHz channel of the station, MHz: 20 (HT20 only), 40, 80; absent or another
 *             value: RTW_WIDE_DEFAULT (80). 2.4 GHz and the GO stay at 20 MHz.
 */
#define RTW_WIFI_DIRECT_DEFAULT  RTW_WFD_ON

static VOID
RtwApplySettings (
  VOID
  )
{
  DECLARE_CONST_UNICODE_STRING (CountryName, L"Country");
  DECLARE_CONST_UNICODE_STRING (PowerSaveName, L"PowerSave");
  DECLARE_CONST_UNICODE_STRING (PmfName, L"Pmf");
  DECLARE_CONST_UNICODE_STRING (WifiDirectName, L"WifiDirect");
  DECLARE_CONST_UNICODE_STRING (BandwidthName, L"Bandwidth");
  WCHAR           Buf[8];
  UNICODE_STRING  Str;
  USHORT          Bytes;
  WDFKEY          Key;
  ULONG           PowerSave;
  ULONG           Pmf;
  ULONG           WifiDirect;
  ULONG           Bandwidth;
  ULONG           n;
  CHAR            Country[3];
  BOOLEAN         Given;
  BOOLEAN         WfdGiven;
  BOOLEAN         BwGiven;
  BOOLEAN         BwBad;
  NTSTATUS        Status;
  int             Ret;

  PowerSave  = RTW_PS_REG_FOLLOW_OS;
  Pmf        = RTW_PMF_OFF;
  WifiDirect = RTW_WIFI_DIRECT_DEFAULT;
  WfdGiven   = FALSE;
  Bandwidth  = RTW_WIDE_DEFAULT;
  BwGiven    = FALSE;
  BwBad      = FALSE;
  Country[0] = '\0';
  Given      = FALSE;
  if (NT_SUCCESS (WdfDriverOpenParametersRegistryKey (WdfGetDriver (), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
    if (!NT_SUCCESS (WdfRegistryQueryULong (Key, &PowerSaveName, &PowerSave))) {
      PowerSave = RTW_PS_REG_FOLLOW_OS;
    }

    if (!NT_SUCCESS (WdfRegistryQueryULong (Key, &PmfName, &Pmf))) {
      Pmf = RTW_PMF_OFF;
    }

    if (NT_SUCCESS (WdfRegistryQueryULong (Key, &WifiDirectName, &WifiDirect)) &&
        ((WifiDirect == RTW_WFD_OFF) || (WifiDirect == RTW_WFD_ON)))
    {
      WfdGiven = TRUE;
    } else {
      WifiDirect = RTW_WIFI_DIRECT_DEFAULT;
    }

    if (NT_SUCCESS (WdfRegistryQueryULong (Key, &BandwidthName, &Bandwidth))) {
      BwGiven = TRUE;
      if ((Bandwidth != 20) && (Bandwidth != 40) && (Bandwidth != 80)) {
        BwBad     = TRUE;
        Bandwidth = RTW_WIDE_DEFAULT;
      }
    } else {
      Bandwidth = RTW_WIDE_DEFAULT;
    }

    RtlInitEmptyUnicodeString (&Str, Buf, sizeof (Buf));
    Status = WdfRegistryQueryUnicodeString (Key, &CountryName, &Bytes, &Str);
    Given  = (Status != STATUS_OBJECT_NAME_NOT_FOUND);    // too long or not REG_SZ: given, but no use
    if (NT_SUCCESS (Status)) {
      n = Str.Length / sizeof (WCHAR);
      while ((n != 0) && (Buf[n - 1] == L'\0')) {
        n--;
      }

      if ((n == 2) && (Buf[0] < 0x80) && (Buf[1] < 0x80)) {
        Country[0] = (CHAR)Buf[0];
        Country[1] = (CHAR)Buf[1];
        Country[2] = '\0';
      }
    }

    WdfRegistryClose (Key);
  }

  rtw_linux_set_ps_policy (RTW_PS_SRC_REG, PowerSave);
  (VOID)rtw_linux_set_pmf (Pmf);  // before the WiFiCx front end reports its capabilities
  rtw_linux_set_wfd (WifiDirect); // likewise
  RtwLog ("WifiDirect: %s%s", (WifiDirect == RTW_WFD_ON) ? "on" : "off", WfdGiven ? "" : " (default)");
  rtw_linux_set_wide (Bandwidth); // likewise
  RtwLog (
    "wide: 5 GHz station up to %u MHz%s",
    Bandwidth,
    BwBad ? " (Parameters\\Bandwidth is not 20, 40 or 80; ignored)" : BwGiven ? " (Parameters\\Bandwidth)" : " (default)"
    );
  Ret = rtw_linux_reg_set_user ((Country[0] != '\0') ? Country : NULL);
  if (Given && ((Country[0] == '\0') || (Ret == -EINVAL))) {
    RtwLog ("Parameters\\Country is not two letters; ignored");
  }
}

LONG
RtwProbe (
  _In_ PRTW_DEVICE  Dev
  )
{
  struct rtw_pci_info  Info;
  UCHAR                Cfg[0x30];
  NTSTATUS             Status;
  int                  Ret;

  if (Dev->Probed) {
    return -EBUSY;
  }

  if ((Dev->Bar == NULL) || !Dev->BusValid ||
      (Dev->Bus.GetBusData (Dev->Bus.Context, PCI_WHICHSPACE_CONFIG, Cfg, 0, sizeof (Cfg)) != sizeof (Cfg)))
  {
    return -ENODEV;
  }

  if (!mLinuxUp) {
    Ret = rtw_linux_init ();
    if (Ret != 0) {
      RtwLog ("rtw_linux_init failed: %d", Ret);
      return Ret;
    }

    mLinuxUp = TRUE;
  }

  Status = RtwOsAttach (Dev);
  if (!NT_SUCCESS (Status)) {
    return (Status == STATUS_INSUFFICIENT_RESOURCES) ? -ENOMEM : -ENODEV;
  }

  RtlZeroMemory (&Info, sizeof (Info));
  Info.os_dev           = Dev;
  Info.bar_va           = (void *)Dev->Bar;
  Info.bar_len          = Dev->BarLen;
  Info.vendor           = *(USHORT *)&Cfg[0x00];
  Info.device           = *(USHORT *)&Cfg[0x02];
  Info.revision         = Cfg[0x08];
  Info.subsystem_vendor = *(USHORT *)&Cfg[0x2C];
  Info.subsystem_device = *(USHORT *)&Cfg[0x2E];
  Info.irq              = Dev->IntRes.u.Interrupt.Vector;
  Ret = rtw_linux_probe (&Info);
  if (Ret != 0) {
    RtwOsDetach (Dev);
    return Ret;
  }

  Dev->Probed = TRUE;
  RtwApplySettings ();
  return 0;
}

VOID
RtwRemove (
  _In_ PRTW_DEVICE  Dev
  )
{
  if (!Dev->Probed) {
    return;
  }

  rtw_linux_stop ();
  rtw_linux_remove ();
  Dev->Probed = FALSE;
  RtwOsDetach (Dev);
}

VOID
RtwLinuxExit (
  VOID
  )
{
  if (mLinuxUp) {
    rtw_linux_exit ();
    mLinuxUp = FALSE;
  }
}
