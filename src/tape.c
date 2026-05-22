/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape.c — CDT/TZX and CAS tape image playback engine.
 *
 * Adapted from CPCEC's CPCEC-K7.H by Cesar Nicolas-Gonzalez.
 * Simplified for the Pico/RP2350 target:
 *   - FatFS file I/O instead of stdio
 *   - CDT/TZX and CAS only (no WAV, PZX, recording)
 *   - No fasttape speed-up heuristics
 *   - PSRAM-backed read buffer
 */

#include "tape.h"

#ifdef PICO_BUILD
#include "ff.h"
#include "hardware/gpio.h"
#include "board_config.h"
#else
#include <stdio.h>
#endif

#include <string.h>
#include <stdio.h>

/* ---- constants ------------------------------------------------------ */

/* TZX timing is in 3.5 MHz Spectrum ticks.  CPC runs at 4 MHz.
 * We accumulate fractional ticks: tape_t += ticks * 3500000,
 * then consume in units of 4000000 (= TICKS_PER_SECOND). */
#define TAPE_TZX_CLOCK    3500000
#define TAPE_CPC_CLOCK    4000000

/* 1<<N ticks per TZX engine packet.  5 is the highest safe value. */
#define TAPE_MAIN_TZX_EXP 5

/* Read buffer size (4 KB). */
#define TAPE_BUF_SIZE     4096

/* ---- file I/O layer ------------------------------------------------- */

#ifdef PICO_BUILD
static FIL tape_fil;
static int tape_fil_open = 0;
#else
static FILE *tape_fp = NULL;
#endif

static uint8_t tape_buffer[TAPE_BUF_SIZE];
static int tape_offset, tape_length;          /* buffer cursor / valid bytes */
static int tape_filetell, tape_filesize;      /* logical position / total size */
static int tape_filebase;                     /* offset of first data block */

static int f_tape_open(const char *path) {
#ifdef PICO_BUILD
    if (tape_fil_open) { f_close(&tape_fil); tape_fil_open = 0; }
    FRESULT fr = f_open(&tape_fil, path, FA_READ);
    if (fr != FR_OK) return -1;
    tape_fil_open = 1;
    tape_filesize = (int)f_size(&tape_fil);
    return 0;
#else
    if (tape_fp) { fclose(tape_fp); tape_fp = NULL; }
    tape_fp = fopen(path, "rb");
    if (!tape_fp) return -1;
    fseek(tape_fp, 0, SEEK_END);
    tape_filesize = (int)ftell(tape_fp);
    fseek(tape_fp, 0, SEEK_SET);
    return 0;
#endif
}

static void f_tape_close(void) {
#ifdef PICO_BUILD
    if (tape_fil_open) { f_close(&tape_fil); tape_fil_open = 0; }
#else
    if (tape_fp) { fclose(tape_fp); tape_fp = NULL; }
#endif
}

static int f_tape_seek(int pos) {
#ifdef PICO_BUILD
    if (!tape_fil_open) return -1;
    return (f_lseek(&tape_fil, (FSIZE_t)pos) == FR_OK) ? 0 : -1;
#else
    if (!tape_fp) return -1;
    return fseek(tape_fp, pos, SEEK_SET);
#endif
}

static int f_tape_read(void *buf, int n) {
#ifdef PICO_BUILD
    UINT br = 0;
    if (!tape_fil_open) return 0;
    f_read(&tape_fil, buf, (UINT)n, &br);
    return (int)br;
#else
    if (!tape_fp) return 0;
    return (int)fread(buf, 1, (size_t)n, tape_fp);
#endif
}

/* Buffered tape reads (adapted from CPCEC) */

static int tape_seek(int i) {
    if (i < 0) i = 0; else if (i > tape_filesize) i = tape_filesize;
    int j = i - tape_filetell + tape_offset;
    if (j >= 0 && j < tape_length) {
        tape_offset = j;
        tape_filetell = i;
        return i;
    }
    f_tape_seek(i);
    tape_offset = tape_length = 0;
    tape_filetell = i;
    return i;
}

static int tape_skip(int i) { return tape_seek(i + tape_filetell); }

