#!/bin/bash
# frank-cpc — Amstrad CPC for RP2350
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-cpc
# SPDX-License-Identifier: GPL-3.0-or-later

# Flash frank-cpc to connected Pico 2 (RP2350)

FIRMWARE="${1:-./build/frank-cpc.elf}"

if [ ! -f "$FIRMWARE" ]; then
    FIRMWARE="${FIRMWARE%.elf}.uf2"
    if [ ! -f "$FIRMWARE" ]; then
        echo "Error: Firmware file not found"
        echo "Usage: $0 [firmware.elf|firmware.uf2]"
        echo "Default: ./build/frank-cpc.elf"
        exit 1
    fi
fi

echo "Flashing: $FIRMWARE"
picotool load -f "$FIRMWARE" && picotool reboot -f
