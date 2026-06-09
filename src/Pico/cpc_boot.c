/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * cpc_boot.c — boot-time welcome screen.
 *
 * Draws directly into SCREEN[] before cpc_pico_main() starts.
 */

#include "cpc_boot.h"

#include "ui_draw.h"
#include "board_config.h"
#include "HDMI.h"
#include "usbhid_wrapper.h"
#include "ps2kbd_wrapper.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define FB_W CPC_FB_WIDTH
#define FB_H CPC_SCREEN_LINES

extern uint8_t *SCREEN[2];
extern volatile uint32_t current_buffer;

#define BOOT_COLOR_BG        230
#define BOOT_COLOR_STAR_FAR  231
#define BOOT_COLOR_STAR_MID  232
#define BOOT_COLOR_STAR_NEAR 233
#define BOOT_COLOR_LOGO      234
#define BOOT_COLOR_LOGO_SH   235
#define BOOT_COLOR_SUBTLE    236
#define BOOT_COLOR_BLINK     237

static void install_boot_palette(void) {
    graphics_set_palette(BOOT_COLOR_BG,        0x0a0e1c);
    graphics_set_palette(BOOT_COLOR_STAR_FAR,  0x3a4056);
    graphics_set_palette(BOOT_COLOR_STAR_MID,  0x8088a8);
    graphics_set_palette(BOOT_COLOR_STAR_NEAR, 0xf0f0f0);
    graphics_set_palette(BOOT_COLOR_LOGO,      0xffffff);
    graphics_set_palette(BOOT_COLOR_LOGO_SH,   0x000000);
    graphics_set_palette(BOOT_COLOR_SUBTLE,    0x9fb2d0);
    graphics_set_palette(BOOT_COLOR_BLINK,     0xffd060);
}

static inline void fb_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    if ((unsigned)x >= (unsigned)FB_W) return;
    if ((unsigned)y >= (unsigned)FB_H) return;
    fb[y * FB_W + x] = color;
}

static void fb_fill(uint8_t *fb, uint8_t color) {
    memset(fb, color, (size_t)FB_W * FB_H);
}

static void fb_text(uint8_t *fb, int x, int y, const char *s, uint8_t color) {
    ui_draw_string(fb, FB_W, x, y, s, color);
}

static void fb_text_center_shadow(uint8_t *fb, int y, const char *s,
                                  uint8_t fg, uint8_t sh) {
    int w = (int)strlen(s) * UI_CHAR_W;
    int x = (FB_W - w) / 2;
    fb_text(fb, x + 1, y + 1, s, sh);
    fb_text(fb, x, y, s, fg);
}

static void fb_char_scaled(uint8_t *fb, int x, int y, char c, int scale, uint8_t color) {
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = ui_font_6x8[(int)c - 32];
    for (int row = 0; row < UI_CHAR_H; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < UI_CHAR_W; ++col) {
            if (bits & (0x80 >> col)) {
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx)
                        fb_pixel(fb, x + col * scale + dx, y + row * scale + dy, color);
            }
        }
    }
}

static int fb_text_scaled_width(const char *s, int scale) {
    return (int)strlen(s) * UI_CHAR_W * scale;
}

static void fb_text_scaled_center(uint8_t *fb, int y, const char *s, int scale,
                                  uint8_t fg, uint8_t sh) {
    int w = fb_text_scaled_width(s, scale);
    int x = (FB_W - w) / 2;
    for (size_t i = 0; s[i]; ++i) {
        int cx = x + (int)i * UI_CHAR_W * scale;
        fb_char_scaled(fb, cx + scale, y + scale, s[i], scale, sh);
        fb_char_scaled(fb, cx, y, s[i], scale, fg);
    }
}

