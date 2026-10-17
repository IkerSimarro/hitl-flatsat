#!/bin/bash
#
# Builds the Raspberry Pi Pico 2 firmware of the four boards: firmware/build-pico/obc.uf2, adcs_node.uf2,
# eps_node.uf2 and modem.uf2. Runs in the flatsat-pico image (tools/pico/Dockerfile: Arm compiler, Pico SDK,
# picotool), which it builds the first time.
#
#   tools/pico/build.sh [TARGET...]     (default: all four; e.g. tools/pico/build.sh obc)
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
cd "$ROOT" || exit 1
docker image inspect flatsat-pico > /dev/null 2>&1 || docker build -t flatsat-pico tools/pico || exit 1
TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(obc adcs_node eps_node modem)
docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" -u "$(id -u):$(id -g)" flatsat-pico sh -c \
    'cmake -S firmware -B firmware/build-pico -DFLATSAT_PLATFORM=pico > /dev/null &&
     make -C firmware/build-pico -j"$(nproc)" "$@"' sh "${TARGETS[@]}" || exit 1
for t in "${TARGETS[@]}"; do ls -l "firmware/build-pico/$t.uf2"; done
