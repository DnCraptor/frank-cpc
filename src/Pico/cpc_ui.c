/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ui.c — settings overlay state machine and renderer.
 *
 * F12 opens the settings screen.  ESC or "Back to CPC" closes it.
 * Navigation: Up/Down (PgUp/PgDn) to select, Left/Right to cycle values.
 * Enter on "Apply and Reset CPC" confirms and resets the machine.
 */

#include "cpc_ui.h"
#include "cpc_settings.h"
#include "ui_draw.h"
#include "board_config.h"

#include <string.h>
#include <stdio.h>

/* KS_* virtual key constants (must match platform.c) */
#define KS_Escape    0xFF1B
#define KS_Return    0xFF0D
#define KS_Up        0xFF52
#define KS_Down      0xFF54
#define KS_Left      0xFF51
#define KS_Right     0xFF53
#define KS_Page_Up   0xFF55
#define KS_Page_Down 0xFF56

/* ---- layout ---------------------------------------------------------- */
#define WIN_X   10
#define WIN_Y   12
#define WIN_W   300
#define WIN_H   200
#define WIN_PAD 6

/* Total rows = settings + "Apply and Reset CPC" + "Back to CPC" */
#define SETTINGS_APPLY_ROW  CPC_SETTING_COUNT
#define SETTINGS_BACK_ROW   (CPC_SETTING_COUNT + 1)
#define SETTINGS_TOTAL_ROWS (CPC_SETTING_COUNT + 2)

/* How many setting rows fit in the content area */
#define SETTINGS_VISIBLE_ROWS 10

/* ---- state ----------------------------------------------------------- */
typedef enum { UI_HIDDEN = 0, UI_SETTINGS, UI_SETTINGS_CONFIRM } ui_state_t;

static ui_state_t s_state            = UI_HIDDEN;
static int        s_setting_row      = 0;
static int        s_settings_scroll  = 0;

/* ---- public ---------------------------------------------------------- */

void cpc_ui_init(void) {
    ui_draw_install_palette();
}

bool cpc_ui_is_visible(void) {
    return s_state != UI_HIDDEN;
}

/* F12 opens; no-op if already open. */
void cpc_ui_toggle(void) {
    if (s_state == UI_HIDDEN) {
        s_state           = UI_SETTINGS;
        s_setting_row     = 0;
        s_settings_scroll = 0;
    }
}

/* ---- key handling ---------------------------------------------------- */

extern void cpc_settings_do_reset(void);

static bool handle_settings_page(unsigned int ks) {
    /* Keep selected row inside the visible scroll window */
    if (s_setting_row < s_settings_scroll)
        s_settings_scroll = s_setting_row;
    else if (s_setting_row >= s_settings_scroll + SETTINGS_VISIBLE_ROWS)
        s_settings_scroll = s_setting_row - SETTINGS_VISIBLE_ROWS + 1;

    switch (ks) {
        case KS_Escape:
            s_state = UI_HIDDEN;
            return true;

        case KS_Up:
            if (--s_setting_row < 0) s_setting_row = SETTINGS_TOTAL_ROWS - 1;
            return true;

        case KS_Down:
            if (++s_setting_row >= SETTINGS_TOTAL_ROWS) s_setting_row = 0;
            return true;

        case KS_Page_Up:
            s_setting_row -= SETTINGS_VISIBLE_ROWS / 2;
            if (s_setting_row < 0) s_setting_row = 0;
            return true;

        case KS_Page_Down:
            s_setting_row += SETTINGS_VISIBLE_ROWS / 2;
            if (s_setting_row >= SETTINGS_TOTAL_ROWS)
                s_setting_row = SETTINGS_TOTAL_ROWS - 1;
            return true;

        case KS_Left:
            if (s_setting_row < CPC_SETTING_COUNT)
                cpc_settings_step((cpc_setting_id_t)s_setting_row, -1);
            return true;

        case KS_Right:
            if (s_setting_row < CPC_SETTING_COUNT)
                cpc_settings_step((cpc_setting_id_t)s_setting_row, +1);
            return true;

        case KS_Return:
            if (s_setting_row == SETTINGS_APPLY_ROW) {
                s_state = UI_SETTINGS_CONFIRM;
            } else if (s_setting_row == SETTINGS_BACK_ROW) {
                s_state = UI_HIDDEN;
            } else if (s_setting_row < CPC_SETTING_COUNT) {
                cpc_settings_step((cpc_setting_id_t)s_setting_row, +1);
            }
            return true;

        default:
            return false;
    }
}

static bool handle_settings_confirm(unsigned int ks) {
    switch (ks) {
        case KS_Return:
            s_state = UI_HIDDEN;
            cpc_settings_do_reset();
            return true;
        case KS_Escape:
            s_state = UI_SETTINGS;
            return true;
        default:
            return false;
    }
}

