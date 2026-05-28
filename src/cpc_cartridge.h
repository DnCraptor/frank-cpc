/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_cartridge.h — CPC Plus cartridge (.cpr) loading support.
 *
 * CPR files use RIFF/AMS! format, containing up to 32 pages of 16KB ROM.
 * Used by CPC Plus (6128+) and GX4000 cartridge games.
 */
#ifndef CPC_CARTRIDGE_H
#define CPC_CARTRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define CPR_MAX_PAGES    32
#define CPR_PAGE_SIZE    (16 * 1024)
#define CPR_MAX_SIZE     (CPR_MAX_PAGES * CPR_PAGE_SIZE)

/* Load a .cpr cartridge image from the given path.
 * Allocates cartridge pages in PSRAM and maps them into the ROM banking system.
 * Returns 0 on success, -1 on failure. */
int cpc_cartridge_insert(const char *path);

/* Eject the currently loaded cartridge and free resources. */
void cpc_cartridge_eject(void);

/* Returns 1 if a cartridge is currently loaded. */
int cpc_cartridge_is_loaded(void);

/* Returns the filename of the currently loaded cartridge, or NULL. */
const char *cpc_cartridge_filename(void);

/* Returns the ROM page pointer for the given cartridge page (0-31), or NULL. */
unsigned char *cpc_cartridge_get_page(int page);

#ifdef __cplusplus
}
#endif

#endif /* CPC_CARTRIDGE_H */
