Developer test scripts for the target device (tools-dev\board\)

These scripts are for development and for fault isolation. You do not need them to install or use the drivers.
For the test sequence, see docs\en\porting.md (section "Test procedure").

How to run a script
1. Copy the scripts to one directory on the target device.
2. Open PowerShell 7 as administrator. An ssh session is also possible.
3. Type: pwsh -NoProfile -File <script> [parameters]

The -Diag parameter
Six scripts call rtwdiag.ps1 of the Wi-Fi driver package: rtw89win-connect.ps1, rtw89win-guardsim.ps1,
rtw89win-restartset.ps1, rtw89win-scanrestart.ps1, rtw89win-serconn.ps1, and rtw89win-serdiag.ps1.
To specify the location of rtwdiag.ps1, use -Diag <path>.
Without -Diag, the script uses the environment variable RTW89WIN_DIAG.
Without the variable, the script uses rtwdiag.ps1 in its own directory.

Private data in the output
The output of many scripts contains network names (SSID), MAC addresses, or Bluetooth addresses.
Mask this data before you share the output.

"Changes state: no" means that the script only reads data.
"Changes state: yes" means that the script changes the device, the driver settings, or Windows.


Wi-Fi: networks and connections

rtw89win-wlanlist.ps1
  Parameters: none.
  Changes state: no.
  Shows the network list and the BSS list of Windows (wlanapi). The output is similar to
  "netsh wlan show networks mode=bssid". When Windows location services are off, the result is
  "WlanGetNetworkBssList 5" (access denied).

rtw89win-profile.ps1 -Ssid <SSID> -Key <passphrase> [-Auth WPA2PSK|WPA3SAE] [-Interface WLAN]
rtw89win-profile.ps1 -Ssid <SSID> -Delete
  Changes state: yes (a WLAN profile).
  Adds a WPA2-Personal (default) or WPA3-Personal profile for one SSID. The profile name is the SSID.
  The connection mode is manual, so Windows does not connect by itself.
  The script writes the profile XML to %TEMP%. After "netsh wlan add profile", it deletes the file at once.
  -Delete removes the profile. Do not write the passphrase into a file.

rtw89win-connect.ps1 -Ssid <SSID> [-TimeoutSec 60] [-Prefix <start of the IPv4 address>] [-Watch] [-Disconnect]
                     [-Interface WLAN] [-Diag <path>]
  Changes state: yes (connects or disconnects).
  Starts "netsh wlan connect" for the profile of that SSID. Then it waits for one of these events:
  the adapter gets an IPv4 address, WLAN-AutoConfig reports a failed connection (event 8002), or the time-out.
  With -Prefix, only an address that starts with this prefix is a success. Without -Prefix, each address
  except 169.254.x.x is a success.
  The output has the time line, the WLAN-AutoConfig events, the driver log of the attempt, and "rtwdiag.ps1 link".
  -Watch does not start a connection. It only monitors (for example, after a connection from the taskbar).
  -Disconnect starts "netsh wlan disconnect" and waits until the adapter is down.

rtw89win-allowssid.ps1 [-Ssid <SSID>] [-Clear]
  Changes state: yes (values of this driver only).
  Sets Parameters\Allow\AllowConnect = 1 and, with -Ssid, AllowSsid. Then only connection requests for that
  SSID go to the mac layer. -Clear deletes the two values.
  The Allow key is volatile: a restart of the computer deletes the values.
  Without the values, all connection requests go through. Use this script only to limit a test to one SSID.

Wi-Fi: device restart and crash loop guard

rtw89win-disen.ps1
  Parameters: none.
  Changes state: yes.
  Disables the network adapter of rtw89win, then enables it again. Other adapters do not change.
  Shows the time and the device state after each step.

rtw89win-restartset.ps1 [-Set <Name>=<Value>,...] [-Diag <path>]
  Changes state: yes (values of this driver, device restart).
  Sets DWORD values under the Parameters key of this driver. Example: -Set ArmRunning=1,BootAttempts=1
  Then it restarts the device (pnputil /restart-device) and waits for the result of the new start.
  The output has the log of the new start, the saved log of the previous session, and the guard values.
  To start diagnostic mode manually, use -Set BootAttempts=2.

