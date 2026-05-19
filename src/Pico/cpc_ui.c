/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ui.c — settings overlay + disk browser state machine and renderer.
 *
 * F12  → Settings screen (ESC or "Back to CPC" closes)
 * F1   → Drive A disk browser
 * F2   → Drive B disk browser
 */

#include "cpc_ui.h"
#include "cpc_settings.h"
#include "cpc_loader.h"
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
#define KS_BackSpace 0xFF08

/* ---- layout ---------------------------------------------------------- */
#define WIN_X   10
#define WIN_Y   12
#define WIN_W   300
#define WIN_H   200
#define WIN_PAD 6

/* Settings rows */
#define SETTINGS_APPLY_ROW  CPC_SETTING_COUNT
#define SETTINGS_BACK_ROW   (CPC_SETTING_COUNT + 1)
#define SETTINGS_TOTAL_ROWS (CPC_SETTING_COUNT + 2)
#define SETTINGS_VISIBLE_ROWS 10

/* Disk browser rows */
#define DISK_VISIBLE_ROWS 12

/* ---- state ----------------------------------------------------------- */
typedef enum {
    UI_HIDDEN = 0,
    UI_SETTINGS,
    UI_SETTINGS_CONFIRM,
    UI_DISK_BROWSER,
} ui_state_t;

static ui_state_t s_state            = UI_HIDDEN;
static int        s_setting_row      = 0;
static int        s_settings_scroll  = 0;

/* Disk browser state */
static int        s_disk_drive       = 0;   /* 0=A, 1=B */
static int        s_disk_row         = 0;
static int        s_disk_scroll      = 0;
static char       s_disk_msg[64]     = "";  /* brief status message */

/* ---- public ---------------------------------------------------------- */

void cpc_ui_init(void) {
    ui_draw_install_palette();
}

bool cpc_ui_is_visible(void) {
    return s_state != UI_HIDDEN;
}

void cpc_ui_toggle(void) {
    if (s_state == UI_HIDDEN) {
        s_state           = UI_SETTINGS;
        s_setting_row     = 0;
        s_settings_scroll = 0;
    }
}

