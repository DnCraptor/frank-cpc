/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * platform.c — CPC Pico platform hooks.
 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "board_config.h"
#include "HDMI.h"
#include "cpc_compat.h"
#include "ps2kbd_wrapper.h"
#include "cpc_adapter.h"
#include "cpc_settings.h"
#include "cpc_ui.h"
#include "cpc_loader.h"
#include "cpc_autotype.h"
#include "cpc_tape_loader.h"
#include "cpc_serial_console.h"
#include "tape.h"
#include "nespad.h"
#include "crash_handler.h"
#include "usbhid_wrapper.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

extern uint8_t *SCREEN[2];
extern volatile uint32_t current_buffer;

void cpc_init_palette(void) {
    uint32_t rgb[32];
    cpc_get_palette_rgb(rgb);
    for (int i = 0; i < 32; i++)
        graphics_set_palette((uint8_t)i, rgb[i]);
    graphics_set_bgcolor(0x000000);
}

volatile bool g_screenshot_pending = false;

void cpc_frame_present(void) {
#if !defined(VGA_HSTX) && !defined(VIDEO_COMPOSITE)
    {
        extern bool SELECT_VGA;
        extern uint8_t * volatile pending_vga_fb;
        if (SELECT_VGA) {
            for (int i = 0; i < 4000000 && pending_vga_fb; i++)
                tight_loop_contents();
            if (pending_vga_fb) {
                return;
            }
        }
    }
#endif

    uint8_t border = cpc_get_border_ink();

    {
        static uint8_t last_border = 0xFF;
        if (border != last_border) {
            last_border = border;
            uint32_t rgb[32];
            cpc_get_palette_rgb(rgb);
            graphics_set_bgcolor(rgb[border]);
        }
    }

    const int top_pad = (CPC_SCREEN_LINES - CPC_FB_HEIGHT) / 2;
    uint8_t *dst = SCREEN[current_buffer];

    /* Scanlines were rendered directly into dst + top_pad*stride by the
     * scanline callback.  Just fill the border rows. */
    memset(dst, border, (size_t)(CPC_FB_WIDTH * top_pad));
    memset(dst + CPC_FB_WIDTH * (top_pad + CPC_FB_HEIGHT), border,
           (size_t)(CPC_FB_WIDTH * (CPC_SCREEN_LINES - top_pad - CPC_FB_HEIGHT)));

    /* Deferred screenshot: save clean CPC frame before UI overlay */
    if (g_screenshot_pending) {
        g_screenshot_pending = false;
        cpc_screenshot_save();
    }

    if (cpc_ui_is_visible())
        cpc_ui_render(dst, CPC_FB_WIDTH, CPC_SCREEN_LINES);

    graphics_set_buffer(dst);
    current_buffer ^= 1u;
}

static uint64_t g_next_frame_us = 0;
#define FRAME_PERIOD_US 20000

/* Audio-driven frame sync: pace emulation so the I2S ring buffer stays fed.
 * Target: keep ring ~2 chunks (1764 frames) ahead.  If ring is getting empty,
 * skip the wait so emulation runs full-speed to catch up.  If ring is healthy,
 * wait the normal 20ms to avoid running ahead. */
extern unsigned audio_ring_avail(void);

void cpc_frame_sync(void) {
    if (g_cpc_settings.fast_tape && cpc_tape_is_loaded() && cpc_tape_get_motor()) {
        g_next_frame_us = time_us_64() + FRAME_PERIOD_US;
        return;
    }

    /* If audio ring has fewer than 2 chunks buffered, skip wait entirely
     * to let emulation run as fast as possible and refill the ring. */
    unsigned avail = audio_ring_avail();
    if (avail < 882 * 2) {
        g_next_frame_us = time_us_64() + FRAME_PERIOD_US;
        return;
    }

    /* Ring is healthy — sync to wall clock as normal */
    if (g_next_frame_us == 0)
        g_next_frame_us = time_us_64() + FRAME_PERIOD_US;

    uint64_t now = time_us_64();
    if (now < g_next_frame_us) {
        while (time_us_64() < g_next_frame_us) tight_loop_contents();
        g_next_frame_us += FRAME_PERIOD_US;
    } else if (now - g_next_frame_us > 2 * FRAME_PERIOD_US) {
        g_next_frame_us = now + FRAME_PERIOD_US;
    } else {
        g_next_frame_us += FRAME_PERIOD_US;
    }
}

#define KS_Return      0xff0d
#define KS_BackSpace   0xff08
#define KS_Tab         0xff09
#define KS_Escape      0xff1b
#define KS_Delete      0xffff
#define KS_Up          0xff52
#define KS_Down        0xff54
#define KS_Left        0xff51
#define KS_Right       0xff53
#define KS_Page_Up     0xff55
#define KS_Page_Down   0xff56
#define KS_F1          0xffbe
#define KS_F2          0xffbf
#define KS_F3          0xffc0
#define KS_F4          0xffc1
#define KS_F5          0xffc2
#define KS_F6          0xffc3
#define KS_F7          0xffc4
#define KS_F8          0xffc5
#define KS_F9          0xffc6
#define KS_F10         0xffc7
#define KS_F11         0xffc8
#define KS_F12         0xffc9
#define KS_Scroll_Lock 0xff14
#define KS_Shift_L     0xffe1
#define KS_Shift_R     0xffe2
#define KS_Ctrl_L      0xffe3
#define KS_Ctrl_R      0xffe4
#define KS_Caps        0xffe5
#define KS_Alt_L       0xffe9

