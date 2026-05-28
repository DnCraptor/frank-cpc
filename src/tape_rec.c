/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape_rec.c — Tape recording: capture cassette-out bit transitions and
 * encode as CDT/TZX Direct Recording block (type 0x15).
 *
 * The CPC cassette-write signal comes from PPI Port C bit 5. Each
 * transition is timestamped in T-states. On save, we produce a TZX/CDT
 * block type 0x15 (Direct Recording) which stores raw sample data at a
 * configurable sample rate.
 */

#include "tape_rec.h"
#include <string.h>
#include <stdio.h>

#ifdef PICO_BUILD
#include "ff.h"
#include "psram_allocator.h"
#define REC_ALLOC(sz) psram_malloc(sz)
#define REC_FREE(p)   psram_free(p)
#else
#include <stdlib.h>
#define REC_ALLOC(sz) malloc(sz)
#define REC_FREE(p)   free(p)
#endif

/* We record raw bit samples at the CPC's T-state rate (4 MHz).
 * TZX Direct Recording block uses samples at a given T-states-per-sample rate.
 * We use 79 T-states per sample ≈ ~50.6 kHz, matching common CDT practice. */
#define TSTATES_PER_SAMPLE 79

/* Maximum recording length: ~2 minutes at 50.6 kHz = ~6 MB of sample bits.
 * At 8 bits per byte that's ~760 KB. We'll cap at 512 KB = ~80 seconds. */
#define MAX_REC_BYTES (512 * 1024)

static bool     s_recording = false;
static uint8_t *s_buffer    = NULL;
static unsigned s_byte_pos  = 0;   /* current byte offset in buffer */
static unsigned s_bit_pos   = 0;   /* bit position within current byte (7..0) */
static int      s_level     = 0;   /* current cassette-out level */
static unsigned long s_last_tstates = 0;

void tape_rec_start(void) {
    if (s_recording) return;
    if (!s_buffer) {
        s_buffer = (uint8_t *)REC_ALLOC(MAX_REC_BYTES);
        if (!s_buffer) return;
    }
    memset(s_buffer, 0, MAX_REC_BYTES);
    s_byte_pos = 0;
    s_bit_pos  = 7;
    s_level    = 0;
    s_last_tstates = 0;
    s_recording = true;
}

int tape_rec_stop(const char *path) {
    if (!s_recording) return -1;
    s_recording = false;

    /* Calculate total number of sample bits recorded */
    unsigned total_bits = s_byte_pos * 8 + (7 - s_bit_pos);
    if (total_bits == 0) return -1;

    unsigned total_bytes = (total_bits + 7) / 8;
    unsigned last_byte_bits = total_bits % 8;
    if (last_byte_bits == 0) last_byte_bits = 8;

    /* Build TZX/CDT file:
     * - 10-byte TZX header
     * - Block 0x15 (Direct Recording):
     *   1 byte  block ID (0x15)
     *   2 bytes T-states per sample (little-endian)
     *   2 bytes pause after block (ms, little-endian)
     *   1 byte  bits used in last byte
     *   3 bytes data length (little-endian)
     *   N bytes sample data
     */
    uint8_t hdr[10] = { 'Z','X','T','a','p','e','!', 0x1A, 1, 20 };
    uint8_t blk_hdr[9];
    blk_hdr[0] = 0x15;  /* Direct Recording block */
    blk_hdr[1] = (uint8_t)(TSTATES_PER_SAMPLE & 0xFF);
    blk_hdr[2] = (uint8_t)((TSTATES_PER_SAMPLE >> 8) & 0xFF);
    blk_hdr[3] = 0x00;  /* pause low */
    blk_hdr[4] = 0x00;  /* pause high (0 ms) */
    blk_hdr[5] = (uint8_t)last_byte_bits;
    blk_hdr[6] = (uint8_t)(total_bytes & 0xFF);
    blk_hdr[7] = (uint8_t)((total_bytes >> 8) & 0xFF);
    blk_hdr[8] = (uint8_t)((total_bytes >> 16) & 0xFF);

#ifdef PICO_BUILD
    FIL fp;
    UINT bw;
    if (f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return -1;
    f_write(&fp, hdr, 10, &bw);
    f_write(&fp, blk_hdr, 9, &bw);
    f_write(&fp, s_buffer, total_bytes, &bw);
    f_close(&fp);
#else
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(hdr, 1, 10, fp);
    fwrite(blk_hdr, 1, 9, fp);
    fwrite(s_buffer, 1, total_bytes, fp);
    fclose(fp);
#endif

    return 0;
}

void tape_rec_write_bit(int level, unsigned long tstates) {
    if (!s_recording || !s_buffer) return;

    /* Fill samples from s_last_tstates to tstates with current level */
    if (s_last_tstates == 0) {
        s_last_tstates = tstates;
        s_level = level;
        return;
    }

    unsigned long elapsed = tstates - s_last_tstates;
    unsigned long samples = elapsed / TSTATES_PER_SAMPLE;

    for (unsigned long i = 0; i < samples; i++) {
        if (s_byte_pos >= MAX_REC_BYTES) {
            s_recording = false;
            return;
        }
        if (s_level)
            s_buffer[s_byte_pos] |= (1u << s_bit_pos);

        if (s_bit_pos == 0) {
            s_bit_pos = 7;
            s_byte_pos++;
        } else {
            s_bit_pos--;
        }
    }

    s_last_tstates = tstates;
    s_level = level;
}

bool tape_rec_active(void) {
    return s_recording;
}