static int tape_getc(void) {
    if (tape_offset >= tape_length) {
        tape_offset = 0;
        tape_length = f_tape_read(tape_buffer, TAPE_BUF_SIZE);
        if (tape_length <= 0) return -1;
    }
    ++tape_filetell;
    return tape_buffer[tape_offset++];
}

#define tape_undo() (--tape_offset, --tape_filetell)

static int tape_getcc(void) {
    int i;
    if ((i = tape_getc()) >= 0) i |= tape_getc() << 8;
    return i;
}

static int tape_getccc(void) {
    int i;
    if ((i = tape_getc()) >= 0)
        if ((i |= tape_getc() << 8) >= 0)
            i |= tape_getc() << 16;
    return i;
}

static int tape_getcccc(void) {
    int i;
    if ((i = tape_getc()) >= 0)
        if ((i |= tape_getc() << 8) >= 0)
            if ((i |= tape_getc() << 16) >= 0)
                i |= tape_getc() << 24;
    return i;
}

/* ---- tape state ----------------------------------------------------- */

static int tape_type;        /* TAPE_TYPE_NONE / TZX / CAS */
static int tape_signal;      /* >0 STOP, <0 EOF */
static int tape_playback;    /* TZX clock scaled frequency */
static int tape_step;        /* safety delay counter */
static char tape_status;     /* current signal level: 0 or 1 */
static char tape_polarity;   /* invert signal? */
static char tape_feedable;   /* current block allows speedup? (unused for now) */
static int tape_seekcccc;    /* sanity check for block $19 */

static int tape_t;           /* fractional tick accumulator */
static int tape_n;           /* sample counter within current phase */
static int tape_heads, tape_tones, tape_datas, tape_waves, tape_tails;
static int tape_loops, tape_loop0, tape_calls, tape_call0;

/* Head/tone timing tables */
static int tape_headcode[256 * 2];
static int tape_head, tape_tail, tape_time;
static int tape_tonecodes, tape_toneitems, tape_datacodes, tape_dataitems;
static short tape_codeitem[256][32];
static short *tape_code;
static int tape_item, tape_mask, tape_bits, tape_byte;

/* TZX config values */
static int tape_tzxpilot, tape_tzxpilots;
static int tape_tzxsync1, tape_tzxsync2;
static int tape_tzxbit0, tape_tzxbit1;
static int tape_tzxhold;

/* Kansas City (CAS) state */
static int tape_kansas, tape_kansas0n, tape_kansas1n;
static int tape_kansasin, tape_kansasi;
static int tape_kansason, tape_kansaso, tape_kansasrl;

/* Motor and GPIO state */
static int s_tape_motor;
static bool s_tape_gpio_mode;

/* CAS header separator */
static const char tape_header_cas1[8] =
    "\037\246\336\272\314\023\175\164";
static const char tape_header_tzx1[9] =
    "ZXTape!\032\001";

/* ---- TZX helper functions (from CPCEC) ------------------------------ */

#define tape_safetydelay() ((tape_type >= 2) && (tape_step = 2000))

static void tape_tzx20(void) {
    tape_tail = 0;
    tape_tails = 3500 * tape_tzxhold;
}

static void tape_tzx14(int l) {
    tape_datacodes = tape_time = tape_item = 0;
    tape_mask = tape_bits = tape_feedable = 1;
    if (tape_datas) tape_datas -= 8;
    tape_datas += l * 8;
    tape_codeitem[0][0] = tape_codeitem[1][0] = 0;
    tape_codeitem[0][1] = tape_codeitem[0][2] = tape_tzxbit0;
    tape_codeitem[1][1] = tape_codeitem[1][2] = tape_tzxbit1;
    tape_codeitem[0][3] = tape_codeitem[1][3] = -1;
    tape_tzx20();
}

static void tape_tzx12(void) {
    tape_time = tape_head = tape_heads = 0;
    if (tape_tzxpilot && tape_tzxpilots) {
        tape_headcode[0] = tape_tzxpilots;
        tape_headcode[1] = tape_tzxpilot;
        tape_heads = 1;
    }
    if (tape_tzxsync1 && tape_tzxsync2) {
        tape_headcode[tape_heads * 2]     = 1;
        tape_headcode[tape_heads * 2 + 1] = tape_tzxsync1;
        ++tape_heads;
        tape_headcode[tape_heads * 2]     = 1;
        tape_headcode[tape_heads * 2 + 1] = tape_tzxsync2;
        ++tape_heads;
    }
}

