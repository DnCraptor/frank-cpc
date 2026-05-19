/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ui.h — settings overlay UI (toggle with F12).
 */
#ifndef CPC_UI_H
#define CPC_UI_H

#include <stdint.h>
#include <stdbool.h>

/* Call once during platform init to install UI palette entries. */
void cpc_ui_init(void);

/* True while the overlay is visible. */
bool cpc_ui_is_visible(void);

/* Toggle the settings overlay (mapped to F12). */
void cpc_ui_toggle(void);

/* Non-blocking key event.  Returns true if consumed by the UI.
 * Key codes use the same KS_* constants as platform.c. */
bool cpc_ui_handle_key(unsigned int ks);

/* Render the overlay onto the supplied 8-bpp framebuffer.
 * stride = row width in bytes (CPC_FB_WIDTH = 320),
 * height = number of rows (CPC_SCREEN_LINES = 240). */
void cpc_ui_render(uint8_t *fb, int stride, int height);

#endif /* CPC_UI_H */
