/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_adapter.cpp — Glue between caprice32 emulation engine and Pico platform.
 *
 * Owns all caprice32 global state, handles ROM/RAM allocation via PSRAM,
 * disk/tape loading via FatFS, audio buffer management, and framebuffer setup.
 */

#include "cpc_adapter.h"

#include "cap32/cap32.h"
#include "cap32/crtc.h"
#include "cap32/z80.h"
#include "cap32/disk.h"
#include "cap32/tape.h"

#include <cstring>
#include <cstdio>

/* Pico SDK / driver headers (C) */
extern "C" {
#include "board_config.h"
#include "psram_allocator.h"
#include "ff.h"
#include "pico/time.h"
}

/* ------------------------------------------------------------------ */
/* caprice32 global state — z80 is defined in z80.cpp                 */
/* ------------------------------------------------------------------ */

extern t_z80regs z80;
t_CPC CPC;
t_CRTC CRTC;
t_FDC FDC;
t_GateArray GateArray;
t_PPI PPI;
t_PSG PSG;
t_VDU VDU;

byte *membank_read[4];
byte *membank_write[4];

byte *pbRAM = nullptr;
byte *pbROM = nullptr;
byte *pbROMlo = nullptr;
byte *pbROMhi = nullptr;
byte *pbExpansionROM = nullptr;
byte *memmap_ROM[256];

byte keyboard_matrix[16];

/* Tape state — iTapeCycleCount is defined in tape.cpp */
extern int iTapeCycleCount;

/* Drives — allocated in PSRAM due to ~168KB each */
t_drive *driveA_p = nullptr;
t_drive *driveB_p = nullptr;

/* CRTC globals */
dword dwXScale = 1;

/* Debug stubs (referenced by crtc.cpp and fdc.cpp) */
dword dwDebugFlag = 0;
FILE *pfoDebug = nullptr;

/* Sound buffer */
#define SND_BUFFER_SIZE 4096
static byte snd_buffer_storage[SND_BUFFER_SIZE];
byte *pbSndBuffer = snd_buffer_storage;
byte *pbSndBufferEnd = snd_buffer_storage + SND_BUFFER_SIZE;

/* Audio output ring: collect samples from PSG, then push to platform */
#define AUDIO_OUT_MAX 2048
static int16_t audio_out_buf[AUDIO_OUT_MAX];
static int audio_out_count = 0;

/* Direct render target: scanline callback writes here.
 * Platform sets this to point into the back screen buffer. */
static byte *scanline_render_target = nullptr;
static int scanline_render_stride = CPC_FB_WIDTH;

/* Scanline buffer in internal RAM — CRTC renders here instead of PSRAM.
 * At each HSYNC, the completed line is copied into cpc_fb.
 * This avoids all PSRAM writes during rendering. */
static byte scanline_buf[CPC_SCR_WIDTH] __attribute__((aligned(4)));
/* CPC hardware colour table — 27 colours (3 levels per RGB channel).
 * Index = CPC hardware colour number (0-31, with gaps).
 * Values = RGB888. */
static const uint32_t cpc_rgb_table[32] = {
    0x808080, /* 0  White (half) */
    0x808080, /* 1  White (half) - duplicate */
    0x00FF80, /* 2  Sea Green */
    0xFFFF80, /* 3  Pastel Yellow */
    0x000080, /* 4  Blue */
    0xFF0080, /* 5  Purple */
    0x008080, /* 6  Cyan */
    0xFF8080, /* 7  Pink */
    0xFF0080, /* 8  Purple - duplicate */
    0xFFFF80, /* 9  Pastel Yellow - duplicate */
    0xFFFF00, /* 10 Bright Yellow */
    0xFFFFFF, /* 11 Bright White */
    0xFF0000, /* 12 Bright Red */
    0xFF00FF, /* 13 Bright Magenta */
    0xFF8000, /* 14 Orange */
    0xFF80FF, /* 15 Pastel Magenta */
    0x000080, /* 16 Blue - duplicate */
    0x00FF80, /* 17 Sea Green - duplicate */
    0x00FF00, /* 18 Bright Green */
    0x00FFFF, /* 19 Bright Cyan */
    0x000000, /* 20 Black */
    0x0000FF, /* 21 Bright Blue */
    0x008000, /* 22 Green */
    0x0080FF, /* 23 Sky Blue */
    0x800080, /* 24 Magenta (half) */
    0x80FF80, /* 25 Pastel Green */
    0x80FF00, /* 26 Lime */
    0x80FFFF, /* 27 Pastel Cyan */
    0x800000, /* 28 Red (half) */
    0x8000FF, /* 29 Mauve */
    0x808000, /* 30 Yellow (half) */
    0x8080FF, /* 31 Pastel Blue */
};

