/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HDMI_vga_hstx.c — VGA HSTX adapter for CPC.
 *
 * Exposes the same `graphics_*` API as the PIO HDMI driver but targets the
 * DispHSTX library's 320x240 RGB332 framebuffer (scaled 2x to 640x480 VGA
 * by DispHSTX).
 *
 * CPC frame (320x200 8-bit indexed) is centred inside the 320x240
 * framebuffer with 20-pixel vertical borders (matching CPC_SCREEN_LINES=240).
 * DispHSTX reads the framebuffer directly from its Core 1 VGA ISR.
 *
 * Audio: I2S on Core 0 (DispHSTX owns Core 1 for VGA scanout).
 */

#include "HDMI.h"

#include "pico/stdlib.h"

#include "disphstx.h"
#include "disphstx_vmode_simple.h"
#include "disphstx_vmode.h"

#include <string.h>

/* ---- DispHSTX framebuffer ------------------------------------------- */
#define FB_W 320
#define FB_H 240

static uint8_t __attribute__((aligned(4))) vga_framebuffer[FB_W * FB_H];

/* ---- Same externally-visible globals the frank-cpc code pokes. ------- */
int graphics_buffer_width   = FB_W;
int graphics_buffer_height  = FB_H;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

/* ---- Palette (RGB332) and raw RGB888 cache for greyscale toggle ----- */
static uint8_t  pal_rgb332[256];
static uint32_t pal_raw_rgb888[256];

static bool crt_active = false;
static bool greyscale_active = false;

/* ---- Current / pending source buffer (owned by platform.c) ---------- */
static const uint8_t *graphics_buffer = NULL;
static const uint8_t *pending_buffer  = NULL;

/* VGA is the only mode for this driver — no autodetection needed. */
bool SELECT_VGA = true;

/* ---- Helpers -------------------------------------------------------- */
static inline uint8_t rgb888_to_rgb332(uint32_t c888) {
    uint8_t r = (c888 >> 16) & 0xff;
    uint8_t g = (c888 >>  8) & 0xff;
    uint8_t b =  c888        & 0xff;
    return (uint8_t)((r & 0xe0) | ((g & 0xe0) >> 3) | ((b & 0xc0) >> 6));
}

static inline uint32_t rgb888_to_grey888(uint32_t c888) {
    uint8_t r = (c888 >> 16) & 0xff;
    uint8_t g = (c888 >>  8) & 0xff;
    uint8_t b =  c888        & 0xff;
    uint8_t Y = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    return ((uint32_t)Y << 16) | ((uint32_t)Y << 8) | Y;
}

/* Blit one CPC frame (320x240, already padded by platform.c) into the
 * DispHSTX framebuffer through the palette LUT. */
static void __not_in_flash("vga_blit") blit_cpc_frame(const uint8_t *src) {
    if (!src) return;
    const uint8_t *pal = pal_rgb332;

    for (int y = 0; y < FB_H; y++) {
        const uint8_t *srow = src + y * FB_W;
        uint8_t *drow = vga_framebuffer + y * FB_W;
        if (crt_active && (y & 1)) {
            memset(drow, 0, FB_W);
            continue;
        }
        int x = 0;
        int fast = FB_W & ~3;
        for (; x < fast; x += 4) {
            uint32_t p = *(const uint32_t *)(srow + x);
            drow[x + 0] = pal[ p        & 0xff];
            drow[x + 1] = pal[(p >> 8)  & 0xff];
            drow[x + 2] = pal[(p >> 16) & 0xff];
            drow[x + 3] = pal[(p >> 24)       ];
        }
        for (; x < FB_W; x++) drow[x] = pal[srow[x]];
    }
}

/* ---- Public API mirror ---------------------------------------------- */
void graphics_set_buffer(uint8_t *buffer) {
    pending_buffer = buffer;
    if (pending_buffer && pending_buffer != graphics_buffer)
        graphics_buffer = pending_buffer;
    if (graphics_buffer)
        blit_cpc_frame(graphics_buffer);
}

uint8_t *graphics_get_buffer(void) {
    return (uint8_t *)(graphics_buffer ? graphics_buffer : pending_buffer);
}

uint32_t graphics_get_width(void)  { return (uint32_t)graphics_buffer_width;  }
uint32_t graphics_get_height(void) { return (uint32_t)graphics_buffer_height; }

void graphics_set_res(int w, int h) {
    graphics_buffer_width  = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    pal_raw_rgb888[i] = color888 & 0x00ffffff;
    uint32_t c = greyscale_active ? rgb888_to_grey888(color888) : color888;
    pal_rgb332[i] = rgb888_to_rgb332(c);
}

uint32_t graphics_get_palette(uint8_t i) {
    return pal_raw_rgb888[i];
}

void graphics_set_mode(enum graphics_mode_t mode) {
    hdmi_graphics_mode = mode;
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_palette(0, color888);
}

void graphics_restore_sync_colors(void) {
    /* DispHSTX generates sync in hardware — nothing to restore. */
}

void graphics_set_crt_active(bool active) { crt_active       = active; }
bool graphics_get_crt_active(void)        { return crt_active;         }

void graphics_set_greyscale(bool active) {
    if (greyscale_active == active) return;
    greyscale_active = active;
    for (int i = 0; i < 256; i++) {
        uint32_t c = greyscale_active ? rgb888_to_grey888(pal_raw_rgb888[i])
                                      : pal_raw_rgb888[i];
        pal_rgb332[i] = rgb888_to_rgb332(c);
    }
}
bool graphics_get_greyscale(void) { return greyscale_active; }

struct video_mode_t graphics_get_video_mode(int mode) {
    (void)mode;
    struct video_mode_t vm = {
        .h_total  = 800,
        .h_width  = 640,
        .freq     = 60,
        .vgaPxClk = 25200000,
    };
    return vm;
}

/* Stubs — not needed for VGA_HSTX but called by shared code. */
int testPins(uint32_t pin0, uint32_t pin1) {
    (void)pin0; (void)pin1;
    return 0;  /* Always VGA */
}

void graphics_init_hdmi(void) { /* no-op */ }
void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) { (void)i; (void)color888; }
void graphics_set_bgcolor_hdmi(uint32_t color888) { (void)color888; }

/* Heartbeat diagnostic stubs (cpc.c references these from HDMI_vga.c). */
uint32_t vga_frame_number = 0;

void vga_diag(uint32_t *out_ctrl_ch, uint32_t *out_data_ch,
              bool *out_ctrl_busy, bool *out_data_busy) {
    *out_ctrl_ch = 0;  *out_data_ch = 0;
    *out_ctrl_busy = false;  *out_data_busy = false;
}

/* ====================================================================
 * graphics_init — bring up DispHSTX on Core 0. DispHSTX claims Core 1
 * itself for the VGA scanout ISR.
 * ==================================================================== */
void graphics_init(g_out out) {
    (void)out;

    for (int i = 0; i < 256; i++) {
        pal_raw_rgb888[i] = 0;
        pal_rgb332[i]     = 0;
    }
    memset(vga_framebuffer, 0, sizeof(vga_framebuffer));

    (void)DispVMode320x240x8_Fast(DISPHSTX_DISPMODE_VGA, vga_framebuffer);

    if (pending_buffer && !graphics_buffer) {
        graphics_buffer = pending_buffer;
        blit_cpc_frame(graphics_buffer);
    }
}
