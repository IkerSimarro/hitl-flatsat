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
#   SIL_GRAPHICS  set to 1 to open 42's 3D view (needs DISPLAY and the X11 socket)
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

# FlatSat CAN bus between the node processes (OBC, ADCS, EPS): virtual CAN interface vcan0
if [ -x "$ROOT/firmware/build/vcan_up" ]; then
    "$ROOT/firmware/build/vcan_up" vcan0 > /dev/null || echo "[sil] warning: no CAN bus (vcan0)" >&2
fi

cp "$NOS3/cfg/build/sims/sim_log_config.xml" "$SIL_LOG_DIR/"
SIM_CFG=$SIM_BIN/nos3-simulator.xml

log "logs in $SIL_LOG_DIR"

# NOS Engine server: it has an interactive menu and exits on stdin EOF, so keep stdin open
# (in a subshell, so its harmless segfault when killed at shutdown is reported to its log, not here)
(cd "$SIL_LOG_DIR" && sleep infinity | nos_engine_server_standalone -f "$SIM_BIN/nos_engine_server_config.json") \
    > "$SIL_LOG_DIR/nos-engine-server.log" 2>&1 &
sleep 1

# 42 must carry the socket parsing fix (NCR-010): without it, stale actuator commands are re-applied
if ! grep -q "NCR-010" "$FORTYTWO_DIR/Source/AutoCode/TxRxIPC.c" 2> /dev/null; then
    echo "[sil] 42 lacks the NCR-010 fix: run 'git apply $NOS3/scripts/cfg/patches/42-ipc-parse-bound.patch' in" \
         "$FORTYTWO_DIR and rebuild it (or re-run make prep)" >&2
    exit 1
fi

# 42 with the mission's InOut files; graphics off unless SIL_GRAPHICS=1 (needs DISPLAY)
rm -rf "$FORTYTWO_DIR/$INOUT_NAME"
cp -r "$NOS3/cfg/build/InOut" "$FORTYTWO_DIR/$INOUT_NAME"
if [ "${SIL_GRAPHICS:-0}" != "1" ]; then
    sed -i 's/^TRUE\( *!  Graphics Front End\)/FALSE\1/' "$FORTYTWO_DIR/$INOUT_NAME/Inp_Sim.txt"
fi
# Initial body rates in deg/s, e.g. SIL_INIT_RATES="2 -3 4" for a tumbling start (detumble tests)
if [ -n "${SIL_INIT_RATES:-}" ]; then
    sed -i "s/^.*\(! Ang Vel (deg\/sec)\)/$SIL_INIT_RATES    \1/" "$FORTYTWO_DIR/$INOUT_NAME/SC_NOS3.txt"
    echo "[sil] initial body rates $SIL_INIT_RATES deg/s"
fi
# Without a GPU, Mesa's llvmpipe renders 42's windows with one thread per CPU, once per simulated
# second; on a laptop those bursts delayed the bridge past its 100 ms deadline. Two render threads and a
# lower priority keep the bridge, simulators and OBC responsive (42 tolerates a few ms of delay).
(cd "$FORTYTWO_DIR" && LP_NUM_THREADS=2 exec nice -n 10 ./42 "$INOUT_NAME" > "$SIL_LOG_DIR/42.log" 2>&1) &

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

# NOS3's command bus bridge: JSON over TCP 12020 to any simulator's command node (sil/sim_cmd.py), used
# for fault injection such as {"node": "imu-command", "cmd": "DISABLE"}
(cd "$SIL_LOG_DIR" && exec "$SIM_BIN/nos3-sim-cmdbus-bridge" -f "$SIM_CFG" > "$SIL_LOG_DIR/cmdbus-bridge.log" 2>&1) &

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
TRUTH_DESTS="127.0.0.1:9032 127.0.0.1:5112" # ground station software, SIL tests and tools (ICD 3.6)
GS_COSMOS=cosmos
if [ -n "${SIL_GROUND_HOST:-}" ]; then
    BRIDGE_ARGS="--umb-host $SIL_GROUND_HOST"
    TRUTH_DESTS="$TRUTH_DESTS $SIL_GROUND_HOST:5111"
    GS_COSMOS=$SIL_GROUND_HOST
fi
# 42 truth reaches this container (readiness check above); the relay passes it on to everyone who needs it
python3 "$ROOT/sil/truth_relay.py" $TRUTH_DESTS > "$SIL_LOG_DIR/truth-relay.log" 2>&1 &

# Ground station software and RF link emulator (ICD 7.4); SIL_GS_ARGS e.g. "--mode always"
python3 "$ROOT/ground/flatsat_gs.py" --cosmos-host "$GS_COSMOS" ${SIL_GS_ARGS:-} > "$SIL_LOG_DIR/gs.log" 2>&1 &

if [ "${SIL_NO_BRIDGE:-0}" != "1" ]; then
    # The bridge attaches once the command has created the umbilical pty
    (while [ ! -e "$SIL_UMB_PTY" ]; do sleep 0.2; done
     exec "$SIM_BIN/nos3-hil-bridge" -d "$SIL_UMB_PTY" $BRIDGE_ARGS -v > "$SIL_LOG_DIR/bridge.log" 2>&1) &
fi

# 42 watchdog (NCR-012): the truth simulator logs 42's simulation time several times a second. If that time
# stops changing for 6 s while everything else runs, results after that point are invalid; say so loudly
# instead of letting it look like a flight software failure. (42's own *.42 output files can't be used: they
# are written in buffered chunks a couple of minutes apart.)
sim_time() { tail -c 4000 "$SIL_LOG_DIR/truth42sim.log" 2> /dev/null | grep -o "([0-9/]*)T[0-9:.]*" | tail -1; }
(last=""; same=0; stalled=0
 while sleep 3; do
     cur=$(sim_time)
     if [ -n "$cur" ] && [ "$cur" = "$last" ]; then
         same=$((same + 1))
     else
         same=0
     fi
     if [ $same -ge 2 ] && [ $stalled = 0 ]; then
         log "WARNING: 42 has stopped advancing (simulation time stuck at $cur): results from now on are invalid"
         { date; pgrep -a -f "./42 $INOUT_NAME"; tail -3 "$SIL_LOG_DIR/truth42sim.log"; } > "$SIL_LOG_DIR/42-stall.txt" 2>&1
         stalled=1
     elif [ $same = 0 ] && [ $stalled = 1 ]; then
         log "42 is advancing again"
         stalled=0
     fi
     last=$cur
 done) &

"$@"
RC=$?
log "command exited with $RC"
exit $RC
