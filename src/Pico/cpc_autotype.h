/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_autotype.h — automatic key injection for unattended boot / testing.
 *
 * Usage:
 *   cpc_autotype_set("RUN\"PRINCE\r");   // \r = Enter; call after CPC init
 *   ...in cpc_ps2_feed_events():
 *   cpc_autotype_tick();                  // call once per frame (50 fps)
 */
#pragma once

/* Queue a NUL-terminated string of CPC keypresses.
 * Supports printable ASCII (32-125) and \r for Return.
 * boot_delay_frames: number of 50fps frames to wait before starting
 *   (150 = 3 s, 250 = 5 s).  Pass 0 to start immediately. */
void cpc_autotype_set(const char *text, unsigned int boot_delay_frames);

/* Called once per frame from cpc_ps2_feed_events().
 * Injects the next keypress/release when the time is right. */
void cpc_autotype_tick(void);
