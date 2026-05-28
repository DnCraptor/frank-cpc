/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "dialogs.h"

char buf[255];
char cmd[255];
int PassDriveSelect;

static FIL g_dialog_file;

/* When non-empty, SelectDiskFile() uses this path instead of scanning. */
static char g_pending_disk_path[256] = "";

void SetPendingDiskPath(const char *path) {
    snprintf(g_pending_disk_path, sizeof(g_pending_disk_path), "%s", path);
}

static int open_selected_disk(const char *path, int *wrprotect) {
    if (f_open(&g_dialog_file, path, FA_READ | FA_WRITE) == FR_OK) {
        *wrprotect = 0;
        return (int)(uintptr_t)&g_dialog_file;
    }
    if (f_open(&g_dialog_file, path, FA_READ) == FR_OK) {
        *wrprotect = 1;
        return (int)(uintptr_t)&g_dialog_file;
    }
    return -1;
}

int SelectDiskFile(char *filename, int *DrvNum, int *WrProtect) {
    /* UI-selected path takes priority */
    if (g_pending_disk_path[0]) {
        int fid = open_selected_disk(g_pending_disk_path, WrProtect);
        snprintf(filename, 255, "%s", g_pending_disk_path);
        g_pending_disk_path[0] = '\0';
        if (fid > 0) {
            printf("Inserted disk[%d]: %s\n", *DrvNum, filename);
            return fid;
        }
        printf("Failed to open disk: %s\n", filename);
        return -1;
    }

    /* Fallback: try default filenames */
    const char *preferred = (*DrvNum == 0) ? "/cpc/disk/drivea.dsk" : "/cpc/disk/driveb.dsk";
    int fid = open_selected_disk(preferred, WrProtect);
    if (fid > 0) {
        snprintf(filename, 255, "%s", preferred);
        printf("Inserted disk[%d]: %s\n", *DrvNum, filename);
        return fid;
    }
    return -1;
}

int SetupDialog(void) { printf("Setup dialog not available on Pico build\n"); return 0; }
void InfoDialog(void) { printf("frank-cpc RP2350 build\n"); }
void FirstStartDialog(void) {}
void PrintCmdLinePars(void) { printf("Command-line UI not available on Pico build\n"); }
void SaveScreenImage(void) { printf("Screen save not available on Pico build\n"); }
