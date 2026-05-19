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

#ifndef MEM_H
#define MEM_H 1
#include "Z80.h"

extern int ROMNumber;
extern int UpperBlockIsRAM;
extern int LowerBlockIsRAM;
extern int RS_UpBlockRAM;
extern int RS_LoBlockRAM;

extern byte *RAM;
extern byte *UpperROM[8];

/* When non-empty, InitMem() loads this path as the BASIC ROM instead
 * of deriving the filename from CPCtype.  Set by cpc_settings_apply(). */
extern char g_basic_rom_override[80];

int InitMem (void);
int ExitMem (void);
void SelectRamBank (byte Bank);
#endif
