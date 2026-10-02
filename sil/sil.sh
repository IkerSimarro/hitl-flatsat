#!/bin/bash
#
# Runs a command inside the software-in-the-loop environment (see sil_env.sh), in the NOS3 Docker image.
#
# Usage: sil/sil.sh COMMAND [ARGS...]
#   e.g. sil/sil.sh firmware/build/test_devices --umb-pty '$SIL_UMB_PTY' --can none
# Arguments are passed through bash inside the container, so '$SIL_UMB_PTY' (single-quoted on the host)
# expands to the umbilical pty path there.
#
set -u

ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
DBOX=$(sed -n 's/^DBOX="\(.*\)"/\1/p' "$ROOT/nos3/scripts/env.sh")

TTY_FLAGS=""
[ -t 0 ] && [ -t 1 ] && TTY_FLAGS="-it"

HOSTS=""
for h in nos-engine-server sc01-nos-engine-server fortytwo trq-sim radio-sim cosmos nos-fsw cryptolib flatsat-gs; do
    HOSTS="$HOSTS --add-host $h:127.0.0.1"
done

exec docker run --rm $TTY_FLAGS $HOSTS \
    -v "$ROOT:$ROOT" -v "$HOME/.nos3:$HOME/.nos3" -w "$ROOT" \
    -e SIL_LOG_DIR="${SIL_LOG_DIR:-$ROOT/sil/logs/$(date +%Y%m%d-%H%M%S)}" \
    "$DBOX" "$ROOT/sil/sil_env.sh" bash -c "$*"
