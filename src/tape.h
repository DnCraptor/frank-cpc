/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tape.h — Platform-level tape GPIO and query functions.
 *
 * The tape engine is now in cap32/tape.cpp; this header provides
 * GPIO22 direct audio input support and query wrappers.
 */
#ifndef TAPE_H
#define TAPE_H

#include <stdint.h>
#include <stdbool.h>

/* Initialise tape GPIO pin (call once at startup). */
void tape_init(void);

/* GPIO22 direct audio input mode. */
void tape_set_gpio_mode(bool active);
bool tape_get_gpio_mode(void);

#endif /* TAPE_H */
