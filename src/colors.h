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

#ifndef COLORS_H
#define COLORS_H 1

#include "Z80.h"

extern long GreenRGBs [32][3];
extern long ColorRGBs [32][3];
extern byte Ink[17];
extern byte InkNum;
extern byte AktInk[17];

extern unsigned long PixColor[64];
extern unsigned long ToolBarColor;

extern int MonoScreen;

void InitColors(void);
unsigned long GetRGBColor(byte R, byte G, byte B);

#endif
