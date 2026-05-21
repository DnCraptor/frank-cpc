/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HDMI_audio.c — Graphics + audio adapter for the frank-hdmi-sound driver.
 *
 * This file implements the graphics_* API (HDMI.h) by delegating to the
 * frank-hdmi-sound library (frank_hdmi_*).  Compiled only when the build
 * selects HDMI_DRIVER=HDMI_PIO_AUDIO.
 *
 * Video: 640x480p60 via libdvi on Core 1 (PIO0, 3 TMDS SMs).
 * Audio: 32 kHz stereo PCM embedded in HDMI data-island packets.
 *        No external DAC, no I2S — the HDMI cable carries everything.
 *
 * VGA autodetection is not supported in this driver path; SELECT_VGA
 * is always false.
 */

#include "board_config.h"
#include "HDMI.h"
#include "frank_hdmi.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Graphics state                                                     */
/* ------------------------------------------------------------------ */

static int gfx_buf_width  = 320;
static int gfx_buf_height = 240;
static int gfx_shift_x    = 0;
static int gfx_shift_y    = 0;
static uint8_t *gfx_buffer = NULL;

static enum graphics_mode_t gfx_mode = GRAPHICSMODE_DEFAULT;
static bool crt_active = false;
static bool greyscale_active = false;

/* Shadow palette in RGB888 for greyscale conversion and readback. */
static uint32_t palette_shadow[256];

/* VGA is not supported in this driver path. */
bool SELECT_VGA = false;

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
/* graphics_* API implementation                                      */
/* ------------------------------------------------------------------ */

int testPins(uint32_t pin0, uint32_t pin1) {
    /* Stub: HDMI_PIO_AUDIO always outputs HDMI. Return non-zero
     * so main.c sets SELECT_VGA = false. */
    (void)pin0;
    (void)pin1;
    return 0x0A;
}

void graphics_set_buffer(uint8_t *buffer) {
    gfx_buffer = buffer;
    frank_hdmi_set_buffer(buffer, gfx_buf_width, gfx_buf_height);
}

uint8_t *graphics_get_buffer(void) {
    return gfx_buffer;
}

uint32_t graphics_get_width(void)  { return gfx_buf_width;  }
uint32_t graphics_get_height(void) { return gfx_buf_height; }

void graphics_set_res(int w, int h) {
    gfx_buf_width  = w;
    gfx_buf_height = h;
    if (gfx_buffer)
        frank_hdmi_set_buffer(gfx_buffer, w, h);
}

void graphics_set_shift(int x, int y) {
    gfx_shift_x = x;
    gfx_shift_y = y;
    /* frank-hdmi-sound centres the buffer automatically; shifts are
     * absorbed by frank_hdmi_set_buffer's pillarbox/letterbox logic. */
}

void graphics_set_mode(enum graphics_mode_t mode) {
    gfx_mode = mode;
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

/* ---- Palette ---- */

void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) {
    color888 &= 0x00ffffffu;
    palette_shadow[i] = color888;
    uint32_t display = greyscale_active ? rgb_to_grey(color888) : color888;
    frank_hdmi_set_palette(i, display);
}

void graphics_set_bgcolor_hdmi(uint32_t color888) {
    /* Use palette index 0 as background (black by convention). */
    graphics_set_palette_hdmi(0, color888);
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    graphics_set_palette_hdmi(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_bgcolor_hdmi(color888);
}

uint32_t graphics_get_palette(uint8_t i) {
    return palette_shadow[i];
}

void graphics_restore_sync_colors(void) {
    /* No TMDS sync-control indices to restore; frank-hdmi-sound
     * handles sync symbols internally in the DVI engine. */
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

/* ---- Init ---- */

void graphics_init_hdmi(void) {
    frank_hdmi_init();
}

void graphics_init(g_out go) {
    (void)go;
    frank_hdmi_init();
}

void startVIDEO(uint8_t vol) { (void)vol; }
void set_palette(uint8_t n)  { (void)n;   }

/* ------------------------------------------------------------------ */
/* Audio: route mono samples into HDMI data-island ring               */
/* ------------------------------------------------------------------ */

/*
 * audio_ring_push_mono — called by aysound.c to push one frame of AY
 * audio.  In HDMI_PIO_AUDIO mode we convert mono int16 samples to
 * stereo and push them into the frank-hdmi-sound audio ring, which
 * embeds them as HDMI data-island packets on Core 1.
 *
 * The AY generator runs at FRANK_HDMI_AUDIO_RATE (32 kHz) in this
 * build, producing nb_samples = 32000/50 = 640 samples per frame.
 */
unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
    /* Convert mono to interleaved stereo on the stack. */
    int16_t stereo[640 * 2];
    if (count > 640) count = 640;
    for (unsigned i = 0; i < count; i++) {
        stereo[i * 2 + 0] = samples[i];
        stereo[i * 2 + 1] = samples[i];
    }
    return frank_hdmi_audio_write(stereo, count);
}

unsigned audio_ring_free(void) {
    return frank_hdmi_audio_free();
}
