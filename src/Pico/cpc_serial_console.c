/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_serial_console.c — USB serial command console for automated control.
 *
 * Protocol:
 *   - Host sends a line terminated by \r or \n (or \r\n).
 *   - Board replies with "OK <payload>\n" or "ERR <message>\n".
 *   - Unsolicited status lines are prefixed with "# ".
 *
 * Commands:
 *   PING                     → OK PONG
 *   RESET                    → OK (hard reset CPC)
 *   DISK A|B INSERT <path>   → OK / ERR
 *   DISK A|B EJECT           → OK
 *   DISK A|B STATUS          → OK INSERTED <name> / OK EMPTY
 *   TAPE INSERT <path>       → OK / ERR
 *   TAPE EJECT               → OK
 *   TAPE STATUS              → OK INSERTED / OK EMPTY
 *   CART INSERT <path>       → OK / ERR
 *   CART EJECT               → OK
 *   CART STATUS              → OK INSERTED <name> / OK EMPTY
 *   TYPE <text>              → OK (autotype, \r in text = Enter)
 *   KEY <row> <bit> PRESS    → OK (press key in matrix)
 *   KEY <row> <bit> RELEASE  → OK (release key)
 *   KEYS RESET               → OK (release all keys)
 *   CAT                      → OK <dir listing> (list current SD dir)
 *   CD <path>                → OK / ERR
 *   STATUS                   → OK ALIVE model=<m> ram=<k>
 *   HELP                     → OK (print command list)
 */

#include "cpc_serial_console.h"
#include "cpc_adapter.h"
#include "cpc_autotype.h"
#include "cpc_loader.h"
#include "crash_handler.h"

#include "pico/stdlib.h"
#include "hardware/structs/watchdog.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define CMD_BUF_SIZE 256

static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

/* ---- helpers ---------------------------------------------------------- */

/* Case-insensitive prefix match. Returns pointer past the prefix, or NULL. */
static const char *match_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix))
            return NULL;
        s++;
        prefix++;
    }
    /* skip one optional space after prefix */
    if (*s == ' ') s++;
    return s;
}

/* Skip leading whitespace. */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ---- command handlers ------------------------------------------------- */

static void cmd_ping(void) {
    printf("OK PONG\n");
}

static void cmd_help(void) {
    printf("OK COMMANDS: PING RESET DISK TAPE CART TYPE KEY KEYS CAT CD STATUS HELP\n");
}

static void cmd_reset(void) {
    cpc_engine_reset();
    printf("OK\n");
}

static void cmd_status(void) {
    printf("OK ALIVE scratch0=0x%08lX scratch1=0x%08lX scratch2=0x%08lX scratch3=0x%08lX wdog_ctrl=0x%08lX\n",
           (unsigned long)watchdog_hw->scratch[0],
           (unsigned long)watchdog_hw->scratch[1],
           (unsigned long)watchdog_hw->scratch[2],
           (unsigned long)watchdog_hw->scratch[3],
           (unsigned long)watchdog_hw->ctrl);
}

static void cmd_disk(const char *args) {
    /* DISK A|B INSERT|EJECT|STATUS [path] */
    int drv = -1;
    if (toupper((unsigned char)args[0]) == 'A') drv = 0;
    else if (toupper((unsigned char)args[0]) == 'B') drv = 1;
    if (drv < 0) { printf("ERR bad drive (use A or B)\n"); return; }

    const char *rest = skip_ws(args + 1);
    const char *p;

    if ((p = match_prefix(rest, "INSERT"))) {
        p = skip_ws(p);
        if (*p == '\0') { printf("ERR missing path\n"); return; }
        int rc = cpc_disk_insert(drv, p);
        if (rc == 0)
            printf("OK\n");
        else
            printf("ERR load failed (%d)\n", rc);
    } else if (match_prefix(rest, "EJECT")) {
        cpc_disk_eject(drv);
        printf("OK\n");
    } else if (match_prefix(rest, "STATUS")) {
        if (cpc_disk_is_inserted(drv)) {
            const char *name = cpc_disk_filename(drv);
            printf("OK INSERTED %s\n", name ? name : "(unknown)");
        } else {
            printf("OK EMPTY\n");
        }
    } else {
        printf("ERR disk subcommand? INSERT|EJECT|STATUS\n");
    }
}