/* Forward declarations */
extern "C" void graphics_set_palette(uint8_t idx, uint32_t rgb);

void InitAY();
void ResetAYChipEmulation();
void InitAYCounterVars();

/* ------------------------------------------------------------------ */
/* ROM loading via FatFS                                              */
/* ------------------------------------------------------------------ */
/* ROM buffers in internal SRAM for fast Z80 access (instead of PSRAM).
 * ROM is read-heavy: BASIC interpreter, BIOS, AMSDOS — keeping them
 * in SRAM eliminates PSRAM latency on every ROM fetch. */
static byte rom_buffer_sram[32768] __attribute__((aligned(4)));
static byte amsdos_rom_sram[16384] __attribute__((aligned(4)));
static byte *rom_buffer = rom_buffer_sram;
static byte *amsdos_rom = amsdos_rom_sram;

static int load_rom_file(const char *path, byte *dest, int max_size) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT br;
    f_read(&f, dest, (UINT)max_size, &br);
    f_close(&f);
    return (int)br;
}

static int load_roms(void) {
    /* ROM buffers are statically allocated in internal SRAM */
    std::memset(rom_buffer, 0, 32768);
    std::memset(amsdos_rom, 0, 16384);

    /* Determine ROM filename based on CPC model */
    const char *rom_name;
    switch (CPC.model) {
        case 0:  rom_name = "/cpc/rom/cpc464.rom"; break;
        case 1:  rom_name = "/cpc/rom/cpc664.rom"; break;
        default: rom_name = "/cpc/rom/cpc6128.rom"; break;
    }

    /* Try custom ROM override first */
    if (CPC.rom_path[0]) {
        if (load_rom_file(CPC.rom_path, rom_buffer, 32768) >= 16384) {
            printf("ROM: loaded custom %s\n", CPC.rom_path);
        } else {
            printf("ROM: custom %s failed, trying default\n", CPC.rom_path);
            CPC.rom_path[0] = 0;
        }
    }

    if (!CPC.rom_path[0]) {
        if (load_rom_file(rom_name, rom_buffer, 32768) < 16384) {
            printf("ROM: FAILED to load %s\n", rom_name);
            return -1;
        }
        printf("ROM: loaded %s\n", rom_name);
    }

    pbROM = rom_buffer;
    pbROMlo = rom_buffer;
    pbROMhi = rom_buffer + 16384;

    /* Load AMSDOS ROM */
    if (load_rom_file("/cpc/rom/amsdos.rom", amsdos_rom, 16384) >= 8192) {
        memmap_ROM[7] = amsdos_rom;
        printf("ROM: loaded amsdos.rom\n");
    }

    /* Set default upper ROM mapping */
    memmap_ROM[0] = pbROMhi;
    pbExpansionROM = pbROMhi;

    return 0;
}

