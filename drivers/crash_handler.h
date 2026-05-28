/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <stdint.h>

/*
 * crash_handler_install() — register HardFault, BusFault, and MemManage
 * exception handlers.  Call once, early in main(), after stdio_init_all().
 *
 * crash_handler_check_and_print() — if the previous reset was caused by a
 * crash, print the saved crash info to stdout, then clear the record.
 * Call after stdio_init_all() so the output is visible.
 */

void crash_handler_install(void);
void crash_handler_check_and_print(void);
void crash_handler_feed(void);

#endif /* CRASH_HANDLER_H */
