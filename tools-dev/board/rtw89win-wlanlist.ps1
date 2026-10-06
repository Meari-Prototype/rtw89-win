# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Read-only: the OS's WLAN BSS list through wlanapi (WlanGetNetworkBssList), like netsh wlan show networks mode=bssid.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public static class WlanRo {
  [DllImport("wlanapi.dll")] static extern uint WlanOpenHandle(uint ver, IntPtr r, out uint neg, out IntPtr h);
  [DllImport("wlanapi.dll")] static extern uint WlanCloseHandle(IntPtr h, IntPtr r);
  [DllImport("wlanapi.dll")] static extern uint WlanEnumInterfaces(IntPtr h, IntPtr r, out IntPtr list);
  [DllImport("wlanapi.dll")] static extern uint WlanGetNetworkBssList(IntPtr h, ref Guid g, IntPtr ssid, int bssType, bool sec, IntPtr r, out IntPtr list);
  [DllImport("wlanapi.dll")] static extern uint WlanGetAvailableNetworkList(IntPtr h, ref Guid g, uint flags, IntPtr r, out IntPtr list);
  [DllImport("wlanapi.dll")] static extern void WlanFreeMemory(IntPtr p);
  public static List<string> Bss() {
    var res = new List<string>();
    IntPtr h; uint neg;
    uint e = WlanOpenHandle(2, IntPtr.Zero, out neg, out h);
    if (e != 0) { res.Add("WlanOpenHandle " + e); return res; }
    IntPtr il;
    e = WlanEnumInterfaces(h, IntPtr.Zero, out il);
    if (e != 0) { res.Add("WlanEnumInterfaces " + e); WlanCloseHandle(h, IntPtr.Zero); return res; }
    int n = Marshal.ReadInt32(il);
    for (int i = 0; i < n; i++) {
      IntPtr ii = il + 8 + i * 532;               // WLAN_INTERFACE_INFO: GUID(16) + WCHAR[256] + state(4)
      Guid g = (Guid)Marshal.PtrToStructure(ii, typeof(Guid));
      string desc = Marshal.PtrToStringUni(ii + 16);
      res.Add("interface " + desc + " " + g);
      IntPtr nl;
      e = WlanGetAvailableNetworkList(h, ref g, 0, IntPtr.Zero, out nl);
      if (e != 0) res.Add("WlanGetAvailableNetworkList " + e);
      else {
        int nn = Marshal.ReadInt32(nl);
        for (int j = 0; j < nn; j++) {
          IntPtr a = nl + 8 + j * 628;            // WLAN_AVAILABLE_NETWORK
          int sl = Marshal.ReadInt32(a + 512);
          byte[] s = new byte[Math.Min(sl, 32)]; Marshal.Copy(a + 516, s, 0, s.Length);
          int nbss = Marshal.ReadInt32(a + 556);
          int quality = Marshal.ReadInt32(a + 600);
          int secEn = Marshal.ReadInt32(a + 604);
          int auth = Marshal.ReadInt32(a + 608); int ciph = Marshal.ReadInt32(a + 612);
          res.Add(string.Format("net ssid={0} bss={1} quality={2} security={3} auth={4} cipher={5}", System.Text.Encoding.UTF8.GetString(s), nbss, quality, secEn, auth, ciph));
        }
        WlanFreeMemory(nl);
      }
      IntPtr bl;
      e = WlanGetNetworkBssList(h, ref g, IntPtr.Zero, 3, false, IntPtr.Zero, out bl);
      if (e != 0) { res.Add("WlanGetNetworkBssList " + e); continue; }
      int nb = Marshal.ReadInt32(bl + 4);
      for (int j = 0; j < nb; j++) {
        IntPtr b = bl + 8 + j * 360;              // WLAN_BSS_ENTRY
        int sl = Marshal.ReadInt32(b);
        byte[] s = new byte[Math.Min(sl, 32)]; Marshal.Copy(b + 4, s, 0, s.Length);
        byte[] mac = new byte[6]; Marshal.Copy(b + 40, mac, 0, 6);
        int phy = Marshal.ReadInt32(b + 52);
        int rssi = Marshal.ReadInt32(b + 56);
        int lq = Marshal.ReadInt32(b + 60);
        int freq = Marshal.ReadInt32(b + 72);
        res.Add(string.Format("bss {0} ssid={1} rssi={2} lq={3} freq={4} phy={5}", BitConverter.ToString(mac).Replace('-', ':').ToLower(), System.Text.Encoding.UTF8.GetString(s), rssi, lq, freq, phy));
      }
      WlanFreeMemory(bl);
    }
    WlanFreeMemory(il);
    WlanCloseHandle(h, IntPtr.Zero);
    return res;
  }
}
'@
[WlanRo]::Bss()
