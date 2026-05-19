#!/bin/bash
#
# frank-cpc — CPC emulator for RP2350
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# build.sh - build frank-cpc for RP2350 (M2 platform, PIO HDMI, I2S audio)
#
# Usage: ./build.sh [CPU_MHZ]
#
set -e

rm -rf ./build
mkdir build
cd build

: "${CPU_SPEED:=${1:-252}}"

cmake \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=m2 \
    -DCPU_SPEED=${CPU_SPEED} \
    ..

make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