/* ------------------------------------------------------------------ */
/* DSK loading via FatFS                                              */
/* ------------------------------------------------------------------ */
static int load_dsk(t_drive *drive, const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;

    t_DSK_header dsk_header;
    UINT br;
    f_read(&f, &dsk_header, sizeof(t_DSK_header), &br);
    if (br < sizeof(t_DSK_header)) { f_close(&f); return -1; }

    int is_extended = (std::memcmp(dsk_header.id, "EXTENDED", 8) == 0);

    drive->tracks = dsk_header.tracks;
    drive->sides = dsk_header.sides;
    if (drive->sides == 0) drive->sides = 1;

    for (unsigned int t = 0; t < drive->tracks; t++) {
        for (unsigned int s = 0; s < drive->sides; s++) {
            dword track_size;
            if (is_extended) {
                track_size = ((dword)dsk_header.track_size[t * drive->sides + s]) * 256;
            } else {
                track_size = dsk_header.track_size[0] | ((dword)dsk_header.track_size[1] << 8);
            }
            if (track_size == 0) continue;

            t_track *trk = &drive->track[t][s];

            /* Allocate track data in PSRAM */
            trk->data = (byte *)psram_malloc(track_size);
            if (!trk->data) { f_close(&f); return -1; }

            f_read(&f, trk->data, track_size, &br);
            if (br < track_size) {
                std::memset(trk->data + br, 0, track_size - br);
            }

            /* Parse track header */
            t_track_header *th = (t_track_header *)trk->data;
            trk->sectors = th->sectors;
            trk->size = track_size;

            /* Set up sector pointers */
            byte *data_ptr = trk->data + 256; /* sector data starts after 256-byte header */
            for (unsigned int sec = 0; sec < trk->sectors && sec < DSK_SECTORMAX; sec++) {
                t_sector *sector = &trk->sector[sec];
                std::memcpy(sector->CHRN, &th->sector[sec][0], 4);
                std::memcpy(sector->flags, &th->sector[sec][4], 4);

                dword sec_size;
                if (is_extended) {
                    sec_size = th->sector[sec][6] | ((dword)th->sector[sec][7] << 8);
                } else {
                    sec_size = 128 << th->bps;
                }

                t_sector_setData(sector, data_ptr);
                t_sector_setSizes(sector, sec_size ? sec_size : (dword)(128 << th->bps), sec_size);
                data_ptr += sec_size ? sec_size : (dword)(128 << th->bps);
            }
        }
    }

    std::strncpy(drive->filename, path, 255);
    drive->filename[255] = 0;
    drive->current_track = 0;
    drive->altered = 0;

    f_close(&f);
    return 0;
}

static void eject_dsk(t_drive *drive) {
    for (unsigned int t = 0; t < DSK_TRACKMAX; t++) {
        for (unsigned int s = 0; s < DSK_SIDEMAX; s++) {
            t_track *trk = &drive->track[t][s];
            if (trk->data) {
                psram_free(trk->data);
                trk->data = nullptr;
            }
            trk->sectors = 0;
            trk->size = 0;
            for (int sec = 0; sec < DSK_SECTORMAX; sec++) {
                trk->sector[sec].data = nullptr;
                trk->sector[sec].data_size = 0;
                trk->sector[sec].total_size = 0;
            }
        }
    }
    drive->tracks = 0;
    drive->filename[0] = 0;
    drive->altered = 0;
}