static void tape_tzx11(int l) {
    tape_tzx12();
    tape_tzx14(l);
}

static void tape_tzx10(int q, int l) {
    tape_tzxpilot  = 2168;
    tape_tzxpilots = q ? 3222 : 8062;
    tape_tzxsync1  = 667;
    tape_tzxsync2  = 735;
    tape_tzxbit0   = 855;
    tape_tzxbit1   = 1710;
    tape_datas     = 8;
    tape_tzx11(l);
}

static void tape_tzx19(int n, int m) {
    for (int i = 0, j; i < n; ++i) {
        tape_codeitem[i][0] = tape_getc();
        for (j = 1; j <= m; ++j)
            if ((tape_codeitem[i][j] = tape_getcc()) < 1)
                tape_codeitem[i][j] = -1;
        tape_codeitem[i][j] = -1;
    }
}

static void tape_firstbit(int m) {
    if (m & 2)
        tape_status = (m & 1) ^ tape_polarity;
    else if (!(m & 1))
        tape_status ^= 1;
}

static void tape_eofmet(void) {
    tape_safetydelay();
    tape_signal = -1;
    /* Rewind to start */
    tape_seek(tape_filebase);
}

/* TZX block size calculator */
static int tape_tzx1size(int i) {
    switch (i) {
        case 0x10: return tape_getcc(), tape_getcc();
        case 0x11: return tape_skip(2+2+2+2+2+2+1+2), tape_getccc();
        case 0x12: return tape_skip(2+2), 0;
        case 0x13: return tape_getc() * 2;
        case 0x14: return tape_skip(2+2+1+2), tape_getccc();
        case 0x15: return tape_skip(2+2+1), tape_getccc();
        case 0x20: return tape_getcc(), 0;
        case 0x2A: return tape_getcccc(), 0;
        case 0x21: return tape_getc();
        case 0x22: return 0;
        case 0x23: return tape_getcc(), 0;
        case 0x24: return tape_getcc(), 0;
        case 0x25: return 0;
        case 0x26: return tape_getcc() * 2;
        case 0x27: return 0;
        case 0x28: return tape_getcc();
        case 0x31: tape_getc(); /* fall through */
        case 0x30: return tape_getc();
        case 0x32: return tape_getcc();
        case 0x33: return tape_getc() * 3;
        case 0x34: return tape_skip(8), 0;
        case 0x35: return tape_skip(16), tape_getcccc();
        case 0x40: return tape_getcccc() >> 8;
        case 0x4B: return tape_getcccc();
        case 0x5A: return tape_skip(9), 0;
        default:   return tape_getcccc();
    }
}

static int tape_tzx1tell(void) {
    int z = tape_filetell;
    tape_seek(tape_filebase);
    int p = -1, i;
    while (tape_filetell < z && (i = tape_getc()) >= 0) {
        ++p;
        tape_skip(tape_tzx1size(i));
    }
    tape_seek(z);
    return p;
}

static int tape_tzx1seek(int p) {
    int i;
    tape_seek(tape_filebase);
    while (p > 0 && (i = tape_getc()) >= 0) {
        --p;
        tape_skip(tape_tzx1size(i));
    }
    return tape_filetell;
}

/* ---- public API ----------------------------------------------------- */

void tape_init(void) {
    tape_type = TAPE_TYPE_NONE;
    tape_signal = 0;
    tape_status = 0;
    tape_polarity = 0;
    s_tape_motor = 0;
    s_tape_gpio_mode = false;
    tape_t = tape_n = 0;
    tape_filetell = tape_filesize = tape_filebase = 0;
    tape_offset = tape_length = 0;
    tape_heads = tape_tones = tape_datas = tape_waves = tape_tails = 0;
    tape_loops = tape_calls = 0;
    tape_kansas = 0;
    tape_feedable = 0;
    tape_seekcccc = 0;

#ifdef PICO_BUILD
    gpio_init(TAPE_IN_PIN);
    gpio_set_dir(TAPE_IN_PIN, GPIO_IN);
    gpio_pull_down(TAPE_IN_PIN);
#endif
}

