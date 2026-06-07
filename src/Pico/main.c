/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * main.c — RP2350 hardware init + emulator entry point for frank-cpc.
 *
 * Core 0: hardware init → mounts SD → starts HDMI → starts CPC emulator
 * Core 1: I2S audio render loop (drains g_audio_ring)
 */

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "board_config.h"
#include "HDMI.h"
#include "psram_init.h"
#include "psram_allocator.h"
#include "crash_handler.h"
#include "ff.h"
#include "ps2kbd_wrapper.h"
#include "ps2.h"
#include "usbhid_wrapper.h"

#if defined(HDMI_PIO_AUDIO)
#include "frank_hdmi.h"
#include "audio.h"       /* I2S driver — used as VGA audio fallback */
#elif defined(VGA_HSTX)
#include "audio.h"
#else
#include "audio.h"
#endif

#include "pwm_audio.h"
#include "cpc_settings.h"
#include "ui_draw.h"
#include "cpc_boot.h"

/* Lazy-init PWM audio on first use. */
static bool s_pwm_audio_inited = false;
static void ensure_pwm_audio_initialized(void) {
    if (s_pwm_audio_inited) return;
    s_pwm_audio_inited = true;
    pwm_audio_init(PWM_PIN0, PWM_PIN1, 44100);
    pwm_audio_set_frame_rate(50);
}

#define FB_W CPC_FB_WIDTH
#define FB_H CPC_FB_HEIGHT

static uint8_t __attribute__((aligned(4))) screen_mem[2][FB_W * CPC_SCREEN_LINES];
uint8_t *SCREEN[2] = { screen_mem[0], screen_mem[1] };
volatile uint32_t current_buffer = 1;  /* Start at 1: SCREEN[0] is the initial display buffer */

static FATFS g_fs;

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;
    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor < 1) divisor = 1;
    if (divisor == 1 && clock_hz >= 166000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) rxdelay += 1;
    qmi_hw->m[0].timing = 0x60007000u
                        | ((uint32_t)rxdelay << QMI_M0_TIMING_RXDELAY_LSB)
                        | ((uint32_t)divisor << QMI_M0_TIMING_CLKDIV_LSB);
}

#if defined(HDMI_PIO_AUDIO)
/* HDMI_PIO_AUDIO: audio_ring_push_mono and audio_ring_free are
 * provided by HDMI_audio.c which routes samples to the HDMI
 * data-island ring via frank_hdmi_audio_write().  When VGA is
 * detected at runtime, HDMI_audio.c redirects into the I2S ring
 * buffer below so audio still works over the DAC. */

/* I2S ring buffer — used as VGA audio fallback when HDMI is unavailable. */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_FRAMES_PER_CHUNK  882
#define AUDIO_RING_FRAMES       (1u << 12)
#define AUDIO_RING_MASK         (AUDIO_RING_FRAMES - 1)

static uint32_t __attribute__((aligned(4))) g_audio_ring[AUDIO_RING_FRAMES];
volatile uint32_t g_audio_prod = 0;
volatile uint32_t g_audio_cons = 0;

