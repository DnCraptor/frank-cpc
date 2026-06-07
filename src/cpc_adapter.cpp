/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_adapter.cpp — Glue between caprice32 emulation engine and Pico platform.
 *
 * Owns all caprice32 global state, handles ROM/RAM allocation via PSRAM,
 * disk/tape loading via FatFS, audio buffer management, and framebuffer setup.
 */

#include "cpc_adapter.h"
#include "cpc_ipf.h"
#include "cpc_cartridge.h"

#include "cap32/cap32.h"
#include "cap32/crtc.h"
#include "cap32/z80.h"
#include "cap32/disk.h"
#include "cap32/tape.h"
#include "cap32/asic.h"

#include <cstring>
#include <cstdio>

/* Pico SDK / driver headers (C) */
extern "C" {
#include "board_config.h"
#include "psram_allocator.h"
#include "crash_handler.h"
#include "ff.h"
#include "Pico/cpc_loader.h"
#include "Pico/cpc_settings.h"
}

/* Live palette RGB values maintained by ASIC/Gate Array rendering */
extern uint32_t asic_dynamic_rgb[256];

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
extern int crtc_type;
extern dword *ModeMaps[4];
extern dword *ModeMap;

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
byte *scanline_render_target = nullptr;
int scanline_render_stride = CPC_FB_WIDTH;

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
extern "C" uint32_t graphics_get_palette(uint8_t idx);

void InitAY();
void ResetAYChipEmulation();
void InitAYCounterVars();
void Calculate_Level_Tables();

/* ------------------------------------------------------------------ */
/* ROM loading via FatFS                                              */
/* ------------------------------------------------------------------ */
/* ROM buffers in internal SRAM for fast Z80 access (instead of PSRAM).
 * ROM is read-heavy: BASIC interpreter, BIOS, AMSDOS — keeping them
 * in SRAM eliminates PSRAM latency on every ROM fetch. */
static byte rom_buffer_sram[32768] __attribute__((aligned(4)));
static byte amsdos_rom_sram[16384] __attribute__((aligned(4)));
static byte mf2_rom[8192] __attribute__((aligned(4)));
static byte *mf2_ram = nullptr;
static byte *rom_buffer = rom_buffer_sram;
static byte *amsdos_rom = amsdos_rom_sram;
static bool mf2_rom_loaded = false;

static int load_rom_file(const char *path, byte *dest, int max_size) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT br;
    f_read(&f, dest, (UINT)max_size, &br);
    f_close(&f);
    return (int)br;
}

static int load_mf2_rom(void) {
    FIL f;
    if (f_open(&f, "/cpc/rom/mf2.rom", FA_READ) != FR_OK) {
        printf("cpc_adapter: mf2.rom not found\n");
        return -1;
    }

    UINT br = 0;
    f_read(&f, mf2_rom, sizeof(mf2_rom), &br);
    f_close(&f);
    if (br < sizeof(mf2_rom)) {
        printf("cpc_adapter: mf2.rom too small (%u bytes)\n", (unsigned)br);
        return -1;
    }

    mf2_rom_loaded = true;
    printf("cpc_adapter: Multiface II ROM loaded\n");
    return 0;
}

