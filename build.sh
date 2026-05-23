#!/bin/bash
#
# frank-cpc — CPC emulator for RP2350
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh - build frank-cpc for RP2350 (M2 platform)
#
# Usage: ./build.sh [CPU_MHZ] [HDMI_DRIVER]
#   HDMI_DRIVER: HDMI_PIO (default, I2S audio)
#                HDMI_PIO_AUDIO (HDMI audio)
#                VGA_HSTX (HSTX VGA + I2S audio on Core 0)
#                COMPOSITE (composite TV + I2S audio on Core 0)
#
set -e

rm -rf ./build
mkdir build
cd build

: "${CPU_SPEED:=${1:-252}}"
: "${HDMI_DRIVER:=${2:-HDMI_PIO}}"

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=m2 \
    -DCPU_SPEED=${CPU_SPEED} \
    -DHDMI_DRIVER=${HDMI_DRIVER} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
