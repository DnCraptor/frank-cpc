/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ui.c — settings overlay state machine and renderer.
 *
 * Toggle with F12. Navigation: Up/Down to select row, Left/Right to
 * cycle value, Enter or F12/Esc to close.  Settings that need a reset
 * show "(reset)" in the row; an "Apply & Reset" action row at the bottom
 * triggers InitMem() + ResetZ80().
 *
 * Draws into the 320×240 SCREEN buffer via the ui_draw primitives, using
 * palette indices 240-247 (well above the 32 CPC hardware colours).
 */

#include "cpc_ui.h"
#include "cpc_settings.h"
#include "ui_draw.h"
#include "board_config.h"

#include <string.h>
#include <stdio.h>

/* KS_* virtual key constants from platform.c */
#define KS_Escape   0xFF1B
#define KS_Return   0xFF0D
#define KS_Up       0xFF52
#define KS_Down     0xFF54
#define KS_Left     0xFF51
#define KS_Right    0xFF53
#define KS_F12      0xFFC9

/* ---- layout ---------------------------------------------------------- */
#define WIN_X    10
#define WIN_Y    30
#define WIN_W    300
#define WIN_H    160
#define WIN_PAD  8

/* Row after the last setting: Apply & Reset */
#define ROW_APPLY  CPC_SETTING_COUNT
#define TOTAL_ROWS (CPC_SETTING_COUNT + 1)

/* ---- state ----------------------------------------------------------- */
typedef enum { UI_HIDDEN = 0, UI_SETTINGS, UI_CONFIRM_RESET } ui_state_t;

static ui_state_t s_state      = UI_HIDDEN;
static int        s_row        = 0;
static bool       s_dirty      = false;  /* any reset-requiring change made */

/* ---- public ---------------------------------------------------------- */

void cpc_ui_init(void) {
    ui_draw_install_palette();
}

bool cpc_ui_is_visible(void) {
    return s_state != UI_HIDDEN;
}

void cpc_ui_toggle(void) {
    if (s_state == UI_HIDDEN) {
        s_state = UI_SETTINGS;
        s_row   = 0;
        s_dirty = false;
    } else {
        s_state = UI_HIDDEN;
    }
}

/* ---- key handling ---------------------------------------------------- */

/* Forward decl for reset callback */
extern void cpc_settings_do_reset(void);

static bool handle_settings(unsigned int ks) {
    switch (ks) {
        case KS_Escape:
        case KS_F12:
            s_state = UI_HIDDEN;
            return true;

        case KS_Up:
            if (--s_row < 0) s_row = TOTAL_ROWS - 1;
            return true;

        case KS_Down:
            if (++s_row >= TOTAL_ROWS) s_row = 0;
            return true;

        case KS_Left:
            if (s_row < CPC_SETTING_COUNT) {
                if (cpc_settings_needs_reset((cpc_setting_id_t)s_row))
                    s_dirty = true;
                cpc_settings_step((cpc_setting_id_t)s_row, -1);
            }
            return true;

        case KS_Right:
        case KS_Return:
            if (s_row < CPC_SETTING_COUNT) {
                if (cpc_settings_needs_reset((cpc_setting_id_t)s_row))
                    s_dirty = true;
                cpc_settings_step((cpc_setting_id_t)s_row, +1);
            } else if (s_row == ROW_APPLY) {
                /* Apply & Reset */
                s_state = UI_CONFIRM_RESET;
            }
            return true;

        default:
            return false;
    }
}

static bool handle_confirm(unsigned int ks) {
    switch (ks) {
        case KS_Return:
            /* Confirmed — apply settings and reset */
            s_state = UI_HIDDEN;
            cpc_settings_do_reset();
            return true;

        case KS_Escape:
        case KS_F12:
            s_state = UI_SETTINGS;
            return true;

        default:
            return false;
    }
}

bool cpc_ui_handle_key(unsigned int ks) {
    switch (s_state) {
        case UI_SETTINGS:      return handle_settings(ks);
        case UI_CONFIRM_RESET: return handle_confirm(ks);
        default: return false;
    }
}

/* ---- renderer -------------------------------------------------------- */

#define TITLE_H   UI_HEADER_H
#define ROW_H     UI_LINE_H
#define VALUE_COL 160   /* x offset for value column */
#define RESET_TAG " *"  /* shown after label if change needs reset */

/* Width of the label column inside the window. */
#define LABEL_MAX_CHARS 18
#define VALUE_MAX_CHARS 14

void cpc_ui_render(uint8_t *fb, int stride, int height) {
    if (s_state == UI_HIDDEN) return;

    /* Dim the background behind the window. */
    for (int y = WIN_Y; y < WIN_Y + WIN_H && y < height; y++)
        for (int x = WIN_X; x < WIN_X + WIN_W; x++)
            fb[y * stride + x] = UI_COLOR_BG;

    ui_draw_border(fb, stride, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_FG);
    ui_draw_header(fb, stride, WIN_X, WIN_Y, WIN_W, "CPC Settings");

    int ry = WIN_Y + TITLE_H + WIN_PAD;

    if (s_state == UI_CONFIRM_RESET) {
        ui_draw_string(fb, stride, WIN_X + WIN_PAD, ry,
                       "Apply settings and reset CPC?", UI_COLOR_FG);
        ry += ROW_H * 2;
        ui_draw_menu_item(fb, stride, WIN_X + WIN_PAD, ry,
                          WIN_W - 2 * WIN_PAD,
                          "  Enter = Confirm    Esc = Cancel  ",
                          34, true);
        return;
    }

    /* Settings rows */
    for (int i = 0; i < TOTAL_ROWS; i++) {
        bool sel = (i == s_row);

        if (i < CPC_SETTING_COUNT) {
            cpc_setting_id_t sid = (cpc_setting_id_t)i;
            const char *lbl = cpc_settings_label(sid);
            const char *val = cpc_settings_value_label(sid);
            bool needs_rst  = cpc_settings_needs_reset(sid);

            /* Highlight the full row */
            if (sel) {
                ui_fill_rect(fb, stride,
                             WIN_X + 1, ry - 1,
                             WIN_W - 2, ROW_H + 1,
                             UI_COLOR_ACCENT);
            }
            uint8_t fg = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

            /* Label (left column) */
            ui_draw_string_truncated(fb, stride,
                                     WIN_X + WIN_PAD, ry,
                                     lbl, LABEL_MAX_CHARS, fg);

            /* Reset marker */
            if (needs_rst)
                ui_draw_char(fb, stride,
                             WIN_X + WIN_PAD + LABEL_MAX_CHARS * UI_CHAR_W, ry,
                             '*', sel ? UI_COLOR_ACCENT_FG : UI_COLOR_DIM);

            /* Value (right column) */
            ui_draw_string_truncated(fb, stride,
                                     WIN_X + VALUE_COL, ry,
                                     val, VALUE_MAX_CHARS, fg);
        } else {
            /* "Apply & Reset" action row */
            const char *lbl = s_dirty ? "Apply & Reset  (Enter)" : "Close  (Esc / F12)";
            ui_draw_menu_item(fb, stride,
                              WIN_X + WIN_PAD, ry,
                              WIN_W - 2 * WIN_PAD,
                              lbl, 26, sel);
        }

        ry += ROW_H + 1;
    }

    /* Footer */
    int footer_y = WIN_Y + WIN_H - ROW_H - 3;
    ui_draw_string(fb, stride, WIN_X + WIN_PAD, footer_y,
                   "\x1f\x1e Up/Dn  \x11\x10 Left/Right  Enter  F12=Close",
                   UI_COLOR_DIM);
}
