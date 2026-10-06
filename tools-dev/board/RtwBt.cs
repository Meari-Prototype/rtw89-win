// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Meari
// Bluetooth test helper, used by rtw89win-bt.ps1 in the logged-on user's session: LE
// advertisement count, classic discovery, pairing by address (accepting the numeric comparison on this side), an
// uncached SDP query of the RFCOMM services, unpairing, and the Windows radio list / state.
// Built with the in-box .NET Framework csc against Windows.Devices.winmd and Windows.Foundation.winmd only, so C# 5
// and no System.Runtime.WindowsRuntime (no AsTask): WinRT async operations are polled (Wait).
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using Windows.Foundation;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.Rfcomm;
using Windows.Devices.Enumeration;
using Windows.Devices.Radios;

public static class RtwBt {
  // Classic Bluetooth association endpoints (the Bluetooth device class GUID as protocol id).
  const string ClassicAqs = "System.Devices.Aep.ProtocolId:=\"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}\"";

  static T Wait<T> (IAsyncOperation<T> op, int timeoutMs) {
    int waited = 0;
    while (op.Status == AsyncStatus.Started) {
      if (waited >= timeoutMs) {
        op.Cancel();
        throw new TimeoutException("async operation still running after " + (timeoutMs / 1000) + " s (cancelled)");
      }
      Thread.Sleep(50);
      waited += 50;
    }
    if (op.Status != AsyncStatus.Completed)
      throw new Exception("async operation " + op.Status + (op.ErrorCode != null ? ": " + op.ErrorCode.Message : ""));
    return op.GetResults();
  }

  static string Addr (ulong a) {
    var sb = new StringBuilder();
    for (int i = 5; i >= 0; i--)
      sb.Append(((a >> (8 * i)) & 0xff).ToString("x2")).Append(i > 0 ? ":" : "");
    return sb.ToString();
  }

  public static ulong ParseAddr (string s) {
    return Convert.ToUInt64(s.Replace(":", "").Replace("-", ""), 16);
  }

  // Counts LE advertisements (active scanning) for the given time: devices, adverts, best RSSI, names.
  public static string LeCount (int seconds) {
    var sb = new StringBuilder();
    var count = new Dictionary<ulong, int>();
    var best = new Dictionary<ulong, short>();
    var names = new Dictionary<ulong, string>();
    int total = 0;
    string stopped = null;
    var w = new BluetoothLEAdvertisementWatcher();
    w.ScanningMode = BluetoothLEScanningMode.Active;
    w.Received += (s, e) => {
      lock (count) {
        ulong a = e.BluetoothAddress;
        int c;
        short r;
        total++;
        count.TryGetValue(a, out c);
        count[a] = c + 1;
        if (!best.TryGetValue(a, out r) || e.RawSignalStrengthInDBm > r)
          best[a] = e.RawSignalStrengthInDBm;
        string n = e.Advertisement.LocalName;
        if (!string.IsNullOrEmpty(n))
          names[a] = n;
      }
    };
    w.Stopped += (s, e) => { lock (count) stopped = e.Error.ToString(); };
    var t0 = DateTime.Now;
    w.Start();
    while ((DateTime.Now - t0).TotalSeconds < seconds && w.Status != BluetoothLEAdvertisementWatcherStatus.Aborted)
      Thread.Sleep(100);
    var status = w.Status;
    w.Stop();
    Thread.Sleep(200);
    lock (count) {
      sb.AppendLine("LE scan " + seconds + " s: watcher status " + status + (stopped != null ? ", stopped with " + stopped : "") +
        "; " + total + " advertisements from " + count.Count + " devices");
      var keys = new List<ulong>(count.Keys);
      keys.Sort((x, y) => count[y].CompareTo(count[x]));
      foreach (var a in keys) {
        string n;
        names.TryGetValue(a, out n);
        sb.AppendLine("  " + Addr(a) + "  " + count[a].ToString().PadLeft(4) + " adv  best " + best[a] + " dBm" +
          (n != null ? "  \"" + n + "\"" : ""));
      }
    }
    return sb.ToString();
  }

