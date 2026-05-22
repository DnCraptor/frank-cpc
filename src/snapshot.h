/*
 * CPC SNA Snapshot support for frank-cpc
 * Supports CPCEMU V3 SNA format (256-byte header + 64K/128K RAM dump)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SNAPSHOT_H
#define SNAPSHOT_H 1

/* Save current emulator state to an SNA file.
 * Returns 0 on success, -1 on failure. */
int snapshot_save(const char *filename);

/* Load emulator state from an SNA file.
 * Returns 0 on success, -1 on failure. */
int snapshot_load(const char *filename);

#endif