enum {
    PSC_Escape       = 0x01,
    PSC_1            = 0x02,
    PSC_2            = 0x03,
    PSC_3            = 0x04,
    PSC_4            = 0x05,
    PSC_5            = 0x06,
    PSC_6            = 0x07,
    PSC_7            = 0x08,
    PSC_8            = 0x09,
    PSC_9            = 0x0A,
    PSC_0            = 0x0B,
    PSC_Minus        = 0x0C,
    PSC_Equals       = 0x0D,
    PSC_BackSpace    = 0x0E,
    PSC_Tab          = 0x0F,
    PSC_Q            = 0x10,
    PSC_W            = 0x11,
    PSC_E            = 0x12,
    PSC_R            = 0x13,
    PSC_T            = 0x14,
    PSC_Y            = 0x15,
    PSC_U            = 0x16,
    PSC_I            = 0x17,
    PSC_O            = 0x18,
    PSC_P            = 0x19,
    PSC_LBr          = 0x1A,
    PSC_RBr          = 0x1B,
    PSC_Return       = 0x1C,
    PSC_LCtrl        = 0x1D,
    PSC_A            = 0x1E,
    PSC_S            = 0x1F,
    PSC_D            = 0x20,
    PSC_F            = 0x21,
    PSC_G            = 0x22,
    PSC_H            = 0x23,
    PSC_J            = 0x24,
    PSC_K            = 0x25,
    PSC_L            = 0x26,
    PSC_Semicolon    = 0x27,
    PSC_Quote        = 0x28,
    PSC_BackQuote    = 0x29,
    PSC_LShift       = 0x2A,
    PSC_BackSlash    = 0x2B,
    PSC_Z            = 0x2C,
    PSC_X            = 0x2D,
    PSC_C            = 0x2E,
    PSC_V            = 0x2F,
    PSC_B            = 0x30,
    PSC_N            = 0x31,
    PSC_M            = 0x32,
    PSC_Comma        = 0x33,
    PSC_Period       = 0x34,
    PSC_Slash        = 0x35,
    PSC_RShift       = 0x36,
    PSC_LAlt         = 0x38,
    PSC_Space        = 0x39,
    PSC_CapsLock     = 0x3A,
    PSC_F1           = 0x3B,
    PSC_F2           = 0x3C,
    PSC_F3           = 0x3D,
    PSC_F4           = 0x3E,
    PSC_F5           = 0x3F,
    PSC_F6           = 0x40,
    PSC_F7           = 0x41,
    PSC_F8           = 0x42,
    PSC_F9           = 0x43,
    PSC_F10          = 0x44,
    PSC_ScrollLock   = 0x46,
    PSC_F11          = 0x57,
    PSC_F12          = 0x58,
    PSC_Up           = 0x5A,
    PSC_Insert       = 0x5E,
    PSC_Delete       = 0x5F,
    PSC_Home         = 0x61,
    PSC_End          = 0x62,
    PSC_PgUp         = 0x63,
    PSC_PgDn         = 0x64,
    PSC_RAlt         = 0x65,
    PSC_RCtrl        = 0x66,
    PSC_KP_Enter     = 0x68,
    PSC_Down         = 0x6A,
    PSC_Left         = 0x6B,
    PSC_Right        = 0x6C,
};

