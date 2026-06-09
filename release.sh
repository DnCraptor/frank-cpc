#!/bin/bash
#
# frank-cpc — Amstrad CPC for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-cpc
# SPDX-License-Identifier: GPL-3.0-or-later
#
#
# Build matrix:
#   m2p2_frank-cpc_*_hdmi-pio.uf2       (M2, PIO HDMI + I2S)
#   m1p2_frank-cpc_*_hdmi-pio.uf2       (M1, PIO HDMI + I2S)
#   m1p2_frank-cpc_*_tv.uf2             (M1, composite TV + I2S)
#   z0p2_frank-cpc_*_hdmi-pio.uf2       (Z0, PIO HDMI + I2S)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# Build matrix: "platform:prefix:video_suffix:hdmi_driver"
BUILD_MATRIX=(
    "m2:m2p2_:hdmi-pio:HDMI_PIO"
    "m2:m2p2_:hdmi-pio-audio:HDMI_PIO_AUDIO"
    "m2:m2p2_:tv:COMPOSITE"
    "m1:m1p2_:hdmi-pio:HDMI_PIO"
    "m1:m1p2_:hdmi-pio-audio:HDMI_PIO_AUDIO"
    "m1:m1p2_:tv:COMPOSITE"
    "z0:z0p2_:hdmi-pio:HDMI_PIO"
    "z0:z0p2_:hdmi-pio-audio:HDMI_PIO_AUDIO"
)

VERSION_FILE="version.txt"

if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    FRANK CPC Release Builder                    │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo -e "Variants: ${CYAN}${#BUILD_MATRIX[@]}${NC} (m2x3, m1x3, z0x2)"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
if [[ -n "$1" ]]; then
    INPUT_VERSION="$1"
    echo -e "Version (from command line): ${CYAN}${INPUT_VERSION}${NC}"
else
    read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
    INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}
fi

if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"

echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"
printf '%d %02d\n' "$MAJOR" "$MINOR" > "$VERSION_FILE"

RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

SUCCEEDED=()
FAILED=()

for ENTRY in "${BUILD_MATRIX[@]}"; do
    IFS=':' read -r PLAT PREFIX VIDEO_SUFFIX DRIVER <<< "$ENTRY"
    LABEL="${PREFIX}${VIDEO_SUFFIX}"
    OUTPUT_NAME="${PREFIX}frank-cpc_${VERSION}_${VIDEO_SUFFIX}.uf2"

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo -e "${CYAN}Building: $OUTPUT_NAME${NC}"
    echo ""

    rm -rf build
    mkdir build
    cd build

    if cmake .. \
        -DPICO_PLATFORM=rp2350 \
        -DPLATFORM="$PLAT" \
        -DHDMI_DRIVER="$DRIVER" \
        -DUSB_HID_ENABLED=ON \
        > /dev/null 2>&1 && \
       make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1; then
        if [[ -f "frank-cpc.uf2" ]]; then
            cp "frank-cpc.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ $LABEL${NC} → release/$OUTPUT_NAME"
            SUCCEEDED+=("$OUTPUT_NAME")
        else
            echo -e "  ${RED}✗ UF2 not found${NC}"
            FAILED+=("$LABEL")
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
        FAILED+=("$LABEL")
    fi

    cd "$SCRIPT_DIR"
done

rm -rf build

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ ${#SUCCEEDED[@]} -gt 0 ]]; then
    echo -e "${GREEN}Succeeded: ${SUCCEEDED[*]}${NC}"
fi
if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${RED}Failed: ${FAILED[*]}${NC}"
fi

echo ""
echo "Release files:"
for FNAME in "${SUCCEEDED[@]}"; do
    ls -la "$RELEASE_DIR/$FNAME" 2>/dev/null | awk '{printf "  %-55s (%s bytes)\n", $9, $5}'
done
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo -e "${YELLOW}Warning: ${#FAILED[@]} variant(s) failed to build${NC}"
fi