static void cmd_tape(const char *args) {
    const char *p;

    if ((p = match_prefix(args, "INSERT"))) {
        p = skip_ws(p);
        if (*p == '\0') { printf("ERR missing path\n"); return; }
        int rc = cpc_tape_insert(p);
        if (rc == 0)
            printf("OK\n");
        else
            printf("ERR load failed (%d)\n", rc);
    } else if (match_prefix(args, "EJECT")) {
        cpc_tape_eject();
        printf("OK\n");
    } else if (match_prefix(args, "STATUS")) {
        if (cpc_tape_is_loaded())
            printf("OK INSERTED\n");
        else
            printf("OK EMPTY\n");
    } else {
        printf("ERR tape subcommand? INSERT|EJECT|STATUS\n");
    }
}

static void cmd_cart(const char *args) {
    const char *p;

    if ((p = match_prefix(args, "INSERT"))) {
        p = skip_ws(p);
        if (*p == '\0') { printf("ERR missing path\n"); return; }
        int rc = cpc_cartridge_insert(p);
        if (rc == 0)
            printf("OK\n");
        else
            printf("ERR load failed (%d)\n", rc);
    } else if (match_prefix(args, "EJECT")) {
        cpc_cartridge_eject();
        printf("OK\n");
    } else if (match_prefix(args, "STATUS")) {
        if (cpc_cartridge_is_loaded()) {
            const char *name = cpc_cartridge_filename();
            printf("OK INSERTED %s\n", name ? name : "(unknown)");
        } else {
            printf("OK EMPTY\n");
        }
    } else {
        printf("ERR cart subcommand? INSERT|EJECT|STATUS\n");
    }
}

static void cmd_type(const char *args) {
    /* TYPE <text>  — queue for autotype.
     * Literal "\r" in args is converted to \r (Enter).
     * Literal "\n" in args is converted to \n (pause). */
    static char buf[256];
    int out = 0;
    for (int i = 0; args[i] && out < (int)sizeof(buf) - 1; i++) {
        if (args[i] == '\\' && args[i+1] == 'r') {
            buf[out++] = '\r';
            i++;
        } else if (args[i] == '\\' && args[i+1] == 'n') {
            buf[out++] = '\n';
            i++;
        } else {
            buf[out++] = args[i];
        }
    }
    buf[out] = '\0';
    cpc_autotype_set(buf, 0);
    printf("OK\n");
}

static void cmd_key(const char *args) {
    /* KEY <row> <bit> PRESS|RELEASE */
    int row, bit;
    char action[16];
    if (sscanf(args, "%d %d %15s", &row, &bit, action) != 3) {
        printf("ERR usage: KEY <row> <bit> PRESS|RELEASE\n");
        return;
    }
    if (row < 0 || row > 15 || bit < 0 || bit > 7) {
        printf("ERR row 0-15, bit 0-7\n");
        return;
    }
    if (toupper((unsigned char)action[0]) == 'P') {
        cpc_key_matrix_set(row, bit, 1);
        printf("OK\n");
    } else if (toupper((unsigned char)action[0]) == 'R') {
        cpc_key_matrix_set(row, bit, 0);
        printf("OK\n");
    } else {
        printf("ERR action? PRESS or RELEASE\n");
    }
}

static void cmd_keys_reset(void) {
    /* Release all keys by setting all matrix bytes to 0xFF (active-low). */
    uint8_t *matrix = cpc_get_keyboard_matrix();
    if (matrix) memset(matrix, 0xFF, 16);
    printf("OK\n");
}

static void cmd_cat(void) {
    /* List current SD card directory. */
    int count = cpc_disk_rescan();
    printf("OK DIR=%s ENTRIES=%d\n", g_cpc_disk_dir, count);
    for (int i = 0; i < count && i < 50; i++) {
        printf("# %s%s\n",
               g_cpc_disk_entries[i].name,
               g_cpc_disk_entries[i].is_dir ? "/" : "");
    }
    if (count > 50) printf("# ... (%d more)\n", count - 50);
}

static void cmd_cd(const char *args) {
    args = skip_ws(args);
    if (*args == '\0') { printf("ERR missing path\n"); return; }

    if (strcmp(args, "..") == 0) {
        int rc = cpc_disk_enter_parent();
        if (rc >= 0)
            printf("OK DIR=%s\n", g_cpc_disk_dir);
        else
            printf("ERR cannot go up\n");
    } else {
        int rc = cpc_disk_enter_subdir(args);
        if (rc >= 0)
            printf("OK DIR=%s\n", g_cpc_disk_dir);
        else
            printf("ERR dir not found\n");
    }
}

