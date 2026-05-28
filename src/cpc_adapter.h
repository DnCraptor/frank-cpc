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

/* Set direct render target for scanline callback.
 * If set, scanlines are written here instead of cpc_fb.
 * Pass NULL to revert to cpc_fb. */
void cpc_set_render_target(uint8_t *buffer, int stride);

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

/* Cartridge operations (.cpr). Returns 0 on success. */
int cpc_cartridge_insert(const char *path);
void cpc_cartridge_eject(void);
int cpc_cartridge_is_loaded(void);
const char *cpc_cartridge_filename(void);

/* Snapshot operations. Returns 0 on success. */
int cpc_snapshot_save(const char *path);
int cpc_snapshot_load(const char *path);

/* Settings — call before cpc_engine_init() or before cpc_engine_reset(). */
void cpc_set_model(int model);       /* 0=464, 1=664, 2=6128, 3=6128+ */
void cpc_set_ram_size(int kb);       /* 64, 128, or 576 */
void cpc_set_rom(int slot, const char *path);
void cpc_set_jumpers(unsigned int jumpers); /* PPI port B jumper config */
void cpc_set_speed(unsigned int speed);     /* 1..32 (×25 = percent) */
void cpc_set_limit_speed(int enabled);      /* 0=unlimited, 1=cap to speed */
void cpc_set_snd_enabled(int enabled);      /* 0=off, 1=on */
void cpc_set_snd_volume(unsigned int vol);  /* 0..100 */
void cpc_set_snd_stereo(unsigned int mode); /* 0=mono, 1=stereo */

/* Re-calculate audio level tables (e.g. after volume change). */
void cpc_audio_reinit_volume(void);

/* Apply green monitor filter to the hardware palette.
 * green=1: convert all colours to green luminance; green=0: restore normal. */
void cpc_apply_green_monitor(int green);

/* Palette: get current CPC hardware palette as RGB888 values.
 * Returns 32 entries (27 CPC colours + padding). */
void cpc_get_palette_rgb(uint32_t *rgb32);

/* Get the current border colour (ink index 0-31). */
uint8_t cpc_get_border_ink(void);

/* Debug: dump CRTC register state.  Writes a human-readable line into
 * buf (at most buflen bytes).  Returns number of bytes written. */
int cpc_debug_crtc_dump(char *buf, int buflen);

/* Debug: dump ASIC state. */
int cpc_debug_asic_dump(char *buf, int buflen);

/* Debug: dump sprite pixel data */
void cpc_debug_sprite_dump(int id);

/* Debug: dump Z80 register state. */
int cpc_debug_z80_dump(char *buf, int buflen);

/* Debug: read Z80 memory (through bank mapping). */
uint8_t cpc_debug_read_mem(uint16_t addr);
void cpc_debug_write_mem(uint16_t addr, uint8_t val);

/* FDC trace control — defined in fdc.cpp */
void cpc_fdc_set_trace(int enable);

/* Sound: check if audio samples are ready after a frame. */
int cpc_audio_samples_ready(void);
/* Get pointer and count of pending audio samples (16-bit signed). */
const int16_t *cpc_audio_get_samples(int *count);

#ifdef __cplusplus
}
#endif

#endif /* CPC_ADAPTER_H */
