#!/bin/bash
#
# End-to-end ground segment test, no GUI: the SIL environment with the OBC, and NOS3's COSMOS
# configuration running headless on the same Docker network, commanding the OBC through the
# FLATSAT_UMB interface. Needs NOS3 configured (make config) and the COSMOS build step run
# (scripts/gsw/gsw_cosmos_build.sh), plus firmware/build/obc.
#
set -u
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
COSMOS_DIR=$ROOT/nos3/gsw/cosmos
NET=flatsat-e2e-$$
SIL_NAME=flatsat-e2e-sil-$$
COSMOS_NAME=flatsat-e2e-cosmos-$$
export SIL_LOG_DIR=$ROOT/sil/logs/e2e-$(date +%Y%m%d-%H%M%S)

cleanup()
{
    docker rm -f "$SIL_NAME" "$COSMOS_NAME" > /dev/null 2>&1
    docker network rm "$NET" > /dev/null 2>&1
}
trap cleanup EXIT

docker network create "$NET" > /dev/null

# COSMOS command and telemetry server, headless, with NOS3's configuration
docker run -d --rm --name "$COSMOS_NAME" --network "$NET" --network-alias flatsat-cosmos \
    -v "$ROOT:$ROOT" -w "$COSMOS_DIR" -e PROCESSOR_ENDIANNESS=LITTLE_ENDIAN \
    ballaerospace/cosmos:4.5.0 ruby tools/CmdTlmServer --no-gui > /dev/null

# SIL environment with the OBC, reachable as nos-fsw
SIL_NETWORK=$NET SIL_CONTAINER_NAME=$SIL_NAME SIL_DETACH=1 "$ROOT/sil/sil.sh" \
    'firmware/build/obc --umb-pty $SIL_UMB_PTY --can none > $SIL_LOG_DIR/obc.log 2>&1 & sleep 600' > /dev/null

echo "[e2e] waiting for the simulation..."
for _ in $(seq 120); do
    docker logs "$SIL_NAME" 2>&1 | grep -q "simulation running" && break
    sleep 1
done
docker logs "$SIL_NAME" 2>&1 | grep -q "simulation running" || { echo "[e2e] simulation did not start"; exit 1; }

docker exec -w "$COSMOS_DIR" "$COSMOS_NAME" ruby "$ROOT/tests/cosmos/test_cosmos_umbilical.rb"
RC=$?
echo "[e2e] logs in $SIL_LOG_DIR"
exit $RC