static void cmd_crtc(void) {
    char buf[256];
    cpc_debug_crtc_dump(buf, sizeof(buf));
    printf("OK %s\n", buf);
}

/* ---- dispatch --------------------------------------------------------- */

static void dispatch_command(const char *line) {
    const char *p;

    /* Feed watchdog before potentially long operations (disk load, etc.) */
    crash_handler_feed();

    if ((p = match_prefix(line, "PING")))        { cmd_ping(); }
    else if ((p = match_prefix(line, "HELP")))    { cmd_help(); }
    else if ((p = match_prefix(line, "RESET")))   { cmd_reset(); }
    else if ((p = match_prefix(line, "STATUS")))  { cmd_status(); }
    else if ((p = match_prefix(line, "DISK")))    { cmd_disk(p); }
    else if ((p = match_prefix(line, "TAPE")))    { cmd_tape(p); }
    else if ((p = match_prefix(line, "CART")))     { cmd_cart(p); }
    else if ((p = match_prefix(line, "TYPE")))    { cmd_type(p); }
    else if ((p = match_prefix(line, "KEYS")))    {
        if (match_prefix(p, "RESET")) cmd_keys_reset();
        else printf("ERR keys subcommand? RESET\n");
    }
    else if ((p = match_prefix(line, "KEY")))     { cmd_key(p); }
    else if ((p = match_prefix(line, "CAT")))     { cmd_cat(); }
    else if ((p = match_prefix(line, "CD")))      { cmd_cd(p); }
    else if ((p = match_prefix(line, "CRTC")))    { cmd_crtc(); }
    else if ((p = match_prefix(line, "ASIC")))    {
        char buf[512];
        cpc_debug_asic_dump(buf, sizeof(buf));
        printf("OK %s\n", buf);
    }
    else if ((p = match_prefix(line, "Z80")))     {
        char buf[320];
        cpc_debug_z80_dump(buf, sizeof(buf));
        printf("%s\n", buf);
    }
    else if ((p = match_prefix(line, "MEM")))     {
        p = skip_ws(p);
        unsigned int addr = 0;
        int count = 32;
        sscanf(p, "%x %d", &addr, &count);
        if (count > 256) count = 256;
        printf("OK %04X:", addr);
        for (int i = 0; i < count; i++) {
            printf(" %02X", cpc_debug_read_mem((uint16_t)(addr + i)));
        }
        printf("\n");
    }
    else if ((p = match_prefix(line, "POKE")))    {
        p = skip_ws(p);
        unsigned int addr = 0, val = 0;
        if (sscanf(p, "%x %x", &addr, &val) == 2 && val <= 0xFF) {
            cpc_debug_write_mem((uint16_t)addr, (uint8_t)val);
            printf("OK POKE %04X=%02X\n", addr, val);
        } else {
            printf("ERR usage: POKE <hex_addr> <hex_val>\n");
        }
    }
    else if ((p = match_prefix(line, "FDC TRACE"))) {
        p = skip_ws(p);
        if (*p == '1' || (p[0]=='O' && p[1]=='N')) { cpc_fdc_set_trace(1); printf("OK FDC trace ON\n"); }
        else { cpc_fdc_set_trace(0); printf("OK FDC trace OFF\n"); }
    }
    else { printf("ERR unknown command. Try HELP\n"); }
}

/* ---- public API ------------------------------------------------------- */

void cpc_serial_poll(void) {
    /* Non-blocking: read all available characters. */
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;

        /* Echo character back so interactive use feels natural. */
        if (c == '\r' || c == '\n') {
            putchar('\n');

            /* Terminate and dispatch. */
            cmd_buf[cmd_pos] = '\0';

            /* Skip empty lines. */
            const char *trimmed = skip_ws(cmd_buf);
            if (*trimmed != '\0')
                dispatch_command(trimmed);

            cmd_pos = 0;
        } else if (c == 0x7F || c == '\b') {
            /* Backspace / delete. */
            if (cmd_pos > 0) {
                cmd_pos--;
                printf("\b \b");
            }
        } else if (cmd_pos < CMD_BUF_SIZE - 1 && c >= 0x20) {
            cmd_buf[cmd_pos++] = (char)c;
            putchar(c);
        }
    }
}
