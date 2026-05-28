/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Composite-TV video shim for frank-cpc.
 *
 * Wraps drivers/tv/ (the software-composite PAL/NTSC driver ported from
 * murmnes) behind the same HDMI.h API the rest of frank-cpc uses, so
 * platform.c / main.c / the UI never need to know which physical
 * video path is active.
 *
 * Only compiled when VIDEO_COMPOSITE=ON.  In that build the PIO HDMI /
 * VGA / HSTX drivers are all excluded — the TV driver owns PIO0,
 * three DMA channels and a Core-1 alarm pool.
 *
 * Layout:
 *   Core 0 → CPC emulation.  graphics_set_buffer() copies the current
 *            320×240 CPC front-buffer into the TV framebuffer.
 *   Core 1 → tv_core1_run(): calls tv_graphics_init(), which claims
 *            PIO0 + DMA and registers a 30 kHz repeating timer on
 *            this core; the IRQ fills scanline buffers.  The core
 *            then idles in __wfi().
 */

#include "board_config.h"
#include "HDMI.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* The TV driver's exported symbols are renamed to tv_* via tv_rename.h
 * so that they don't collide with the HDMI.h API.  We don't include the
 * TV header here — just forward-declare what we call. */
enum tv_graphics_mode_t { TV_TEXTMODE_DEFAULT, TV_GRAPHICSMODE_DEFAULT };

extern void tv_graphics_init(void);
extern void tv_graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height);
extern void tv_graphics_set_mode(int mode);
extern void tv_graphics_set_palette(uint8_t i, uint32_t color888);
extern void tv_graphics_set_offset(int x, int y);

/* ------------------------------------------------------------------ */
/* SELECT_VGA / testPins stubs — no VGA autodetect on composite. */
bool SELECT_VGA = false;

int testPins(uint32_t pin0, uint32_t pin1) {
    (void)pin0; (void)pin1;
    return 0xFF;
}

/* ------------------------------------------------------------------ */
#define TV_FB_W     320
#define TV_FB_H     240

/* The TV scanline IRQ reads this framebuffer without locking; a memcpy
 * from Core 0 can tear a single line mid-scan, but on 60 Hz composite
 * that's invisible. */
static uint8_t __attribute__((aligned(4))) tv_frame[TV_FB_W * TV_FB_H];

/* ------------------------------------------------------------------ */
static int tv_buffer_width  = TV_FB_W;
static int tv_buffer_height = TV_FB_H;
static int tv_shift_x = 0;
static int tv_shift_y = 0;
static volatile bool tv_core1_ready = false;

/* Palette slots 200..215 are reserved by the TV driver for its
 * hard-coded CGA-16 text palette. */
#define TV_PALETTE_RESERVED_LO  200
#define TV_PALETTE_RESERVED_HI  215

/* ------------------------------------------------------------------ */
/* Core 1 entry point. */
static void tv_core1_run(void) {
    tv_graphics_init();
    tv_graphics_set_buffer(tv_frame, TV_FB_W, TV_FB_H);
    tv_graphics_set_mode(TV_GRAPHICSMODE_DEFAULT);
    /* CPC framebuffer is exactly 320 px wide — the TV active area is
     * ~320 source pixels, so no horizontal offset needed. */
    tv_graphics_set_offset(0, 0);
    tv_graphics_set_palette(TV_PALETTE_RESERVED_LO, 0x000000);
    __dmb();
    tv_core1_ready = true;
    __dmb();
    while (true) __wfi();
}

/* ------------------------------------------------------------------ */
/* HDMI.h API surface */

void graphics_init(g_out g) {
    (void)g;
    if (tv_core1_ready) return;
    memset(tv_frame, 0, sizeof(tv_frame));
    multicore_launch_core1(tv_core1_run);
    while (!tv_core1_ready) tight_loop_contents();
}

/* SRAM-resident aligned-word copy — avoids flash XIP cache thrash. */
static void __not_in_flash("tv_blit") tv_blit_frame(uint8_t *dst,
                                                    const uint8_t *src,
                                                    size_t n_words) {
    uint32_t *d = (uint32_t *)dst;
    const uint32_t *s = (const uint32_t *)src;
    while (n_words >= 8) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
        d += 8; s += 8; n_words -= 8;
    }
    while (n_words--) *d++ = *s++;
}

void __not_in_flash_func(graphics_set_buffer)(uint8_t *buffer) {
    if (!buffer) return;
    /* CPC framebuffer is exactly 320×240 — copy the whole thing. */
    tv_blit_frame(tv_frame, buffer,
                  ((size_t)TV_FB_W * TV_FB_H) >> 2);
}

uint8_t *graphics_get_buffer(void) { return tv_frame; }

uint32_t graphics_get_width(void)  { return (uint32_t)tv_buffer_width;  }
uint32_t graphics_get_height(void) { return (uint32_t)tv_buffer_height; }

void graphics_set_res(int w, int h) {
    tv_buffer_width  = w;
    tv_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    tv_shift_x = x;
    tv_shift_y = y;
}

void __not_in_flash_func(graphics_set_palette)(uint8_t i, uint32_t color888) {
    if (!tv_core1_ready) return;
    if (i >= TV_PALETTE_RESERVED_LO && i <= TV_PALETTE_RESERVED_HI) return;
    tv_graphics_set_palette(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_palette(0, color888);
}

uint32_t graphics_get_palette(uint8_t i) {
    (void)i;
    return 0;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    if (!tv_core1_ready) return;
    tv_graphics_set_mode((int)mode == TEXTMODE_DEFAULT
                         ? TV_TEXTMODE_DEFAULT
                         : TV_GRAPHICSMODE_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* Stubs: HDMI-only features that the TV path doesn't support. */
void graphics_set_crt_active(bool active)   { (void)active; }
bool graphics_get_crt_active(void)          { return false; }
void graphics_set_greyscale(bool active)    { (void)active; }
bool graphics_get_greyscale(void)           { return false; }
void graphics_restore_sync_colors(void)     { }

void graphics_init_hdmi(void)                         { }
void graphics_set_palette_hdmi(uint8_t i, uint32_t c) { (void)i; (void)c; }
void graphics_set_bgcolor_hdmi(uint32_t c)            { (void)c; }

void startVIDEO(uint8_t vol) { (void)vol; }
void set_palette(uint8_t n)  { (void)n;  }
