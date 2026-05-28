/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape_gpio.c — GPIO22 tape input support.
 *
 * Provides tape_init(), tape_set_gpio_mode(), tape_get_gpio_mode().
 * The tape engine itself is now in cap32/tape.cpp + cpc_adapter.cpp.
 */

#include "tape.h"

#ifdef PICO_BUILD
#include "hardware/gpio.h"
#include "board_config.h"
#endif

static bool s_tape_gpio_mode = false;

void tape_init(void) {
    s_tape_gpio_mode = false;
#ifdef PICO_BUILD
#ifdef TAPE_IN_PIN
    gpio_init(TAPE_IN_PIN);
    gpio_set_dir(TAPE_IN_PIN, GPIO_IN);
    gpio_pull_down(TAPE_IN_PIN);
#endif
#endif
}

void tape_set_gpio_mode(bool active) { s_tape_gpio_mode = active; }
bool tape_get_gpio_mode(void)        { return s_tape_gpio_mode; }
