/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * cpc_compat.h — Stubs replacing X11 types and APIs for the Pico build.
 *
 * Included by cpc.h when PICO_BUILD is defined. Provides dummy types for
 * all X11 structures that are declared as extern globals in cpc.h, plus
 * the CPC_SETPIX macro for framebuffer writes.
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include "board_config.h"

typedef void    Display;
typedef void   *Drawable;
typedef void   *GC;
typedef struct { int type; } XEvent;
typedef struct { int x, y, width, height; } XWindowAttributes;
typedef unsigned long XID;
typedef XID     Window;

typedef struct {
    int width, height;
} XImage;

extern uint8_t cpc_fb[CPC_FB_HEIGHT][CPC_FB_WIDTH];

#define CPC_SETPIX(x, y, ink) \
    do { \
        int _x = (int)(x) >> 1; \
        int _y = (int)(y) >> 1; \
        if ((unsigned)_x < CPC_FB_WIDTH && (unsigned)_y < CPC_FB_HEIGHT) \
            cpc_fb[_y][_x] = (uint8_t)(ink); \
    } while(0)

#define XPutPixel(img, x, y, color)  CPC_SETPIX(x, y, color)

void cpc_init_palette(void);
void cpc_ps2_feed_events(void);
void cpc_autotype_tick(void);
void cpc_frame_sync(void);
void cpc_frame_present(void);
