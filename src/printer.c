/*
 * frank-cpc — Amstrad CPC for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-cpc
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include "printer.h"

FILE *PrinterFile = 0;
unsigned int PrinterFileNo = 0;
char PrinterFileName[255];
char NoCR, NoLF, NoFF;

void InitPrinter(void) {}
void ClosePrinter(void) {}
void PrintChar(char c) { (void)c; }
