/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_loader.c — SD card disk image scanner and mount helpers.
 */

#include "cpc_loader.h"
#include "cpc_settings.h"
#include "cpc_tape_loader.h"
#include "cpc_adapter.h"
#include "ff.h"
#include "psram_allocator.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- entry table ---------------------------------------------------- */

cpc_disk_entry_t *g_cpc_disk_entries    = NULL;
int               g_cpc_disk_entry_count = 0;
char              g_cpc_disk_dir[CPC_DISK_PATH_LEN] = "/cpc/disk";

/* Mounted disk paths (persisted so name stays valid after mount). */
static char g_mounted_path[2][CPC_DISK_PATH_LEN] = { "", "" };

static void ensure_table(void) {
    if (g_cpc_disk_entries) return;
    g_cpc_disk_entries = (cpc_disk_entry_t *)psram_malloc(
        sizeof(cpc_disk_entry_t) * CPC_DISK_MAX_ENTRIES);
}

static int is_dsk(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    if (name[n-4] != '.') return 0;
    char e1 = tolower((unsigned char)name[n-3]);
    char e2 = tolower((unsigned char)name[n-2]);
    char e3 = tolower((unsigned char)name[n-1]);
    /* .dsk */
    if (e1 == 'd' && e2 == 's' && e3 == 'k') return 1;
    /* .ipf */
    if (e1 == 'i' && e2 == 'p' && e3 == 'f') return 1;
    return 0;
}

static int is_tape(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    if (name[n-4] != '.') return 0;
    char e1 = tolower((unsigned char)name[n-3]);
    char e2 = tolower((unsigned char)name[n-2]);
    char e3 = tolower((unsigned char)name[n-1]);
    /* .cdt or .cas */
    if (e1 == 'c' && e2 == 'd' && e3 == 't') return 1;
    if (e1 == 'c' && e2 == 'a' && e3 == 's') return 1;
    return 0;
}

static int is_cpr(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    if (name[n-4] != '.') return 0;
    char e1 = tolower((unsigned char)name[n-3]);
    char e2 = tolower((unsigned char)name[n-2]);
    char e3 = tolower((unsigned char)name[n-1]);
    return (e1 == 'c' && e2 == 'p' && e3 == 'r');
}

static int is_ipf(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    if (name[n-4] != '.') return 0;
    char e1 = tolower((unsigned char)name[n-3]);
    char e2 = tolower((unsigned char)name[n-2]);
    char e3 = tolower((unsigned char)name[n-1]);
    return (e1 == 'i' && e2 == 'p' && e3 == 'f');
}

static int is_media(const char *name) {
    return is_dsk(name) || is_tape(name) || is_cpr(name);
}

static cpc_filter_t g_filter = CPC_FILTER_DISK;

void cpc_disk_set_filter(cpc_filter_t filter) {
    g_filter = filter;
}

static int matches_filter(const char *name) {
    switch (g_filter) {
        case CPC_FILTER_DISK:      return is_dsk(name);
        case CPC_FILTER_TAPE:      return is_tape(name);
        case CPC_FILTER_CARTRIDGE: return is_cpr(name);
        default:                   return is_media(name);
    }
}

static int cmp_entry(const void *pa, const void *pb) {
    const cpc_disk_entry_t *a = (const cpc_disk_entry_t *)pa;
    const cpc_disk_entry_t *b = (const cpc_disk_entry_t *)pb;
    if (a->is_dir && !b->is_dir) return -1;
    if (!a->is_dir && b->is_dir) return  1;
    return strcasecmp(a->name, b->name);
}

int cpc_disk_rescan(void) {
    ensure_table();
    if (!g_cpc_disk_entries) return 0;

    g_cpc_disk_entry_count = 0;

    DIR dir;
    FILINFO fi;
    FRESULT fr = f_opendir(&dir, g_cpc_disk_dir);
    if (fr != FR_OK) {
        /* Fallback to root */
        snprintf(g_cpc_disk_dir, sizeof(g_cpc_disk_dir), "/cpc/disk");
        fr = f_opendir(&dir, g_cpc_disk_dir);
        if (fr != FR_OK) return 0;
    }

    while (g_cpc_disk_entry_count < CPC_DISK_MAX_ENTRIES) {
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK || fi.fname[0] == 0) break;
        if (fi.fname[0] == '.') continue;

        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (!is_dir && !matches_filter(fi.fname)) continue;

        size_t n = strlen(fi.fname);
        if (n >= CPC_DISK_FILENAME_LEN) continue;

        cpc_disk_entry_t *e = &g_cpc_disk_entries[g_cpc_disk_entry_count++];
        memcpy(e->name, fi.fname, n + 1);
        e->is_dir = is_dir;
    }
    f_closedir(&dir);

    qsort(g_cpc_disk_entries, (size_t)g_cpc_disk_entry_count,
          sizeof(g_cpc_disk_entries[0]), cmp_entry);

    printf("cpc_loader: %d entries in %s\n", g_cpc_disk_entry_count, g_cpc_disk_dir);
    return g_cpc_disk_entry_count;
}

