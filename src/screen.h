/***************************************/
/**                                   **/
/** AMSTRAD/Schneider CPC-Emulator    **/
/** for Linux and X11                 **/
/**                                   **/
/** GNU GENERAL PUBLIC LICENSE        **/
/** 1999, 2000, 2001                  **/
/** Ulrich Cordes                     **/
/** Vor der Dorneiche 1               **/
/** 34317 HABICHTSWALD / Germany      **/
/**                                   **/
/** email:  ulrich.cordes@gmx.de      **/
/** WWW:    http://www.amstrad-cpc.de **/
/**                                   **/
/***************************************/

/*
 *  If you want to make changes, please do not(!) use TABs !!!!!
 */

#ifndef SCREEN_H
#define SCREEN_H 1

#include <stdio.h>
#include "Z80.h"

extern word HD6845Register[18];
extern byte HD6845RegisterPointer;
extern word ScreenAddr;
extern word ScreenOffset;
extern word ScreenBlock;
extern word ScreenBank;
extern unsigned int RedrawScreen;
extern unsigned int ImageOffset;
extern unsigned int LineOffset;
extern unsigned int ScreenMode;
extern unsigned int ScreenModified;
extern unsigned int ChangeInk;

void InitScreen (void);
void WrScreenMem (register word Addr, register byte Value);
void RedrawScreenImage (void);
void RedrawLastLine (void);
void RedrawFirstLine (void);
void SaveScreenAsXPM (char *filename);

#ifdef PICO_BUILD
void RedrawDirtyRows(void);
void pico_record_period_state(int period);
#endif

#endif
