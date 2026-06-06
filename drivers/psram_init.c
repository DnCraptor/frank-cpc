/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "psram_init.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>

// PSRAM max frequency from build config (default 133 MHz)
#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

#define PSRAM_BASE       0x11000000u
#define PSRAM_NOCACHE    0x15000000u
#define PSRAM_PROBE_SIZE (8u * 1024u * 1024u)

static bool s_psram_available = false;

bool psram_is_available(void) {
    return s_psram_available;
}

static bool __no_inline_not_in_flash_func(psram_probe_memory)(void) {
    static const uint32_t offsets[] = {
        0u,
        4u,
        1024u * 1024u,
        4u * 1024u * 1024u,
        PSRAM_PROBE_SIZE - 4u,
    };
    volatile uint32_t *base = (volatile uint32_t *)(uintptr_t)PSRAM_NOCACHE;
    uint32_t saved[sizeof(offsets) / sizeof(offsets[0])];

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        saved[i] = base[offsets[i] / 4u];
    }

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint32_t pattern = 0xA55A0000u ^ (offsets[i] * 2654435761u) ^ i;
        base[offsets[i] / 4u] = pattern;
        __dmb();
        if (base[offsets[i] / 4u] != pattern) {
            for (unsigned j = 0; j < sizeof(offsets) / sizeof(offsets[0]); ++j) {
                base[offsets[j] / 4u] = saved[j];
            }
            __dmb();
            return false;
        }
    }

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint32_t pattern = 0x5AA50000u ^ (~offsets[i]) ^ (i << 16);
        base[offsets[i] / 4u] = pattern;
        __dmb();
        if (base[offsets[i] / 4u] != pattern) {
            for (unsigned j = 0; j < sizeof(offsets) / sizeof(offsets[0]); ++j) {
                base[offsets[j] / 4u] = saved[j];
            }
            __dmb();
            return false;
        }
    }

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        base[offsets[i] / 4u] = saved[i];
    }
    __dmb();
    return true;
}

bool __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    const int clock_hz = clock_get_hz(clk_sys); 

    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | 
                        QMI_DIRECT_CSR_EN_BITS | 
                        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);

    const uint CMD_QPI_EN = 0x35; 
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | CMD_QPI_EN;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS);


    const int max_psram_freq = PSRAM_MAX_FREQ_MHZ * 1000000; 
    
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) {
        divisor = 2;
    }
    
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) {
        rxdelay += 1; 
    }

    const int clock_period_fs = 1000000000000000ll / clock_hz;
    
    const int max_select_val = (125 * 1000000) / clock_period_fs;

    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    qmi_hw->m[1].timing = 
        1 << QMI_M1_TIMING_COOLDOWN_LSB | 
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB | 
        max_select_val << QMI_M1_TIMING_MAX_SELECT_LSB | 
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB | 
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB | 
        divisor << QMI_M1_TIMING_CLKDIV_LSB;

    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB | 
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB | 
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB | 
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB | 
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB | 
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB | 
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;
    
    qmi_hw->m[1].rcmd = 0xEB; 

    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
    
    qmi_hw->m[1].wcmd = 0x38; 

    qmi_hw->direct_csr = 0;
    
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    s_psram_available = psram_probe_memory();
    if (!s_psram_available) {
        hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    }
    return s_psram_available;
}
