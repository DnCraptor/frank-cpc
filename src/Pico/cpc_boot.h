/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * cpc_boot.h — boot-time welcome screen.
 *
 * Runs before the emulator starts and draws directly into SCREEN[].
 * The welcome screen exits on any key/button press or timeout.
 */
#ifndef CPC_BOOT_H
#define CPC_BOOT_H

#include <stdint.h>

void cpc_boot_welcome(uint32_t timeout_ms);

#endif /* CPC_BOOT_H */
