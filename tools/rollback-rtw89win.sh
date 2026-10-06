#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Meari
# Run as root on another (Linux) system of the same board when Windows no longer boots after installing rtw89win
# (bugcheck while starting the Wi-Fi card): renames rtw89win.sys in the Windows driver store, so Windows cannot load
# it and the card just stays without a working driver. Nothing else is changed. Then boot Windows and run
# uninstall-rtw89win.ps1.
# (Full undo of the install: restore the SYSTEM hive that install-rtw89win.ps1 saved in
# <WorkDir>\backup\rtw89win-<time>\SYSTEM.hiv, default WorkDir %ProgramData%\rtw89win, over
# Windows\System32\config\SYSTEM of the offline Windows.)
# The kernel needs the ntfs3 driver.
# Usage: sh rollback-rtw89win.sh <Windows partition, e.g. /dev/nvme0n1p3>
set -eu
if [ $# -ne 1 ]; then
    echo "usage: sh $0 <Windows partition>" >&2
    echo "NTFS partitions:" >&2
    lsblk -l -o NAME,FSTYPE,LABEL,SIZE,MOUNTPOINT | awk 'NR == 1 || $2 == "ntfs"' >&2 || true
    exit 2
fi
DEV=$1
[ -b "$DEV" ] || { echo "$DEV is not a block device" >&2; exit 2; }
WIN=$(findmnt -n -o TARGET "$DEV" || true)
if [ -z "$WIN" ]; then
    WIN=/mnt/win-fix
    mkdir -p "$WIN"
    mount -t ntfs3 "$DEV" "$WIN"
    trap 'umount "$WIN" 2>/dev/null || true' EXIT
fi
TS=$(date +%Y%m%d-%H%M%S)
found=0
for f in "$WIN"/Windows/System32/DriverStore/FileRepository/rtw89win.inf_arm64_*/rtw89win.sys; do
    [ -f "$f" ] || continue
    mv "$f" "$f.disabled-$TS"
    echo "disabled $f"
    found=1
done
sync
[ "$found" = 1 ] || echo "no rtw89win.sys in $WIN/Windows/System32/DriverStore/FileRepository"
