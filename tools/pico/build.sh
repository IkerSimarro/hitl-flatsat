#!/bin/bash
#
# Builds the Raspberry Pi Pico 2 firmware: firmware/build-pico/obc.uf2. Runs in the flatsat-pico image
# (tools/pico/Dockerfile: Arm compiler, Pico SDK, picotool), which it builds the first time.
#
#   tools/pico/build.sh
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
cd "$ROOT" || exit 1
docker image inspect flatsat-pico > /dev/null 2>&1 || docker build -t flatsat-pico tools/pico || exit 1
docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" -u "$(id -u):$(id -g)" flatsat-pico sh -c \
    'cmake -S firmware -B firmware/build-pico -DFLATSAT_PLATFORM=pico > /dev/null &&
     make -C firmware/build-pico -j"$(nproc)" obc' || exit 1
ls -l firmware/build-pico/obc.uf2
