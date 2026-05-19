#!/bin/bash
#
# frank-cpc — CPC emulator for RP2350
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release.sh - build release UF2 for frank-cpc (M2 platform)
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

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

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
if [[ -n "$1" ]]; then
    INPUT_VERSION="$1"
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

OUTPUT_NAME="frank-cpc_m2-hdmi-pio_${VERSION}.uf2"

rm -rf build
mkdir build
cd build

if cmake .. \
    -DPICO_PLATFORM=rp2350 \
    -DPLATFORM=m2 \
    > /dev/null 2>&1 && \
   make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) > /dev/null 2>&1; then
    if [[ -f "frank-cpc.uf2" ]]; then
        cp "frank-cpc.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
        echo -e "  ${GREEN}✓ m2-hdmi-pio${NC} → release/$OUTPUT_NAME"
    else
        echo -e "  ${RED}✗ UF2 not found${NC}"
    fi
else
    echo -e "  ${RED}✗ Build failed${NC}"
fi

cd "$SCRIPT_DIR"
rm -rf build

echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
echo ""
