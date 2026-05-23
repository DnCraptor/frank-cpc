/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_compat.h — Platform compatibility layer for Pico build.
 */
#pragma once

#include <stdint.h>
#include "board_config.h"

extern uint8_t cpc_fb[CPC_FB_HEIGHT][CPC_FB_WIDTH];

/* Platform hook declarations */
void cpc_init_palette(void);
void cpc_ps2_feed_events(void);
void cpc_autotype_tick(void);
void cpc_frame_sync(void);
void cpc_frame_present(void);