unsigned i2s_ring_push(const int16_t *samples, unsigned count) {
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t s = samples[i];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned i2s_ring_push_stereo(const int16_t *samples, unsigned count) {
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t l = samples[i * 2];
        int16_t r = samples[i * 2 + 1];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned i2s_ring_free(void) {
    return AUDIO_RING_FRAMES - (g_audio_prod - g_audio_cons);
}

static volatile bool core1_ready = false;

void __time_critical_func(render_core_i2s)(void) {
    static i2s_config_t i2s_cfg;
    i2s_cfg = i2s_get_default_config();
    i2s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;
    i2s_cfg.dma_trans_count = AUDIO_FRAMES_PER_CHUNK;
    i2s_volume(&i2s_cfg, 0);
    i2s_init(&i2s_cfg);

    static uint32_t __attribute__((aligned(32))) chunk[AUDIO_FRAMES_PER_CHUNK];

    __dmb();
    core1_ready = true;
    __dmb();

    while (true) {
        uint32_t prod = g_audio_prod;
        uint32_t cons = g_audio_cons;
        uint32_t avail = prod - cons;

        if (avail >= AUDIO_FRAMES_PER_CHUNK) {
            for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
                chunk[i] = g_audio_ring[(cons + i) & AUDIO_RING_MASK];
            __dmb();
            g_audio_cons = cons + AUDIO_FRAMES_PER_CHUNK;
        } else {
            for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
                chunk[i] = 0;
        }
        i2s_dma_write(&i2s_cfg, (const int16_t *)chunk);
    }
}

#elif defined(VGA_HSTX) || defined(VIDEO_COMPOSITE)
/* VGA_HSTX / VIDEO_COMPOSITE: Core 1 is claimed for scanout.
 * I2S audio ring buffer + render loop on Core 0.
 * audio_ring_push_mono is called by aysound.c; i2s_audio_drain()
 * is called once per frame from the emulation loop. */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_FRAMES_PER_CHUNK  882
#define AUDIO_RING_FRAMES       (1u << 12)
#define AUDIO_RING_MASK         (AUDIO_RING_FRAMES - 1)

static uint32_t __attribute__((aligned(4))) g_audio_ring[AUDIO_RING_FRAMES];
static volatile uint32_t g_audio_prod = 0;
static volatile uint32_t g_audio_cons = 0;

static i2s_config_t g_i2s_cfg;
static uint32_t __attribute__((aligned(32))) g_i2s_chunk[AUDIO_FRAMES_PER_CHUNK];
static bool g_i2s_initialized = false;

unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
    if (g_cpc_settings.audio_driver == CPC_AUDIO_PWM) {
        ensure_pwm_audio_initialized();
        pwm_audio_push_samples(samples, (int)count);
        return count;
    }
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t s = samples[i];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned audio_ring_push_stereo(const int16_t *samples, unsigned count) {
    if (g_cpc_settings.audio_driver == CPC_AUDIO_PWM) {
        ensure_pwm_audio_initialized();
        int16_t mono[882];
        unsigned n = count > 882 ? 882 : count;
        for (unsigned i = 0; i < n; ++i)
            mono[i] = (int16_t)(((int32_t)samples[i*2] + (int32_t)samples[i*2+1]) >> 1);
        pwm_audio_push_samples(mono, (int)n);
        return n;
    }
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t l = samples[i * 2];
        int16_t r = samples[i * 2 + 1];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned audio_ring_free(void) {
    return AUDIO_RING_FRAMES - (g_audio_prod - g_audio_cons);
}

static void i2s_audio_init_core0(void) {
    g_i2s_cfg = i2s_get_default_config();
    g_i2s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;
    g_i2s_cfg.dma_trans_count = AUDIO_FRAMES_PER_CHUNK;
    i2s_volume(&g_i2s_cfg, 0);
    i2s_init(&g_i2s_cfg);
    g_i2s_initialized = true;
}

/* Called once per frame from the emulation loop (cpc.c). Drains
 * available ring data into I2S DMA buffers. */
void i2s_audio_drain(void) {
    if (!g_i2s_initialized) return;
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t avail = prod - cons;

    if (avail >= AUDIO_FRAMES_PER_CHUNK) {
        for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
            g_i2s_chunk[i] = g_audio_ring[(cons + i) & AUDIO_RING_MASK];
        __dmb();
        g_audio_cons = cons + AUDIO_FRAMES_PER_CHUNK;
    } else {
        for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
            g_i2s_chunk[i] = 0;
    }
    i2s_dma_write(&g_i2s_cfg, (const int16_t *)g_i2s_chunk);
}

#else
/* HDMI_PIO: I2S audio ring buffer + Core 1 render loop. */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_FRAMES_PER_CHUNK  882
#define AUDIO_RING_FRAMES       (1u << 13)   /* 8192 frames — headroom for adaptive sync */
#define AUDIO_RING_MASK         (AUDIO_RING_FRAMES - 1)

static uint32_t __attribute__((aligned(4))) g_audio_ring[AUDIO_RING_FRAMES];
volatile uint32_t g_audio_prod = 0;
volatile uint32_t g_audio_cons = 0;

unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
    if (g_cpc_settings.audio_driver == CPC_AUDIO_PWM) {
        ensure_pwm_audio_initialized();
        pwm_audio_push_samples(samples, (int)count);
        return count;
    }
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t s = samples[i];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned audio_ring_push_stereo(const int16_t *samples, unsigned count) {
    if (g_cpc_settings.audio_driver == CPC_AUDIO_PWM) {
        ensure_pwm_audio_initialized();
        int16_t mono[882];
        unsigned n = count > 882 ? 882 : count;
        for (unsigned i = 0; i < n; ++i)
            mono[i] = (int16_t)(((int32_t)samples[i*2] + (int32_t)samples[i*2+1]) >> 1);
        pwm_audio_push_samples(mono, (int)n);
        return n;
    }
    uint32_t prod = g_audio_prod;
    uint32_t cons = g_audio_cons;
    uint32_t free_slots = AUDIO_RING_FRAMES - (prod - cons);
    if (count > free_slots) count = free_slots;
    for (unsigned i = 0; i < count; ++i) {
        int16_t l = samples[i * 2];
        int16_t r = samples[i * 2 + 1];
        g_audio_ring[(prod + i) & AUDIO_RING_MASK] =
            ((uint32_t)(uint16_t)r << 16) | (uint16_t)l;
    }
    __dmb();
    g_audio_prod = prod + count;
    return count;
}

unsigned audio_ring_free(void) {
    return AUDIO_RING_FRAMES - (g_audio_prod - g_audio_cons);
}

unsigned audio_ring_avail(void) {
    return g_audio_prod - g_audio_cons;
}

static volatile bool core1_ready = false;

void __time_critical_func(render_core)(void) {
    static i2s_config_t i2s_cfg;
    i2s_cfg = i2s_get_default_config();
    i2s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;
    i2s_cfg.dma_trans_count = AUDIO_FRAMES_PER_CHUNK;
    i2s_volume(&i2s_cfg, 0);
    i2s_init(&i2s_cfg);

    static uint32_t __attribute__((aligned(32))) chunk[AUDIO_FRAMES_PER_CHUNK];
    memset(chunk, 0, sizeof(chunk));

    __dmb();
    core1_ready = true;
    __dmb();

    while (true) {
        uint32_t prod = g_audio_prod;
        __dmb();
        uint32_t cons = g_audio_cons;
        uint32_t avail = prod - cons;

        if (avail >= AUDIO_FRAMES_PER_CHUNK) {
            for (uint32_t i = 0; i < AUDIO_FRAMES_PER_CHUNK; ++i)
                chunk[i] = g_audio_ring[(cons + i) & AUDIO_RING_MASK];
            __dmb();
            g_audio_cons = cons + AUDIO_FRAMES_PER_CHUNK;
        }
        /* If not enough data, replay last chunk (stays in chunk[]) */

        i2s_dma_write(&i2s_cfg, (const int16_t *)chunk);
    }
}
#endif /* HDMI_PIO_AUDIO / VGA_HSTX / HDMI_PIO */

extern void cpc_pico_main(void);

/* Draw a text line with a black padded backdrop over the plasma background */
static void boot_draw_line(uint8_t *fb, int x, int y, const char *text, uint8_t color) {
    int w = (int)strlen(text) * UI_CHAR_W;
    ui_fill_rect(fb, FB_W, x - 4, y - 2, w + 8, UI_CHAR_H + 4, UI_COLOR_BLACK);
    ui_draw_string(fb, FB_W, x, y, text, color);
}

static void boot_error_screen(bool psram_ok, FRESULT sd_result) {
    /* Sine LUT for plasma — in flash (static const) to avoid using RAM */
    static const int8_t plasma_sin[256] = {
           0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
          49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
          90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
         117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
         127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
         117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
          90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
          49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
           0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
         -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
         -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
        -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
        -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
        -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
         -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
         -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
    };

    /* 64-color red plasma palette — black → deep red → bright red (no white) — in flash */
    static const uint32_t plasma_pal[64] = {
        0x080000, 0x1a0000, 0x280000, 0x350000, 0x400100, 0x4b0100, 0x560200, 0x5f0300,
        0x690300, 0x720400, 0x7b0500, 0x830600, 0x8b0700, 0x920700, 0x990800, 0xa00900,
        0xa70a00, 0xad0b00, 0xb20b00, 0xb80c00, 0xbd0d00, 0xc10e00, 0xc50e00, 0xc90f00,
        0xcd0f00, 0xd01000, 0xd21000, 0xd41100, 0xd61100, 0xd81100, 0xd91100, 0xd91100,
        0xda1200, 0xd91100, 0xd91100, 0xd81100, 0xd61100, 0xd41100, 0xd21000, 0xd01000,
        0xcd0f00, 0xc90f00, 0xc50e00, 0xc10e00, 0xbd0d00, 0xb80c00, 0xb20b00, 0xad0b00,
        0xa70a00, 0xa00900, 0x990800, 0x920700, 0x8b0700, 0x830600, 0x7b0500, 0x720400,
        0x690300, 0x5f0300, 0x560200, 0x4b0100, 0x400100, 0x350000, 0x280000, 0x1a0000,
    };

    ui_draw_install_palette();

    /* Load plasma palette into slots 16-79 (CPC not running, these are free) */
    for (int i = 0; i < 64; i++)
        graphics_set_palette((uint8_t)(16 + i), plasma_pal[i]);

    /* Draw plasma into the framebuffer background (one-time) */
    for (int py = 0; py < CPC_SCREEN_LINES; py++) {
        for (int px = 0; px < FB_W; px++) {
            int v = (int)plasma_sin[(uint8_t)(px * 2)]
                  + (int)plasma_sin[(uint8_t)(py * 2)]
                  + (int)plasma_sin[(uint8_t)(px + py)]
                  + (int)plasma_sin[(uint8_t)(px - py)];
            SCREEN[0][py * FB_W + px] = (uint8_t)(16 + (uint8_t)((v + 508) >> 4));
        }
    }
    graphics_set_buffer(SCREEN[0]);

    /* Draw error text on top — uses UI_COLOR_* indices 241-247, unaffected by cycling */
    const int x = 18;
    int y = 24;
    boot_draw_line(SCREEN[0], x, y, "frank-cpc failed to start", UI_COLOR_FG);
    y += 18;

    if (!psram_ok) {
        boot_draw_line(SCREEN[0], x, y, "PSRAM: not detected", UI_COLOR_ERROR);
        y += 10;
    } else {
        boot_draw_line(SCREEN[0], x, y, "PSRAM: OK", UI_COLOR_OK);
        y += 10;
    }

    if (sd_result != FR_OK) {
        char buf[48];
        snprintf(buf, sizeof(buf), "SD card: could not be mounted (error %d)", (int)sd_result);
        boot_draw_line(SCREEN[0], x, y, buf, UI_COLOR_ERROR);
        y += 10;
    } else {
        boot_draw_line(SCREEN[0], x, y, "SD card: OK", UI_COLOR_OK);
        y += 10;
    }

    y += 8;
    boot_draw_line(SCREEN[0], x, y, "The emulator could not be started.", UI_COLOR_FG);
    y += 10;
    boot_draw_line(SCREEN[0], x, y, "Please solder PSRAM and insert an SD card,", UI_COLOR_FG);
    y += 10;
    boot_draw_line(SCREEN[0], x, y, "then reboot.", UI_COLOR_FG);

    y += 20;
    boot_draw_line(SCREEN[0], x, y, "github.com/rh1tech/frank-cpc", UI_COLOR_DIM);
    y += 10;
    {
        char info[50];
        snprintf(info, sizeof(info), "frank-cpc v%s | (c) 2026 Mikhail Matveev",
                 FRANK_CPC_VERSION);
        boot_draw_line(SCREEN[0], x, y, info, UI_COLOR_DIM);
    }

    /* Animate: cycle palette phase each frame — zero extra RAM, ~30 fps */
    uint8_t phase = 0;
    while (true) {
        for (int i = 0; i < 64; i++)
            graphics_set_palette((uint8_t)(16 + i), plasma_pal[(uint8_t)(i + phase) & 63]);
        phase++;
        sleep_ms(33);
    }
}

static void boot_halt_if_required(bool psram_ok, FRESULT sd_result) {
    if (psram_ok && sd_result == FR_OK) return;
    boot_error_screen(psram_ok, sd_result);
}

int main(void) {
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ, FLASH_MAX_FREQ_MHZ);
    sleep_ms(100);
#endif

    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false))
        set_sys_clock_khz(252 * 1000, true);

    stdio_init_all();

#ifndef USB_HID_ENABLED
    /* Wait for USB CDC serial to enumerate so early printf output is
     * visible.  Skipped when USB HID is enabled because the native USB
     * port is in host mode and there's no CDC device to wait for. */
    for (int i = 0; i < 10; ++i) sleep_ms(500);
#endif

    crash_handler_check_and_print();

    printf("\n========================================\n");
    printf("  frank-cpc — Amstrad CPC for RP2350\n");
#if defined(PLATFORM_Z0)
    printf("  version %s  board Z0\n", FRANK_CPC_VERSION);
#elif defined(PLATFORM_M1)
    printf("  version %s  board M1\n", FRANK_CPC_VERSION);
#else
    printf("  version %s  board M2\n", FRANK_CPC_VERSION);
#endif
    printf("  cpu=%lu MHz\n", clock_get_hz(clk_sys) / 1000000u);
    printf("========================================\n");

    crash_handler_install();
    printf("Crash handler installed\n");

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    uint psram_pin = get_psram_pin();
    bool psram_ok = psram_init(psram_pin);
    if (psram_ok) {
        psram_reset();
        printf("PSRAM initialized\n");
    } else {
        printf("WARNING: PSRAM not detected\n");
    }

    memset(screen_mem, 0, sizeof(screen_mem));

#if defined(VGA_HSTX)
    /* VGA_HSTX: bring DispHSTX up FIRST (before SD + PS/2) so the VGA
     * scanout starts producing sync immediately. DispHSTX claims Core 1.
     * Restore clk_peri to 48 MHz after init so SD SPI works correctly. */
    printf("Initializing video output (VGA HSTX, pre-SD)...\n");
    graphics_init(g_out_HDMI);
    clock_configure_undivided(clk_peri,
                              0,
                              CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                              48000000);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, CPC_SCREEN_LINES);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (VGA HSTX %dx%d)\n", FB_W, CPC_SCREEN_LINES);
