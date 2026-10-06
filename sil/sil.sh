#!/bin/bash
#
# Runs a command inside the software-in-the-loop environment (see sil_env.sh), in the NOS3 Docker image.
#
# Usage: sil/sil.sh COMMAND [ARGS...]
#   e.g. sil/sil.sh firmware/build/test_devices --umb-pty '$SIL_UMB_PTY' --can none
# Arguments are passed through bash inside the container, so '$SIL_UMB_PTY' (single-quoted on the host)
# expands to the umbilical pty path there.
#
# Environment:
#   SIL_LOG_DIR         log directory (default sil/logs/<timestamp>)
#   SIL_NETWORK         join this Docker network as hosts "nos-fsw" and "truth42sim", so a ground segment
#                       on the same network can reach the bridge; umbilical telemetry and 42 truth are then
#                       sent to host "flatsat-cosmos" (give COSMOS that network alias). Without it, every
#                       host name maps to the container itself
#   SIL_CONTAINER_NAME  container name
#   SIL_DETACH=1        run in the background (docker run -d)
#   SIL_TTY=0           don't allocate a terminal even when run from one
#   SIL_DOCKER_ARGS     extra docker run arguments
#   SIL_GRAPHICS=1      open 42's 3D view (also pass DISPLAY and the X11 socket in SIL_DOCKER_ARGS)
#   SIL_INIT_RATES      initial body rates in deg/s, e.g. "2 -3 4" (default: NOS3's 2.8 deg/s deployment tip-off)
#   SIL_NO_BRIDGE=1     no HIL bridge: something else drives the NOS3 buses (NOS3's own cFS, test TC-15)
#   SIL_GS_ARGS         ground station options (ground/flatsat_gs.py), e.g. "--mode always" or "--loss 0.1"
#
set -u

ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
DBOX=$(sed -n 's/^DBOX="\(.*\)"/\1/p' "$ROOT/nos3/scripts/env.sh")

DOCKER_FLAGS="--rm"
if [ "${SIL_DETACH:-0}" = "1" ]; then
    DOCKER_FLAGS="$DOCKER_FLAGS -d"
elif [ "${SIL_TTY:-auto}" != "0" ] && [ -t 0 ] && [ -t 1 ]; then
    DOCKER_FLAGS="$DOCKER_FLAGS -it"
fi
[ -n "${SIL_CONTAINER_NAME:-}" ] && DOCKER_FLAGS="$DOCKER_FLAGS --name $SIL_CONTAINER_NAME"

# Host names of everything that runs inside this container
# ("cosmos" stays local so the readiness check and SIL tests receive 42 truth; see SIL_GROUND_HOST)
LOCAL_HOSTS="nos-engine-server sc01-nos-engine-server fortytwo trq-sim radio-sim cryptolib cosmos"
GROUND_HOST=""
if [ -n "${SIL_NETWORK:-}" ]; then
    DOCKER_FLAGS="$DOCKER_FLAGS --network $SIL_NETWORK --network-alias nos-fsw --network-alias truth42sim"
    DOCKER_FLAGS="$DOCKER_FLAGS --network-alias flatsat-gs -h nos-fsw"
    GROUND_HOST=flatsat-cosmos
    LOCAL_HOSTS="$LOCAL_HOSTS flatsat-gs"
else
    LOCAL_HOSTS="$LOCAL_HOSTS nos-fsw flatsat-gs"
fi
for h in $LOCAL_HOSTS; do
    DOCKER_FLAGS="$DOCKER_FLAGS --add-host $h:127.0.0.1"
done

# The FlatSat CAN bus is a Linux virtual CAN interface created inside the container (sil_env.sh):
# the vcan module must be loaded in the (shared) kernel, and creating the interface needs NET_ADMIN
if ! grep -q "^vcan " /proc/modules 2> /dev/null; then
    modprobe vcan 2> /dev/null || echo "[sil] warning: could not load the vcan kernel module; CAN nodes won't run" >&2
fi
DOCKER_FLAGS="$DOCKER_FLAGS --cap-add NET_ADMIN"

exec docker run $DOCKER_FLAGS ${SIL_DOCKER_ARGS:-} \
    -v "$ROOT:$ROOT" -v "$HOME/.nos3:$HOME/.nos3" -w "$ROOT" \
    -e SIL_LOG_DIR="${SIL_LOG_DIR:-$ROOT/sil/logs/$(date +%Y%m%d-%H%M%S)}" -e SIL_GROUND_HOST="$GROUND_HOST" \
    -e SIL_GRAPHICS="${SIL_GRAPHICS:-0}" -e SIL_INIT_RATES="${SIL_INIT_RATES:-}" -e SIL_GS_ARGS="${SIL_GS_ARGS:-}" \
    -e SIL_NO_BRIDGE="${SIL_NO_BRIDGE:-0}" \
    "$DBOX" "$ROOT/sil/sil_env.sh" bash -c "$*"