void cpc_ui_open_disk_browser(int drv) {
    s_disk_drive  = drv;
    s_disk_row    = 0;
    s_disk_scroll = 0;
    s_disk_msg[0] = '\0';
    cpc_disk_rescan();
    s_state = UI_DISK_BROWSER;
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

static bool handle_disk_browser_key(unsigned int ks) {
    int total = g_cpc_disk_entry_count;
    switch (ks) {
        case KS_Escape:
            s_state = UI_HIDDEN;
            return true;

        case KS_Up:
            if (s_disk_row > 0) {
                --s_disk_row;
                if (s_disk_row < s_disk_scroll) s_disk_scroll = s_disk_row;
            }
            return true;

        case KS_Down:
            if (s_disk_row < total - 1) {
                ++s_disk_row;
                if (s_disk_row >= s_disk_scroll + DISK_VISIBLE_ROWS)
                    s_disk_scroll = s_disk_row - DISK_VISIBLE_ROWS + 1;
            }
            return true;

        case KS_Page_Up:
            s_disk_row -= DISK_VISIBLE_ROWS;
            if (s_disk_row < 0) s_disk_row = 0;
            if (s_disk_row < s_disk_scroll) s_disk_scroll = s_disk_row;
            return true;

        case KS_Page_Down:
            s_disk_row += DISK_VISIBLE_ROWS;
            if (s_disk_row >= total) s_disk_row = total > 0 ? total - 1 : 0;
            if (s_disk_row >= s_disk_scroll + DISK_VISIBLE_ROWS)
                s_disk_scroll = s_disk_row - DISK_VISIBLE_ROWS + 1;
            return true;

        case KS_BackSpace:
        case KS_Left: {
            int cnt = cpc_disk_enter_parent();
            s_disk_row = 0; s_disk_scroll = 0;
            snprintf(s_disk_msg, sizeof(s_disk_msg),
                     "%d item%s", cnt, cnt == 1 ? "" : "s");
            return true;
        }

        case KS_Return:
        case KS_Right:
            if (total <= 0) return true;
            if (g_cpc_disk_entries[s_disk_row].is_dir) {
                int cnt = cpc_disk_enter_subdir(g_cpc_disk_entries[s_disk_row].name);
                s_disk_row = 0; s_disk_scroll = 0;
                snprintf(s_disk_msg, sizeof(s_disk_msg),
                         "%d item%s", cnt, cnt == 1 ? "" : "s");
            } else {
                /* Mount the disk */
                char path[CPC_DISK_PATH_LEN];
                cpc_disk_entry_path(s_disk_row, path, sizeof(path));
                if (cpc_mount_disk(s_disk_drive, path) == 0) {
                    snprintf(s_disk_msg, sizeof(s_disk_msg),
                             "Drive %c: %s",
                             'A' + s_disk_drive,
                             g_cpc_disk_entries[s_disk_row].name);
                    s_state = UI_HIDDEN;
                } else {
                    snprintf(s_disk_msg, sizeof(s_disk_msg), "Failed to mount");
                }
            }
            return true;

        default:
            return false;
    }
}


bool cpc_ui_handle_key(unsigned int ks) {
    switch (s_state) {
        case UI_SETTINGS:         return handle_settings_page(ks);
        case UI_SETTINGS_CONFIRM: return handle_settings_confirm(ks);
        case UI_DISK_BROWSER:     return handle_disk_browser_key(ks);
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

/* ---- disk browser renderer ------------------------------------------- */

static void render_disk_browser(uint8_t *fb, int stride) {
    char title[48];
    snprintf(title, sizeof(title), " Drive %c: disk browser ", 'A' + s_disk_drive);
    draw_chrome(fb, stride, title);

    int x  = content_x();
    int cw = content_w();

    /* Current directory path line */
    int dir_y = WIN_Y + UI_HEADER_H + WIN_PAD;
    ui_fill_rect  (fb, stride, x, dir_y, cw, UI_LINE_H, UI_COLOR_BG);
    ui_draw_string(fb, stride, x, dir_y + 1, g_cpc_disk_dir, UI_COLOR_DIM);

    /* Mounted disk indicator */
    const char *mounted = cpc_mounted_disk_name(s_disk_drive);
    if (mounted) {
        char ind[CPC_DISK_FILENAME_LEN + 8];
        snprintf(ind, sizeof(ind), "[%s]", mounted);
        int ind_x = WIN_X + WIN_W - WIN_PAD - (int)strlen(ind) * UI_CHAR_W;
        ui_draw_string(fb, stride, ind_x, dir_y + 1, ind, UI_COLOR_ACCENT);
    }

    int y  = dir_y + UI_LINE_H + 2;

    int total = g_cpc_disk_entry_count;
    if (total == 0) {
        ui_draw_string(fb, stride, x + 4, y + 4, "(no .dsk files)", UI_COLOR_DIM);
    } else {
        int last = s_disk_scroll + DISK_VISIBLE_ROWS;
        if (last > total) last = total;

        for (int i = s_disk_scroll; i < last; ++i) {
            bool    sel = (i == s_disk_row);
            uint8_t bg  = sel ? UI_COLOR_ACCENT    : UI_COLOR_BG;
            uint8_t fg  = sel ? UI_COLOR_ACCENT_FG : UI_COLOR_FG;

            ui_fill_rect(fb, stride, x, y, cw, UI_LINE_H, bg);

            char item[CPC_DISK_FILENAME_LEN + 8];
            if (g_cpc_disk_entries[i].is_dir)
                snprintf(item, sizeof(item), "[%s/]", g_cpc_disk_entries[i].name);
            else
                snprintf(item, sizeof(item), " %s",  g_cpc_disk_entries[i].name);
            ui_draw_string(fb, stride, x + 2, y + 1, item, fg);

            y += UI_LINE_H + 1;
        }

        if (total > DISK_VISIBLE_ROWS) {
            ui_draw_scrollbar(fb, stride,
                              WIN_X + WIN_W - WIN_PAD - 4,
                              dir_y + UI_LINE_H + 2,
                              DISK_VISIBLE_ROWS * (UI_LINE_H + 1) - 1,
                              total, DISK_VISIBLE_ROWS, s_disk_scroll);
        }
    }

    /* Status message or entry count */
    if (s_disk_msg[0]) {
        int msg_y = WIN_Y + WIN_H - UI_LINE_H * 2 - WIN_PAD;
        ui_fill_rect  (fb, stride, x, msg_y, cw, UI_LINE_H, UI_COLOR_BG);
        ui_draw_string(fb, stride, x, msg_y + 1, s_disk_msg, UI_COLOR_ACCENT);
    }

    draw_footer(fb, stride, "UP/DN PG  ENTER=select  BKSP=up  ESC");
}

/* ---- main render entry point ----------------------------------------- */

void cpc_ui_render(uint8_t *fb, int stride, int height) {
    (void)height;
    if (s_state == UI_HIDDEN)           return;
    if (s_state == UI_SETTINGS)         render_settings_page(fb, stride);
    if (s_state == UI_SETTINGS_CONFIRM) render_settings_confirm(fb, stride);
    if (s_state == UI_DISK_BROWSER)     render_disk_browser(fb, stride);
}
