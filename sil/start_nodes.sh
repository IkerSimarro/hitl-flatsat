#!/bin/bash
#
# Starts the FlatSat plant model and the ADCS and EPS nodes in the background, inside the SIL container
# (they share the vcan0 CAN bus and the /flatsat-harness shared memory with the OBC). Logs go to
# $SIL_LOG_DIR. Extra arguments go to the plant model, e.g. --soc 0.5 or --usb.
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
BIN=$ROOT/firmware/build
LOG=${SIL_LOG_DIR:-/tmp}

for b in flatsat_plant eps_node adcs_node; do
    [ -x "$BIN/$b" ] || { echo "[nodes] $BIN/$b not built" >&2; exit 1; }
done

"$BIN/flatsat_plant" "$@" > "$LOG/plant.log" 2>&1 &
sleep 0.2 # the plant initialises the harness and the battery state first
"$BIN/eps_node" --can vcan0 > "$LOG/eps_node.log" 2>&1 &
"$BIN/adcs_node" --can vcan0 --run-line adcs > "$LOG/adcs_node.log" 2>&1 &
echo "[nodes] plant model, EPS node and ADCS node started"
