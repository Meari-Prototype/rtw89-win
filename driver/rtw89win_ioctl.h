// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
/*
 * Diagnostic IOCTLs of rtw89win, opened as \\.\rtw89win (administrators and SYSTEM only).
 * tools\rtwdiag.ps1 uses the same numbers and layouts. Structures named struct rtw_* are defined in
 * compat\include\rtw\glue.h (same layout for the driver and the Linux side).
 */
#pragma once

#define RTW_IOCTL_TYPE  0x8852

/* In: ULONG64 From (byte position in the log stream; 0 = oldest kept).
   Out: RTW_LOG_READ_OUT followed by log text (UTF-8, '\n' separated). */
#define IOCTL_RTW_LOG_READ  CTL_CODE (RTW_IOCTL_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* In: ULONG Offset into BAR2 (4-byte aligned).  Out: ULONG value. */
#define IOCTL_RTW_READ32    CTL_CODE (RTW_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Start rtw89 on the device (power on, firmware download, efuse, register). Out: LONG result (0 or -errno). */
#define IOCTL_RTW_PROBE     CTL_CODE (RTW_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Stop rtw89 (rtw_linux_stop first) and release its interrupt and DMA memory. No buffers. */
#define IOCTL_RTW_REMOVE    CTL_CODE (RTW_IOCTL_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* On a probed device (rtw_linux_* in glue.h). The control IOCTLs (PROBE ... SCAN) run one at a time;
   LOG_READ, READ32 and BSS_LIST may run alongside them. */

/* drv_start + station interface; the chip then idles. No input. Out: LONG result. */
#define IOCTL_RTW_START     CTL_CODE (RTW_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Cancel scan/listening, remove the interface, drv_stop. No buffers. */
#define IOCTL_RTW_STOP      CTL_CODE (RTW_IOCTL_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Listen on a 20 MHz channel (receive beacons of all BSSs). In: ULONG Channel (0 = stop). Out: LONG result. */
#define IOCTL_RTW_MONITOR   CTL_CODE (RTW_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Firmware scan of all enabled channels; returns when it ends or times out.
   In: struct rtw_scan_param (12 bytes).  Out: struct rtw_scan_result (16 bytes). */
#define IOCTL_RTW_SCAN      CTL_CODE (RTW_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* The BSS table. No input. Out: struct rtw_bss_list (40 bytes) followed by count struct rtw_bss (64 bytes each),
   as many as fit. */
#define IOCTL_RTW_BSS_LIST  CTL_CODE (RTW_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* The mode of this start of the device and the WiFiCx front end's state. No input. Out: RTW_MODE_OUT.
   In WiFiCx mode the WiFiCx front end owns the device: PROBE, REMOVE, START, STOP, MONITOR and SCAN then
   return -EBUSY (LONG output where they have one) without touching it. */
#define IOCTL_RTW_MODE      CTL_CODE (RTW_IOCTL_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RTW_MODE_DIAGNOSTIC 0     // diagnostic mode (the Parameters key could not be opened)
#define RTW_MODE_WIFICX     1     // WiFiCx mode (every start unless the guard tripped)
#define RTW_MODE_GUARDED    2     // diagnostic mode because the crash loop guard tripped (driver.c)

typedef struct _RTW_MODE_OUT {
  ULONG    Mode;          // RTW_MODE_*: the mode of this start of the device
  ULONG    HwReady;       // probed and capabilities reported
  ULONG    AdapterPort;   // WDI port of the station adapter, 0xFFFFFFFF = no adapter
  ULONG    RadioOn;       // software radio state as WiFiCx last set it
  ULONG    Started;       // the mac layer is started (rtw_linux_start)
  ULONG    ScanPending;   // a WDI scan task is running
  ULONG    OsWdiVersion;  // WifiDeviceGetOsWdiVersion
  ULONG    Received;      // WiFiCx commands received
  ULONG    Completed;     // ... and completed
  ULONG    Unsupported;   // ... answered with STATUS_NOT_SUPPORTED
  ULONG    Scans;         // scan tasks completed (SCAN_COMPLETE sent)
  ULONG    LastScanBss;   // BSS entries indicated by the last scan
} RTW_MODE_OUT;           // 48 bytes

/* Diagnostic mode, on a started mac layer that listens on a 2.4 GHz channel without NO_IR (monitor 1..11):
   send 1-4 broadcast probe requests (wildcard SSID) and count the tx status and the probe responses to us. Every
   driver load may send 32 such frames at most. In: struct rtw_txprobe_param (12 bytes).
   Out: struct rtw_txprobe_result (40 bytes). */
