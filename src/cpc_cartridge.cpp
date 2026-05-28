/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_cartridge.cpp — CPC Plus cartridge (.cpr) loading support.
 *
 * CPR format: RIFF/AMS! container with "cbXX" chunks, each up to 16KB.
 * Reference: http://www.cpcwiki.eu/index.php/Format:CPR_CPC_Plus_cartridge_file_format
 *
 * Cartridge pages are loaded into PSRAM and mapped into the ROM banking
 * system via memmap_ROM[].  Page 0 becomes the lower ROM (pbROMlo).
 */

#include "cpc_cartridge.h"
#include "cpc_adapter.h"
#include "cpc_dandanator.h"

#include "cap32/cap32.h"
#include "cap32/disk.h"

#include <cstring>
#include <cstdio>

extern "C" {
#include "psram_allocator.h"
#include "ff.h"
}

/* Caprice32 globals we need to touch */
extern byte *pbROMlo;
extern byte *pbROMhi;
extern byte *pbExpansionROM;
extern byte *memmap_ROM[256];
extern t_CPC CPC;

/* Cartridge state */
static byte *cartridge_pages[CPR_MAX_PAGES] = {};
static char  cartridge_path[256] = "";
static bool  cartridge_loaded = false;

/* Original ROM pointers to restore on eject */
static byte *saved_pbROMlo = nullptr;
static int   saved_model = 2;  /* CPC model before CPR load */

/* ------------------------------------------------------------------ */

static uint32_t read_le32(const byte *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void cpc_cartridge_eject(void) {
    if (!cartridge_loaded) return;

    /* Restore original lower ROM */
    if (saved_pbROMlo) {
        pbROMlo = saved_pbROMlo;
        saved_pbROMlo = nullptr;
    }

    /* Unmap cartridge pages from ROM banking and free PSRAM */
    for (int i = 0; i < CPR_MAX_PAGES; i++) {
        if (cartridge_pages[i]) {
            memmap_ROM[i] = nullptr;
            psram_free(cartridge_pages[i]);
            cartridge_pages[i] = nullptr;
        }
    }

    /* Restore upper ROM mapping */
    if (pbROMhi) {
        memmap_ROM[0] = pbROMhi;
        pbExpansionROM = pbROMhi;
    }

    cartridge_path[0] = '\0';
    cartridge_loaded = false;

    /* Restore original CPC model */
    cpc_set_model(saved_model);

    printf("cartridge: ejected\n");
}

int cpc_cartridge_insert(const char *path) {
    FIL f;
    UINT br;
    byte header[12];

    if (f_open(&f, path, FA_READ) != FR_OK) {
        printf("cartridge: cannot open %s\n", path);
        return -1;
    }

    /* Eject any previous cartridge and conflicting Dandanator media */
    cpc_cartridge_eject();
    dandanator_eject();

    /* Read and validate RIFF header (12 bytes) */
    f_read(&f, header, 12, &br);
    if (br < 12) {
        printf("cartridge: file too short\n");
        f_close(&f);
        return -1;
    }

    if (std::memcmp(header, "RIFF", 4) != 0) {
        printf("cartridge: not a RIFF file\n");
        f_close(&f);
        return -1;
    }

    if (std::memcmp(header + 8, "AMS!", 4) != 0) {
        printf("cartridge: not a CPR file (no AMS! tag)\n");
        f_close(&f);
        return -1;
    }

    uint32_t total_size = read_le32(header + 4);

    /* Save current lower ROM pointer for restore on eject */
    saved_pbROMlo = pbROMlo;

    /* Parse chunks */
    uint32_t offset = 12; /* past RIFF header */
    int page_index = 0;

    while (offset < total_size + 8 && page_index < CPR_MAX_PAGES) {
        byte chunk_header[8];
        f_read(&f, chunk_header, 8, &br);
        if (br < 8) break;
        offset += 8;

        uint32_t chunk_size = read_le32(chunk_header + 4);

        /* Allocate a full 16KB page in PSRAM */
        byte *page = (byte *)psram_malloc(CPR_PAGE_SIZE);
        if (!page) {
            printf("cartridge: PSRAM alloc failed at page %d\n", page_index);
            cpc_cartridge_eject();
            f_close(&f);
            return -1;
        }
        std::memset(page, 0, CPR_PAGE_SIZE);

        /* Read chunk data (up to 16KB) */
        uint32_t to_read = chunk_size < CPR_PAGE_SIZE ? chunk_size : CPR_PAGE_SIZE;
        if (to_read > 0) {
            f_read(&f, page, to_read, &br);
            offset += br;
        }

        /* Skip excess data beyond 16KB */
        if (chunk_size > CPR_PAGE_SIZE) {
            uint32_t skip = chunk_size - CPR_PAGE_SIZE;
            f_lseek(&f, f_tell(&f) + skip);
            offset += skip;
        }

        /* RIFF chunks are padded to even size */
        if (chunk_size & 1) {
            f_lseek(&f, f_tell(&f) + 1);
            offset++;
        }

        cartridge_pages[page_index] = page;
        memmap_ROM[page_index] = page;
        page_index++;
    }

    f_close(&f);

    if (page_index == 0) {
        printf("cartridge: no pages found in %s\n", path);
        cpc_cartridge_eject();
        return -1;
    }

    /* Page 0 is the lower ROM (boot ROM) */
    pbROMlo = cartridge_pages[0];

    /* Set upper ROM to page 0 as well (BASIC replacement) */
    pbExpansionROM = cartridge_pages[0];
    memmap_ROM[0] = cartridge_pages[0];

    std::strncpy(cartridge_path, path, sizeof(cartridge_path) - 1);
    cartridge_path[sizeof(cartridge_path) - 1] = '\0';
    cartridge_loaded = true;

    /* CPR cartridges require CPC Plus mode (model 3) */
    saved_model = CPC.model;
    cpc_set_model(3);

    printf("cartridge: loaded %s (%d pages), switching to CPC Plus mode\n", path, page_index);
    return 0;
}

int cpc_cartridge_is_loaded(void) {
    return cartridge_loaded ? 1 : 0;
}

const char *cpc_cartridge_filename(void) {
    if (!cartridge_loaded || !cartridge_path[0]) return nullptr;
    const char *slash = std::strrchr(cartridge_path, '/');
    return slash ? slash + 1 : cartridge_path;
}

unsigned char *cpc_cartridge_get_page(int page) {
    if (page < 0 || page >= CPR_MAX_PAGES) return nullptr;
    return cartridge_pages[page];
}