#elif defined(VIDEO_COMPOSITE)
    /* VIDEO_COMPOSITE: bring TV driver up BEFORE SD + PS/2.
     * graphics_init() launches Core 1 for composite scanout.
     * Restore clk_peri to 48 MHz after init so SD SPI works correctly. */
    printf("Initializing video output (Composite TV, pre-SD)...\n");
    graphics_init(g_out_HDMI);
    clock_configure_undivided(clk_peri,
                              0,
                              CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                              48000000);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, CPC_SCREEN_LINES);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (Composite TV %dx%d)\n", FB_W, CPC_SCREEN_LINES);
#endif

    FRESULT fr = f_mount(&g_fs, "", 1);
    if (fr != FR_OK)
        printf("WARNING: SD card not mounted (%d)\n", fr);
    else
        printf("SD card mounted\n");

    printf("Initializing PS/2 keyboard...\n");
    ps2kbd_init();
    printf("PS/2 keyboard initialized\n");

    /* USB HID host (keyboard/gamepad/XInput). Stubs out to nothing when
     * USB_HID_ENABLED is off — the native USB port is then owned by
     * pico_stdio_usb for CDC printf instead. */
    printf("Initializing USB HID host...\n");
    usbhid_wrapper_init();
    printf("USB HID host initialized\n");

