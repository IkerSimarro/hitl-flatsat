#!/bin/bash
#
# Software-in-the-loop environment, run inside the NOS3 Docker image (use sil/sil.sh from the host).
#
# Starts the NOS Engine server, the NOS3 time driver, 42 without graphics, every simulator 42 waits
# for, and the HIL bridge, then runs the given command. The command must create the OBC umbilical pty
# at $SIL_UMB_PTY (e.g. obc --umb-pty "$SIL_UMB_PTY"); the bridge attaches to it once it exists.
# Everything shares the container's network, with the NOS3 host names mapped to 127.0.0.1.
#
# Usage: sil_env.sh COMMAND [ARGS...]
# Environment:
#   SIL_LOG_DIR   where component logs go (default: a new temporary directory)
#   SIL_NO_BRIDGE set to 1 to skip the bridge
#   SIL_GROUND_HOST  if set, umbilical telemetry and 42 truth go to this host (the ground segment)
#                    instead of staying inside the container
#
set -u

ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
NOS3=$ROOT/nos3
SIM_BIN=$NOS3/sims/build/bin
FORTYTWO_DIR=${FORTYTWO_DIR:-$HOME/.nos3/42}
INOUT_NAME=FlatSatSilInOut

export SIL_LOG_DIR=${SIL_LOG_DIR:-$(mktemp -d /tmp/flatsat-sil.XXXXXX)}
export SIL_UMB_PTY=$SIL_LOG_DIR/obc-umbilical.pty
mkdir -p "$SIL_LOG_DIR"

if [ $# -eq 0 ]; then
    echo "usage: $0 COMMAND [ARGS...]" >&2
    exit 2
fi

log() { echo "[sil] $*"; }

cleanup()
{
    local pids
    pids=$(jobs -p)
    [ -n "$pids" ] && kill $pids 2> /dev/null
    sleep 1
    pids=$(jobs -p)
    [ -n "$pids" ] && kill -9 $pids 2> /dev/null
    wait 2> /dev/null
    rm -rf "$FORTYTWO_DIR/$INOUT_NAME"
}
trap cleanup EXIT

for f in "$SIM_BIN/nos3-single-simulator" "$SIM_BIN/nos3-hil-bridge" "$FORTYTWO_DIR/42"; do
    if [ ! -x "$f" ]; then
        echo "[sil] missing $f: build NOS3 first (make prep, make config, make)" >&2
        exit 1
    fi
done

cp "$NOS3/cfg/build/sims/sim_log_config.xml" "$SIL_LOG_DIR/"
SIM_CFG=$SIM_BIN/nos3-simulator.xml

log "logs in $SIL_LOG_DIR"

# NOS Engine server: it has an interactive menu and exits on stdin EOF, so keep stdin open
# (in a subshell, so its harmless segfault when killed at shutdown is reported to its log, not here)
(cd "$SIL_LOG_DIR" && sleep infinity | nos_engine_server_standalone -f "$SIM_BIN/nos_engine_server_config.json") \
    > "$SIL_LOG_DIR/nos-engine-server.log" 2>&1 &
sleep 1

# 42 with the mission's InOut files, graphics off
rm -rf "$FORTYTWO_DIR/$INOUT_NAME"
cp -r "$NOS3/cfg/build/InOut" "$FORTYTWO_DIR/$INOUT_NAME"
sed -i 's/^TRUE\( *!  Graphics Front End\)/FALSE\1/' "$FORTYTWO_DIR/$INOUT_NAME/Inp_Sim.txt"
(cd "$FORTYTWO_DIR" && exec ./42 "$INOUT_NAME" > "$SIL_LOG_DIR/42.log" 2>&1) &

# Time driver: it draws a curses screen, so give it a pseudo-terminal
(cd "$SIL_LOG_DIR" && sleep infinity | TERM=xterm script -qfec "$SIM_BIN/nos3-single-simulator -f $SIM_CFG time" \
    /dev/null > "$SIL_LOG_DIR/time.log" 2>&1) &

# Every simulator 42 waits for (Inp_IPC.txt), in its connection order
SIMS="generic-reactionwheel-sim0 generic-reactionwheel-sim1 generic-reactionwheel-sim2 generic-torquer-sim
      generic-thruster-sim gps generic-css-sim generic-mag-sim truth42sim generic-fss-sim generic-imu-sim
      generic-star-tracker-sim generic-eps-sim generic-radio-sim"
for sim in $SIMS; do
    (cd "$SIL_LOG_DIR" && exec "$SIM_BIN/nos3-single-simulator" -f "$SIM_CFG" "$sim" > "$SIL_LOG_DIR/$sim.log" 2>&1) &
done

# Ready when 42 truth data arrives (42 only starts stepping once every simulator is connected)
log "waiting for 42 and the simulators..."
if ! python3 - <<'EOF'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 5111))
s.settimeout(120)
try:
    # truth42sim sends zero-filled packets until its 42 connection is up; wait for real data (year != 0)
    while s.recv(1024)[:2] == b"\0\0":
        pass
except socket.timeout:
    sys.exit(1)
EOF
then
    echo "[sil] no 42 truth data after 120 s; see $SIL_LOG_DIR" >&2
    exit 1
fi
log "simulation running"

BRIDGE_ARGS=""
if [ -n "${SIL_GROUND_HOST:-}" ]; then
    BRIDGE_ARGS="--umb-host $SIL_GROUND_HOST"
    # 42 truth is sent to this container (readiness check above); pass it on to the ground segment
    python3 "$ROOT/sil/truth_relay.py" "$SIL_GROUND_HOST" > "$SIL_LOG_DIR/truth-relay.log" 2>&1 &
fi

if [ "${SIL_NO_BRIDGE:-0}" != "1" ]; then
    # The bridge attaches once the command has created the umbilical pty
    (while [ ! -e "$SIL_UMB_PTY" ]; do sleep 0.2; done
     exec "$SIM_BIN/nos3-hil-bridge" -d "$SIL_UMB_PTY" $BRIDGE_ARGS -v > "$SIL_LOG_DIR/bridge.log" 2>&1) &
fi

"$@"
RC=$?
log "command exited with $RC"
exit $RC