void tape_close(void) {
    f_tape_close();
    tape_type = TAPE_TYPE_NONE;
    tape_signal = 0;
    tape_status = 0;
    tape_t = tape_n = 0;
    tape_filetell = tape_filesize = tape_filebase = 0;
    tape_offset = tape_length = 0;
    tape_heads = tape_tones = tape_datas = tape_waves = tape_tails = 0;
    tape_loops = tape_calls = 0;
    tape_kansas = 0;
    tape_feedable = 0;
    tape_seekcccc = 0;
    tape_step = 0;
    tape_playback = 0;
}

int tape_open(const char *path) {
    tape_close();
    if (f_tape_open(path) != 0)
        return -1;

    tape_filetell = 0;
    tape_offset = tape_length = 0;

    /* Read first bytes to detect format */
    if (tape_getc() < 0) {
        tape_close();
        return -1;
    }
    tape_undo();

    /* CDT/TZX: "ZXTape!\x1A\x01" */
    if (tape_length >= 9 && !memcmp(tape_buffer, tape_header_tzx1, 9)) {
        tape_type = TAPE_TYPE_TZX;
        tape_playback = TAPE_TZX_CLOCK >> TAPE_MAIN_TZX_EXP;
        tape_seek(10);
    }
    /* CAS: 8-byte separator */
    else if (tape_length >= 8 && !memcmp(tape_buffer, tape_header_cas1, 8)) {
        tape_type = TAPE_TYPE_CAS;
        tape_playback = TAPE_TZX_CLOCK >> TAPE_MAIN_TZX_EXP;
    }
    else {
        tape_close();
        return -1; /* unknown format */
    }

    tape_filebase = tape_filetell;
    tape_safetydelay();
    printf("tape: opened %s, type=%d, size=%d, base=%d\n",
           path, tape_type, tape_filesize, tape_filebase);
    return 0;
}

bool tape_is_loaded(void) {
    return tape_type != TAPE_TYPE_NONE || s_tape_gpio_mode;
}

void tape_set_motor(int on) {
    int prev = s_tape_motor;
    s_tape_motor = on ? 1 : 0;
    if (prev != s_tape_motor)
        printf("tape: motor %s (loaded=%d type=%d)\n",
               on ? "ON" : "OFF", tape_type != TAPE_TYPE_NONE, tape_type);
}
int  tape_get_motor(void)   { return s_tape_motor; }

void tape_set_gpio_mode(bool active) { s_tape_gpio_mode = active; }
bool tape_get_gpio_mode(void)        { return s_tape_gpio_mode; }

void tape_set_polarity(int pol) { tape_polarity = pol ? 1 : 0; }

int tape_get_status(void) {
#ifdef PICO_BUILD
    if (s_tape_gpio_mode)
        return gpio_get(TAPE_IN_PIN) ? 1 : 0;
#endif
    return tape_status ^ tape_polarity;
}

/* ---- tape_main: advance playback by `ticks` CPC T-states ------------ */

