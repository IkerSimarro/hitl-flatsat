#!/bin/bash
#
# Fault injection: freezes the IMU simulator (SIGSTOP) briefly and then for longer, and checks the OBC
# counts isolated missed reads without declaring a fault, but declares one after 3 consecutive misses
# and reports the recovery (NCR-005). Known limitation: a frozen simulator blocks the bridge, so every
# device misses during the freeze (NCR-006, open). Runs inside the SIL environment:
#
#   sil/sil.sh tests/system/test_fault_persistence.sh
#
set -u
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
OBC_LOG=$SIL_LOG_DIR/obc.log

"$ROOT/firmware/build/obc" --umb-pty "$SIL_UMB_PTY" --can none > "$OBC_LOG" 2>&1 &
OBC=$!
trap 'kill $OBC 2> /dev/null' EXIT

imu() { pkill "-$1" -f "nos3-single-simulator .* generic-imu-sim"; }
faults() { grep -c "IMU failed" "$OBC_LOG"; }
recoveries() { grep -c "IMU recovered" "$OBC_LOG"; }

# Latest OBC_HK SENSOR_MISSES from the umbilical telemetry (port 9011 inside the SIL container)
misses()
{
    python3 - "$ROOT" <<'EOF'
import socket, sys
sys.path.insert(0, sys.argv[1] + "/ground")
import flatsat_icd as icd
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9011))
s.settimeout(3)
while True:
    name, _, f = icd.parse_telemetry(s.recv(4096))
    if name == "OBC_HK":
        print(f["SENSOR_MISSES"])
        break
EOF
}

PASS=0
FAIL=0
check()
{
    if [ "$1" = "1" ]; then echo "PASS $2"; PASS=$((PASS + 1)); else echo "FAIL $2"; FAIL=$((FAIL + 1)); fi
}

sleep 8 # link up, buses opened, a few clean acquisition cycles
m0=$(misses)
check "$([ "$m0" = "0" ] && echo 1)" "no misses in normal operation (SENSOR_MISSES $m0)"

# Short freeze: one or two missed reads, no fault event
imu STOP; sleep 1.5; imu CONT; sleep 3
m1=$(misses)
check "$([ "$(faults)" = "0" ] && [ "$m1" -ge 1 ] && echo 1)" \
    "1.5 s freeze: misses counted ($m0 -> $m1), no fault declared ($(faults) fault events)"

# Long freeze: fault declared once after 3 consecutive misses, then recovery. While the IMU sim is
# frozen the bridge is blocked too (NCR-006, open), so the other devices miss and the umbilical link
# drops; after it returns the OBC waits its 2 s settle time, hence the longer wait before checking.
imu STOP; sleep 6; imu CONT; sleep 8
m2=$(misses)
check "$([ "$(faults)" = "1" ] && [ "$(recoveries)" = "1" ] && echo 1)" \
    "6 s freeze: exactly one fault and one recovery event ($(faults) / $(recoveries)), misses $m1 -> $m2"

echo "OBC events:"
grep "EVENT" "$OBC_LOG" | sed 's/^/  /'
echo "---- $PASS passed, $FAIL failed ----"
[ $FAIL -eq 0 ]
