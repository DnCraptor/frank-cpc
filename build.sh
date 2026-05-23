#!/bin/bash
#
# frank-cpc — CPC emulator for RP2350
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh - build frank-cpc for RP2350
#
# Usage: PLATFORM=m1 ./build.sh [CPU_MHZ] [HDMI_DRIVER]
#   PLATFORM:    m1, m2 (default), z0
#   HDMI_DRIVER: HDMI_PIO (default, I2S audio)
#                HDMI_PIO_AUDIO (HDMI audio, M2 only)
#                VGA_HSTX (HSTX VGA + I2S audio, M2 only)
#                COMPOSITE (composite TV + I2S audio, M1/M2 only)
#
set -e

rm -rf ./build
mkdir build
cd build

: "${PLATFORM:=m2}"
: "${CPU_SPEED:=${1:-252}}"
: "${HDMI_DRIVER:=${2:-HDMI_PIO}}"

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=${PLATFORM} \
    -DCPU_SPEED=${CPU_SPEED} \
    -DHDMI_DRIVER=${HDMI_DRIVER} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