/* ------------------------------------------------------------------ */
/* Tape loading via FatFS                                             */
/* ------------------------------------------------------------------ */
static int load_tape_image(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;

    FSIZE_t sz = f_size(&f);
    if (sz == 0 || sz > 512 * 1024) { f_close(&f); return -1; }

    if (pbTapeImage) {
        psram_free(pbTapeImage);
        pbTapeImage = nullptr;
    }

    pbTapeImage = (byte *)psram_malloc((size_t)sz);
    if (!pbTapeImage) { f_close(&f); return -1; }

    UINT br;
    f_read(&f, pbTapeImage, (UINT)sz, &br);
    f_close(&f);

    dwTapeImageSize = (dword)br;
    pbTapeImageEnd = pbTapeImage + br;

    Tape_Rewind();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendering: scanline-at-a-time into cpc_fb (internal RAM)           */
/* ------------------------------------------------------------------ */

/* Vertical mapping constants — computed once in cpc_engine_init */
static int fb_y_start = 0; /* first VDU.scrln that maps to cpc_fb row 0 */

/* Called by CRTC at each HSYNC with completed scanline number.
 * Uses DMA to copy scanline data to PSRAM while CPU continues.
 * Double-buffers: CRTC switches to other buffer while DMA copies this one. */
void __attribute__((section(".time_critical.adapter"))) scanline_complete(int scrln) {
    int fb_y = scrln - fb_y_start;
    if ((unsigned)fb_y >= (unsigned)CPC_FB_HEIGHT) return;
    if (!scanline_render_target) return;

    const uint32_t *src = (const uint32_t *)(scanline_buf + 32);
    uint32_t *dst = (uint32_t *)(scanline_render_target + fb_y * scanline_render_stride);
    /* Copy 320 bytes = 80 words, unrolled 8× for pipeline efficiency */
    for (int i = 0; i < 80; i += 8) {
        uint32_t a = src[i], b = src[i+1], c = src[i+2], d = src[i+3];
        uint32_t e = src[i+4], f = src[i+5], g = src[i+6], h = src[i+7];
        dst[i] = a; dst[i+1] = b; dst[i+2] = c; dst[i+3] = d;
        dst[i+4] = e; dst[i+5] = f; dst[i+6] = g; dst[i+7] = h;
    }
}

/* Legacy render_frame_to_fb — no longer needed since we render per-scanline.
 * Kept as empty stub in case anything calls it. */
static void render_frame_to_fb(void) {
    /* no-op: rendering happens in scanline_complete() callback */
}

/* ------------------------------------------------------------------ */
/* Platform palette setup                                             */
/* ------------------------------------------------------------------ */
static void setup_hw_palette(void) {
    for (int i = 0; i < 32; i++) {
        graphics_set_palette((uint8_t)i, cpc_rgb_table[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Audio: convert PSG buffer to platform audio ring                   */
/* ------------------------------------------------------------------ */
static void flush_audio(void) {
    /* PSG resets snd_bufferptr to pbSndBuffer when buffer is full,
     * so the pointer offset is always 0 when we get here.
     * The buffer contains SND_BUFFER_SIZE bytes of audio data. */
    int frames = SND_BUFFER_SIZE / 4;  /* 16-bit stereo: 4 bytes per frame */
    if (frames > AUDIO_OUT_MAX / 2) frames = AUDIO_OUT_MAX / 2;

    const int16_t *src = (const int16_t *)pbSndBuffer;
    audio_out_count = frames;

    /* Two-pole cascaded IIR low-pass to smooth PSG square-wave edges,
     * mimicking the analog RC filtering on the real CPC audio output.
     * Each pole: alpha = 3/4 → combined -3dB at ~6.5 kHz, -12dB/oct. */
    static int32_t lp1_l = 0, lp1_r = 0;
    static int32_t lp2_l = 0, lp2_r = 0;
#ifdef HDMI_PIO_AUDIO
    static int32_t lp3_l = 0, lp3_r = 0;   /* 3rd pole for HDMI */
#endif
    /* DC-blocking high-pass (AC coupling like real CPC hardware).
     * Uses a slow-tracking DC estimator: dc += (x - dc) >> 8
     * which gives a ~17 Hz cutoff at 44100 Hz. */
    static int32_t dc_l = 0, dc_r = 0;
    for (int i = 0; i < frames; ++i) {
        int32_t l = src[i * 2];
        int32_t r = src[i * 2 + 1];
        /* Track and remove DC offset */
        dc_l += (l - dc_l) >> 8;
        dc_r += (r - dc_r) >> 8;
        l -= dc_l;
        r -= dc_r;
        /* Low-pass filter to smooth PSG square-wave edges.
         * HDMI: 3-pole at alpha=1/2 (~1.8 kHz -3dB, -18dB/oct)
         * I2S:  2-pole at alpha=3/4 (~6.5 kHz -3dB, -12dB/oct) */
#ifdef HDMI_PIO_AUDIO
        lp1_l += (l - lp1_l) >> 1;
        lp1_r += (r - lp1_r) >> 1;
        lp2_l += (lp1_l - lp2_l) >> 1;
        lp2_r += (lp1_r - lp2_r) >> 1;
        lp3_l += (lp2_l - lp3_l) >> 1;
        lp3_r += (lp2_r - lp3_r) >> 1;
        /* Attenuate to match real CPC analog output levels */
        audio_out_buf[i * 2]     = (int16_t)(lp3_l >> 2);
        audio_out_buf[i * 2 + 1] = (int16_t)(lp3_r >> 2);
#else
        lp1_l += ((l - lp1_l) * 3) >> 2;
        lp1_r += ((r - lp1_r) * 3) >> 2;
        lp2_l += ((lp1_l - lp2_l) * 3) >> 2;
        lp2_r += ((lp1_r - lp2_r) * 3) >> 2;
        audio_out_buf[i * 2]     = (int16_t)lp2_l;
        audio_out_buf[i * 2 + 1] = (int16_t)lp2_r;
#endif
    }
}

/* ------------------------------------------------------------------ */
/* Public C API implementation                                        */
/* ------------------------------------------------------------------ */

/* Forward declaration — defined in cap32.cpp (C++ linkage) */
void ga_memory_manager();

extern "C" {

/* Platform audio functions (from main.c) */
extern unsigned audio_ring_push_stereo(const int16_t *samples, unsigned count);
extern unsigned audio_ring_push_mono(const int16_t *samples, unsigned count);

int cpc_engine_init(void) {
    /* Allocate large structures in PSRAM (too big for internal RAM) */
    if (!driveA_p) {
        driveA_p = (t_drive *)psram_malloc(sizeof(t_drive));
        if (!driveA_p) { printf("cpc_adapter: PSRAM alloc driveA failed\n"); return -1; }
    }
    if (!driveB_p) {
        driveB_p = (t_drive *)psram_malloc(sizeof(t_drive));
        if (!driveB_p) { printf("cpc_adapter: PSRAM alloc driveB failed\n"); return -1; }
    }

    /* Zero all state */
    std::memset(&z80, 0, sizeof(z80));
    std::memset(&CPC, 0, sizeof(CPC));
    std::memset(&CRTC, 0, sizeof(CRTC));
    std::memset(&FDC, 0, sizeof(FDC));
    std::memset(&GateArray, 0, sizeof(GateArray));
    std::memset(&PPI, 0, sizeof(PPI));
    std::memset(&PSG, 0, sizeof(PSG));
    std::memset(&VDU, 0, sizeof(VDU));
    std::memset(&driveA, 0, sizeof(t_drive));
    std::memset(&driveB, 0, sizeof(t_drive));
    std::memset(memmap_ROM, 0, sizeof(memmap_ROM));
    std::memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));

    /* Defaults */
    CPC.model = 2;        /* 6128 */
    CPC.ram_size = 128;
    CPC.speed = DEF_SPEED_SETTING;
    CPC.jumpers = 0x1e;
    CPC.scr_bpp = 8;
    CPC.snd_enabled = 1;
#ifdef HDMI_PIO_AUDIO
    CPC.snd_playback_rate = 5;   /* 32000 Hz — matches HDMI audio rate exactly */
#else
    CPC.snd_playback_rate = 2;   /* 44100 Hz (index into freq_table) */
#endif
    CPC.snd_bits = 1;            /* 16-bit */
    CPC.snd_stereo = 1;         /* stereo */
    CPC.snd_volume = 80;
    CPC.snd_buffersize = SND_BUFFER_SIZE;
    CPC.snd_bufferptr = pbSndBuffer;
    CPC.snd_ready = true;
    CPC.max_tracksize = 6144;
    CPC.limit_speed = 1;

    /* Rendering: CRTC writes into scanline_buf (internal RAM) one line at a time.
     * The scanline_complete callback copies each line into cpc_fb. */
    std::memset(scanline_buf, 0, sizeof(scanline_buf));
    CPC.scr_bps = CPC_SCR_WIDTH;    /* bytes per scanline */
    CPC.scr_line_offs = 0;          /* DON'T advance — we reuse scanline_buf each line */
    CPC.scr_base = scanline_buf;
    CPC.scr_pos = scanline_buf;
    CPC.scr_line_complete = scanline_complete;
    CPC.dwYScale = 1;

    /* Vertical centering: map 312 PAL lines → 240 visible rows.
     * Tuned to center typical CPC display (including overscan games). */
    fb_y_start = 21;
    crtc_fb_y_start = fb_y_start;

    /* Allocate CPC RAM in PSRAM */
    int ram_bytes = CPC.ram_size * 1024;
    pbRAM = (byte *)psram_malloc(ram_bytes);
    if (!pbRAM) {
        printf("cpc_adapter: PSRAM alloc failed (%d KB)\n", CPC.ram_size);
        return -1;
    }
    std::memset(pbRAM, 0, ram_bytes);
    printf("cpc_adapter: allocated %d KB CPC RAM in PSRAM\n", CPC.ram_size);

    /* Load ROMs */
    if (load_roms() != 0) {
        printf("cpc_adapter: ROM loading failed\n");
        return -1;
    }

    /* Initialize engine */
    emulator_init();
    emulator_reset();
    z80_sync_ram_shadow(); /* copy CPC RAM to SRAM shadow for fast reads */

    /* Set up hardware palette */
    setup_hw_palette();

    printf("cpc_adapter: caprice32 engine initialized\n");
    return 0;
}

void cpc_engine_reset(void) {
    emulator_reset();
    z80_sync_ram_shadow();
}

void cpc_engine_run_frame(void) {
    int exit_code;
    static int profile_counter = 0;
    static uint64_t accum_z80_us = 0;
    static int snd_buf_count = 0;

    absolute_time_t t0 = get_absolute_time();

    /* Reset scanline buffer for new frame */
    CPC.scr_base = scanline_buf;
    CPC.scr_pos = scanline_buf;

    do {
        exit_code = z80_execute();

        if (exit_code == EC_SOUND_BUFFER) {
            snd_buf_count++;
            flush_audio();
            audio_ring_push_stereo(audio_out_buf, audio_out_count);
        }
    } while (exit_code != EC_FRAME_COMPLETE);

    absolute_time_t t1 = get_absolute_time();

    accum_z80_us += absolute_time_diff_us(t0, t1);
    profile_counter++;
    if (profile_counter >= 50) {
        printf("PROFILE: frame=%llu us, snd_bufs=%d, snd_en=%d, bufptr_off=%d\n",
               accum_z80_us / 50, snd_buf_count,
               (int)CPC.snd_enabled,
               (int)(CPC.snd_bufferptr - pbSndBuffer));
        accum_z80_us = 0;
        profile_counter = 0;
        snd_buf_count = 0;
    }
}

void cpc_key_matrix_set(int row, int bit, int pressed) {
    if (row < 0 || row >= 16 || bit < 0 || bit > 7) return;
    if (pressed)
        keyboard_matrix[row] &= ~(1u << bit);
    else
        keyboard_matrix[row] |= (1u << bit);
}

void cpc_set_render_target(uint8_t *buffer, int stride) {
    scanline_render_target = buffer;
    scanline_render_stride = stride;
}

uint8_t *cpc_get_keyboard_matrix(void) {
    return keyboard_matrix;
}

int cpc_disk_insert(int drive, const char *path) {
    t_drive *d = (drive == 0) ? &driveA : &driveB;
    eject_dsk(d);
    int rc = load_dsk(d, path);
    if (rc == 0) {
        printf("cpc_adapter: disk %c: %s\n", 'A' + drive, path);
    }
    return rc;
}

void cpc_disk_eject(int drive) {
    t_drive *d = (drive == 0) ? &driveA : &driveB;
    eject_dsk(d);
}

int cpc_disk_is_inserted(int drive) {
    t_drive *d = (drive == 0) ? &driveA : &driveB;
    return d->tracks > 0 ? 1 : 0;
}

const char *cpc_disk_filename(int drive) {
    t_drive *d = (drive == 0) ? &driveA : &driveB;
    return d->filename[0] ? d->filename : nullptr;
}

int cpc_tape_insert(const char *path) {
    return load_tape_image(path);
}

void cpc_tape_eject(void) {
    if (pbTapeImage) {
        psram_free(pbTapeImage);
        pbTapeImage = nullptr;
    }
    pbTapeImageEnd = nullptr;
    dwTapeImageSize = 0;
}

int cpc_tape_is_loaded(void) {
    return pbTapeImage != nullptr;
}

void cpc_tape_set_motor(int on) {
    CPC.tape_motor = on ? 0x10 : 0;
}

int cpc_tape_get_motor(void) {
    return CPC.tape_motor ? 1 : 0;
}

int cpc_tape_get_level(void) {
    extern byte bTapeLevel;
    return bTapeLevel ? 1 : 0;
}

void cpc_tape_rewind(void) {
    Tape_Rewind();
}

int cpc_snapshot_save(const char *path) {
    /* TODO: implement SNA save with caprice32 structures */
    (void)path;
    return -1;
}

int cpc_snapshot_load(const char *path) {
    /* Load SNA file and restore state */
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;

    t_SNA_header sh;
    UINT br;
    f_read(&f, &sh, sizeof(sh), &br);
    if (br < sizeof(sh)) { f_close(&f); return -1; }

    /* Verify it's a snapshot */
    if (std::memcmp(sh.id, "MV - SNA", 8) != 0) { f_close(&f); return -1; }

    /* Load RAM dump */
    dword ram_size = sh.ram_size[0] | ((dword)sh.ram_size[1] << 8);
    if (ram_size > (dword)CPC.ram_size) ram_size = CPC.ram_size;
    f_read(&f, pbRAM, ram_size * 1024, &br);
    f_close(&f);

    /* Restore Z80 registers */
    z80.AF.b.l = sh.AF[0]; z80.AF.b.h = sh.AF[1];
    z80.BC.b.l = sh.BC[0]; z80.BC.b.h = sh.BC[1];
    z80.DE.b.l = sh.DE[0]; z80.DE.b.h = sh.DE[1];
    z80.HL.b.l = sh.HL[0]; z80.HL.b.h = sh.HL[1];
    z80.IX.b.l = sh.IX[0]; z80.IX.b.h = sh.IX[1];
    z80.IY.b.l = sh.IY[0]; z80.IY.b.h = sh.IY[1];
    z80.SP.b.l = sh.SP[0]; z80.SP.b.h = sh.SP[1];
    z80.PC.b.l = sh.PC[0]; z80.PC.b.h = sh.PC[1];
    z80.AFx.b.l = sh.AFx[0]; z80.AFx.b.h = sh.AFx[1];
    z80.BCx.b.l = sh.BCx[0]; z80.BCx.b.h = sh.BCx[1];
    z80.DEx.b.l = sh.DEx[0]; z80.DEx.b.h = sh.DEx[1];
    z80.HLx.b.l = sh.HLx[0]; z80.HLx.b.h = sh.HLx[1];
    z80.I = sh.I;
    z80.R = sh.R;
    z80.IFF1 = sh.IFF0;
    z80.IFF2 = sh.IFF1;
    z80.IM = sh.IM;
    z80.HALT = 0;
    z80.int_pending = sh.z80_int_pending;

    /* Restore Gate Array */
    GateArray.pen = sh.ga_pen;
    std::memcpy(GateArray.ink_values, sh.ga_ink_values, 17);
    GateArray.ROM_config = sh.ga_ROM_config;
    GateArray.RAM_config = sh.ga_RAM_config;
    GateArray.upper_ROM = sh.upper_ROM;
    GateArray.sl_count = sh.ga_sl_count;
    GateArray.int_delay = sh.ga_int_delay;
    GateArray.scr_mode = sh.scr_modes[0] & 3;
    GateArray.requested_scr_mode = GateArray.scr_mode;
    video_set_palette();

    /* Restore CRTC */
    CRTC.reg_select = sh.crtc_reg_select;
    std::memcpy(CRTC.registers, sh.crtc_registers, 18);

    /* Restore PPI */
    PPI.portA = sh.ppi_A;
    PPI.portB = sh.ppi_B;
    PPI.portC = sh.ppi_C;
    PPI.control = sh.ppi_control;

    /* Restore PSG */
    PSG.reg_select = sh.psg_reg_select;
    std::memcpy(PSG.RegisterAY.Index, sh.psg_registers, 16);

    /* Restore FDC motor */
    FDC.motor = sh.fdc_motor;

    /* Apply memory banking */
    ga_memory_manager();

    printf("cpc_adapter: snapshot loaded (%lu KB)\n", (unsigned long)ram_size);
    z80_sync_ram_shadow(); /* sync shadow after RAM was overwritten */
    return 0;
}

void cpc_set_model(int model) {
    if (model < 0 || model > 2) model = 2;
    CPC.model = model;
}

void cpc_set_ram_size(int kb) {
    if (kb <= 64) CPC.ram_size = 64;
    else if (kb <= 128) CPC.ram_size = 128;
    else CPC.ram_size = 576;
}

void cpc_set_rom(int slot, const char *path) {
    if (slot == 0) {
        std::strncpy(CPC.rom_path, path, 255);
        CPC.rom_path[255] = 0;
    }
}

void cpc_get_palette_rgb(uint32_t *rgb32) {
    std::memcpy(rgb32, cpc_rgb_table, 32 * sizeof(uint32_t));
}

uint8_t cpc_get_border_ink(void) {
    return (uint8_t)(GateArray.ink_values[16] & 0x1f);
}

int cpc_audio_samples_ready(void) {
    return audio_out_count > 0;
}

const int16_t *cpc_audio_get_samples(int *count) {
    *count = audio_out_count;
    audio_out_count = 0;
    return audio_out_buf;
}

/* CreateBlankDsk — generate a standard formatted DSK image.
 * 40 tracks, 1 head, 9 sectors/track (C1-C9), 512 bytes/sector. */
int CreateBlankDsk(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;

    /* Disk header (256 bytes) */
    byte hdr[256];
    std::memset(hdr, 0, 256);
    std::memcpy(hdr, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
    hdr[0x30] = 40; /* tracks */
    hdr[0x31] = 1;  /* sides */
    hdr[0x32] = 0x00; hdr[0x33] = 0x13; /* track size = 0x1300 */

    UINT bw;
    f_write(&f, hdr, 256, &bw);

    /* 40 tracks */
    for (int t = 0; t < 40; t++) {
        byte trk[256];
        std::memset(trk, 0, 256);
        std::memcpy(trk, "Track-Info\r\n", 12);
        trk[0x10] = (byte)t;  /* track number */
        trk[0x11] = 0;        /* side */
        trk[0x14] = 2;        /* BPS = 2 (512 bytes) */
        trk[0x15] = 9;        /* SPT = 9 */
        trk[0x16] = 0x4E;     /* GAP3 */
        trk[0x17] = 0xE5;     /* filler */
        /* Sector info for 9 sectors (C1-C9) */
        for (int s = 0; s < 9; s++) {
            trk[0x18 + s * 8 + 0] = (byte)t;       /* track */
            trk[0x18 + s * 8 + 1] = 0;              /* head */
            trk[0x18 + s * 8 + 2] = (byte)(0xC1 + s); /* sector ID */
            trk[0x18 + s * 8 + 3] = 2;              /* BPS */
        }
        f_write(&f, trk, 256, &bw);

        /* 9 × 512 = 4608 bytes of sector data (0xE5 filled) */
        byte fill[512];
        std::memset(fill, 0xE5, 512);
        for (int s = 0; s < 9; s++)
            f_write(&f, fill, 512, &bw);
    }

    f_close(&f);
    return 0;
}

} /* extern "C" */
