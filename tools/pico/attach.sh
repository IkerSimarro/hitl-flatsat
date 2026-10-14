#!/bin/bash
#
# Attaches the Pico running FlatSat firmware (USB ID 2e8a:0009) to WSL, so it appears as /dev/ttyACM0.
#
#   tools/pico/attach.sh                 attach once (waits up to 10 s for /dev/ttyACM0)
#   tools/pico/attach.sh -- CMD [ARGS]   attach, then run CMD while keeping the Pico attached: it re-attaches
#                                        whenever the Pico restarts (its USB port disappears and comes back)
#
# Needs usbipd-win on Windows, with the ID shared once, as administrator: usbipd bind --hardware-id 2e8a:0009
#
USBIPD="/mnt/c/Program Files/usbipd-win/usbipd.exe"
ID=2e8a:0009

[ -x "$USBIPD" ] || { echo "[pico] usbipd-win not installed (winget install --exact dorssel.usbipd-win)" >&2; exit 1; }

if [ ! -e /dev/ttyACM0 ]; then
    "$USBIPD" attach --wsl --hardware-id $ID 2>&1 | tr -d '\r' | grep -v "^usbipd: info"
    for _ in $(seq 1 20); do
        [ -e /dev/ttyACM0 ] && break
        sleep 0.5
    done
fi
[ -e /dev/ttyACM0 ] || { echo "[pico] no Pico at /dev/ttyACM0: is it plugged in and running FlatSat firmware?" >&2; exit 1; }

[ "${1:-}" = "--" ] || exit 0
shift
"$USBIPD" attach --wsl --hardware-id $ID --auto-attach > /dev/null 2>&1 &
keeper=$!
"$@"
rc=$?
kill $keeper 2> /dev/null
exit $rc