static bool any_input_pressed(void) {
    bool pressed = false;
    int down;
    unsigned char sc;

    ps2kbd_tick();
    while (ps2kbd_get_key(&down, &sc)) {
        if (down) pressed = true;
    }

    usbhid_wrapper_tick();
    while (usbhid_wrapper_get_key(&down, &sc)) {
        if (down) pressed = true;
    }
    if (usbhid_wrapper_get_joystick() != 0) pressed = true;

    return pressed;
}

static uint8_t *boot_back(void) {
    return SCREEN[current_buffer];
}

static void boot_flip(void) {
    uint32_t next = current_buffer ^ 1u;
    graphics_set_buffer(SCREEN[current_buffer]);
    current_buffer = next;
}

static void boot_clear_front(uint8_t color) {
    memset(SCREEN[!current_buffer], color, (size_t)FB_W * FB_H);
}

static void sleep_vsync(void) {
    sleep_us(16666);
}

#define STAR_COUNT 48

typedef struct {
    int32_t x;
    int32_t y;
    int16_t vx;
    uint8_t color;
} star_t;

static uint32_t star_rng(uint32_t *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static void init_starfield(star_t stars[STAR_COUNT], uint32_t *rng_state) {
    const uint8_t tier_color[3] = {
        BOOT_COLOR_STAR_FAR,
        BOOT_COLOR_STAR_MID,
        BOOT_COLOR_STAR_NEAR,
    };
    const int16_t tier_vx[3] = { -24, -64, -128 };
    for (int i = 0; i < STAR_COUNT; ++i) {
        int tier = (int)(star_rng(rng_state) % 3);
        stars[i].x = (int32_t)(star_rng(rng_state) % (FB_W << 8));
        stars[i].y = (int32_t)(star_rng(rng_state) % (FB_H << 8));
        stars[i].vx = tier_vx[tier];
        stars[i].color = tier_color[tier];
    }
}

static void tick_starfield(uint8_t *fb, star_t stars[STAR_COUNT]) {
    for (int i = 0; i < STAR_COUNT; ++i) {
        stars[i].x += stars[i].vx;
        if (stars[i].x < 0) stars[i].x += (FB_W << 8);
        fb_pixel(fb, stars[i].x >> 8, stars[i].y >> 8, stars[i].color);
    }
}

void cpc_boot_welcome(uint32_t timeout_ms) {
    star_t stars[STAR_COUNT];
    uint32_t rng_state = 0xC0FFEE17u;

    install_boot_palette();
    init_starfield(stars, &rng_state);
    boot_clear_front(BOOT_COLOR_BG);

    uint64_t t0 = time_us_64();
    uint32_t frame = 0;

    (void)any_input_pressed();

    while (true) {
        uint64_t now = time_us_64();
        if ((now - t0) / 1000 >= timeout_ms) break;

        uint8_t *fb = boot_back();
        fb_fill(fb, BOOT_COLOR_BG);
        tick_starfield(fb, stars);

        fb_text_scaled_center(fb, 72, "FRANK CPC", 4, BOOT_COLOR_LOGO, BOOT_COLOR_LOGO_SH);

#ifdef FRANK_CPC_VERSION
        char vline[24];
        snprintf(vline, sizeof(vline), "v%s", FRANK_CPC_VERSION);
        fb_text_center_shadow(fb, 120, vline, BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
#endif
        fb_text_center_shadow(fb, 140, "Amstrad CPC for RP2350", BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
        fb_text_center_shadow(fb, 154, "by Mikhail Matveev", BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);
        fb_text_center_shadow(fb, 168, "github.com/rh1tech/frank-cpc", BOOT_COLOR_SUBTLE, BOOT_COLOR_LOGO_SH);

        if (frame >= 60 && ((frame / 30) & 1u) == 0u) {
            fb_text_center_shadow(fb, 200, "PRESS ANY KEY", BOOT_COLOR_BLINK, BOOT_COLOR_LOGO_SH);
        }

        boot_flip();
        ++frame;

        if (frame >= 30 && any_input_pressed()) break;
        sleep_vsync();
    }
}