int cpc_disk_enter_subdir(const char *name) {
    size_t cur = strlen(g_cpc_disk_dir);
    size_t add = strlen(name);
    if (cur + 1 + add + 1 >= sizeof(g_cpc_disk_dir)) return -1;
    if (strcmp(g_cpc_disk_dir, "/") != 0) {
        g_cpc_disk_dir[cur] = '/';
        memcpy(g_cpc_disk_dir + cur + 1, name, add + 1);
    } else {
        memcpy(g_cpc_disk_dir + 1, name, add + 1);
    }
    return cpc_disk_rescan();
}

int cpc_disk_enter_parent(void) {
    if (strcmp(g_cpc_disk_dir, "/") == 0) return g_cpc_disk_entry_count;
    char *slash = strrchr(g_cpc_disk_dir, '/');
    if (!slash) return g_cpc_disk_entry_count;
    if (slash == g_cpc_disk_dir) g_cpc_disk_dir[1] = 0;
    else                         *slash = 0;
    return cpc_disk_rescan();
}

void cpc_disk_entry_path(int idx, char *buf, size_t sz) {
    if (idx < 0 || idx >= g_cpc_disk_entry_count) { if (sz) buf[0] = 0; return; }
    const char *name = g_cpc_disk_entries[idx].name;
    if (strcmp(g_cpc_disk_dir, "/") == 0)
        snprintf(buf, sz, "/%s", name);
    else
        snprintf(buf, sz, "%s/%s", g_cpc_disk_dir, name);
}

/* ---- mount / eject -------------------------------------------------- */

int cpc_mount_disk(int drv, const char *path) {
    if (drv < 0 || drv > 1) return -1;
    if (cpc_disk_insert(drv, path) != 0) {
        g_mounted_path[drv][0] = '\0';
        return -1;
    }
    snprintf(g_mounted_path[drv], sizeof(g_mounted_path[drv]), "%s", path);
    printf("cpc_loader: drive %c = %s\n", 'A' + drv, path);
    return 0;
}

void cpc_eject_disk(int drv) {
    if (drv < 0 || drv > 1) return;
    cpc_disk_eject(drv);
    g_mounted_path[drv][0] = '\0';
    printf("cpc_loader: drive %c ejected\n", 'A' + drv);
}

const char *cpc_mounted_disk_name(int drv) {
    if (drv < 0 || drv > 1 || !g_mounted_path[drv][0]) return NULL;
    const char *slash = strrchr(g_mounted_path[drv], '/');
    return slash ? slash + 1 : g_mounted_path[drv];
}

/* ---- autoload ------------------------------------------------------- */

void cpc_disk_autoload(void) {
    FILINFO fi;

    /* Priority 1: explicit paths from settings (disk_a / disk_b). */
    if (g_cpc_settings.disk_a[0]) {
        printf("cpc_loader: autoloading drive A (settings): %s\n", g_cpc_settings.disk_a);
        cpc_mount_disk(0, g_cpc_settings.disk_a);
    }
    if (g_cpc_settings.disk_b[0]) {
        printf("cpc_loader: autoloading drive B (settings): %s\n", g_cpc_settings.disk_b);
        cpc_mount_disk(1, g_cpc_settings.disk_b);
    }

    /* Priority 2: well-known fixed names. */
    const char *paths[2] = { "/cpc/disk/drivea.dsk", "/cpc/disk/driveb.dsk" };
    for (int d = 0; d < 2; ++d) {
        if (g_mounted_path[d][0]) continue; /* already mounted via settings */
        if (f_stat(paths[d], &fi) == FR_OK) {
            printf("cpc_loader: autoloading drive %c: %s\n", 'A' + d, paths[d]);
            cpc_mount_disk(d, paths[d]);
        }
    }

    /* Priority 3: if drive A still empty and there is exactly one .dsk in the
     * disk directory, auto-mount it — convenient for single-game setups. */
    if (!g_mounted_path[0][0]) {
        cpc_disk_rescan();
        int dsk_count = 0;
        int dsk_idx   = -1;
        for (int i = 0; i < g_cpc_disk_entry_count; ++i) {
            if (!g_cpc_disk_entries[i].is_dir && is_dsk(g_cpc_disk_entries[i].name)) {
                dsk_count++;
                dsk_idx = i;
            }
        }
        if (dsk_count == 1) {
            char path[CPC_DISK_PATH_LEN];
            cpc_disk_entry_path(dsk_idx, path, sizeof(path));
            printf("cpc_loader: single disk found, autoloading drive A: %s\n", path);
            cpc_mount_disk(0, path);
        }
    }
}

int cpc_is_tape_file(const char *name) {
    return is_tape(name);
}

int cpc_is_cpr_file(const char *name) {
    return is_cpr(name);
}

int cpc_is_ipf_file(const char *name) {
    return is_ipf(name);
}