#define IOCTL_RTW_TXPROBE      CTL_CODE (RTW_IOCTL_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* 0x80B is retired: it restarted the device through WdfDeviceFailedAttemptRestart, i.e. pci.sys re-enumerating the
   device, which leaves the RTL8852BE unable to take its firmware until the next power cycle. */

/* Both modes, any time: connection state and data path counters. No input.
   Out: struct rtw_link_stats (glue.h); an output buffer of at least 160 bytes is accepted and only min(buffer, sizeof)
   is filled, so an older rtwdiag.ps1 built for a shorter layout (160, 192 or 256 bytes) still works against a newer
   driver. */
#define IOCTL_RTW_LINK         CTL_CODE (RTW_IOCTL_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Both modes, while connected or listening (a channel context exists, so the chip is powered): have the
   firmware raise a CPU exception, so rtw89's SER runs a level-2 restart (mac layer recovers, reports the connection
   lost, Wlansvc reconnects). Out: LONG result (0 or -errno; -EBUSY while scanning or restarting). */
#define IOCTL_RTW_SER          CTL_CODE (RTW_IOCTL_TYPE, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Both modes, any time: regulatory domain, power save and Bluetooth coexistence state. No input.
   Out: struct rtw_radio_info (200 bytes, glue.h); with nothing probed only the power save policy is filled. */
#define IOCTL_RTW_RADIO        CTL_CODE (RTW_IOCTL_TYPE, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Both modes: rtw89's coexistence report (the text of Linux's debugfs btc_info, up to the buffer; 16 KB is
   plenty). Only while the chip is powered (connected, listening or scanning): it sends H2C and reads registers.
   No input. Out: LONG result (bytes of text, or -errno: -ENETDOWN chip powered off, -ENODEV not probed), then the
   text (not NUL terminated). */
#define IOCTL_RTW_BTC          CTL_CODE (RTW_IOCTL_TYPE, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Both modes, any time: the BSS table with the last beacon and probe response bodies (rtw_linux_bss_export).
   In: ULONG Cursor (0 = from the start). Out: ULONG NextCursor (128 = all done), ULONG Reserved, then records of
   struct rtw_bss_rec (32 bytes) each followed by its bodies and padding to 8 bytes, as many as fit. */
#define IOCTL_RTW_BSS_IES      CTL_CODE (RTW_IOCTL_TYPE, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Both modes, any time: the mobile hotspot's group owner. No input. Out: struct rtw_ap_stats (340 bytes, glue.h:
   the mac layer's GO, its clients and counters), then RTW_WFD_OUT (the WiFiCx front end's Wi-Fi Direct ports) when
   the output buffer has room for it. */
#define IOCTL_RTW_AP           CTL_CODE (RTW_IOCTL_TYPE, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RTW_WFD_OUT {
  ULONG    Wfd;           // Parameters\WifiDirect took effect: Wi-Fi Direct capabilities reported
  ULONG    WfdPort;       // WDI port of the Wi-Fi Direct device, 0xFFFFFFFF = none
  ULONG    RolePort;      // WDI port of the role adapter (the GO's), 0xFFFFFFFF = none
  ULONG    ApRunning;     // START_AP succeeded, not stopped since
  ULONG    ApPeers;       // clients the OS knows as associated
  ULONG    ApStarts;      // START_AP tasks since the device was added
  ULONG    Band;          // of the last START_AP: NL80211_BAND_*
  ULONG    Channel;
} RTW_WFD_OUT;            // 32 bytes

/* IOCTL_RTW_MODE appends this after RTW_MODE_OUT when the output buffer has room for it (an older rtwdiag.ps1
   asks for the 48 bytes only). WiFiCx mode only; zero in the diagnostic mode. */
#define RTW_WDI_IDS            256
typedef struct _RTW_WDI_COUNTS {
  ULONG    Received[RTW_WDI_IDS];      // requests per WDI message id
  ULONG    Unsupported[RTW_WDI_IDS];   // ... answered with STATUS_NOT_SUPPORTED
} RTW_WDI_COUNTS;                      // 2048 bytes

typedef struct _RTW_LOG_READ_OUT {
  ULONG64  Next;      // pass as From in the next call
  ULONG64  Dropped;   // bytes overwritten before they could be read
} RTW_LOG_READ_OUT;
