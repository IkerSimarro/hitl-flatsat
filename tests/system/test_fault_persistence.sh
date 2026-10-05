#!/bin/bash
#
# Fault injection: disables the IMU simulator through NOS3's command bus, briefly and then for longer
# (it stays connected to 42 but stops answering as a device), and checks the OBC
# counts isolated missed reads without declaring a fault, but declares one after about 2.5 s of misses
# and reports the recovery (NCR-005), and that the freeze affects only the IMU: the bridge's per-bus
# workers keep every other device and the umbilical link running (NCR-006). Runs inside the SIL
# environment:
#
#   sil/sil.sh tests/system/test_fault_persistence.sh
#
set -u
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
OBC_LOG=$SIL_LOG_DIR/obc.log

"$ROOT/sil/start_nodes.sh" > /dev/null || exit 1
"$ROOT/firmware/build/obc" --umb-pty "$SIL_UMB_PTY" > "$OBC_LOG" 2>&1 &
OBC=$!
trap 'kill $OBC 2> /dev/null' EXIT

imu() { python3 "$ROOT/sil/sim_cmd.py" imu-command "$1"; }
faults() { grep -c "IMU failed" "$OBC_LOG"; }
all_faults() { grep -c " failed: " "$OBC_LOG"; }
link_drops() { grep -c "link down" "$OBC_LOG"; }
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

sleep 2 # umbilical link up
# NOS3's 2.8 deg/s deployment tip-off would start automatic detumbling; this test needs a quiet spacecraft
python3 "$ROOT/sil/obc_cmd.py" OBC_SET_AUTO_MODES STATE=0
sleep 6 # buses opened, a few clean acquisition cycles
m0=$(misses)
check "$([ "$m0" = "0" ] && echo 1)" "no misses in normal operation (SENSOR_MISSES $m0)"

# Short outage: about 7 IMU reads missed (the ADCS reads it at 5 Hz), nothing else, no fault event
imu DISABLE; sleep 1.5; imu ENABLE; sleep 3
m1=$(misses)
check "$([ "$(faults)" = "0" ] && [ "$m1" -ge 5 ] && [ "$m1" -le 10 ] && echo 1)" \
    "1.5 s outage: $m1 IMU reads missed (expected 5-10), no fault declared ($(faults) fault events)"

# Long outage: the IMU is declared failed after 12 consecutive misses (2.4 s) and recovers afterwards
imu DISABLE; sleep 6; imu ENABLE; sleep 4
m2=$(misses)
check "$([ "$(faults)" = "1" ] && [ "$(recoveries)" = "1" ] && echo 1)" \
    "6 s outage: one IMU fault and one IMU recovery event ($(faults) / $(recoveries)), misses $m1 -> $m2"

# Isolation (NCR-006): no other device faulted and the umbilical stayed up throughout
check "$([ "$(all_faults)" = "1" ] && [ "$(link_drops)" = "0" ] && echo 1)" \
    "fault isolated to the IMU: $(all_faults) device fault event(s) in total, $(link_drops) link drop(s)"

# Unresponsive simulator (NCR-006): freezing the IMU simulator process means its bus never answers.
# The bridge's IMU worker waits while the other buses carry on. 42 stalls too while the frozen
# simulator stops reading its socket, so GPS fixes stop and GPS may legitimately fault; every other
# bus device must keep working and the umbilical must stay up.
before=$(all_faults)
pkill -STOP -f "nos3-single-simulator .* generic-imu-sim"; sleep 6; pkill -CONT -f "nos3-single-simulator .* generic-imu-sim"
sleep 5
others=$(sed -n "$((before + 1)),\$p" <(grep " failed: " "$OBC_LOG") | grep -v -c "IMU failed\|GPS failed")
check "$([ "$others" = "0" ] && [ "$(link_drops)" = "0" ] && echo 1)" \
    "IMU simulator frozen 6 s: $others bus device fault(s) besides IMU/GPS, $(link_drops) link drop(s)"

echo "OBC events:"
grep "EVENT" "$OBC_LOG" | sed 's/^/  /'
echo "---- $PASS passed, $FAIL failed ----"
[ $FAIL -eq 0 ]
