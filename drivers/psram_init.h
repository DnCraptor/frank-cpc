/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PSRAM_INIT_H
#define PSRAM_INIT_H

#include "pico/stdlib.h"
#include <stdbool.h>

bool psram_init(uint cs_pin);
bool psram_is_available(void);

#endif
