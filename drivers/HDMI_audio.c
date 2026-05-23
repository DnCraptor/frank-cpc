/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HDMI_audio.c — Graphics + audio adapter for the frank-hdmi-sound driver.
 *
 * This file implements the HDMI-specific graphics_*_hdmi() entry points
 * by delegating to the frank-hdmi-sound library (frank_hdmi_*).  The
 * top-level graphics_init() / graphics_set_palette() / graphics_set_bgcolor()
 * dispatchers live in HDMI_vga.c and route to VGA or here based on the
 * runtime-detected SELECT_VGA flag.
 *
 * Compiled only when the build selects HDMI_DRIVER=HDMI_PIO_AUDIO.
 *
 * Video: 640x480p60 via libdvi on Core 1 (PIO0, 3 TMDS SMs).
 * Audio: 32 kHz stereo PCM embedded in HDMI data-island packets.
 *        No external DAC, no I2S — the HDMI cable carries everything.
 */

#include "board_config.h"
#include "HDMI.h"
#include "frank_hdmi.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdalign.h>

/* ------------------------------------------------------------------ */
/* Globals expected by HDMI_vga.c via extern                          */
/* ------------------------------------------------------------------ */

int graphics_buffer_width = 320;
int graphics_buffer_height = 240;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
enum graphics_mode_t hdmi_graphics_mode = GRAPHICSMODE_DEFAULT;

/* conv_color[] — HDMI.c uses this for TMDS palette conversion; in
 * HDMI_PIO_AUDIO mode we don't need it for HDMI, but HDMI_vga.c
 * reuses it as VGA line-pattern storage when SELECT_VGA is active. */
alignas(4096) uint32_t conv_color[1224];

/* Shadow of the framebuffer pointer in __scratch_y — the VGA ISR in
 * HDMI_vga.c reads from this.  Updated by graphics_set_buffer(). */
extern uint8_t *vga_fb;

/* SELECT_VGA is defined in HDMI_vga.c; we need it to guard frank_hdmi
 * calls that must not run when the library hasn't been initialized. */
extern bool SELECT_VGA;

static uint8_t *graphics_buffer = NULL;

/* CRT scanline effect and greyscale mode. */
static bool crt_active = false;
static bool greyscale_active = false;

/* Shadow palette in RGB888 for greyscale conversion and readback. */
static uint32_t palette_shadow[256];

/* ------------------------------------------------------------------ */
/* Greyscale helper                                                   */
/* ------------------------------------------------------------------ */

static inline uint32_t rgb_to_grey(uint32_t color888) {
    uint8_t R = (color888 >> 16) & 0xff;
    uint8_t G = (color888 >> 8)  & 0xff;
    uint8_t B = color888 & 0xff;
    uint8_t Y = (uint8_t)((R * 77 + G * 150 + B * 29) >> 8);
    return (Y << 16) | (Y << 8) | Y;
}

/* ------------------------------------------------------------------ */
/* graphics_* API — shared entry points                               */
/* ------------------------------------------------------------------ */

void graphics_set_buffer(uint8_t *buffer) {
    graphics_buffer = buffer;
    vga_fb = buffer;
    if (!SELECT_VGA)
        frank_hdmi_set_buffer(buffer, graphics_buffer_width, graphics_buffer_height);
}

uint8_t *graphics_get_buffer(void) {
    return graphics_buffer;
}

uint32_t graphics_get_width(void)  { return graphics_buffer_width;  }
uint32_t graphics_get_height(void) { return graphics_buffer_height; }

void graphics_set_res(int w, int h) {
    graphics_buffer_width  = w;
    graphics_buffer_height = h;
    if (!SELECT_VGA && graphics_buffer)
        frank_hdmi_set_buffer(graphics_buffer, w, h);
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    hdmi_graphics_mode = mode;
}

uint8_t *get_line_buffer(int line) {
    if (!graphics_buffer) return NULL;
    if (line < 0 || line >= graphics_buffer_height) return NULL;
    return graphics_buffer + line * graphics_buffer_width;
}

struct video_mode_t graphics_get_video_mode(int mode) {
    (void)mode;
    struct video_mode_t vm = {
        .h_total  = 524,
        .h_width  = 480,
        .freq     = 60,
        .vgaPxClk = 25175000,
    };
    return vm;
}

/* ------------------------------------------------------------------ */
/* HDMI-specific entry points (called by HDMI_vga.c dispatcher)       */
/* ------------------------------------------------------------------ */

void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) {
    color888 &= 0x00ffffffu;
    palette_shadow[i] = color888;
    uint32_t display = greyscale_active ? rgb_to_grey(color888) : color888;
    frank_hdmi_set_palette(i, display);
}

void graphics_set_bgcolor_hdmi(uint32_t color888) {
    graphics_set_palette_hdmi(0, color888);
}

uint32_t graphics_get_palette(uint8_t i) {
    return palette_shadow[i];
}

void graphics_restore_sync_colors(void) {
    /* frank-hdmi-sound handles sync symbols internally. */
}

static void graphics_convert_all_palette(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = palette_shadow[i];
        uint32_t display = greyscale_active ? rgb_to_grey(c) : c;
        frank_hdmi_set_palette((uint8_t)i, display);
    }
}

/* ---- CRT / greyscale ---- */

void graphics_set_crt_active(bool active) { crt_active = active; }
bool graphics_get_crt_active(void)        { return crt_active; }

void graphics_set_greyscale(bool active) {
    if (greyscale_active == active) return;
    greyscale_active = active;
    graphics_convert_all_palette();
}

bool graphics_get_greyscale(void) { return greyscale_active; }

/* ---- HDMI init ---- */

void graphics_init_hdmi(void) {
    frank_hdmi_init();
}

/* ---- Stubs ---- */

void startVIDEO(uint8_t vol) { (void)vol; }
void set_palette(uint8_t n)  { (void)n;   }

/* ------------------------------------------------------------------ */
/* Audio: route mono samples into HDMI data-island ring               */
/* ------------------------------------------------------------------ */

/* I2S ring buffer (defined in main.c) — used as VGA audio fallback. */
extern unsigned i2s_ring_push(const int16_t *samples, unsigned count);
extern unsigned i2s_ring_push_stereo(const int16_t *samples, unsigned count);
extern unsigned i2s_ring_free(void);

/*
 * audio_ring_push_mono — called by aysound.c to push one frame of AY
 * audio.  Routes to HDMI data-island (32 kHz) or I2S ring (44.1 kHz)
 * depending on whether VGA was detected at boot.
 */
unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
    if (SELECT_VGA)
        return i2s_ring_push(samples, count);
    int16_t stereo[640 * 2];
    if (count > 640) count = 640;
    for (unsigned i = 0; i < count; i++) {
        stereo[i * 2 + 0] = samples[i];
        stereo[i * 2 + 1] = samples[i];
    }
    return frank_hdmi_audio_write(stereo, count);
}

unsigned audio_ring_push_stereo(const int16_t *samples, unsigned count) {
    if (SELECT_VGA)
        return i2s_ring_push_stereo(samples, count);
    if (count > 640) count = 640;
    return frank_hdmi_audio_write(samples, count);
}

unsigned audio_ring_free(void) {
    if (SELECT_VGA)
        return i2s_ring_free();
    return frank_hdmi_audio_free();
}
