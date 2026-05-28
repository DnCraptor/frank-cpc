/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_loader.h — SD card disk image browser and mount helpers.
 */
#ifndef CPC_LOADER_H
#define CPC_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CPC_DISK_MAX_ENTRIES  200
#define CPC_DISK_FILENAME_LEN  64
#define CPC_DISK_PATH_LEN     160

typedef struct {
    char name[CPC_DISK_FILENAME_LEN];
    bool is_dir;
} cpc_disk_entry_t;

/* PSRAM-backed entry table, populated by cpc_disk_rescan(). */
extern cpc_disk_entry_t *g_cpc_disk_entries;
extern int               g_cpc_disk_entry_count;
extern char              g_cpc_disk_dir[CPC_DISK_PATH_LEN];

/* File type filter for the browser. */
typedef enum {
    CPC_FILTER_DISK = 0,  /* show .dsk/.ipf files only */
    CPC_FILTER_TAPE,      /* show .cdt/.cas files only */
    CPC_FILTER_CARTRIDGE, /* show .cpr files only */
} cpc_filter_t;

/* Set the file type filter for subsequent cpc_disk_rescan() calls. */
void cpc_disk_set_filter(cpc_filter_t filter);

/* Scan g_cpc_disk_dir for media files and subdirs. Returns entry count. */
int cpc_disk_rescan(void);

/* Enter a subdirectory by name; rescans. Returns new entry count or -1. */
int cpc_disk_enter_subdir(const char *name);

/* Leave one directory level; rescans. */
int cpc_disk_enter_parent(void);

/* Build the full absolute path for entry idx into buf. */
void cpc_disk_entry_path(int idx, char *buf, size_t sz);

/* Mount .dsk at path into drive drv (0=A, 1=B).
 * Returns 0 on success. */
int cpc_mount_disk(int drv, const char *path);

/* Eject disk from drive. */
void cpc_eject_disk(int drv);

/* Basename of the currently mounted disk, or NULL if empty. */
const char *cpc_mounted_disk_name(int drv);

/* Auto-load drivea.dsk / driveb.dsk from /cpc/disk on startup. */
void cpc_disk_autoload(void);

/* Returns true if the filename has a tape extension (.cdt/.cas). */
int cpc_is_tape_file(const char *name);

/* Returns true if the filename has a cartridge extension (.cpr). */
int cpc_is_cpr_file(const char *name);

/* Returns true if the filename has an IPF extension (.ipf). */
int cpc_is_ipf_file(const char *name);

#endif /* CPC_LOADER_H */