rtw89win-guardsim.ps1 [-Crash] [-Diag <path>]
  Changes state: yes (values of this driver, device restart).
  Restarts the device. With -Crash, the script sets ArmRunning = 1 again after the old session clears it.
  The next start then sees a session that did not end in order. Two such sessions in sequence trip the guard,
  and the driver starts in diagnostic mode.
  The output has the log of the new start, the saved log, the mode, and the guard values.
  To return to WiFiCx mode, use "rtwdiag.ps1 arm".

rtw89win-scanrestart.ps1 [-Diag <path>]
  Changes state: yes (device restart).
  Restarts the device with "rtwdiag.ps1 arm" and waits for the WiFiCx adapter. When a scan starts, the script
  restarts the device again. Then it shows the saved log.

Wi-Fi: system error recovery (SER)

rtw89win-serconn.ps1 [-Prefix <start of the IPv4 address>] [-ReconnectSec 90] [-Diag <path>]
  Changes state: yes (the connection stops, and Wlansvc connects again).
  Use it in WiFiCx mode when the station is connected. The script starts one SER ("rtwdiag.ps1 ser").
  Then it monitors the end of the SER, the adapter status, and the new connection of Wlansvc.
  All waits have a time limit. -Prefix has the same function as in rtw89win-connect.ps1.
  The output has the driver log, the link counters, the WLAN-AutoConfig events, the System warnings and errors,
  and new minidumps.

rtw89win-serdiag.ps1 [-Channel 6] [-Diag <path>]
  Changes state: yes (starts and stops rtw89).
  Use it only in diagnostic mode, with no connection. The script does these steps: probe, start, monitor on
  the channel, SER, scan, remove. The output has the driver log, the System warnings and errors, and new
  minidumps.

Mobile hotspot

rtw89win-hotspot.ps1 -Action state|configure|start|stop|passphrase [-Ssid <name>]
                     [-Band TwoPointFourGigahertz|FiveGigahertz|Auto] [-TimeoutSec 60] [-Here]
  Changes state: yes for configure, start, and stop. No for state and passphrase.
  Controls the mobile hotspot of Windows (NetworkOperatorTetheringManager) on the wired uplink.
  state: shows the tethering capability, the operational state, the clients, and the configuration.
  configure: sets the SSID and the band. The script keeps the passphrase that Windows made.
  start, stop: starts or stops the hotspot.
  passphrase: writes the passphrase to the console only. This action is possible only with -Here.
  The script does the WinRT calls in the desktop session of the user who is logged on. For this, it makes
  a temporary scheduled task and deletes it after the action. The task writes rtw89win-hotspot.log next to
  the script. The log does not contain the passphrase. -Here does the action in the current session.
  After a device restart or a SER, Windows does not start the hotspot again. Use -Action start.

Bluetooth (the patch loader is in bt\rtkbtfw\)

rtw89win-btquery.ps1 [-UsbId VID_13D3&PID_3572] [-MaxSystemEvents 40000]
  Changes state: no.
  Shows the Bluetooth part of the module: devices, driver package, device properties, local radio data
  (HCI and LMP version and subversion), filters, services, Bluetooth events, and the Realtek Bluetooth
  packages. It does not read BTHPORT\Parameters\Keys. The output contains the address of the local radio.

rtw89win-bt.ps1 -Action le|discover|pair|unpair|sdp|radios [-Seconds <n>] [-Address <address>]
                [-State On|Off] [-PairTimeoutSec 60] [-Here]
  Changes state: yes for pair, unpair, and -State. No for the other actions.
  Put RtwBt.cs in the same directory as this script.
  le: counts the LE advertisements and the devices (default 20 s).
  discover: does a classic inquiry and shows the devices (default 30 s). Windows also shows devices from
  its cache. Only a line with "first update" is a new result of this inquiry.
  pair: pairs the device with this address. The script accepts the numeric comparison on this side.
  Confirm it on the other device.
  unpair: removes the pairing of the device with this address.
  sdp: does a new SDP query for the RFCOMM services of the device. This query pages the device.
  radios: shows the radio list of Windows. -State On or -State Off sets the Bluetooth radio.
  The script does the action in the desktop session of the user who is logged on, because WinRT pairing
  from an ssh session gives AccessDenied. For this, it makes a temporary scheduled task and deletes it
  after the action. The task compiles RtwBt.cs one time with the csc of the .NET Framework in Windows.
  It writes rtw89win-bt.log next to the script. -Here does the action in the current session.

RtwBt.cs
  The C# part of rtw89win-bt.ps1 (WinRT Bluetooth calls). Do not run it alone.
