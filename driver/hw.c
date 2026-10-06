// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Hardware access at every start: find and map BAR2, log PCI config space and the always-on SYS registers.
 * Register offsets are from Linux rtw89 v7.2 (reg.h); rtw89 reads the same SYS registers before
 * powering the MAC on (rtw89_read_chip_ver, rtw8852b_pwr_on_func), so reading them here is safe.
 */
#include "rtw89win.h"

#define R_AX_SYS_ISO_CTRL     0x0000
#define R_AX_SYS_PW_CTRL      0x0004
#define R_AX_SYS_CLK_CTRL     0x0008
#define R_AX_PLATFORM_ENABLE  0x0088
#define R_AX_SYS_CFG1         0x00F0    // [15:12] chip version (enum rtw89_cv)
#define R_AX_SYS_STATUS1      0x00F4
#define R_AX_IC_PWR_STATE     0x03F0    // [9:8] MAC power state (0 off, 1 on, 2 low power), read by reset_pwr_state

#define RTW_BAR_INDEX         2         // rtw89_pci_setup_mapping: bar_id = 2

static ULONG
CfgRead (
  _In_ PRTW_DEVICE  Dev,
  _Out_writes_bytes_ (Len) PVOID  Buf,
  _In_ ULONG        Offset,
  _In_ ULONG        Len
  )
{
  RtlZeroMemory (Buf, Len);
  if (!Dev->BusValid) {
    return 0;
  }

  return Dev->Bus.GetBusData (Dev->Bus.Context, PCI_WHICHSPACE_CONFIG, Buf, Offset, Len);
}

/* Logs the IDs, command/status, BARs and the PCIe link state; returns the BAR2 bus address (0 if unknown). */
static ULONG64
LogPciConfig (
  _In_ PRTW_DEVICE  Dev
  )
{
  UCHAR    Cfg[256];
  ULONG    Bar[6];
  ULONG64  Bar2;
  ULONG    Ext;
  ULONG    Hdr;
  UCHAR    Cap;
  ULONG    Guard;
  ULONG    i;

  if (CfgRead (Dev, Cfg, 0, sizeof (Cfg)) != sizeof (Cfg)) {
    RtwLog ("PCI config space not readable");
    return 0;
  }

  RtwLog (
    "PCI %04x:%04x subsys %04x:%04x rev %02x class %02x%02x%02x cmd %04x sts %04x",
    *(USHORT *)&Cfg[0x00],
    *(USHORT *)&Cfg[0x02],
    *(USHORT *)&Cfg[0x2C],
    *(USHORT *)&Cfg[0x2E],
    Cfg[0x08],
    Cfg[0x0B],
    Cfg[0x0A],
    Cfg[0x09],
    *(USHORT *)&Cfg[0x04],
    *(USHORT *)&Cfg[0x06]
    );

  RtlCopyMemory (Bar, &Cfg[0x10], sizeof (Bar));
  RtwLog (
    "BARs %08x %08x %08x %08x %08x %08x  int line %u pin %u",
    Bar[0], Bar[1], Bar[2], Bar[3], Bar[4], Bar[5], Cfg[0x3C], Cfg[0x3D]
    );

  Bar2 = Bar[RTW_BAR_INDEX] & ~0xFULL;
  if ((Bar[RTW_BAR_INDEX] & 0x7) == 0x4) {
    // 64-bit memory BAR
    Bar2 |= (ULONG64)Bar[RTW_BAR_INDEX + 1] << 32;
  }

  Cap   = Cfg[0x34] & 0xFC;
  Guard = 0;
  // capabilities live in 0x40..0xFF; stop at anything that would read past Cfg (PCIe cap reads up to +0x13)
  while ((Cap >= 0x40) && (Cap <= sizeof (Cfg) - 0x14) && (Guard++ < 48)) {
    if (Cfg[Cap] == 0x10) {
      // PCI Express capability: DevCap +04, DevCtl +08, LinkCap +0C, LinkCtl +10, LinkSts +12
      RtwLog (
        "PCIe cap @%02x: devcap %08x (FLR %u) devctl %04x linkcap %08x linkctl %04x (ASPM %u, CLKREQ %u) "
        "linksts %04x (gen %u x%u)",
        Cap,
        *(ULONG *)&Cfg[Cap + 0x04],
        (*(ULONG *)&Cfg[Cap + 0x04] >> 28) & 0x1,
        *(USHORT *)&Cfg[Cap + 0x08],
        *(ULONG *)&Cfg[Cap + 0x0C],
        *(USHORT *)&Cfg[Cap + 0x10],
        *(USHORT *)&Cfg[Cap + 0x10] & 0x3,
        (*(USHORT *)&Cfg[Cap + 0x10] >> 8) & 0x1,
        *(USHORT *)&Cfg[Cap + 0x12],
        *(USHORT *)&Cfg[Cap + 0x12] & 0xF,
        (*(USHORT *)&Cfg[Cap + 0x12] >> 4) & 0x3F
        );
    } else {
      RtwLog ("cap %02x @%02x", Cfg[Cap], Cap);
    }

    Cap = Cfg[Cap + 1] & 0xFC;
  }

  // Extended capabilities (L1 PM substates = 0x1E, LTR = 0x18, ...): IDs and offsets only.
  Ext   = 0x100;
  Guard = 0;
  while ((Ext >= 0x100) && (Ext < 0x1000) && (Guard++ < 64)) {
    if (CfgRead (Dev, &Hdr, Ext, sizeof (Hdr)) != sizeof (Hdr)) {
      break;
    }

    if ((Hdr == 0) || (Hdr == 0xFFFFFFFF)) {
      break;
    }

    if ((Hdr & 0xFFFF) == 0x1E) {
      ULONG  L1ss[3];

      CfgRead (Dev, L1ss, Ext + 4, sizeof (L1ss));
      RtwLog ("ext cap 1e (L1SS) @%03x: cap %08x ctl1 %08x ctl2 %08x", Ext, L1ss[0], L1ss[1], L1ss[2]);
    } else {
      RtwLog ("ext cap %04x v%u @%03x", Hdr & 0xFFFF, (Hdr >> 16) & 0xF, Ext);
    }

    Ext = Hdr >> 20;
  }

  for (i = 0; i < 6; i++) {
    if (Bar[i] == 0xFFFFFFFF) {
      RtwLog ("BAR%u reads all ones: device not responding?", i);
      break;
    }
  }

  return Bar2;
}

