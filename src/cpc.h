/*
 * Patched for frank-cpc RP2350 port.
 * Original: AMSTRAD/Schneider CPC-Emulator (C) 1999-2001 Ulrich Cordes
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CPC_H
#define CPC_H 1

#ifdef PICO_BUILD
#include "cpc_compat.h"
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#include <stdio.h>
#include "Z80.h"
#ifdef PICO_BUILD
#include "z80_arm.h"
#endif

extern Display *mydisplay;
extern Drawable mywindow;
extern XWindowAttributes mywindowattributes;
extern GC mygc;
extern int myscreen;
extern XEvent myevent;

extern unsigned int depth;
extern int format;
extern unsigned int width, height;
extern int bitmap_pad;
extern XImage *myimage;
extern int ExitCPC;

extern Z80 cpu;
extern char AYRegister[16];

extern int CPCMaxMem;
extern int CPCtype;
extern char ROMFile[8][80];
extern char DiscDir[2][80];
extern char Language[10];
#ifndef PICO_BUILD
extern char InstallDir[];
extern char UserSubDir[];
#endif
extern char WorkDirectory[];
extern char RCfilename[255];
extern char PrinterCmdLine[255];

extern int tmp1, tmp2, tmp3;
extern int NoDebug;
extern FILE *DebugFP;

/* Gate Array IRQ timer reset flag.  Set by io.c when MRER bit 4 is written;
 * cleared by LoopZ80 to skip one IRQ period (~52 HSYNCs) for raster sync. */
extern byte irq_reset_pending;

void WriteRcFile(void);
void InitCPC(int Start);

#endif
