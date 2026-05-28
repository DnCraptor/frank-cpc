/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_tape_loader.c — Tape image mount/eject helpers for the UI.
 */

#include "cpc_tape_loader.h"
#include "cpc_adapter.h"

#include <string.h>
#include <stdio.h>

static char g_tape_path[CPC_TAPE_PATH_LEN] = "";

int cpc_mount_tape(const char *path) {
    if (cpc_tape_insert(path) != 0) {
        g_tape_path[0] = '\0';
        return -1;
    }
    snprintf(g_tape_path, sizeof(g_tape_path), "%s", path);
    printf("cpc_tape: mounted %s\n", path);
    return 0;
}

void cpc_eject_tape(void) {
    cpc_tape_eject();
    g_tape_path[0] = '\0';
    printf("cpc_tape: ejected\n");
}

const char *cpc_mounted_tape_name(void) {
    if (!g_tape_path[0]) return NULL;
    const char *slash = strrchr(g_tape_path, '/');
    return slash ? slash + 1 : g_tape_path;
}
