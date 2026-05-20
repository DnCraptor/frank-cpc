/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_loader.c — SD card disk image scanner and mount helpers.
 */

#include "cpc_loader.h"
#include "cpc_settings.h"
#include "dialogs.h"
#include "disc.h"
#include "ff.h"
#include "psram_allocator.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

extern void InsertDisk(int DrvNum);

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
    return (name[n-4] == '.')
        && (tolower((unsigned char)name[n-3]) == 'd')
        && (tolower((unsigned char)name[n-2]) == 's')
        && (tolower((unsigned char)name[n-1]) == 'k');
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
        if (!is_dir && !is_dsk(fi.fname)) continue;

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
    SetPendingDiskPath(path);
    InsertDisk(drv);
    /* Check it actually loaded (fid will be -1 if it failed) */
    if (dsk[drv].fid < 0 && dsk[drv].ImageName[0] == '\0') {
        g_mounted_path[drv][0] = '\0';
        return -1;
    }
    snprintf(g_mounted_path[drv], sizeof(g_mounted_path[drv]), "%s", path);
    printf("cpc_loader: drive %c = %s\n", 'A' + drv, path);
    return 0;
}

void cpc_eject_disk(int drv) {
    if (drv < 0 || drv > 1) return;
    dsk[drv].fid = -1;
    dsk[drv].ImageName[0] = '\0';
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
            if (!g_cpc_disk_entries[i].is_dir) {
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
