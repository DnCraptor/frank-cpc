/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_adapter.h — C-compatible interface between caprice32 engine and Pico platform.
 */
#ifndef CPC_ADAPTER_H
#define CPC_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize caprice32 engine: allocate RAM, load ROMs, set up state. */
int cpc_engine_init(void);

/* Reset the CPC (preserving loaded disks/tape). */
void cpc_engine_reset(void);

/* Run one 20ms frame (~80000 Z80 cycles). Renders into cpc_fb[][]. */
void cpc_engine_run_frame(void);

/* Keyboard matrix: press/release a key in the CPC matrix.
 * row = 0..9, bit = 0..7.  Active-low: press clears, release sets. */
void cpc_key_matrix_set(int row, int bit, int pressed);

/* Get pointer to keyboard matrix (16 bytes, active-low). */
uint8_t *cpc_get_keyboard_matrix(void);

/* Disk operations (FatFS paths). Returns 0 on success. */
int cpc_disk_insert(int drive, const char *path);
void cpc_disk_eject(int drive);
int cpc_disk_is_inserted(int drive);
const char *cpc_disk_filename(int drive);

/* Tape operations. Returns 0 on success. */
int cpc_tape_insert(const char *path);
void cpc_tape_eject(void);
int cpc_tape_is_loaded(void);
void cpc_tape_set_motor(int on);
int cpc_tape_get_motor(void);
int cpc_tape_get_level(void);
void cpc_tape_rewind(void);

/* Snapshot operations. Returns 0 on success. */
int cpc_snapshot_save(const char *path);
int cpc_snapshot_load(const char *path);

/* Settings — call before cpc_engine_init() or before cpc_engine_reset(). */
void cpc_set_model(int model);       /* 0=464, 1=664, 2=6128 */
void cpc_set_ram_size(int kb);       /* 64, 128, or 576 */
void cpc_set_rom(int slot, const char *path);

/* Palette: get current CPC hardware palette as RGB888 values.
 * Returns 32 entries (27 CPC colours + padding). */
void cpc_get_palette_rgb(uint32_t *rgb32);

/* Get the current border colour (ink index 0-31). */
uint8_t cpc_get_border_ink(void);

/* Sound: check if audio samples are ready after a frame. */
int cpc_audio_samples_ready(void);
/* Get pointer and count of pending audio samples (16-bit signed). */
const int16_t *cpc_audio_get_samples(int *count);

#ifdef __cplusplus
}
#endif

#endif /* CPC_ADAPTER_H */