NTSTATUS
RtwHwMap (
  _In_ PRTW_DEVICE   Dev,
  _In_ WDFCMRESLIST  Raw,
  _In_ WDFCMRESLIST  Translated
  )
{
  PCM_PARTIAL_RESOURCE_DESCRIPTOR  R;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR  T;
  PHYSICAL_ADDRESS                 Start;
  PHYSICAL_ADDRESS                 RawStart;
  PHYSICAL_ADDRESS                 First;
  ULONG64                          Bar2;
  ULONG64                          Len;
  ULONG64                          FirstLen;
  ULONG                            Count;
  ULONG                            i;
  PVOID                            Va;
  KIRQL                            Irql;

  Bar2 = LogPciConfig (Dev);

  First.QuadPart = 0;
  FirstLen       = 0;
  Count          = WdfCmResourceListGetCount (Translated);
  for (i = 0; i < Count; i++) {
    R = WdfCmResourceListGetDescriptor (Raw, i);
    T = WdfCmResourceListGetDescriptor (Translated, i);
    if ((R == NULL) || (T == NULL)) {
      continue;
    }

    switch (T->Type) {
      case CmResourceTypeMemory:
      case CmResourceTypeMemoryLarge:
        Len = RtlCmDecodeMemIoResource (T, (PULONGLONG)&Start.QuadPart);
        RtlCmDecodeMemIoResource (R, (PULONGLONG)&RawStart.QuadPart);
        RtwLog ("res %u: mem cpu 0x%llx bus 0x%llx len 0x%llx", i, Start.QuadPart, RawStart.QuadPart, Len);
        if (FirstLen == 0) {
          First    = Start;
          FirstLen = Len;
        }

        if ((Bar2 != 0) && ((ULONG64)RawStart.QuadPart == Bar2)) {
          Dev->BarPhys = Start;
          Dev->BarLen  = (SIZE_T)Len;
        }

        break;

      case CmResourceTypePort:
        RtwLog ("res %u: port 0x%llx len 0x%x", i, T->u.Port.Start.QuadPart, T->u.Port.Length);
        break;

      case CmResourceTypeInterrupt:
        if (!(T->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) && !Dev->IntResValid) {
          Dev->IntRes      = *T;
          Dev->IntResValid = TRUE;
        }

        RtwLog (
          "res %u: interrupt %s vector %u level %u affinity 0x%llx flags 0x%x (connected only while probed)",
          i,
          (T->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ? "MSI" : "line",
          T->u.Interrupt.Vector,
          T->u.Interrupt.Level,
          (ULONG64)T->u.Interrupt.Affinity,
          T->Flags
          );
        break;

      default:
        RtwLog ("res %u: type %u", i, T->Type);
        break;
    }
  }

  if (Dev->BarLen == 0) {
    if (FirstLen == 0) {
      RtwLog ("no memory resource");
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RtwLog ("BAR2 bus address 0x%llx not among the resources; using the first memory resource", Bar2);
    Dev->BarPhys = First;
    Dev->BarLen  = (SIZE_T)FirstLen;
  }

  Va = MmMapIoSpaceEx (Dev->BarPhys, Dev->BarLen, PAGE_READWRITE | PAGE_NOCACHE);
  if (Va == NULL) {
    RtwLog ("MmMapIoSpaceEx 0x%llx len 0x%llx failed", Dev->BarPhys.QuadPart, (ULONG64)Dev->BarLen);
    Dev->BarLen = 0;
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  KeAcquireSpinLock (&Dev->BarLock, &Irql);
  Dev->Bar = Va;
  KeReleaseSpinLock (&Dev->BarLock, Irql);

  RtwLog ("BAR2 0x%llx len 0x%llx mapped", Dev->BarPhys.QuadPart, (ULONG64)Dev->BarLen);
  return STATUS_SUCCESS;
}

VOID
RtwHwUnmap (
  _In_ PRTW_DEVICE  Dev
  )
{
  PVOID   Va;
  SIZE_T  Len;
  KIRQL   Irql;

  Dev->IntResValid = FALSE;
  // IOCTL_RTW_READ32 may run at any time (control device): take the mapping away under BarLock first
  KeAcquireSpinLock (&Dev->BarLock, &Irql);
  Va          = (PVOID)Dev->Bar;
  Len         = Dev->BarLen;
  Dev->Bar    = NULL;
  Dev->BarLen = 0;
  KeReleaseSpinLock (&Dev->BarLock, Irql);
  if (Va != NULL) {
    MmUnmapIoSpace (Va, Len);
    RtwLog ("BAR2 unmapped");
  }
}

ULONG
RtwRead32 (
  _In_ PRTW_DEVICE  Dev,
  _In_ ULONG        Offset
  )
{
  return READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Bar + Offset));
}

NTSTATUS
RtwHwRead32Checked (
  _In_ PRTW_DEVICE  Dev,
  _In_ ULONG        Offset,
  _Out_ ULONG       *Value
  )
{
  NTSTATUS  Status;
  KIRQL     Irql;

  *Value = 0;
  Status = STATUS_INVALID_PARAMETER;
  KeAcquireSpinLock (&Dev->BarLock, &Irql);
  if ((Dev->Bar != NULL) && ((Offset & 3) == 0) && ((SIZE_T)Offset + 4 <= Dev->BarLen)) {
    *Value = RtwRead32 (Dev, Offset);
    Status = STATUS_SUCCESS;
  }

  KeReleaseSpinLock (&Dev->BarLock, Irql);
  return Status;
}

VOID
RtwHwLogChipInfo (
  _In_ PRTW_DEVICE  Dev
  )
{
  static const PCSTR  CvName[] = { "CAV", "CBV", "CCV", "CDV", "CEV", "CFV" };
  ULONG               Cfg1;
  ULONG               Sts1;
  ULONG               Cv;

  Cfg1 = RtwRead32 (Dev, R_AX_SYS_CFG1);
  Sts1 = RtwRead32 (Dev, R_AX_SYS_STATUS1);
  RtwLog (
    "SYS ISO_CTRL %08x PW_CTRL %08x CLK_CTRL %08x PLATFORM_EN %08x CFG1 %08x STATUS1 %08x",
    RtwRead32 (Dev, R_AX_SYS_ISO_CTRL),
    RtwRead32 (Dev, R_AX_SYS_PW_CTRL),
    RtwRead32 (Dev, R_AX_SYS_CLK_CTRL),
    RtwRead32 (Dev, R_AX_PLATFORM_ENABLE),
    Cfg1,
    Sts1
    );

  if (Cfg1 == 0xFFFFFFFF) {
    RtwLog ("SYS_CFG1 reads all ones: device not responding");
    return;
  }

  Cv = (Cfg1 >> 12) & 0xF;
  RtwLog (
    "chip version %u (%s), MAC power state %u (IC_PWR_STATE %08x)",
    Cv,
    (Cv < ARRAYSIZE (CvName)) ? CvName[Cv] : "?",
    (RtwRead32 (Dev, R_AX_IC_PWR_STATE) >> 8) & 0x3,
    RtwRead32 (Dev, R_AX_IC_PWR_STATE)
    );
}
