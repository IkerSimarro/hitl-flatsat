# Build image for the FlatSat's Raspberry Pi Pico 2 (RP2350) firmware: the Arm compiler, the Pico SDK and
# picotool (which the SDK uses to make .uf2 files). tools/pico/build.sh builds and uses it.
FROM debian:bookworm-slim

ARG PICO_SDK_VERSION=2.3.1
ARG PICOTOOL_VERSION=2.3.1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake git python3 pkg-config libusb-1.0-0-dev \
        gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch ${PICO_SDK_VERSION} https://github.com/raspberrypi/pico-sdk.git /opt/pico-sdk \
    && git -C /opt/pico-sdk submodule update --init --depth 1 lib/tinyusb
ENV PICO_SDK_PATH=/opt/pico-sdk

RUN git clone --depth 1 --branch ${PICOTOOL_VERSION} https://github.com/raspberrypi/picotool.git /tmp/picotool \
    && cmake -S /tmp/picotool -B /tmp/picotool/build -DPICOTOOL_NO_LIBUSB=0 -DCMAKE_INSTALL_PREFIX=/opt/picotool \
    && make -C /tmp/picotool/build -j"$(nproc)" install \
    && rm -rf /tmp/picotool
ENV picotool_DIR=/opt/picotool/lib/cmake/picotool
ENV PATH=/opt/picotool/bin:$PATH
