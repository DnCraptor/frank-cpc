/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape_rec.h — Tape recording: capture cassette-out transitions and
 * encode as CDT/TZX Direct Recording block (type 0x15).
 */
#ifndef TAPE_REC_H
#define TAPE_REC_H

#include <stdbool.h>

/* Start recording. Allocates internal buffer. */
void tape_rec_start(void);

/* Stop recording and save the captured data to a CDT file.
 * Returns 0 on success, -1 on failure. */
int tape_rec_stop(const char *path);

/* Call from PPI write handler whenever the cassette-write bit (Port C bit 5)
 * changes. `level` is 0 or 1, `tstates` is the current T-state counter. */
void tape_rec_write_bit(int level, unsigned long tstates);

/* Returns true if currently recording. */
bool tape_rec_active(void);

#endif /* TAPE_REC_H */