bool cpc_ui_handle_key(unsigned int ks) {
    switch (s_state) {
        case UI_SETTINGS:         return handle_settings_page(ks);
        case UI_SETTINGS_CONFIRM: return handle_settings_confirm(ks);
        default: return false;
    }
}

/* ---- chrome helpers -------------------------------------------------- */

static int content_x(void) { return WIN_X + WIN_PAD; }
static int content_y(void) { return WIN_Y + UI_HEADER_H + WIN_PAD; }
static int content_w(void) { return WIN_W - 2 * WIN_PAD; }

static void draw_chrome(uint8_t *fb, int stride, const char *title) {
    ui_fill_rect  (fb, stride, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_BG);
    ui_draw_border(fb, stride, WIN_X, WIN_Y, WIN_W, WIN_H, UI_COLOR_FG);
    ui_draw_header(fb, stride, WIN_X, WIN_Y, WIN_W, title);
}

static void draw_footer(uint8_t *fb, int stride, const char *hint) {
    int fy = WIN_Y + WIN_H - UI_LINE_H - WIN_PAD;
    ui_fill_rect  (fb, stride, content_x(), fy, content_w(), UI_LINE_H, UI_COLOR_BG);
    ui_draw_string(fb, stride, content_x(), fy + 1, hint, UI_COLOR_FG);
}

/* ---- page renderers -------------------------------------------------- */

static void render_settings_page(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Settings ");

    int x  = content_x();
    int y  = content_y();
    int cw = content_w();

    int last = s_settings_scroll + SETTINGS_VISIBLE_ROWS;
    if (last > SETTINGS_TOTAL_ROWS) last = SETTINGS_TOTAL_ROWS;

    for (int i = s_settings_scroll; i < last; ++i) {
        bool    sel = (i == s_setting_row);
        uint8_t bg  = sel ? UI_COLOR_ACCENT    : UI_COLOR_BG;
        uint8_t fg  = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

        if (i < CPC_SETTING_COUNT) {
            ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);

            /* Label left-aligned */
            ui_draw_string(fb, stride, x + 2, y + 1,
                           cpc_settings_label((cpc_setting_id_t)i), fg);

            /* Value right-aligned; chevrons < > around value on selected row */
            const char *val  = cpc_settings_value_label((cpc_setting_id_t)i);
            int         vlen = (int)strlen(val);
            int         vx   = x + cw - 4 - (vlen + 2) * UI_CHAR_W;
            if (sel) ui_draw_string(fb, stride, vx - UI_CHAR_W,          y + 1, "<", fg);
            ui_draw_string        (fb, stride, vx,                        y + 1, val, fg);
            if (sel) ui_draw_string(fb, stride, vx + vlen * UI_CHAR_W + 2, y + 1, ">", fg);

        } else if (i == SETTINGS_APPLY_ROW) {
            ui_draw_menu_item(fb, stride, x, y, cw,
                              "Apply and Reset CPC",
                              (cw - 4) / UI_CHAR_W, sel);
        } else if (i == SETTINGS_BACK_ROW) {
            ui_draw_menu_item(fb, stride, x, y, cw,
                              "Back to CPC",
                              (cw - 4) / UI_CHAR_W, sel);
        }
        y += UI_LINE_H + 1;
    }

    if (SETTINGS_TOTAL_ROWS > SETTINGS_VISIBLE_ROWS) {
        ui_draw_scrollbar(fb, stride,
                          WIN_X + WIN_W - WIN_PAD - 4,
                          content_y(),
                          SETTINGS_VISIBLE_ROWS * (UI_LINE_H + 1) - 1,
                          SETTINGS_TOTAL_ROWS,
                          SETTINGS_VISIBLE_ROWS,
                          s_settings_scroll);
    }

    draw_footer(fb, stride, "UP/DN PG  LEFT/RIGHT  ENTER  ESC");
}

static void render_settings_confirm(uint8_t *fb, int stride) {
    draw_chrome(fb, stride, " Reset required ");
    int x = content_x();
    int y = content_y();
    ui_draw_string(fb, stride, x, y,
                   "New settings will reset the CPC.", UI_COLOR_FG);
    y += UI_LINE_H + 2;
    ui_draw_string(fb, stride, x, y,
                   "All unsaved state will be lost.",  UI_COLOR_FG);
    y += UI_LINE_H + 6;
    ui_draw_string(fb, stride, x, y, "Continue?", UI_COLOR_FG);
    draw_footer(fb, stride, "ENTER confirm  ESC cancel");
}

/* ---- main render entry point ----------------------------------------- */

void cpc_ui_render(uint8_t *fb, int stride, int height) {
    (void)height;
    if (s_state == UI_HIDDEN)          return;
    if (s_state == UI_SETTINGS)        render_settings_page(fb, stride);
    if (s_state == UI_SETTINGS_CONFIRM) render_settings_confirm(fb, stride);
}