  // Classic inquiry through a DeviceWatcher on association endpoints (paired and unpaired) for the given time.
  // Devices Windows already knows (an earlier pairing attempt) are listed at once from its cache, so each device also
  // gets its updates: a signal strength or presence update during the run is a fresh inquiry result.
  public static string Discover (int seconds) {
    var sb = new StringBuilder();
    var lines = new Dictionary<string, string>();
    var updates = new Dictionary<string, int>();
    var fresh = new Dictionary<string, string>();
    string[] props = { "System.Devices.Aep.DeviceAddress", "System.Devices.Aep.SignalStrength", "System.Devices.Aep.IsPresent",
      "System.Devices.Aep.Bluetooth.LastSeenTime" };
    var w = DeviceInformation.CreateWatcher(ClassicAqs, props, DeviceInformationKind.AssociationEndpoint);
    double completed = -1;
    var t0 = DateTime.Now;
    w.Added += (s, d) => {
      lock (lines) {
        object addr, rssi, present, seen;
        d.Properties.TryGetValue("System.Devices.Aep.DeviceAddress", out addr);
        d.Properties.TryGetValue("System.Devices.Aep.SignalStrength", out rssi);
        d.Properties.TryGetValue("System.Devices.Aep.IsPresent", out present);
        d.Properties.TryGetValue("System.Devices.Aep.Bluetooth.LastSeenTime", out seen);
        lines[d.Id] = (addr ?? "?") + "  \"" + d.Name + "\"  paired " + d.Pairing.IsPaired + ", can pair " + d.Pairing.CanPair +
          (rssi != null ? ", " + rssi + " dBm" : "") + ", present " + (present ?? "?") +
          (seen != null ? ", last seen " + seen : "") + ", after " + (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " s";
        updates[d.Id] = 0;
      }
    };
    w.Updated += (s, u) => {
      lock (lines) {
        int c;
        object v;
        updates.TryGetValue(u.Id, out c);
        updates[u.Id] = c + 1;
        var parts = new List<string>();
        if (u.Properties.TryGetValue("System.Devices.Aep.SignalStrength", out v) && v != null)
          parts.Add(v + " dBm");
        if (u.Properties.TryGetValue("System.Devices.Aep.IsPresent", out v) && v != null)
          parts.Add("present " + v);
        if (u.Properties.TryGetValue("System.Devices.Aep.Bluetooth.LastSeenTime", out v) && v != null)
          parts.Add("last seen " + v);
        if (parts.Count > 0 && !fresh.ContainsKey(u.Id))
          fresh[u.Id] = "first update (" + string.Join(", ", parts.ToArray()) + ") after " +
            (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " s";
      }
    };
    w.EnumerationCompleted += (s, o) => { lock (lines) completed = (DateTime.Now - t0).TotalSeconds; };
    w.Start();
    while ((DateTime.Now - t0).TotalSeconds < seconds && w.Status != DeviceWatcherStatus.Aborted)
      Thread.Sleep(100);
    var status = w.Status;
    if (status == DeviceWatcherStatus.Started || status == DeviceWatcherStatus.EnumerationCompleted)
      w.Stop();
    lock (lines) {
      sb.AppendLine("classic discovery " + seconds + " s: watcher status " + status + ", " + lines.Count + " devices" +
        (completed >= 0 ? ", enumeration completed after " + completed.ToString("0.0") + " s" : ""));
      foreach (var kv in lines) {
        string f;
        fresh.TryGetValue(kv.Key, out f);
        sb.AppendLine("  " + kv.Value + ", " + updates[kv.Key] + " updates" + (f != null ? "; " + f : "; no fresh result"));
      }
    }
    return sb.ToString();
  }

  // Pairs by address (paging only), accepting the numeric comparison / confirmation on this side; the other side
  // confirms on its own screen.
  public static string Pair (ulong address, int timeoutSec) {
    var sb = new StringBuilder();
    var dev = Wait(BluetoothDevice.FromBluetoothAddressAsync(address), 30000);
    if (dev == null)
      return "no device object for " + Addr(address);
    var di = dev.DeviceInformation;
    sb.AppendLine("device " + Addr(address) + ": \"" + dev.Name + "\", class 0x" + dev.ClassOfDevice.RawValue.ToString("x6") +
      ", can pair " + di.Pairing.CanPair + ", paired " + di.Pairing.IsPaired);
    if (di.Pairing.IsPaired)
      return sb.Append("already paired").ToString();
    var c = di.Pairing.Custom;
    c.PairingRequested += (s, e) => {
      lock (sb) {
        sb.AppendLine("request: " + e.PairingKind + (string.IsNullOrEmpty(e.Pin) ? "" : ", code " + e.Pin));
        if (e.PairingKind == DevicePairingKinds.ConfirmOnly || e.PairingKind == DevicePairingKinds.ConfirmPinMatch ||
            e.PairingKind == DevicePairingKinds.DisplayPin)
          e.Accept();
        else
          sb.AppendLine("  (not handled: needs input on this side)");
      }
    };
    var t0 = DateTime.Now;
    var r = Wait(c.PairAsync(DevicePairingKinds.ConfirmOnly | DevicePairingKinds.ConfirmPinMatch | DevicePairingKinds.DisplayPin),
      timeoutSec * 1000);
    lock (sb)
      sb.AppendLine("status: " + r.Status + ", protection " + r.ProtectionLevelUsed + ", after " +
        (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " s");
    return sb.ToString();
  }

  // The device's RFCOMM services from a fresh SDP query (BluetoothCacheMode.Uncached: pages the device, so the
  // query really goes over the controller instead of being answered from Windows' cache).
  public static string Sdp (ulong address) {
    var sb = new StringBuilder();
    var dev = Wait(BluetoothDevice.FromBluetoothAddressAsync(address), 30000);
    if (dev == null)
      return "no device object for " + Addr(address);
    var t0 = DateTime.Now;
    var r = Wait(dev.GetRfcommServicesAsync(BluetoothCacheMode.Uncached), 60000);
    sb.AppendLine("SDP (uncached) " + Addr(address) + " \"" + dev.Name + "\": " + r.Error + ", " + r.Services.Count +
      " RFCOMM services after " + (DateTime.Now - t0).TotalSeconds.ToString("0.0") + " s");
    foreach (var s in r.Services)
      sb.AppendLine("  " + s.ServiceId.Uuid + "  \"" + s.ConnectionServiceName + "\"");
    return sb.ToString();
  }

  public static string Unpair (ulong address) {
    var dev = Wait(BluetoothDevice.FromBluetoothAddressAsync(address), 30000);
    if (dev == null)
      return "no device object for " + Addr(address);
    if (!dev.DeviceInformation.Pairing.IsPaired)
      return "device " + Addr(address) + " (\"" + dev.Name + "\") is not paired";
    var r = Wait(dev.DeviceInformation.Pairing.UnpairAsync(), 30000);
    return "unpair " + Addr(address) + " (\"" + dev.Name + "\"): " + r.Status;
  }

  // Lists the radios; with state "On" / "Off" sets the Bluetooth radio(s) to it first.
  public static string Radios (string state) {
    var sb = new StringBuilder();
    if (string.IsNullOrEmpty(state))
      state = null;
    sb.AppendLine("radio access: " + Wait(Radio.RequestAccessAsync(), 10000));
    var list = Wait(Radio.GetRadiosAsync(), 10000);
    foreach (var r in list) {
      if (state != null && r.Kind == RadioKind.Bluetooth) {
        var want = (RadioState)Enum.Parse(typeof(RadioState), state, true);
        sb.AppendLine("set " + r.Name + " " + want + ": " + Wait(r.SetStateAsync(want), 15000));
      }
      sb.AppendLine("  " + r.Kind + " \"" + r.Name + "\": " + r.State);
    }
    return sb.ToString();
  }
}