void tape_main(int ticks) {
    if (tape_type == TAPE_TYPE_NONE || !s_tape_motor)
        return;

    /* Scale CPC ticks (4 MHz) to TZX ticks (3.5 MHz) via accumulator.
     * p = number of TZX sample periods elapsed. */
    int p = (tape_t += (ticks * tape_playback)) / TAPE_CPC_CLOCK;
    if (p <= 0) return;
    tape_t %= TAPE_CPC_CLOCK;

    /* TZX / CAS engine */
    tape_n -= p << TAPE_MAIN_TZX_EXP;
    int watchdog = 99;
    int t;

    while (tape_n <= 0) {
        if (tape_heads) {
            /* Pre-defined head tones */
            if (!tape_time)
                tape_time = tape_headcode[tape_head++];
            tape_status ^= 1;
            tape_n += tape_headcode[tape_head];
            if (!--tape_time)
                ++tape_head, --tape_heads;
        }
        else if (tape_tones) {
            /* Encoded tones (block $19) */
            if (tape_tonecodes) {
                tape_tzx19(tape_tonecodes, tape_toneitems);
                tape_tonecodes = tape_time = 0;
            }
            if (!tape_time) {
                tape_code = tape_codeitem[tape_byte = tape_getc()];
                tape_time = tape_getcc();
                tape_item = 0;
            }
            if (!tape_item)
                tape_firstbit(*tape_code);
            if ((t = *++tape_code) >= 0) {
                tape_n += t;
                if (tape_item++) tape_status ^= 1;
            } else {
                tape_item = 0;
                if (--tape_time)
                    tape_code = tape_codeitem[tape_byte];
                else
                    --tape_tones;
            }
        }
        else if (tape_datas) {
            /* Encoded data bits */
            if (tape_datacodes) {
                tape_tzx19(tape_datacodes, tape_dataitems);
                tape_mask = (1 << (tape_bits =
                    tape_datacodes <= 2 ? 1 :
                    tape_datacodes <= 4 ? 2 :
                    tape_datacodes <= 16 ? 4 : 8)) - 1;
                tape_feedable = tape_bits == 1 &&
                    !tape_codeitem[0][0] && !tape_codeitem[1][0] &&
                    tape_codeitem[0][2] > 0 && tape_codeitem[1][2] > 0 &&
                    tape_codeitem[0][3] < 0 && tape_codeitem[1][3] < 0;
                tape_datacodes = tape_time = tape_item = 0;
            }
            if (!tape_time)
                tape_byte = tape_getc(), tape_time = 8;
            if (!tape_item)
                tape_firstbit(*(tape_code = tape_codeitem[
                    (tape_byte >> (tape_time - tape_bits)) & tape_mask]));
            if ((t = *++tape_code) >= 0) {
                tape_n += t;
                if (tape_item++) tape_status ^= 1;
            } else {
                tape_item = 0;
                tape_time -= tape_bits;
                --tape_datas;
            }
        }
        else if (tape_kansas) {
            /* Kansas City Standard (CAS format) */
            if (!tape_time)
                tape_byte = tape_getc(), tape_time = 10, tape_item = 0;
            if (!tape_item) {
                if (tape_time > 9)
                    tape_mask = tape_kansasi, tape_item = tape_kansasin;
                else if (tape_time > 1) {
                    if ((tape_byte >> (tape_kansasrl ? tape_time - 2 : 9 - tape_time)) & 1)
                        tape_mask = tape_tzxbit1, tape_item = tape_kansas1n;
                    else
                        tape_mask = tape_tzxbit0, tape_item = tape_kansas0n;
                } else {
                    tape_mask = tape_kansaso, tape_item = tape_kansason;
                }
            }
            if (tape_item) {
                tape_status ^= 1;
                tape_n += tape_mask;
                --tape_item;
            }
            if (!tape_item)
                if (!--tape_time) --tape_kansas;
        }
        else if (tape_waves) {
            /* 1-bit samples (block $15) */
            if (!tape_time)
                tape_time = 8, tape_byte = tape_getc();
            tape_status = ((tape_byte >> --tape_time) ^ tape_polarity) & 1;
            tape_n += tape_mask;
            --tape_waves;
        }
        else if (tape_tails) {
            /* Tail / pause */
            if (tape_tails > 3500) {
                tape_n += 3500;
                tape_tails -= 3500;
            } else {
                tape_n += tape_tails;
                tape_tails = 0;
            }
            if (tape_tail)
                tape_status = tape_polarity;
            else {
                tape_status ^= tape_tail = 1;
            }
        }
        else do {
            /* Fetch next block */
            if (!--watchdog) { tape_close(); tape_signal = -1; return; }

            if (tape_step) {
                /* Safety delay after open/rewind */
                tape_tzxhold = tape_step;
                tape_tzx20();
                tape_step = 0;
            }
            else if (tape_type == TAPE_TYPE_CAS) {
                /* CAS: Kansas City Standard blocks */
                if (tape_getc() < 0) { tape_eofmet(); return; }
                tape_undo();
                tape_time = tape_head = 0;
                if (tape_offset + 8 <= tape_length &&
                    !memcmp(&tape_buffer[tape_offset], tape_header_cas1, 8)) {
                    /* Separator found — generate pilot tone */
                    tape_skip(8);
                    tape_heads = 2;
                    tape_status = tape_polarity;
                    tape_headcode[0] = 5;
                    tape_headcode[1] = 700000;      /* ~1 s pause */
                    tape_headcode[2] = 9999;
                    tape_headcode[3] = 729;          /* pilot tone */
                }
                else {
                    /* Data chunk — 8 bytes Kansas City modulation */
                    tape_kansas = 8;
                    tape_kansasrl = 0;
                    tape_kansasin = (tape_kansas0n = 2);
                    tape_kansasi = tape_tzxbit0 = 1458;
                    tape_kansason = (tape_kansas1n = 4) * 2;
                    tape_kansaso = tape_tzxbit1 = 729;
                }
            }
            else {
                /* TZX/CDT block decoder */
                if (tape_seekcccc) {
                    tape_seek(tape_seekcccc);
                    tape_seekcccc = 0;
                }
                if ((p = tape_getc()) <= (tape_feedable = 0)) {
                    tape_eofmet();
                    return;
                }
                switch (p) {
                    case 0x10: /* NORMAL DATA */
                        tape_tzxhold = tape_getcc();
                        p = tape_getcc();
                        tape_tzx10(tape_getc() & 128, p);
                        tape_undo();
                        break;
                    case 0x11: /* CUSTOM DATA */
                        tape_tzxpilot  = tape_getcc();
                        tape_tzxsync1  = tape_getcc();
                        tape_tzxsync2  = tape_getcc();
                        tape_tzxbit0   = tape_getcc();
                        tape_tzxbit1   = tape_getcc();
                        tape_tzxpilots = tape_getcc();
                        tape_datas     = tape_getc();
                        tape_tzxhold   = tape_getcc();
                        tape_tzx11(tape_getccc());
                        break;
                    case 0x12: /* PURE TONE */
                        tape_tzxpilot  = tape_getcc();
                        tape_tzxpilots = tape_getcc();
                        tape_tzxsync1 = tape_tzxsync2 = 0;
                        tape_tzx12();
                        break;
                    case 0x13: /* PURE SYNC */
                        tape_heads = tape_getc();
                        for (p = 0; p < tape_heads; ++p) {
                            tape_headcode[p * 2]     = 1;
                            tape_headcode[p * 2 + 1] = tape_getcc();
                        }
                        tape_time = tape_head = 0;
                        break;
                    case 0x14: /* PURE DATA */
                        tape_tzxbit0 = tape_getcc();
                        tape_tzxbit1 = tape_getcc();
                        tape_datas   = tape_getc();
                        tape_tzxhold = tape_getcc();
                        tape_tzx14(tape_getccc());
                        break;
                    case 0x15: /* SAMPLES */
                        tape_time = 0;
                        tape_mask = tape_getcc();
                        tape_tzxhold = tape_getcc();
                        tape_tzx20();
                        if ((tape_waves = tape_getc()))
                            tape_waves -= 8;
                        tape_waves += tape_getccc() << 3;
                        break;
                    case 0x19: /* GENERALIZED DATA */
                        tape_seekcccc = tape_getcccc();
                        tape_seekcccc += tape_filetell;
                        tape_tzxhold = tape_getcc();
                        tape_tzx20();
                        tape_tones     = tape_getcccc();
                        tape_toneitems = tape_getc();
                        tape_tonecodes = tape_getc();
                        if (!tape_tonecodes) tape_tonecodes = 256;
                        tape_datas     = tape_getcccc();
                        tape_dataitems = tape_getc();
                        tape_datacodes = tape_getc();
                        if (!tape_datacodes) tape_datacodes = 256;
                        break;
                    case 0x20: /* HOLD (or STOP) */
                        tape_tzxhold = tape_getcc();
                        if (tape_tzxhold)
                            tape_tzx20();
                        else
                            tape_signal |= 1;
                        break;
                    case 0x2A: /* STOP ON 48K */
                        tape_getcccc();
                        tape_signal |= 2;
                        break;
                    case 0x2B: /* SET SIGNAL LEVEL */
                        p = tape_getcccc() - 1;
                        tape_status = (tape_getc() & 1) ^ tape_polarity;
                        tape_skip(p);
                        break;
                    case 0x4B: /* KANSAS CITY DATA (TSX/MSX extension) */
                        tape_kansas  = tape_getcccc() - 12;
                        tape_tzxhold = tape_getcc();
                        tape_tzx20();
                        tape_tzxpilot  = tape_getcc();
                        tape_tzxpilots = tape_getcc();
                        tape_tzxsync1 = tape_tzxsync2 = 0;
                        tape_tzx12();
                        tape_tzxbit0 = tape_getcc();
                        tape_tzxbit1 = tape_getcc();
                        p = tape_getc();
                        tape_kansas0n = (p >> 4) ? (p >> 4) : 16;
                        tape_kansas1n = (p & 15) ? (p & 15) : 16;
                        tape_kansasrl = (p = tape_getc()) & 1;
                        tape_kansasin = (p >> 6) & 3;
                        tape_kansason = (p >> 3) & 3;
                        if ((p >> 5) & 1) {
                            tape_kansasin *= tape_kansas1n;
                            tape_kansasi = tape_tzxbit1;
                        } else {
                            tape_kansasin *= tape_kansas0n;
                            tape_kansasi = tape_tzxbit0;
                        }
                        if ((p >> 2) & 1) {
                            tape_kansason *= tape_kansas1n;
                            tape_kansaso = tape_tzxbit1;
                        } else {
                            tape_kansason *= tape_kansas0n;
                            tape_kansaso = tape_tzxbit0;
                        }
                        break;
                    /* Non-signal blocks */
                    case 0x21: /* GROUP START */
                        tape_skip(tape_getc());
                        /* fall through */
                    case 0x22: /* GROUP END */
                        break;
                    case 0x24: /* LOOP START */
                        tape_loops = tape_getcc();
                        tape_loop0 = tape_filetell;
                        break;
                    case 0x25: /* LOOP END */
                        if (tape_loops && --tape_loops)
                            tape_seek(tape_loop0);
                        break;
                    case 0x23: /* JUMP TO BLOCK */
                        p = tape_tzx1tell();
                        tape_tzx1seek(p + (signed short)tape_getcc());
                        break;
                    case 0x26: /* CALL SEQUENCE */
                        if ((tape_calls = tape_getcc())) {
                            tape_call0 = tape_filetell + 2;
                            p = tape_tzx1tell();
                            tape_tzx1seek(p + (signed short)tape_getcc());
                        }
                        break;
                    case 0x27: /* RETURN FROM CALL */
                        if (tape_calls) {
                            tape_seek(tape_call0);
                            if (--tape_calls) {
                                tape_call0 += 2;
                                p = tape_tzx1tell();
                                tape_tzx1seek(p + (signed short)tape_getcc());
                            }
                        }
                        break;
                    case 0x28: /* SELECT BLOCK */
                        tape_skip(tape_getcc());
                        break;
                    case 0x31: /* MESSAGE BLOCK */
                        tape_getc();
                        /* fall through */
                    case 0x30: /* TEXT DESCRIPTION */
                        tape_skip(tape_getc());
                        break;
                    case 0x32: /* ARCHIVE INFO */
                        tape_skip(tape_getcc());
                        break;
                    case 0x33: /* HARDWARE TYPE */
                        tape_skip(tape_getc() * 3);
                        break;
                    case 0x34: /* EMULATION INFO */
                        tape_skip(8);
                        break;
                    case 0x35: /* CUSTOM INFO */
                        tape_skip(16);
                        tape_skip(tape_getcccc());
                        break;
                    case 0x40: /* SNAPSHOT INFO */
                        tape_skip(tape_getcccc() >> 8);
                        break;
                    case 0x5A: /* GLUE */
                        tape_skip(9);
                        break;
                    default: /* UNKNOWN — skip via length */
                        tape_skip(tape_getcccc());
                        break;
                }
            }
        } while (!(tape_heads | tape_tones | tape_datas |
                   tape_kansas | tape_waves | tape_tails));
    }
}
