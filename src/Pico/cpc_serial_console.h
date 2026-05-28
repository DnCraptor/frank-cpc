/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_serial_console.h — USB serial command console for automated control.
 *
 * Provides a line-based command protocol over USB CDC.
 * Call cpc_serial_poll() once per frame from the main loop.
 * Commands are newline-terminated, responses prefixed with OK/ERR.
 */
#ifndef CPC_SERIAL_CONSOLE_H
#define CPC_SERIAL_CONSOLE_H

/* Poll for incoming serial commands (non-blocking).
 * Call once per frame from the main emulation loop. */
void cpc_serial_poll(void);

#endif /* CPC_SERIAL_CONSOLE_H */
