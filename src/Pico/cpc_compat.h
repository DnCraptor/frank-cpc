/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_compat.h — Platform compatibility layer for Pico build.
 */
#pragma once

#include <stdint.h>
#include "board_config.h"

/* Platform hook declarations */
void cpc_init_palette(void);
void cpc_ps2_feed_events(void);
void cpc_autotype_tick(void);
void cpc_frame_sync(void);
void cpc_frame_present(void);