static void mf2_reset_state(void) {
    if (mf2_ram) {
        std::memset(mf2_ram, 0, 8192);
    }
    CPC.mf2 = (mf2_rom_loaded && mf2_ram) ? MF2_ACTIVE : 0;
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
    if (drive->tracks > DSK_TRACKMAX)
        drive->tracks = DSK_TRACKMAX;
    unsigned int num_sides = dsk_header.sides;
    if (num_sides > DSK_SIDEMAX)
        num_sides = DSK_SIDEMAX;
    /* FDC uses zero-based side count: 0 = single-sided, 1 = double-sided */
    drive->sides = (num_sides > 0) ? num_sides - 1 : 0;

    for (unsigned int t = 0; t < drive->tracks; t++) {
        for (unsigned int s = 0; s < num_sides; s++) {
            crash_handler_feed();
            dword track_size;
            if (is_extended) {
                track_size = ((dword)dsk_header.track_size[t * num_sides + s]) * 256;
            } else {
                /* Standard DSK: track size is at file offset 0x32–0x33,
                   which maps to the unused2[] field in the struct. */
                track_size = dsk_header.unused2[0] | ((dword)dsk_header.unused2[1] << 8);
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
    /* Call IPF eject hook if set (cleans up CAPS state) */
    if (drive->eject_hook) {
        drive->eject_hook(drive);
    }

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
int fb_y_start = 0; /* first VDU.scrln that maps to cpc_fb row 0 */

/* Called by CRTC at each HSYNC with completed scanline number.
 * Uses DMA to copy scanline data to PSRAM while CPU continues.
 * Double-buffers: CRTC switches to other buffer while DMA copies this one. */
static volatile uint32_t dbg_scanline_calls = 0;
static volatile uint32_t dbg_scanline_rendered = 0;
static volatile uint8_t  dbg_last_pixel = 0xFF;

/* Mid-frame diagnostic: capture palette_byte + pixel data at a specific scanline */
volatile int dbg_capture_scrln = -1; /* set >0 to capture */
byte dbg_palette_byte_snap[34];
byte dbg_scanline_snap[64];
volatile bool dbg_snap_ready = false;

void __attribute__((section(".time_critical.adapter"))) scanline_complete(int scrln) {
    dbg_scanline_calls++;
    int fb_y = scrln - fb_y_start;
    if ((unsigned)fb_y >= (unsigned)CPC_FB_HEIGHT) return;
    if (!scanline_render_target) return;
    dbg_scanline_rendered++;

    /* Mid-frame diagnostic capture — capture BEFORE border check */
    extern int crtc_active_display_offset;
    if (fb_y == dbg_capture_scrln && !dbg_snap_ready) {
        extern byte palette_byte[34];
        memcpy(dbg_palette_byte_snap, palette_byte, 34);
        memcpy(dbg_scanline_snap, scanline_buf + crtc_active_display_offset, 64);
        dbg_snap_ready = true;
    }

    extern int crtc_scanline_had_active;

    uint32_t *dst = (uint32_t *)(scanline_render_target + fb_y * scanline_render_stride);

    if (!crtc_scanline_had_active && CPC.model > 2) {
        /* CPC Plus border-only scanline — fill with true black (index 255)
         * to hide raster palette artifacts that would flash in the vertical
         * border area.  Standard CPC border scanlines copy normally below. */
        memset(dst, 0xFF, 320);
        return;
    }

    const uint32_t *src = (const uint32_t *)(scanline_buf + crtc_active_display_offset);
    /* Copy 320 bytes = 80 words, unrolled 8× for pipeline efficiency */
    for (int i = 0; i < 80; i += 8) {
        uint32_t a = src[i], b = src[i+1], c = src[i+2], d = src[i+3];
        uint32_t e = src[i+4], f = src[i+5], g = src[i+6], h = src[i+7];
        dst[i] = a; dst[i+1] = b; dst[i+2] = c; dst[i+3] = d;
        dst[i+4] = e; dst[i+5] = f; dst[i+6] = g; dst[i+7] = h;
    }

    /* Hide leftmost overscan pixels when the active display starts
     * later than the standard position (offset > 32).  CPC Plus games with
     * non-standard R2 (e.g., Robocop 2 R2=45, offset=40) leave stale
     * border content at the left edge that would be hidden by a real CRT bezel.
     * Mask (offset - 32) bytes to cover the full border leak. */
    if (CPC.model > 2 && crtc_active_display_offset > 32) {
        int mask_bytes = crtc_active_display_offset - 32;
        uint8_t *p = (uint8_t *)dst;
        uint8_t fill = p[mask_bytes]; /* first real pixel after border */
        memset(p, fill, mask_bytes);
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
        asic_dynamic_rgb[i] = cpc_rgb_table[i];
    }
    /* Reserve index 255 as guaranteed black for border blanking */
    graphics_set_palette(255, 0x000000);
    asic_dynamic_rgb[255] = 0x000000;
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

    /* Three-pole cascaded IIR low-pass to smooth PSG square-wave edges.
     * All paths: alpha = 1/2 → cascade -3dB at ~3 kHz, -18dB/oct.
     * This matches the HDMI path and provides good suppression of
     * square-wave harmonics for both I2S DAC and PWM RC-filter outputs. */
    static int32_t lp1_l = 0, lp1_r = 0;
    static int32_t lp2_l = 0, lp2_r = 0;
    static int32_t lp3_l = 0, lp3_r = 0;
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
        /* 3-pole IIR LPF, alpha=1/2 on all paths */
        lp1_l += (l    - lp1_l) >> 1;
        lp1_r += (r    - lp1_r) >> 1;
        lp2_l += (lp1_l - lp2_l) >> 1;
        lp2_r += (lp1_r - lp2_r) >> 1;
        lp3_l += (lp2_l - lp3_l) >> 1;
        lp3_r += (lp2_r - lp3_r) >> 1;
        /* Attenuate to match real CPC analog output levels (-12 dB) */
        audio_out_buf[i * 2]     = (int16_t)(lp3_l >> 2);
        audio_out_buf[i * 2 + 1] = (int16_t)(lp3_r >> 2);
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

    if (!mf2_ram) {
        mf2_ram = (byte *)psram_malloc(8192);
        if (!mf2_ram) {
            printf("cpc_adapter: MF2 RAM alloc failed\n");
        }
    }

    /* Load ROMs */
    if (load_roms() != 0) {
        printf("cpc_adapter: ROM loading failed\n");
        return -1;
    }
    load_mf2_rom();

    /* Allocate ASIC register page in PSRAM (16KB for CPC Plus) */
    if (!pbRegisterPage) {
        pbRegisterPage = (byte *)psram_malloc(16384);
        if (pbRegisterPage) {
            std::memset(pbRegisterPage, 0, 16384);
            printf("cpc_adapter: ASIC register page allocated (16KB)\n");
        }
    }

    /* Initialize engine */
    emulator_init();
    emulator_reset();
    mf2_reset_state();
    z80_sync_ram_shadow(); /* copy CPC RAM to SRAM shadow for fast reads */

    /* Set up hardware palette, then re-apply green monitor if active */
    setup_hw_palette();
    cpc_apply_green_monitor(g_cpc_settings.monitor);

    printf("cpc_adapter: caprice32 engine initialized\n");
    return 0;
}

void cpc_engine_reset(void) {
    emulator_reset();
    mf2_reset_state();
    z80_sync_ram_shadow();
    /* Restore standard CPC hardware palette after leaving Plus mode */
    if (CPC.model <= 2) {
        setup_hw_palette();
        cpc_apply_green_monitor(g_cpc_settings.monitor);
    }
}

void cpc_mf2_page_in(void) {
    if (!mf2_rom_loaded || !mf2_ram || (CPC.mf2 & MF2_INVISIBLE)) {
        return;
    }
    CPC.mf2 |= MF2_ACTIVE | MF2_RUNNING;
}

void cpc_mf2_page_out(void) {
    CPC.mf2 &= ~MF2_RUNNING;
    ga_memory_manager();
}

int cpc_mf2_available(void) {
    return (mf2_rom_loaded && mf2_ram) ? 1 : 0;
}

int cpc_mf2_read(uint16_t addr, uint8_t *val) {
    if (!val || !mf2_ram || (CPC.mf2 & MF2_RUNNING) == 0 || addr >= 0x4000) {
        return 0;
    }
    if (addr < 0x2000) {
        *val = mf2_rom[addr];
    } else {
        *val = mf2_ram[addr - 0x2000];
    }
    return 1;
}

int cpc_mf2_write(uint16_t addr, uint8_t val) {
    if (!mf2_ram || (CPC.mf2 & MF2_RUNNING) == 0 || addr < 0x2000 || addr >= 0x4000) {
        return 0;
    }
    mf2_ram[addr - 0x2000] = val;
    return 1;
}

void cpc_mf2_stop(void) {
    if (!mf2_rom_loaded || !mf2_ram || (CPC.mf2 & MF2_INVISIBLE)) {
        return;
    }

    cpc_mf2_page_in();

    z80.IFF2 = z80.IFF1;
    z80.IFF1 = 0;
    z80.HALT = 0;
    z80.EI_issued = 0;
    z80_write_mem((word)(--z80.SP.w.l), z80.PC.b.h);
    z80_write_mem((word)(--z80.SP.w.l), z80.PC.b.l);
    z80.PC.w.l = 0x0066;
}

void cpc_engine_run_frame(void) {
    int exit_code;

    /* Reset scanline buffer for new frame */
    CPC.scr_base = scanline_buf;
    CPC.scr_pos = scanline_buf;

    /* Snapshot ASIC palette before rendering starts — captures
       the "base" palette set during the previous vblank, before
       raster effects modify entries mid-frame. */
    if (CPC.model > 2) {
        asic_snapshot_palette();
    }

    do {
        exit_code = z80_execute();

        if (exit_code == EC_SOUND_BUFFER) {
            flush_audio();
            audio_ring_push_stereo(audio_out_buf, audio_out_count);
        }
        crash_handler_feed();
    } while (exit_code != EC_FRAME_COMPLETE);

    /* Flush deferred ASIC palette changes and draw CPC Plus sprites */
    if (CPC.model > 2) {
        asic_flush_palette();
        asic_draw_sprites();
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

    int rc;
    if (cpc_is_ipf_file(path)) {
        rc = cpc_ipf_load(path, d);
    } else {
        rc = load_dsk(d, path);
    }

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
    t_SNA_header sh;
    std::memset(&sh, 0, sizeof(sh));

    /* Identifier */
    std::memcpy(sh.id, "MV - SNA", 8);
    sh.version = 3; /* SNA v3 — supports 128K+ */

    /* Z80 registers */
    sh.AF[0] = z80.AF.b.l; sh.AF[1] = z80.AF.b.h;
    sh.BC[0] = z80.BC.b.l; sh.BC[1] = z80.BC.b.h;
    sh.DE[0] = z80.DE.b.l; sh.DE[1] = z80.DE.b.h;
    sh.HL[0] = z80.HL.b.l; sh.HL[1] = z80.HL.b.h;
    sh.IX[0] = z80.IX.b.l; sh.IX[1] = z80.IX.b.h;
    sh.IY[0] = z80.IY.b.l; sh.IY[1] = z80.IY.b.h;
    sh.SP[0] = z80.SP.b.l; sh.SP[1] = z80.SP.b.h;
    sh.PC[0] = z80.PC.b.l; sh.PC[1] = z80.PC.b.h;
    sh.AFx[0] = z80.AFx.b.l; sh.AFx[1] = z80.AFx.b.h;
    sh.BCx[0] = z80.BCx.b.l; sh.BCx[1] = z80.BCx.b.h;
    sh.DEx[0] = z80.DEx.b.l; sh.DEx[1] = z80.DEx.b.h;
    sh.HLx[0] = z80.HLx.b.l; sh.HLx[1] = z80.HLx.b.h;
    sh.I = z80.I;
    sh.R = z80.R;
    sh.IFF0 = z80.IFF1;
    sh.IFF1 = z80.IFF2;
    sh.IM = z80.IM;
    sh.z80_int_pending = z80.int_pending;

    /* Gate Array */
    sh.ga_pen = GateArray.pen;
    std::memcpy(sh.ga_ink_values, GateArray.ink_values, 17);
    sh.ga_ROM_config = GateArray.ROM_config;
    sh.ga_RAM_config = GateArray.RAM_config;
    sh.upper_ROM = GateArray.upper_ROM;
    sh.ga_sl_count = GateArray.sl_count;
    sh.ga_int_delay = GateArray.int_delay;
    sh.scr_modes[0] = (unsigned char)(GateArray.scr_mode & 3);

    /* CRTC */
    sh.crtc_reg_select = CRTC.reg_select;
    std::memcpy(sh.crtc_registers, CRTC.registers, 18);
    sh.crtc_type = (unsigned char)crtc_type;
    sh.crtc_line_count = (unsigned char)CRTC.line_count;
    sh.crtc_raster_count = (unsigned char)CRTC.raster_count;
    sh.crtc_hsw_count = (unsigned char)CRTC.hsw_count;
    sh.crtc_vsw_count = (unsigned char)CRTC.vsw_count;
    sh.crtc_char_count[0] = (unsigned char)(CRTC.char_count & 0xFF);
    sh.crtc_char_count[1] = (unsigned char)((CRTC.char_count >> 8) & 0xFF);
    {
        unsigned int a = CRTC.requested_addr;
        sh.crtc_addr[0] = (unsigned char)(a & 0xFF);
        sh.crtc_addr[1] = (unsigned char)((a >> 8) & 0xFF);
    }
    {
        unsigned int f = 0;
        if (CRTC.flag_invsync) f |= VS_flag;
        sh.crtc_flags[0] = (unsigned char)(f & 0xFF);
        sh.crtc_flags[1] = (unsigned char)((f >> 8) & 0xFF);
    }

    /* PPI */
    sh.ppi_A = PPI.portA;
    sh.ppi_B = PPI.portB;
    sh.ppi_C = PPI.portC;
    sh.ppi_control = PPI.control;

    /* PSG */
    sh.psg_reg_select = PSG.reg_select;
    std::memcpy(sh.psg_registers, PSG.RegisterAY.Index, 16);

    /* FDC */
    sh.fdc_motor = (unsigned char)FDC.motor;
    sh.drvA_current_track = (unsigned char)driveA.current_track;
    sh.drvB_current_track = (unsigned char)driveB.current_track;

    /* CPC model and RAM size */
    sh.cpc_model = (unsigned char)CPC.model;
    sh.ram_size[0] = (unsigned char)(CPC.ram_size & 0xFF);
    sh.ram_size[1] = (unsigned char)((CPC.ram_size >> 8) & 0xFF);

    /* Write to file */
    FIL f;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;

    UINT bw;
    f_write(&f, &sh, sizeof(sh), &bw);
    if (bw < sizeof(sh)) { f_close(&f); return -1; }

    /* Write RAM dump */
    dword ram_bytes = (dword)CPC.ram_size * 1024;
    f_write(&f, pbRAM, ram_bytes, &bw);
    f_close(&f);

    printf("cpc_adapter: snapshot saved (%lu KB)\n", (unsigned long)CPC.ram_size);
    return 0;
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
    if (br < ram_size * 1024) {
        printf("cpc_adapter: snapshot RAM truncated (%lu/%lu bytes)\n",
               (unsigned long)br, (unsigned long)(ram_size * 1024));
        return -1;
    }

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
    crtc_update_palette_cache();
    ModeMap = ModeMaps[GateArray.scr_mode];

    /* Restore CRTC */
    CRTC.reg_select = sh.crtc_reg_select;
    std::memcpy(CRTC.registers, sh.crtc_registers, 18);
    crtc_type = (sh.crtc_type <= 4) ? sh.crtc_type : 0;
    crtc_update_r3();

    /* Restore CRTC counters and flags */
    CRTC.line_count = sh.crtc_line_count;
    CRTC.raster_count = sh.crtc_raster_count;
    CRTC.hsw_count = sh.crtc_hsw_count;
    CRTC.vsw_count = sh.crtc_vsw_count;
    CRTC.char_count = sh.crtc_char_count[0] | ((unsigned int)sh.crtc_char_count[1] << 8);
    {
        unsigned int f = sh.crtc_flags[0] | ((unsigned int)sh.crtc_flags[1] << 8);
        CRTC.flag_invsync = (f & VS_flag) ? 1 : 0;
    }

    /* Recalculate CRTC screen address from registers 12/13 */
    CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
    CRTC.addr = CRTC.requested_addr;
    CRTC.next_addr = CRTC.requested_addr;

    /* Recalculate CRTC match flags from restored counters */
    CRTC.r7match = (CRTC.line_count == CRTC.registers[7]) ? 1 : 0;
    CRTC.r9match = (CRTC.raster_count == CRTC.registers[9]) ? 1 : 0;

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
    if (model < 0 || model > 3) model = 2;
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

void cpc_set_jumpers(unsigned int jumpers) {
    CPC.jumpers = jumpers;
}

void cpc_set_crtc_type(int type) {
    if (type < 0 || type > 4) type = 0;
    crtc_type = type;
    crtc_update_r3();
}

void cpc_set_speed(unsigned int speed) {
    if (speed < MIN_SPEED_SETTING) speed = MIN_SPEED_SETTING;
    if (speed > MAX_SPEED_SETTING) speed = MAX_SPEED_SETTING;
    CPC.speed = speed;
}

void cpc_set_limit_speed(int enabled) {
    CPC.limit_speed = enabled ? 1 : 0;
}

void cpc_set_snd_enabled(int enabled) {
    CPC.snd_enabled = enabled ? 1 : 0;
}

void cpc_set_snd_volume(unsigned int vol) {
    if (vol > 100) vol = 100;
    CPC.snd_volume = vol;
}

void cpc_set_snd_stereo(unsigned int mode) {
    CPC.snd_stereo = mode ? 1 : 0;
}

void cpc_audio_reinit_volume(void) {
    Calculate_Level_Tables();
}

void cpc_apply_green_monitor(int green) {
    uint32_t rgb[32];
    std::memcpy(rgb, cpc_rgb_table, sizeof(rgb));
    if (green) {
        for (int i = 0; i < 32; i++) {
            unsigned r = (rgb[i] >> 16) & 0xFF;
            unsigned g = (rgb[i] >>  8) & 0xFF;
            unsigned b = (rgb[i]      ) & 0xFF;
            unsigned lum = (r * 77 + g * 150 + b * 29) >> 8;
            rgb[i] = lum << 8;
        }
    }
    for (int i = 0; i < 32; i++) {
        graphics_set_palette((uint8_t)i, rgb[i]);
        asic_dynamic_rgb[i] = rgb[i];
    }
}

void cpc_get_palette_rgb(uint32_t *rgb32) {
    std::memcpy(rgb32, cpc_rgb_table, 32 * sizeof(uint32_t));
}

uint8_t cpc_get_border_ink(void) {
    return (uint8_t)(GateArray.ink_values[16] & 0x1f);
}

int cpc_debug_crtc_dump(char *buf, int buflen) {
    int n = snprintf(buf, buflen,
        "R0=%d R1=%d R2=%d R3=$%02X R4=%d R5=%d R6=%d R7=%d R8=$%02X R9=%d "
        "R12=$%02X R13=$%02X scrln=%d scanline=%d line_count=%d raster=%d "
        "fb_y_start=%d mode=%d "
        "inks=%02X.%02X.%02X.%02X border=%02X "
        "ROM_cfg=%02X RAM_cfg=%02X RAM_bank=%02X "
        "sl_calls=%u sl_rend=%u last_px=%02X",
        CRTC.registers[0], CRTC.registers[1], CRTC.registers[2],
        CRTC.registers[3], CRTC.registers[4], CRTC.registers[5],
        CRTC.registers[6], CRTC.registers[7], CRTC.registers[8],
        CRTC.registers[9], CRTC.registers[12], CRTC.registers[13],
        VDU.scrln, VDU.scanline, CRTC.line_count, CRTC.raster_count,
        crtc_fb_y_start, GateArray.requested_scr_mode,
        GateArray.ink_values[0], GateArray.ink_values[1],
        GateArray.ink_values[2], GateArray.ink_values[3],
        GateArray.ink_values[16],
        GateArray.ROM_config, GateArray.RAM_config, GateArray.RAM_bank,
        dbg_scanline_calls, dbg_scanline_rendered, dbg_last_pixel);
    dbg_scanline_calls = 0;
    dbg_scanline_rendered = 0;
    return n;
}

extern int fdc_trace_enabled;

void cpc_fdc_set_trace(int enable) {
    fdc_trace_enabled = enable;
}

int cpc_debug_z80_dump(char *buf, int buflen) {
    int n = snprintf(buf, buflen,
        "OK PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X "
        "I=%02X R=%02X IFF1=%d IFF2=%d IM=%d HALT=%d int_pending=%d "
        "memR0=%s memR3=%s",
        z80.PC.w.l, z80.SP.w.l, z80.AF.w.l, z80.BC.w.l, z80.DE.w.l, z80.HL.w.l,
        z80.IX.w.l, z80.IY.w.l,
        z80.I, z80.R, z80.IFF1, z80.IFF2, z80.IM, z80.HALT, z80.int_pending,
        (membank_read[0] == pbRAM) ? "RAM" : "ROM",
        (membank_read[3] == pbRAM + 0xC000) ? "RAM" : "ROM");
    return n;
}

uint8_t cpc_debug_read_mem(uint16_t addr) {
    return z80_read_mem(addr);
}

void cpc_debug_sprite_dump(int id) {
    printf("SPR%d pos=(%d,%d) mag=%dx%d\n", id,
        asic.sprites_x[id], asic.sprites_y[id],
        asic.sprites_mag_x[id], asic.sprites_mag_y[id]);
    for (int y = 0; y < 16; y++) {
        printf("  Y%02d:", y);
        for (int x = 0; x < 16; x++) {
            printf(" %02X", asic.sprites[id][x][y]);
        }
        printf("\n");
    }
}

int cpc_debug_asic_dump(char *buf, int buflen) {
    extern uint32_t asic_rgb[32];
    extern int crtc_sl0_scrln;
    extern int crtc_active_display_offset;
    extern byte palette_byte[34];
    int n = snprintf(buf, buflen,
        "model=%d locked=%d hscroll=%u vscroll=%u split_sl=%d split_addr=%04X "
        "regPageOn=%d int_sl=%d sl0=%d fby0=%d adoff=%d",
        CPC.model, asic.locked, asic.hscroll, asic.vscroll,
        CRTC.split_sl, CRTC.split_addr,
        GateArray.registerPageOn, CRTC.interrupt_sl,
        crtc_sl0_scrln, fb_y_start, crtc_active_display_offset);
    /* Dump palette_byte mapping */
    n += snprintf(buf + n, buflen - n, "\nPB:");
    for (int i = 0; i < 17 && n < buflen - 10; i++) {
        n += snprintf(buf + n, buflen - n, " %d", palette_byte[i]);
    }
    /* Dump all 32 palette RGB values */
    n += snprintf(buf + n, buflen - n, "\nPAL:");
    for (int i = 0; i < 32 && n < buflen - 10; i++) {
        n += snprintf(buf + n, buflen - n, " %06X", asic_rgb[i]);
    }
    /* Dump first 32 bytes of the most recent scanline_buf */
    n += snprintf(buf + n, buflen - n, "\nSCN:");
    for (int i = 0; i < 32 && n < buflen - 10; i++) {
        n += snprintf(buf + n, buflen - n, " %02X", scanline_buf[crtc_active_display_offset + i]);
    }
    /* Append active sprites */
    for (int i = 0; i < 16 && n < buflen - 40; i++) {
        if (asic.sprites_mag_x[i] > 0 && asic.sprites_mag_y[i] > 0) {
            n += snprintf(buf + n, buflen - n, " S%d(%d,%d)m%dx%d",
                i, asic.sprites_x[i], asic.sprites_y[i],
                asic.sprites_mag_x[i], asic.sprites_mag_y[i]);
        }
    }
    return n;
}

void cpc_debug_write_mem(uint16_t addr, uint8_t val) {
    z80_write_mem(addr, val);
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

/* ------------------------------------------------------------------ */
/* Screenshot: save framebuffer as 8bpp BMP to SD card                */
/* ------------------------------------------------------------------ */

/* Persistent screenshot counter — set once on first call by scanning
 * the screenshot directory, then incremented on each subsequent call. */
static int screenshot_counter = -1;

int cpc_screenshot_save(void) {
    if (!scanline_render_target) return -1;

    /* On first call, scan directory to find highest existing number */
    if (screenshot_counter < 0) {
        screenshot_counter = 0;
        DIR dir;
        FILINFO fi;
        if (f_opendir(&dir, "/cpc/screenshot") == FR_OK) {
            while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
                if (fi.fname[0] == 'C' && fi.fname[1] == 'P' &&
                    fi.fname[2] == 'C' && fi.fname[3] == '_') {
                    int n = 0;
                    for (int j = 4; j < 8 && fi.fname[j] >= '0' && fi.fname[j] <= '9'; j++)
                        n = n * 10 + (fi.fname[j] - '0');
                    if (n > screenshot_counter) screenshot_counter = n;
                }
            }
            f_closedir(&dir);
        }
    }

    screenshot_counter++;
    if (screenshot_counter > 9999) screenshot_counter = 1;

    char path[40];
    snprintf(path, sizeof(path), "/cpc/screenshot/CPC_%04d.BMP", screenshot_counter);

    FIL f;
    f_mkdir("/cpc/screenshot");

    const int W = CPC_FB_WIDTH;   /* 320 */
    const int H = CPC_FB_HEIGHT;  /* 240 */
    const int row_bytes = (W + 3) & ~3;
    const uint32_t palette_size = 256 * 4;
    const uint32_t pixel_offset = 14 + 40 + palette_size;
    const uint32_t pixel_data_size = (uint32_t)row_bytes * H;
    const uint32_t file_size = pixel_offset + pixel_data_size;

    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;

    UINT bw;

    /* Combined BMP file header (14) + DIB header (40) = 54 bytes on stack */
    uint8_t hdr[54];
    std::memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(file_size);        hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16);   hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = (uint8_t)(pixel_offset);     hdr[11] = (uint8_t)(pixel_offset >> 8);
    hdr[12] = (uint8_t)(pixel_offset >> 16); hdr[13] = (uint8_t)(pixel_offset >> 24);
    hdr[14] = 40;
    hdr[18] = (uint8_t)(W);       hdr[19] = (uint8_t)(W >> 8);
    hdr[22] = (uint8_t)(H);       hdr[23] = (uint8_t)(H >> 8);
    hdr[26] = 1;  /* planes */
    hdr[28] = 8;  /* bpp */
    f_write(&f, hdr, 54, &bw);

    /* Palette: read the authoritative RGB888 from the display driver
     * via graphics_get_palette(), which returns the exact value last
     * passed to graphics_set_palette() for each index.
     * Write 32 entries at a time (128-byte chunks). */
    uint8_t pal4[128];
    for (int chunk = 0; chunk < 8; chunk++) {
        std::memset(pal4, 0, sizeof(pal4));
        int base = chunk * 32;
        for (int i = 0; i < 32; i++) {
            int idx = base + i;
            uint32_t rgb = graphics_get_palette((uint8_t)idx);
            pal4[i * 4 + 0] = (uint8_t)(rgb & 0xFF);         /* B */
            pal4[i * 4 + 1] = (uint8_t)((rgb >> 8) & 0xFF);  /* G */
            pal4[i * 4 + 2] = (uint8_t)((rgb >> 16) & 0xFF); /* R */
        }
        f_write(&f, pal4, 128, &bw);
    }

    /* Pixel data — bottom-to-top row order */
    uint8_t row_pad[4] = {0, 0, 0, 0};
    int pad = row_bytes - W;
    for (int y = H - 1; y >= 0; y--) {
        const uint8_t *src = scanline_render_target + y * scanline_render_stride;
        f_write(&f, src, (UINT)W, &bw);
        if (pad > 0)
            f_write(&f, row_pad, (UINT)pad, &bw);
    }

    f_close(&f);
    printf("cpc_adapter: screenshot saved to %s\n", path);
    return 0;
}

} /* extern "C" */
