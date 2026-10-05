#!/bin/bash
#
# Captures the FlatSat COSMOS screens with live data, for the documentation (docs/images/), without opening
# any window on the desktop:
#   - the SIL environment (OBC, CAN nodes, NOS3, 42) and a headless COSMOS server on a private Docker network,
#     with the ground station at NASA Wallops so a pass starts about 4 minutes in (ICD 7.4)
#   - Telemetry Viewer on virtual displays (Xvfb) inside the COSMOS container, opened early so the graphs show
#     the detumble, the slew to the Sun and the pass
#   - screenshots in the middle of the pass
#
#   tools/capture_screens.sh [OUTPUT_DIR] [SECONDS_AFTER_START]     (defaults: docs/images, 480)
#
# Needs the COSMOS build step (nos3/scripts/gsw/gsw_cosmos_build.sh) and the firmware build; takes ~9 minutes.
#
set -u
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
OUT=${1:-$ROOT/docs/images}
AT=${2:-480}
COSMOS_DIR=$ROOT/nos3/gsw/cosmos
NET=flatsat-capture-$$
SIL_NAME=flatsat-capture-sil-$$
COSMOS_NAME=flatsat-capture-cosmos-$$
export SIL_LOG_DIR=$ROOT/sil/logs/capture-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd) # docker -v needs an absolute path (a relative one names a volume)

cleanup()
{
    docker rm -f "$SIL_NAME" "$COSMOS_NAME" > /dev/null 2>&1
    docker network rm "$NET" > /dev/null 2>&1
}
trap cleanup EXIT

docker network create "$NET" > /dev/null
docker run -d --rm --name "$COSMOS_NAME" --network "$NET" --network-alias flatsat-cosmos \
    -v "$ROOT:$ROOT" -v "$OUT:/shots" -w "$COSMOS_DIR" -e PROCESSOR_ENDIANNESS=LITTLE_ENDIAN \
    ballaerospace/cosmos:4.5.0 ruby tools/CmdTlmServer --no-gui > /dev/null
echo "[capture] installing a virtual display in the COSMOS container..."
for try in 1 2 3; do # the package mirrors occasionally time out
    docker exec "$COSMOS_NAME" bash -c 'apt-get update -qq && apt-get install -y -qq xvfb imagemagick' > /dev/null 2>&1 &&
        break
    [ $try = 3 ] && { echo "[capture] could not install Xvfb" >&2; exit 1; }
    sleep 10
done

SIL_NETWORK=$NET SIL_CONTAINER_NAME=$SIL_NAME SIL_DETACH=1 SIL_GS_ARGS="--lat 37.9402 --lon -75.4664 --alt 10" \
    "$ROOT/sil/sil.sh" 'sil/start_nodes.sh && { firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 & sleep 3600; }' \
    > /dev/null
for _ in $(seq 120); do
    docker logs "$SIL_NAME" 2>&1 | grep -q "simulation running" && break
    sleep 1
done
start=$(date +%s)
echo "[capture] simulation running; screens open in 30 s, screenshots at $AT s"
sleep 30

# One virtual display per screen, so the windows do not overlap; large, so the (centred) pointer sits outside the
# window and no tooltip ends up in the picture
docker exec -d "$COSMOS_NAME" bash -c 'Xvfb :91 -screen 0 2400x1800x24 & Xvfb :92 -screen 0 2400x1800x24 & sleep 2
    DISPLAY=:91 ruby tools/TlmViewer -n -s "FLATSAT OVERVIEW" > /tmp/tv1.log 2>&1 &
    DISPLAY=:92 ruby tools/TlmViewer -n -s "FLATSAT_RF GROUND_STATION" > /tmp/tv2.log 2>&1 &
    sleep 7200'
sleep $((AT - ($(date +%s) - start)))
docker exec "$COSMOS_NAME" bash -c 'DISPLAY=:91 import -window root -trim +repage /shots/cosmos_overview.png &&
    DISPLAY=:92 import -window root -trim +repage /shots/cosmos_ground_station.png'
echo "[capture] wrote $OUT/cosmos_overview.png and $OUT/cosmos_ground_station.png (logs in $SIL_LOG_DIR)"