static unsigned int scancode_to_keysym(unsigned int sc, bool shifted) {
    static const struct { unsigned int sc; char normal; char shifted; } ascii_map[] = {
        {PSC_Space,     ' ', ' '},
        {PSC_1,         '1', '!'}, {PSC_2, '2', '"'}, {PSC_3, '3', '#'},
        {PSC_4,         '4', '$'}, {PSC_5, '5', '%'}, {PSC_6, '6', '&'},
        {PSC_7,         '7', '\''}, {PSC_8, '8', '('}, {PSC_9, '9', ')'},
        {PSC_0,         '0', '_'}, {PSC_Minus, '-', '='}, {PSC_Equals, '=', '+'},
        {PSC_Q,         'q', 'Q'}, {PSC_W, 'w', 'W'}, {PSC_E, 'e', 'E'}, {PSC_R, 'r', 'R'},
        {PSC_T,         't', 'T'}, {PSC_Y, 'y', 'Y'}, {PSC_U, 'u', 'U'}, {PSC_I, 'i', 'I'},
        {PSC_O,         'o', 'O'}, {PSC_P, 'p', 'P'},
        {PSC_A,         'a', 'A'}, {PSC_S, 's', 'S'}, {PSC_D, 'd', 'D'}, {PSC_F, 'f', 'F'},
        {PSC_G,         'g', 'G'}, {PSC_H, 'h', 'H'}, {PSC_J, 'j', 'J'}, {PSC_K, 'k', 'K'},
        {PSC_L,         'l', 'L'},
        {PSC_Z,         'z', 'Z'}, {PSC_X, 'x', 'X'}, {PSC_C, 'c', 'C'}, {PSC_V, 'v', 'V'},
        {PSC_B,         'b', 'B'}, {PSC_N, 'n', 'N'}, {PSC_M, 'm', 'M'},
        {PSC_Comma,     ',', '<'}, {PSC_Period, '.', '>'}, {PSC_Slash, '/', '?'},
        {PSC_Semicolon, ';', ':'}, {PSC_Quote, '\'', '@'}, {PSC_LBr, '[', '{'},
        {PSC_RBr,       ']', '}'}, {PSC_BackSlash, '\\', '|'}, {PSC_BackQuote, '`', '~'},
        {0, 0, 0}
    };

    for (int i = 0; ascii_map[i].sc; ++i) {
        if (ascii_map[i].sc == sc)
            return (unsigned int)(shifted ? ascii_map[i].shifted : ascii_map[i].normal);
    }

    switch (sc) {
        case PSC_Return:    return KS_Return;
        case PSC_BackSpace: return KS_BackSpace;
        case PSC_Tab:       return KS_Tab;
        case PSC_Escape:    return KS_Escape;
        case PSC_Delete:    return KS_Delete;
        case PSC_Up:        return KS_Up;
        case PSC_Down:      return KS_Down;
        case PSC_Left:      return KS_Left;
        case PSC_Right:     return KS_Right;
        case PSC_PgUp:      return KS_Page_Up;
        case PSC_PgDn:      return KS_Page_Down;
        case PSC_F1:        return KS_F1;
        case PSC_F2:        return KS_F2;
        case PSC_F3:        return KS_F3;
        case PSC_F4:        return KS_F4;
        case PSC_F5:        return KS_F5;
        case PSC_F6:        return KS_F6;
        case PSC_F7:        return KS_F7;
        case PSC_F8:        return KS_F8;
        case PSC_F9:        return KS_F9;
        case PSC_F10:       return KS_F10;
        case PSC_ScrollLock:return KS_Scroll_Lock;
        case PSC_F11:       return KS_F11;
        case PSC_F12:       return KS_F12;
        case PSC_LShift:    return KS_Shift_L;
        case PSC_RShift:    return KS_Shift_R;
        case PSC_LCtrl:     return KS_Ctrl_L;
        case PSC_RCtrl:     return KS_Ctrl_R;
        case PSC_CapsLock:  return KS_Caps;
        case PSC_LAlt:
        case PSC_RAlt:      return KS_Alt_L;
        default:            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Direct PS/2 scancode → CPC keyboard matrix mapping.                */
/* Each entry is {row, bit}.  row>=10 means unmapped.                 */
/* This bypasses the stateful keysym-based CPCKeyPress/Release,       */
/* eliminating sticky-key bugs from shift-state coupling.             */
/* ------------------------------------------------------------------ */
#define CPC_MATRIX_SIZE 0x6D  /* one past highest mapped scancode */
/* Row 0xFF = unmapped.  We must explicitly mark gaps so that
 * zero-initialized {0,0} entries don't accidentally map to row 0 bit 0. */
#define _U 0xFF
static const uint8_t ps2_to_cpc_matrix[CPC_MATRIX_SIZE][2] = {
    [0x00] = {_U, 0},  /* no scancode 0 */
    [PSC_Escape]    = {8, 2},
    [PSC_1] = {8, 0}, [PSC_2] = {8, 1}, [PSC_3] = {7, 1}, [PSC_4] = {7, 0},
    [PSC_5] = {6, 1}, [PSC_6] = {6, 0}, [PSC_7] = {5, 1}, [PSC_8] = {5, 0},
    [PSC_9] = {4, 1}, [PSC_0] = {4, 0},
    [PSC_Minus]     = {3, 1},
    [PSC_Equals]    = {3, 4},  /* = → CPC ;/+ key */
    [PSC_BackSpace] = {9, 7},
    [PSC_Tab]       = {8, 4},
    [PSC_Q] = {8, 3}, [PSC_W] = {7, 3}, [PSC_E] = {7, 2}, [PSC_R] = {6, 2},
    [PSC_T] = {6, 3}, [PSC_Y] = {5, 3}, [PSC_U] = {5, 2}, [PSC_I] = {4, 3},
    [PSC_O] = {4, 2}, [PSC_P] = {3, 3},
    [PSC_LBr]       = {2, 1},
    [PSC_RBr]       = {2, 3},
    [PSC_Return]    = {2, 2},
    [PSC_LCtrl]     = {2, 7},
    [PSC_A] = {8, 5}, [PSC_S] = {7, 4}, [PSC_D] = {7, 5}, [PSC_F] = {6, 5},
    [PSC_G] = {6, 4}, [PSC_H] = {5, 4}, [PSC_J] = {5, 5}, [PSC_K] = {4, 5},
    [PSC_L] = {4, 4},
    [PSC_Semicolon] = {3, 5},
    [PSC_Quote]     = {3, 2},  /* ' → CPC @ key */
    [PSC_BackQuote] = {3, 0},  /* ` → CPC ^ key */
    [PSC_LShift]    = {2, 5},
    [PSC_BackSlash] = {2, 6},
    [PSC_Z] = {8, 7}, [PSC_X] = {7, 7}, [PSC_C] = {7, 6}, [PSC_V] = {6, 7},
    [PSC_B] = {6, 6}, [PSC_N] = {5, 6}, [PSC_M] = {4, 6},
    [PSC_Comma]     = {4, 7},
    [PSC_Period]    = {3, 7},
    [PSC_Slash]     = {3, 6},
    [PSC_RShift]    = {2, 5},
    /* 0x37 = unmapped */ [0x37] = {_U, 0},
    [PSC_LAlt]      = {1, 1},  /* Alt → CPC COPY key */
    [PSC_Space]     = {5, 7},
    [PSC_CapsLock]  = {8, 6},
    /* F1-F12: handled via keysym interception, not matrix */
    [PSC_F1] = {_U, 0}, [PSC_F2] = {_U, 0}, [PSC_F3] = {_U, 0},
    [PSC_F4] = {_U, 0}, [PSC_F5] = {_U, 0}, [PSC_F6] = {_U, 0},
    [PSC_F7] = {_U, 0}, [PSC_F8] = {_U, 0}, [PSC_F9] = {_U, 0},
    [PSC_F10] = {_U, 0}, [PSC_F11] = {_U, 0}, [PSC_F12] = {_U, 0},
    /* Gap 0x45-0x59 */
    [0x45] = {_U, 0}, [0x46] = {_U, 0}, [0x47] = {_U, 0}, [0x48] = {_U, 0},
    [0x49] = {_U, 0}, [0x4A] = {_U, 0}, [0x4B] = {_U, 0}, [0x4C] = {_U, 0},
    [0x4D] = {_U, 0}, [0x4E] = {_U, 0}, [0x4F] = {_U, 0}, [0x50] = {_U, 0},
    [0x51] = {_U, 0}, [0x52] = {_U, 0}, [0x53] = {_U, 0}, [0x54] = {_U, 0},
    [0x55] = {_U, 0}, [0x56] = {_U, 0}, [0x59] = {_U, 0},
    [PSC_Up]        = {0, 0},
    [0x5B] = {_U, 0}, [0x5C] = {_U, 0}, [0x5D] = {_U, 0},
    [PSC_Insert]    = {_U, 0},
    [PSC_Delete]    = {2, 0},
    [0x60] = {_U, 0},
    [PSC_Home]      = {_U, 0},
    [PSC_End]       = {_U, 0},
    [PSC_PgUp]      = {_U, 0},
    [PSC_PgDn]      = {_U, 0},
    [PSC_RAlt]      = {1, 1},
    [PSC_RCtrl]     = {2, 7},
    [0x67] = {_U, 0},
    [PSC_KP_Enter]  = {0, 6},
    [0x69] = {_U, 0},
    [PSC_Down]      = {0, 2},
    [PSC_Left]      = {1, 0},
    [PSC_Right]     = {0, 1},
};

static const uint8_t ps2_to_cpc_matrix_fr[CPC_MATRIX_SIZE][2] = {
    [0x00] = {_U, 0},  /* no scancode 0 */
    [PSC_Escape]    = {8, 2},
    [PSC_1] = {8, 0}, [PSC_2] = {8, 1}, [PSC_3] = {7, 1}, [PSC_4] = {7, 0},
    [PSC_5] = {6, 1}, [PSC_6] = {6, 0}, [PSC_7] = {5, 1}, [PSC_8] = {5, 0},
    [PSC_9] = {4, 1}, [PSC_0] = {4, 0},
    [PSC_Minus]     = {3, 1},
    [PSC_Equals]    = {3, 4},
    [PSC_BackSpace] = {9, 7},
    [PSC_Tab]       = {8, 4},
    [PSC_Q] = {8, 5}, [PSC_W] = {8, 7}, [PSC_E] = {7, 2}, [PSC_R] = {6, 2},
    [PSC_T] = {6, 3}, [PSC_Y] = {5, 3}, [PSC_U] = {5, 2}, [PSC_I] = {4, 3},
    [PSC_O] = {4, 2}, [PSC_P] = {3, 3},
    [PSC_LBr]       = {2, 1},
    [PSC_RBr]       = {2, 3},
    [PSC_Return]    = {2, 2},
    [PSC_LCtrl]     = {2, 7},
    [PSC_A] = {8, 3}, [PSC_S] = {7, 4}, [PSC_D] = {7, 5}, [PSC_F] = {6, 5},
    [PSC_G] = {6, 4}, [PSC_H] = {5, 4}, [PSC_J] = {5, 5}, [PSC_K] = {4, 5},
    [PSC_L] = {4, 4},
    [PSC_Semicolon] = {4, 6},
    [PSC_Quote]     = {3, 2},
    [PSC_BackQuote] = {3, 0},
    [PSC_LShift]    = {2, 5},
    [PSC_BackSlash] = {2, 6},
    [PSC_Z] = {7, 3}, [PSC_X] = {7, 7}, [PSC_C] = {7, 6}, [PSC_V] = {6, 7},
    [PSC_B] = {6, 6}, [PSC_N] = {5, 6}, [PSC_M] = {4, 7},
    [PSC_Comma]     = {4, 7},
    [PSC_Period]    = {3, 7},
    [PSC_Slash]     = {3, 6},
    [PSC_RShift]    = {2, 5},
    [0x37] = {_U, 0},
    [PSC_LAlt]      = {1, 1},
    [PSC_Space]     = {5, 7},
    [PSC_CapsLock]  = {8, 6},
    [PSC_F1] = {_U, 0}, [PSC_F2] = {_U, 0}, [PSC_F3] = {_U, 0},
    [PSC_F4] = {_U, 0}, [PSC_F5] = {_U, 0}, [PSC_F6] = {_U, 0},
    [PSC_F7] = {_U, 0}, [PSC_F8] = {_U, 0}, [PSC_F9] = {_U, 0},
    [PSC_F10] = {_U, 0}, [PSC_F11] = {_U, 0}, [PSC_F12] = {_U, 0},
    [0x45] = {_U, 0}, [0x46] = {_U, 0}, [0x47] = {_U, 0}, [0x48] = {_U, 0},
    [0x49] = {_U, 0}, [0x4A] = {_U, 0}, [0x4B] = {_U, 0}, [0x4C] = {_U, 0},
    [0x4D] = {_U, 0}, [0x4E] = {_U, 0}, [0x4F] = {_U, 0}, [0x50] = {_U, 0},
    [0x51] = {_U, 0}, [0x52] = {_U, 0}, [0x53] = {_U, 0}, [0x54] = {_U, 0},
    [0x55] = {_U, 0}, [0x56] = {_U, 0}, [0x59] = {_U, 0},
    [PSC_Up]        = {0, 0},
    [0x5B] = {_U, 0}, [0x5C] = {_U, 0}, [0x5D] = {_U, 0},
    [PSC_Insert]    = {_U, 0},
    [PSC_Delete]    = {2, 0},
    [0x60] = {_U, 0},
    [PSC_Home]      = {_U, 0},
    [PSC_End]       = {_U, 0},
    [PSC_PgUp]      = {_U, 0},
    [PSC_PgDn]      = {_U, 0},
    [PSC_RAlt]      = {1, 1},
    [PSC_RCtrl]     = {2, 7},
    [0x67] = {_U, 0},
    [PSC_KP_Enter]  = {0, 6},
    [0x69] = {_U, 0},
    [PSC_Down]      = {0, 2},
    [PSC_Left]      = {1, 0},
    [PSC_Right]     = {0, 1},
};

static const uint8_t ps2_to_cpc_matrix_es[CPC_MATRIX_SIZE][2] = {
    [0x00] = {_U, 0},  /* no scancode 0 */
    [PSC_Escape]    = {8, 2},
    [PSC_1] = {8, 0}, [PSC_2] = {8, 1}, [PSC_3] = {7, 1}, [PSC_4] = {7, 0},
    [PSC_5] = {6, 1}, [PSC_6] = {6, 0}, [PSC_7] = {5, 1}, [PSC_8] = {5, 0},
    [PSC_9] = {4, 1}, [PSC_0] = {4, 0},
    [PSC_Minus]     = {3, 1},
    [PSC_Equals]    = {3, 4},  /* = → CPC ;/+ key */
    [PSC_BackSpace] = {9, 7},
    [PSC_Tab]       = {8, 4},
    [PSC_Q] = {8, 3}, [PSC_W] = {7, 3}, [PSC_E] = {7, 2}, [PSC_R] = {6, 2},
    [PSC_T] = {6, 3}, [PSC_Y] = {5, 3}, [PSC_U] = {5, 2}, [PSC_I] = {4, 3},
    [PSC_O] = {4, 2}, [PSC_P] = {3, 3},
    [PSC_LBr]       = {2, 1},
    [PSC_RBr]       = {2, 3},
    [PSC_Return]    = {2, 2},
    [PSC_LCtrl]     = {2, 7},
    [PSC_A] = {8, 5}, [PSC_S] = {7, 4}, [PSC_D] = {7, 5}, [PSC_F] = {6, 5},
    [PSC_G] = {6, 4}, [PSC_H] = {5, 4}, [PSC_J] = {5, 5}, [PSC_K] = {4, 5},
    [PSC_L] = {4, 4},
    [PSC_Semicolon] = {3, 5},
    [PSC_Quote]     = {3, 2},
    [PSC_BackQuote] = {3, 0},
    [PSC_LShift]    = {2, 5},
    [PSC_BackSlash] = {2, 6},
    [PSC_Z] = {8, 7}, [PSC_X] = {7, 7}, [PSC_C] = {7, 6}, [PSC_V] = {6, 7},
    [PSC_B] = {6, 6}, [PSC_N] = {5, 6}, [PSC_M] = {4, 6},
    [PSC_Comma]     = {4, 7},
    [PSC_Period]    = {3, 7},
    [PSC_Slash]     = {3, 6},
    [PSC_RShift]    = {2, 5},
    [0x37] = {_U, 0},
    [PSC_LAlt]      = {1, 1},
    [PSC_Space]     = {5, 7},
    [PSC_CapsLock]  = {8, 6},
    [PSC_F1] = {_U, 0}, [PSC_F2] = {_U, 0}, [PSC_F3] = {_U, 0},
    [PSC_F4] = {_U, 0}, [PSC_F5] = {_U, 0}, [PSC_F6] = {_U, 0},
    [PSC_F7] = {_U, 0}, [PSC_F8] = {_U, 0}, [PSC_F9] = {_U, 0},
    [PSC_F10] = {_U, 0}, [PSC_F11] = {_U, 0}, [PSC_F12] = {_U, 0},
    [0x45] = {_U, 0}, [0x46] = {_U, 0}, [0x47] = {_U, 0}, [0x48] = {_U, 0},
    [0x49] = {_U, 0}, [0x4A] = {_U, 0}, [0x4B] = {_U, 0}, [0x4C] = {_U, 0},
    [0x4D] = {_U, 0}, [0x4E] = {_U, 0}, [0x4F] = {_U, 0}, [0x50] = {_U, 0},
    [0x51] = {_U, 0}, [0x52] = {_U, 0}, [0x53] = {_U, 0}, [0x54] = {_U, 0},
    [0x55] = {_U, 0}, [0x56] = {_U, 0}, [0x59] = {_U, 0},
    [PSC_Up]        = {0, 0},
    [0x5B] = {_U, 0}, [0x5C] = {_U, 0}, [0x5D] = {_U, 0},
    [PSC_Insert]    = {_U, 0},
    [PSC_Delete]    = {2, 0},
    [0x60] = {_U, 0},
    [PSC_Home]      = {_U, 0},
    [PSC_End]       = {_U, 0},
    [PSC_PgUp]      = {_U, 0},
    [PSC_PgDn]      = {_U, 0},
    [PSC_RAlt]      = {1, 1},
    [PSC_RCtrl]     = {2, 7},
    [0x67] = {_U, 0},
    [PSC_KP_Enter]  = {0, 6},
    [0x69] = {_U, 0},
    [PSC_Down]      = {0, 2},
    [PSC_Left]      = {1, 0},
    [PSC_Right]     = {0, 1},
};

static const uint8_t ps2_to_cpc_matrix_de[CPC_MATRIX_SIZE][2] = {
    [0x00] = {_U, 0},  /* no scancode 0 */
    [PSC_Escape]    = {8, 2},
    [PSC_1] = {8, 0}, [PSC_2] = {8, 1}, [PSC_3] = {7, 1}, [PSC_4] = {7, 0},
    [PSC_5] = {6, 1}, [PSC_6] = {6, 0}, [PSC_7] = {5, 1}, [PSC_8] = {5, 0},
    [PSC_9] = {4, 1}, [PSC_0] = {4, 0},
    [PSC_Minus]     = {3, 1},
    [PSC_Equals]    = {3, 4},  /* = → CPC ;/+ key */
    [PSC_BackSpace] = {9, 7},
    [PSC_Tab]       = {8, 4},
    [PSC_Q] = {8, 3}, [PSC_W] = {7, 3}, [PSC_E] = {7, 2}, [PSC_R] = {6, 2},
    [PSC_T] = {6, 3}, [PSC_Y] = {8, 7}, [PSC_U] = {5, 2}, [PSC_I] = {4, 3},
    [PSC_O] = {4, 2}, [PSC_P] = {3, 3},
    [PSC_LBr]       = {2, 1},
    [PSC_RBr]       = {2, 3},
    [PSC_Return]    = {2, 2},
    [PSC_LCtrl]     = {2, 7},
    [PSC_A] = {8, 5}, [PSC_S] = {7, 4}, [PSC_D] = {7, 5}, [PSC_F] = {6, 5},
    [PSC_G] = {6, 4}, [PSC_H] = {5, 4}, [PSC_J] = {5, 5}, [PSC_K] = {4, 5},
    [PSC_L] = {4, 4},
    [PSC_Semicolon] = {3, 5},
    [PSC_Quote]     = {3, 2},
    [PSC_BackQuote] = {3, 0},
    [PSC_LShift]    = {2, 5},
    [PSC_BackSlash] = {2, 6},
    [PSC_Z] = {5, 3}, [PSC_X] = {7, 7}, [PSC_C] = {7, 6}, [PSC_V] = {6, 7},
    [PSC_B] = {6, 6}, [PSC_N] = {5, 6}, [PSC_M] = {4, 6},
    [PSC_Comma]     = {4, 7},
    [PSC_Period]    = {3, 7},
    [PSC_Slash]     = {3, 6},
    [PSC_RShift]    = {2, 5},
    [0x37] = {_U, 0},
    [PSC_LAlt]      = {1, 1},
    [PSC_Space]     = {5, 7},
    [PSC_CapsLock]  = {8, 6},
    [PSC_F1] = {_U, 0}, [PSC_F2] = {_U, 0}, [PSC_F3] = {_U, 0},
    [PSC_F4] = {_U, 0}, [PSC_F5] = {_U, 0}, [PSC_F6] = {_U, 0},
    [PSC_F7] = {_U, 0}, [PSC_F8] = {_U, 0}, [PSC_F9] = {_U, 0},
    [PSC_F10] = {_U, 0}, [PSC_F11] = {_U, 0}, [PSC_F12] = {_U, 0},
    [0x45] = {_U, 0}, [0x46] = {_U, 0}, [0x47] = {_U, 0}, [0x48] = {_U, 0},
    [0x49] = {_U, 0}, [0x4A] = {_U, 0}, [0x4B] = {_U, 0}, [0x4C] = {_U, 0},
    [0x4D] = {_U, 0}, [0x4E] = {_U, 0}, [0x4F] = {_U, 0}, [0x50] = {_U, 0},
    [0x51] = {_U, 0}, [0x52] = {_U, 0}, [0x53] = {_U, 0}, [0x54] = {_U, 0},
    [0x55] = {_U, 0}, [0x56] = {_U, 0}, [0x59] = {_U, 0},
    [PSC_Up]        = {0, 0},
    [0x5B] = {_U, 0}, [0x5C] = {_U, 0}, [0x5D] = {_U, 0},
    [PSC_Insert]    = {_U, 0},
    [PSC_Delete]    = {2, 0},
    [0x60] = {_U, 0},
    [PSC_Home]      = {_U, 0},
    [PSC_End]       = {_U, 0},
    [PSC_PgUp]      = {_U, 0},
    [PSC_PgDn]      = {_U, 0},
    [PSC_RAlt]      = {1, 1},
    [PSC_RCtrl]     = {2, 7},
    [0x67] = {_U, 0},
    [PSC_KP_Enter]  = {0, 6},
    [0x69] = {_U, 0},
    [PSC_Down]      = {0, 2},
    [PSC_Left]      = {1, 0},
    [PSC_Right]     = {0, 1},
};

static const uint8_t (*active_ps2_matrix)[2] = ps2_to_cpc_matrix;

void cpc_set_keyboard_layout(int layout) {
    switch (layout) {
        case 1: active_ps2_matrix = ps2_to_cpc_matrix_fr; break;
        case 2: active_ps2_matrix = ps2_to_cpc_matrix_es; break;
        case 3: active_ps2_matrix = ps2_to_cpc_matrix_de; break;
        default: active_ps2_matrix = ps2_to_cpc_matrix; break;
    }
}

#undef _U

/* Shared key-event handler for both PS/2 and USB HID keyboards. */
static bool s_shifted = false;
static bool s_ctrl_held = false;
static bool s_alt_held = false;

static void handle_key_event(int pressed, unsigned char key) {
    uint8_t *km = cpc_get_keyboard_matrix();

    if (key == PSC_LShift || key == PSC_RShift)
        s_shifted = pressed != 0;
    if (key == PSC_LCtrl || key == PSC_RCtrl)
        s_ctrl_held = pressed != 0;
    if (key == PSC_LAlt || key == PSC_RAlt)
        s_alt_held = pressed != 0;

    unsigned int ks = scancode_to_keysym((unsigned int)key,
                                         s_shifted && key != PSC_LShift && key != PSC_RShift);

    if (pressed) {
        /* Ctrl+Alt+Delete: reset emulator */
        if (ks == KS_Delete && s_ctrl_held && s_alt_held) {
            extern void cpc_settings_do_reset(void);
            cpc_settings_do_reset();
            return;
        }
        /* Scroll Lock: Multiface II STOP */
        if (ks == KS_Scroll_Lock) {
            cpc_mf2_stop();
            return;
        }
        /* F11: disk browser menu */
        if (ks == KS_F11) {
            cpc_ui_open_disk_menu();
            return;
        }
        /* F12: toggle settings overlay */
        if (ks == KS_F12) {
            cpc_ui_toggle();
            return;
        }
        /* While overlay is open, route all keys to it */
        if (cpc_ui_wants_keys()) {
            cpc_ui_handle_key(ks);
            return;
        }
    } else {
        /* Release events must always reach the matrix — if the UI
         * overlay opens while a key is held, the release would be
         * swallowed and the key stays "pressed" in the CPC. */
        if (cpc_ui_wants_keys()) {
            /* fall through to matrix update below */
        }
    }

    /* Direct CPC keyboard matrix manipulation.
     * Each PS/2 scancode maps to a fixed (row, bit) in the CPC's
     * 10×8 keyboard matrix. Press clears the bit, release sets it. */
    if (key < CPC_MATRIX_SIZE) {
        uint8_t row = active_ps2_matrix[key][0];
        uint8_t bit = active_ps2_matrix[key][1];
        if (row < 10) {
            if (pressed)
                km[row] &= ~(1u << bit);
            else
                km[row] |= (1u << bit);
        }
    }
}

void cpc_ps2_feed_events(void) {
    int pressed;
    unsigned char key;

    /* PS/2 keyboard (PIO-based driver). */
    ps2kbd_tick();
    while (ps2kbd_get_key(&pressed, &key))
        handle_key_event(pressed, key);

    /* USB HID keyboard + gamepad. Stubs out to zero when
     * USB_HID_ENABLED is off — no runtime cost on PS/2-only builds. */
    usbhid_wrapper_tick();
    while (usbhid_wrapper_get_key(&pressed, &key))
        handle_key_event(pressed, key);
}

/* ------------------------------------------------------------------ */
/* NES gamepad → CPC joystick row 9.                                  */
/* CPC joystick matrix (active-low):                                  */
/*   bit 0 = Up, bit 1 = Down, bit 2 = Left, bit 3 = Right,          */
/*   bit 4 = Fire 1, bit 5 = Fire 2                                  */
/* ------------------------------------------------------------------ */
static bool g_nespad_initialized = false;

void cpc_nespad_init(void) {
    uint32_t cpu_khz = clock_get_hz(clk_sys) / 1000;
    g_nespad_initialized = nespad_begin(cpu_khz,
                                        NESPAD_GPIO_CLK,
                                        NESPAD_GPIO_DATA,
                                        NESPAD_DATA_PIN_NONE,
                                        NESPAD_GPIO_LATCH);
    if (g_nespad_initialized)
        printf("NES gamepad initialized (CLK=%d DATA=%d LATCH=%d)\n",
               NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH);
}

void cpc_nespad_poll(void) {
    uint8_t *km = cpc_get_keyboard_matrix();
    uint8_t joy = 0xFF;  /* all released (active-low) */

    /* NES/SNES wired gamepad */
    if (g_nespad_initialized) {
        nespad_read();

        uint32_t s = nespad_state;

        if (s & DPAD_UP)    joy &= ~(1u << 0);
        if (s & DPAD_DOWN)  joy &= ~(1u << 1);
        if (s & DPAD_LEFT)  joy &= ~(1u << 2);
        if (s & DPAD_RIGHT) joy &= ~(1u << 3);
        if (s & DPAD_A)     joy &= ~(1u << 4);  /* A → Fire 1 */
        if (s & DPAD_B)     joy &= ~(1u << 5);  /* B → Fire 2 */
        if (s & DPAD_Y)     joy &= ~(1u << 4);  /* Y → Fire 1 (alt) */
        if (s & DPAD_X)     joy &= ~(1u << 5);  /* X → Fire 2 (alt) */
    }

    /* USB gamepad (stubs to 0 when USB_HID_ENABLED is off). Convert
     * BTN_* bitmask to CPC active-low joystick format. */
    {
        unsigned int usb = usbhid_wrapper_get_joystick();
        if (usb & 0x0004) joy &= ~(1u << 0);  /* BTN_UP    → bit 0 */
        if (usb & 0x0008) joy &= ~(1u << 1);  /* BTN_DOWN  → bit 1 */
        if (usb & 0x0001) joy &= ~(1u << 2);  /* BTN_LEFT  → bit 2 */
        if (usb & 0x0002) joy &= ~(1u << 3);  /* BTN_RIGHT → bit 3 */
        if (usb & 0x0010) joy &= ~(1u << 4);  /* BTN_FIREA → Fire 1 */
        if (usb & 0x0020) joy &= ~(1u << 5);  /* BTN_FIREB → Fire 2 */
        if (usb & 0x1000) joy &= ~(1u << 4);  /* BTN_FIREY → Fire 1 (alt) */
        if (usb & 0x0800) joy &= ~(1u << 5);  /* BTN_FIREX → Fire 2 (alt) */
    }

    /* Merge with keyboard: keep keyboard bits that are already pressed
     * (cleared), OR in gamepad bits (also active-low). */
    km[9] &= joy;

    /* Release gamepad-only bits: bits where joy=1 (released on pad)
     * AND no keyboard key is holding them down.
     * We only manage bits 0-5 (joystick); bits 6-7 are DEL and backspace. */
    static uint8_t prev_joy = 0xFF;
    uint8_t released = joy & ~prev_joy & 0x3F;  /* bits that went from 0→1 */
    km[9] |= released;
    prev_joy = joy;
}

void cpc_pico_main(void) {
    cpc_settings_load();
    cpc_settings_apply();

    if (cpc_engine_init() != 0) {
        printf("Failed to initialize CPC engine\n");
        while (true) tight_loop_contents();
    }

    cpc_ui_init();
    cpc_nespad_init();
    tape_init();

    cpc_disk_autoload();

    if (g_cpc_settings.tape[0]) {
        if (cpc_tape_insert(g_cpc_settings.tape) == 0)
            printf("tape: auto-mounted %s\n", g_cpc_settings.tape);
        else
            printf("tape: failed to mount %s\n", g_cpc_settings.tape);
    }

    /* Arm auto-type if configured via autorun= in /cpc/cpc.ini.
     * \n in the string = 10-second pause (wait for disk load / title screen).
     * Boot delay of 200 frames = 4 s gives BASIC time to show the Ready prompt.
     * Example cpc.ini entries:
     *   autorun = RUN"PRINCE
     *   (add \n characters in the value for multi-second waits before Space etc.)
     */
    {
        /* If autorun= is set in cpc.ini, use it verbatim.
         * \r = Enter, \n = 10-second pause.
         * Fallback: if any disk is mounted (by any autoload path), try
         * RUN"DISC + 30s wait + Space — works for most autobooting CPC games. */
        char cmd[128];
        if (g_cpc_settings.autorun[0]) {
            /* Unescape \n → 0x0A (10s wait) and \r → Enter in the autorun string */
            const char *src = g_cpc_settings.autorun;
            int di = 0;
            while (*src && di < (int)sizeof(cmd) - 2) {
                if (src[0] == '\\' && src[1] == 'n') { cmd[di++] = '\n'; src += 2; }
                else if (src[0] == '\\' && src[1] == 'r') { cmd[di++] = '\r'; src += 2; }
                else cmd[di++] = *src++;
            }
            cmd[di] = '\0';
            printf("autorun: %s\n", g_cpc_settings.autorun);
            cpc_autotype_set(cmd, 200);
        } else if (cpc_mounted_disk_name(0)) {
            const char *disk = cpc_mounted_disk_name(0);
            /* Detect Exolon trainer disk — answer Y to 4 trainer questions */
            int is_exolon = (strstr(disk, "xolon") || strstr(disk, "XOLON"));
            if (is_exolon) {
                printf("autorun: Exolon detected, typing run+trainer bypass\n");
                /* RUN"EXOLON + Enter, wait 10s, Y×4 for trainer questions,
                   wait 10s, then space to advance past title to planets. */
                cpc_autotype_set("RUN\"EXOLON\r\nyyyy\n \n", 200);
            } else {
                printf("autorun: disk mounted, typing RUN\"PRINCE\n");
                cpc_autotype_set("RUN\"PRINCE\r\n\n\n ", 200);
            }
        }
    }

    printf("CPC initialized. Starting emulation...\n");

    uint64_t fps_last_us = 0;
    uint32_t fps_frames = 0;

    while (1) {
        crash_handler_feed();
        cpc_ps2_feed_events();
        cpc_nespad_poll();
        cpc_autotype_tick();

        /* Point scanline callback directly at the back screen buffer,
         * skipping the intermediate cpc_fb and the memcpy in present. */
        {
            const int top_pad = (CPC_SCREEN_LINES - CPC_FB_HEIGHT) / 2;
            uint8_t *back = SCREEN[current_buffer];
            /* Only clear top/bottom border regions (CRTC fills active area) */
            uint8_t border = cpc_get_border_ink();
            memset(back, border, (size_t)(CPC_FB_WIDTH * 16));
            memset(back + CPC_FB_WIDTH * (CPC_SCREEN_LINES - 16), border, (size_t)(CPC_FB_WIDTH * 16));
            cpc_set_render_target(back + CPC_FB_WIDTH * top_pad, CPC_FB_WIDTH);
        }

        cpc_engine_run_frame();
        cpc_frame_present();
        cpc_serial_poll();
        cpc_frame_sync();

        fps_frames++;
        uint64_t now = time_us_64();
        if (fps_last_us == 0) fps_last_us = now;
        if (now - fps_last_us >= 1000000u) {
            printf("# FPS %u\n", fps_frames);
            fps_frames = 0;
            fps_last_us = now;
        }
    }
}
