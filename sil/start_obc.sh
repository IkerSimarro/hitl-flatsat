#!/bin/bash
#
# Starts the OBC for a test, inside the SIL environment (after sil/start_nodes.sh):
#   Linux build (default)        firmware/build/obc on the umbilical pty, in the background, logging to obc.log
#   real OBC (SIL_UMB_DEV set)   the Pico: restarted over the umbilical (sil/obc_restart.py). Its log lines arrive
#                                as LOG frames that the bridge prints; they are copied to obc.log from the
#                                restart on, with the bridge's own "MCU link down" reports, so tests read the
#                                same log either way
# Prints "[obc] ready" when done, for tests that watch the container's output (tests/cosmos/test_cosmos_e2e.sh).
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )

if [ -z "${SIL_UMB_DEV:-}" ]; then
    "$ROOT/firmware/build/obc" --umb-pty "$SIL_UMB_PTY" > "$SIL_LOG_DIR/obc.log" 2>&1 &
    echo "[obc] ready"
    exit 0
fi

OFFSET=$(python3 "$ROOT/sil/obc_restart.py") || exit 1
(tail -c +$((OFFSET + 1)) -F "$SIL_LOG_DIR/bridge.log" 2> /dev/null |
     sed -u -n -e 's/^hil_bridge: MCU: //p' -e '/MCU link down/p' > "$SIL_LOG_DIR/obc.log") &
echo "[obc] ready"
exit 0
