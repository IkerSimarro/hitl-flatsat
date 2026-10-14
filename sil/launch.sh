#!/bin/bash
#
# One-command software-in-the-loop launch of the HITL FlatSat
#
#   sil/launch.sh             42 3D view + COSMOS ground station GUI + simulators + OBC + bridge
#   sil/launch.sh --headless  the same without windows (COSMOS server only)
#   sil/launch.sh --pico      the OBC is the Raspberry Pi Pico on USB (/dev/ttyACM0, or $PICO_DEV), attached
#                             to WSL with usbipd; no CAN nodes (they need the CAN boards). Combines with --headless
#
# Otherwise the OBC runs as a Linux process with its umbilical on a pty; everything else is NOS3. Ctrl-C stops
# everything. Before the first run: build NOS3 (make prep, config, all, in nos3/), run
# nos3/scripts/gsw/gsw_cosmos_build.sh, and build the firmware (see README).
#
set -u

ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
COSMOS_DIR=$ROOT/nos3/gsw/cosmos
NET=flatsat-sil
SIL_NAME=flatsat-sil
COSMOS_NAME=flatsat-cosmos

HEADLESS=0
PICO=""
for arg in "$@"; do
    case $arg in
        --headless) HEADLESS=1 ;;
        --pico) PICO=${PICO_DEV:-/dev/ttyACM0} ;;
        *) echo "usage: $0 [--headless] [--pico]" >&2; exit 2 ;;
    esac
done

fail() { echo "[launch] $*" >&2; exit 1; }

# ---- Preconditions ----
if [ -n "$PICO" ]; then
    [ -e "$PICO" ] || fail "no Pico at $PICO: plug it in and attach it to WSL (usbipd attach --wsl --busid ...)"
else
    [ -x "$ROOT/firmware/build/obc" ] || fail "OBC not built: see README (firmware build)"
fi
[ -x "$ROOT/nos3/sims/build/bin/nos3-hil-bridge" ] || fail "NOS3 not built: run make in nos3/"
[ -d "$COSMOS_DIR/COMPONENTS/FLATSAT" ] || fail "COSMOS not prepared: run nos3/scripts/gsw/gsw_cosmos_build.sh"
grep -q "FLATSAT_UMB" "$COSMOS_DIR/config/tools/cmd_tlm_server/cmd_tlm_server.txt" ||
    fail "FlatSat interface missing from COSMOS: run make config in nos3/"
if [ $HEADLESS = 0 ]; then
    [ -n "${DISPLAY:-}" ] && [ -d /tmp/.X11-unix ] || fail "no X display: use --headless"
fi

cleanup()
{
    echo "[launch] stopping..."
    docker rm -f "$SIL_NAME" "$COSMOS_NAME" > /dev/null 2>&1
    docker network rm "$NET" > /dev/null 2>&1
}
trap cleanup EXIT
trap 'exit 130' INT TERM

docker rm -f "$SIL_NAME" "$COSMOS_NAME" > /dev/null 2>&1
docker network create "$NET" > /dev/null 2>&1

X11_ARGS=""
[ $HEADLESS = 0 ] && X11_ARGS="-e DISPLAY=$DISPLAY -e QT_X11_NO_MITSHM=1 -v /tmp/.X11-unix:/tmp/.X11-unix:ro"

# ---- Ground segment: COSMOS with NOS3's configuration and the FLATSAT target ----
if [ $HEADLESS = 0 ]; then
    COSMOS_CMD="ruby Launcher"
else
    COSMOS_CMD="ruby tools/CmdTlmServer --no-gui"
fi
# --init: COSMOS's GUI tools call setpgrp(), which fails (EPERM) for PID 1, a session leader (NCR-007)
docker run -d --rm --init --name "$COSMOS_NAME" --network "$NET" --network-alias flatsat-cosmos $X11_ARGS \
    -v "$ROOT:$ROOT" -w "$COSMOS_DIR" -e PROCESSOR_ENDIANNESS=LITTLE_ENDIAN \
    ballaerospace/cosmos:4.5.0 $COSMOS_CMD > /dev/null || fail "could not start COSMOS"
echo "[launch] COSMOS started$([ $HEADLESS = 0 ] && echo ": open 'Command and Telemetry Server', then Packet Viewer > FLATSAT")"

# ---- Space segment: NOS3 simulators, 42, the HIL bridge, the OBC and the ADCS and EPS nodes on CAN ----
export SIL_LOG_DIR=$ROOT/sil/logs/launch-$(date +%Y%m%d-%H%M%S)
echo "[launch] logs in $SIL_LOG_DIR"
# Runs as a background job so Ctrl-C reaches this script's trap straight away (bash defers traps
# while a foreground child runs); the trap removes the containers, which ends the job.
if [ -n "$PICO" ]; then
    # The Pico's log lines arrive over the umbilical; show them, and the link coming and going
    echo "[launch] OBC: the Pico on $PICO"
    SPACE='tail -F $SIL_LOG_DIR/bridge.log 2> /dev/null | grep --line-buffered -E "MCU: |serial device|MCU link"'
else
    SPACE='sil/start_nodes.sh && firmware/build/obc --umb-pty $SIL_UMB_PTY 2>&1 | tee $SIL_LOG_DIR/obc.log'
fi
SIL_NETWORK=$NET SIL_CONTAINER_NAME=$SIL_NAME SIL_GRAPHICS=$((1 - HEADLESS)) SIL_DOCKER_ARGS="$X11_ARGS" SIL_TTY=0 \
    SIL_UMB_DEV=$PICO "$ROOT/sil/sil.sh" "$SPACE" &
wait $!
