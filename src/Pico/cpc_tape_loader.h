/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_tape_loader.h — Tape image mount/eject helpers.
 */
#ifndef CPC_TAPE_LOADER_H
#define CPC_TAPE_LOADER_H

#define CPC_TAPE_PATH_LEN 160

/* Mount a tape image (.cdt/.cas) at path.  Returns 0 on success. */
int cpc_mount_tape(const char *path);

/* Eject the currently mounted tape. */
void cpc_eject_tape(void);

/* Basename of the currently mounted tape, or NULL if none. */
const char *cpc_mounted_tape_name(void);

#endif /* CPC_TAPE_LOADER_H */
