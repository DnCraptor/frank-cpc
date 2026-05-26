/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ipf.h — IPF (Interchangeable Preservation Format) disk image support.
 */
#ifndef CPC_IPF_H
#define CPC_IPF_H

#include "cap32/disk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load an IPF disk image into a drive.
 * Reads the file via FatFS into PSRAM, then uses the capsimg library
 * to decode the MFM track data.
 * Returns 0 on success, -1 on failure. */
int cpc_ipf_load(const char *path, t_drive *drive);

#ifdef __cplusplus
}
#endif

#endif /* CPC_IPF_H */
