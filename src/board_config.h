/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * board_config.h — Board configuration dispatcher for frank-cpc.
 *
 * Supported platforms: M1 (Murmulator 1.x), M2 (Murmulator 2.0),
 *                      Z0 (Waveshare RP2350-PiZero)
 *
 * Select platform via CMake: -DPLATFORM=m1|m2|z0
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

#if defined(PLATFORM_M1)
#  include "board_m1.h"
#elif defined(PLATFORM_Z0)
#  include "board_z0.h"
#elif defined(PLATFORM_M2)
#  include "board_m2.h"
#else
#  error "No platform defined — set -DPLATFORM=m1|m2|z0 in CMake"
#endif

#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef PSRAM_MAX_FREQ_MHZ
#define PSRAM_MAX_FREQ_MHZ 133
#endif

#ifndef FLASH_MAX_FREQ_MHZ
#define FLASH_MAX_FREQ_MHZ 66
#endif

#ifndef CPU_VOLTAGE
#  if CPU_CLOCK_MHZ >= 504
#    define CPU_VOLTAGE VREG_VOLTAGE_1_65
#  elif CPU_CLOCK_MHZ >= 300
#    define CPU_VOLTAGE VREG_VOLTAGE_1_60
#  else
#    define CPU_VOLTAGE VREG_VOLTAGE_1_50
#  endif
#endif

#ifndef PSRAM_CS_PIN_RP2350A
#  error "Board header must define PSRAM_CS_PIN_RP2350A"
#endif
#ifndef PSRAM_CS_PIN_RP2350B
#  error "Board header must define PSRAM_CS_PIN_RP2350B"
#endif

#define PSRAM_PIN_RP2350A PSRAM_CS_PIN_RP2350A
#define PSRAM_PIN_RP2350B PSRAM_CS_PIN_RP2350B

static inline uint get_psram_pin(void) {
#if PICO_RP2350
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) return PSRAM_CS_PIN_RP2350A;
    return PSRAM_CS_PIN_RP2350B;
#else
    return 0;
#endif
}

#ifdef PS2_MOUSE_CLK
#  define HAS_PS2_MOUSE 1
#else
#  define PS2_MOUSE_CLK  PS2_PIN_CLK
#  define PS2_MOUSE_DATA PS2_PIN_DATA
#endif

#define CPC_FB_WIDTH    320
#define CPC_FB_HEIGHT   240   /* CPC logical render height — full screen */
/* HDMI.c uses CONTENT_SCANLINES=240*2 and accesses SCREEN[line 0..239].
 * screen_mem must be exactly CPC_SCREEN_LINES rows tall.
 * With CPC_FB_HEIGHT=240, active lines fill the entire screen. */
#define CPC_SCREEN_LINES 240

#endif /* BOARD_CONFIG_H */
