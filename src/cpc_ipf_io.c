/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ipf_io.c — FatFS file I/O helper for the IPF loader.
 *
 * Isolated from cpc_ipf.cpp to avoid BYTE/WORD/DWORD typedef conflicts
 * between FatFS (unsigned) and capsimg (signed).
 */

#include "ff.h"
#include "psram_allocator.h"
#include <stdio.h>
#include <stdint.h>

uint8_t *ipf_read_file(const char *path, size_t *out_size) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("ipf: cannot open %s\n", path);
        return NULL;
    }

    FSIZE_t sz = f_size(&f);
    if (sz == 0 || sz > 4 * 1024 * 1024) {
        printf("ipf: file too large or empty (%lu)\n", (unsigned long)sz);
        f_close(&f);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)psram_malloc((size_t)sz);
    if (!buf) {
        printf("ipf: PSRAM alloc failed for %lu bytes\n", (unsigned long)sz);
        f_close(&f);
        return NULL;
    }

    UINT br;
    f_read(&f, buf, (UINT)sz, &br);
    f_close(&f);

    *out_size = (size_t)br;
    return buf;
}
