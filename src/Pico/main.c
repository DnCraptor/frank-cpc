/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
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
#include "ff.h"
#include "ps2kbd_wrapper.h"
#include "ps2.h"

#ifdef HDMI_PIO_AUDIO
#include "frank_hdmi.h"
#else
#include "audio.h"
#endif

#define FB_W CPC_FB_WIDTH
#define FB_H CPC_FB_HEIGHT

static uint8_t __attribute__((aligned(4))) screen_mem[2][FB_W * CPC_SCREEN_LINES];
uint8_t *SCREEN[2] = { screen_mem[0], screen_mem[1] };
volatile uint32_t current_buffer = 0;

uint8_t cpc_fb[FB_H][FB_W];

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

#ifdef HDMI_PIO_AUDIO
/* HDMI_PIO_AUDIO: audio_ring_push_mono and audio_ring_free are
 * provided by HDMI_audio.c which routes samples to the HDMI
 * data-island ring via frank_hdmi_audio_write().  The I2S ring
 * buffer and Core 1 render loop are not needed. */
#else
/* HDMI_PIO: I2S audio ring buffer + Core 1 render loop. */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_FRAMES_PER_CHUNK  882
#define AUDIO_RING_FRAMES       (1u << 12)
#define AUDIO_RING_MASK         (AUDIO_RING_FRAMES - 1)

static uint32_t __attribute__((aligned(4))) g_audio_ring[AUDIO_RING_FRAMES];
volatile uint32_t g_audio_prod = 0;
volatile uint32_t g_audio_cons = 0;

unsigned audio_ring_push_mono(const int16_t *samples, unsigned count) {
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

unsigned audio_ring_free(void) {
    return AUDIO_RING_FRAMES - (g_audio_prod - g_audio_cons);
}

static volatile bool core1_ready = false;

void __time_critical_func(render_core)(void) {
    static i2s_config_t i2s_cfg;
    i2s_cfg = i2s_get_default_config();
    i2s_cfg.sample_freq     = AUDIO_SAMPLE_RATE;
    i2s_cfg.dma_trans_count = AUDIO_FRAMES_PER_CHUNK;
    i2s_volume(&i2s_cfg, 0);   /* no extra shift; volume controlled in mix_notes */
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
#endif /* !HDMI_PIO_AUDIO */

extern void cpc_pico_main(void);

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

    for (int i = 0; i < 10; ++i) sleep_ms(500);

    printf("\n========================================\n");
    printf("  frank-cpc — CPC emulator for RP2350\n");
    printf("  version %s  board M2\n", FRANK_CPC_VERSION);
    printf("  cpu=%lu MHz\n", clock_get_hz(clk_sys) / 1000000u);
    printf("========================================\n");

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
#endif

    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);
    psram_reset();
    printf("PSRAM initialized\n");

    memset(screen_mem, 0, sizeof(screen_mem));
    memset(cpc_fb, 0, sizeof(cpc_fb));

    FRESULT fr = f_mount(&g_fs, "", 1);
    if (fr != FR_OK)
        printf("WARNING: SD card not mounted (%d)\n", fr);
    else
        printf("SD card mounted\n");

    printf("Initializing PS/2 keyboard...\n");
    ps2kbd_init();
    printf("PS/2 keyboard initialized\n");

#ifdef HDMI_PIO_AUDIO
    /* HDMI_PIO_AUDIO: skip VGA probe — always HDMI with embedded audio. */
    printf("Video: HDMI_PIO_AUDIO driver\n");
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, FB_H);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (%dx%d)\n", FB_W, FB_H);

    multicore_launch_core1(frank_hdmi_run_core1);
    sleep_ms(50);
    printf("HDMI audio started (32000 Hz, data-island)\n");
#else
    /* HDMI_PIO: original PIO HDMI + I2S audio path. */
    {
        int link = testPins(HDMI_BASE_PIN, HDMI_BASE_PIN + 1);
        SELECT_VGA = (link == 0) || (link == 0x1F);
        printf("Video: link=0x%02X -> %s\n", (unsigned)link, SELECT_VGA ? "VGA" : "HDMI");
    }
    graphics_init(g_out_HDMI);
    graphics_set_buffer(SCREEN[0]);
    graphics_set_res(FB_W, FB_H);
    graphics_set_shift((320 - FB_W) / 2, 0);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    printf("Video initialized (%dx%d)\n", FB_W, FB_H);

    multicore_launch_core1(render_core);
    while (!core1_ready) tight_loop_contents();
    printf("I2S audio started (44100 Hz)\n");
#endif

    cpc_pico_main();

    while (1) tight_loop_contents();
    return 0;
}
