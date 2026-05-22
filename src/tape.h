/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape.h — CDT/TZX and CAS tape image playback engine.
 *
 * Adapted from CPCEC (C) Cesar Nicolas-Gonzalez, simplified for
 * the Pico/RP2350 target (FatFS, no recording, no PZX/WAV).
 */
#ifndef TAPE_H
#define TAPE_H

#include <stdint.h>
#include <stdbool.h>

/* Tape format types */
#define TAPE_TYPE_NONE  0
#define TAPE_TYPE_TZX   2   /* CDT/TZX */
#define TAPE_TYPE_CAS   3   /* CAS (Kansas City) */

/* Initialise tape subsystem (call once at startup). */
void tape_init(void);

/* Open a tape image file. Returns 0 on success, non-zero on error. */
int tape_open(const char *path);

/* Close the currently loaded tape. */
void tape_close(void);

/* Advance tape playback by `ticks` CPC T-states (4 MHz clock).
 * Only call when tape motor is on and a tape is loaded. */
void tape_main(int ticks);

/* Return the current tape signal level for PPI Port B bit 7.
 * When GPIO22 mode is active, reads the pin directly instead. */
int tape_get_status(void);

/* Return true if a tape is currently loaded (file or GPIO mode). */
bool tape_is_loaded(void);

/* Motor control — called from PPI Port C handler. */
void tape_set_motor(int on);
int  tape_get_motor(void);

/* GPIO22 direct audio input mode. */
void tape_set_gpio_mode(bool active);
bool tape_get_gpio_mode(void);

/* Polarity inversion (useful for some tape signals). */
void tape_set_polarity(int pol);

#endif /* TAPE_H */