#if defined(VGA_HSTX) || defined(VIDEO_COMPOSITE)
    boot_halt_if_required(psram_ok, fr);
#endif

#if defined(VGA_HSTX)
    /* VGA_HSTX: DispHSTX already claimed Core 1. I2S audio on Core 0. */
    i2s_audio_init_core0();
    printf("VGA_HSTX: Core 1 running DispHSTX scanout, I2S audio on Core 0\n");
#elif defined(VIDEO_COMPOSITE)
    /* VIDEO_COMPOSITE: TV driver already claimed Core 1. I2S audio on Core 0. */
    i2s_audio_init_core0();
    printf("COMPOSITE: Core 1 running TV scanout, I2S audio on Core 0\n");
#elif defined(HDMI_PIO_AUDIO)
    /* HDMI_PIO_AUDIO: VGA autodetect, then HDMI or VGA init. */
    {
        int link = testPins(HDMI_BASE_PIN, HDMI_BASE_PIN + 1);
#if defined(PLATFORM_Z0)
        SELECT_VGA = false; // TODO: (link == 0x1F);
#else
        SELECT_VGA = (link == 0) || (link == 0x1F);
#endif
        printf("Video: HDMI_PIO_AUDIO link=0x%02X -> %s\n", (unsigned)link, SELECT_VGA ? "VGA" : "HDMI");
    }
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, CPC_SCREEN_LINES);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (%dx%d)\n", FB_W, CPC_SCREEN_LINES);
    boot_halt_if_required(psram_ok, fr);

    if (!SELECT_VGA) {
        multicore_launch_core1(frank_hdmi_run_core1);
        sleep_ms(50);
        printf("HDMI audio started (32000 Hz, data-island)\n");
    } else {
        /* VGA mode — no HDMI audio, fall back to I2S on Core 1. */
        multicore_launch_core1(render_core_i2s);
        while (!core1_ready) tight_loop_contents();
        printf("VGA mode — I2S audio started (44100 Hz)\n");
    }
#else
    /* HDMI_PIO: original PIO HDMI + I2S audio path. */
    {
        int link = testPins(HDMI_BASE_PIN, HDMI_BASE_PIN + 1);
#if defined(PLATFORM_Z0)
        SELECT_VGA = false; // TODO: (link == 0x1F);
#else
        SELECT_VGA = (link == 0) || (link == 0x1F);
#endif
        printf("Video: link=0x%02X -> %s\n", (unsigned)link, SELECT_VGA ? "VGA" : "HDMI");
    }
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, CPC_SCREEN_LINES);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (%dx%d)\n", FB_W, CPC_SCREEN_LINES);
    boot_halt_if_required(psram_ok, fr);

    multicore_launch_core1(render_core);
    while (!core1_ready) tight_loop_contents();
    printf("I2S audio started (44100 Hz)\n");
#endif

    printf("Showing welcome screen...\n");
    cpc_boot_welcome(10000);
    printf("Welcome screen done\n");

    cpc_pico_main();

    while (1) tight_loop_contents();
    return 0;
}
