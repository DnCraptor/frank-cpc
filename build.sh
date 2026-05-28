#!/bin/bash
#
# frank-cpc — Amstrad CPC for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-cpc
# SPDX-License-Identifier: GPL-3.0-or-later
#
#
# Usage: PLATFORM=m1 USB_HID=1 ./build.sh [CPU_MHZ] [HDMI_DRIVER]
#   PLATFORM:    m1, m2 (default), z0
#   HDMI_DRIVER: HDMI_PIO (default, I2S audio)
#                HDMI_PIO_AUDIO (HDMI audio, M2 only)
#                VGA_HSTX (HSTX VGA + I2S audio, M2 only)
#                COMPOSITE (composite TV + I2S audio, M1/M2 only)
#   USB_HID:     0 (default) PS/2 only + USB serial console
#                1 USB HID keyboard/gamepad + PS/2 (no USB serial console)
#
set -e

rm -rf ./build
mkdir build
cd build

: "${PLATFORM:=m2}"
: "${CPU_SPEED:=${1:-504}}"
: "${HDMI_DRIVER:=${2:-HDMI_PIO}}"
: "${PSRAM_SPEED:=100}"
: "${FLASH_SPEED:=66}"
: "${USB_HID:=0}"

USB_HID_FLAG=""
if [ "$USB_HID" = "1" ]; then
    USB_HID_FLAG="-DUSB_HID_ENABLED=ON"
fi

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=${PLATFORM} \
    -DCPU_SPEED=${CPU_SPEED} \
    -DPSRAM_SPEED=${PSRAM_SPEED} \
    -DFLASH_SPEED=${FLASH_SPEED} \
    -DHDMI_DRIVER=${HDMI_DRIVER} \
    ${USB_HID_FLAG} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
