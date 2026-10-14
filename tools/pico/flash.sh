#!/bin/bash
#
# Flashes a Raspberry Pi Pico running FlatSat firmware from WSL, without touching its BOOTSEL button:
#   1. restarts it in its USB bootloader (opening its serial port at 1200 baud, from WSL if it is attached
#      there, otherwise from Windows)
#   2. copies the .uf2 onto the bootloader's drive (RP2350) with Windows
#   3. attaches the restarted Pico to WSL again (usbipd), so /dev/ttyACM0 is back
#
#   tools/pico/flash.sh [firmware/build-pico/obc.uf2]
#
# Needs usbipd-win, with the FlatSat firmware's USB ID shared once (as administrator):
#   usbipd bind --hardware-id 2e8a:0009
# A Pico that isn't running FlatSat firmware (or a new one) must be put in the bootloader by hand: hold
# BOOTSEL while plugging it in. The script then just copies.
#
set -u
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
UF2=${1:-$ROOT/firmware/build-pico/obc.uf2}
USBIPD="& 'C:\\Program Files\\usbipd-win\\usbipd.exe'"
ps() { timeout 120 powershell.exe -NoProfile -Command "$1" 2>&1 | tr -d '\r' | sed '/^$/d'; }

[ -f "$UF2" ] || { echo "no $UF2: build it first (tools/pico/build.sh)" >&2; exit 1; }
WIN_UF2=$(wslpath -w "$UF2")

drive() { ps "(Get-CimInstance Win32_LogicalDisk | Where-Object VolumeName -eq 'RP2350').DeviceID"; }

if [ -z "$(drive)" ]; then
    echo "[flash] restarting the Pico in its bootloader"
    if [ -e /dev/ttyACM0 ]; then
        stty -F /dev/ttyACM0 1200 2> /dev/null
    else
        ps "\$p = Get-CimInstance Win32_PnPEntity | Where-Object { \$_.PNPDeviceID -match 'VID_2E8A&PID_0009&MI_00' };
            if (\$p.Name -match '\\((COM\\d+)\\)') { try { \$s = New-Object System.IO.Ports.SerialPort \$Matches[1],1200;
            \$s.Open(); Start-Sleep -Milliseconds 300; \$s.Close() } catch {} } else { 'no FlatSat Pico found' }"
    fi
    for _ in $(seq 1 40); do
        [ -n "$(drive)" ] && break
        sleep 0.5
    done
fi
D=$(drive)
[ -n "$D" ] || { echo "[flash] no RP2350 drive: hold BOOTSEL while plugging the Pico in, then run this again" >&2; exit 1; }

echo "[flash] copying $(basename "$UF2") to $D"
ps "Copy-Item -LiteralPath '$WIN_UF2' -Destination '$D\\' -ErrorAction Stop" || exit 1

echo "[flash] attaching the restarted Pico to WSL"
for _ in $(seq 1 40); do
    ps "$USBIPD list" | grep -q "2e8a:0009" && break
    sleep 0.5
done
ps "$USBIPD attach --wsl --hardware-id 2e8a:0009" | grep -v "^usbipd: info"
for _ in $(seq 1 20); do
    [ -e /dev/ttyACM0 ] && { echo "[flash] done: /dev/ttyACM0"; exit 0; }
    sleep 0.5
done
echo "[flash] the Pico didn't come back as /dev/ttyACM0 (is its firmware a FlatSat one?)" >&2
exit 1
