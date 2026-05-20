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

#ifdef PICO_BUILD
#define XK_Return      0xff0d
#define XK_BackSpace   0xff08
#define XK_Tab         0xff09
#define XK_Escape      0xff1b
#define XK_Delete      0xffff
#define XK_Up          0xff52
#define XK_Down        0xff54
#define XK_Left        0xff51
#define XK_Right       0xff53
#define XK_F1          0xffbe
#define XK_F2          0xffbf
#define XK_F3          0xffc0
#define XK_F4          0xffc1
#define XK_F5          0xffc2
#define XK_F6          0xffc3
#define XK_F7          0xffc4
#define XK_F8          0xffc5
#define XK_F9          0xffc6
#define XK_F10         0xffc7
#define XK_F11         0xffc8
#define XK_F12         0xffc9
#define XK_Shift_L     0xffe1
#define XK_Shift_R     0xffe2
#define XK_Control_L   0xffe3
#define XK_Control_R   0xffe4
#define XK_Caps_Lock   0xffe5
#define XK_Meta_L      0xffe7
#define XK_Alt_L       0xffe9
#define XK_KP_0        0xffb0
#define XK_KP_1        0xffb1
#define XK_KP_2        0xffb2
#define XK_KP_3        0xffb3
#define XK_KP_4        0xffb4
#define XK_KP_5        0xffb5
#define XK_KP_6        0xffb6
#define XK_KP_7        0xffb7
#define XK_KP_8        0xffb8
#define XK_KP_9        0xffb9
#define XK_KP_Decimal  0xffae
#define XK_KP_Enter    0xff8d
#define XK_KP_Left     0xff96
#define XK_KP_Up       0xff97
#define XK_KP_Right    0xff98
#define XK_KP_Down     0xff99
#define XK_KP_Begin    0xff9d
#define XK_KP_Insert   0xff9e
#else
#include <X11/keysym.h>
#endif
#include "defines.h"
#include "Z80.h"
#include "cpc.h"
#include "mem.h"
#include "disc.h"
#include "dialogs.h"
#include "printer.h"
#include "aysound.h"

byte Keyport[10];
byte KeyRow;
byte CtrlKeyPressed;
byte ShiftPressed;
byte CPCShiftPressed;

unsigned int LastReleasedKey;

static int KeyAsciiMask [94][4] = {

      /*  Keyrow, Pressval,Releaseval,Shift   */

  /*32*/  { 5,127,128,FALSE}, { 8,254,  1,TRUE },
          { 8,253,  2,TRUE }, { 7,253,  2,TRUE },
          { 7,254,  1,TRUE }, { 6,253,  2,TRUE },
          { 6,254,  1,TRUE }, { 5,253,  2,TRUE },
          { 5,254,  1,TRUE }, { 4,253,  2,TRUE },

  /*42*/  { 3,223, 32,TRUE }, { 3,239, 16,TRUE },
          { 4,127,128,FALSE}, { 3,253,  2,FALSE},
          { 3,127,128,FALSE}, { 3,191, 64,FALSE},
          { 4,254,  1,FALSE}, { 8,254,  1,FALSE},
          { 8,253,  2,FALSE}, { 7,253,  2,FALSE},

  /*52*/  { 7,254,  1,FALSE}, { 6,253,  2,FALSE},
          { 6,254,  1,FALSE}, { 5,253,  2,FALSE},
          { 5,254,  1,FALSE}, { 4,253,  2,FALSE},
          { 3,223, 32,FALSE}, { 3,239, 16,FALSE},
          { 4,127,128,TRUE }, { 3,253,  2,TRUE },

  /*62*/  { 3,127,128,TRUE }, { 3,191, 64,TRUE },
          { 3,251,  4,FALSE}, 

  /* Capitalized letters */
  /*A*/   { 8,223, 32,TRUE }, { 6,191, 64,TRUE },
          { 7,191, 64,TRUE }, { 7,223, 32,TRUE },
          { 7,251,  4,TRUE }, { 6,223, 32,TRUE },
          { 6,239, 16,TRUE }, { 5,239, 16,TRUE },
          { 4,247,  8,TRUE }, { 5,223, 32,TRUE },

  /*K*/   { 4,223, 32,TRUE }, { 4,239, 16,TRUE },
          { 4,191, 64,TRUE }, { 5,191, 64,TRUE },
          { 4,251,  4,TRUE }, { 3,247,  8,TRUE },
          { 8,247,  8,TRUE }, { 6,251,  4,TRUE },
          { 7,239, 16,TRUE }, { 6,247,  8,TRUE },

  /*U*/   { 5,251,  4,TRUE }, { 6,127,128,TRUE },
          { 7,247,  8,TRUE }, { 7,127,128,TRUE },
          { 5,247,  8,TRUE }, { 8,127,128,TRUE },
          { 2,253,  2,FALSE}, { 2,191, 64,FALSE},
          { 2,247,  8,FALSE}, { 3,254,  1,FALSE},

          { 4,254,  1,TRUE }, { 2,191, 64,TRUE },

  /* Lower case letters */
  /*A*/   { 8,223, 32,FALSE}, { 6,191, 64,FALSE},
          { 7,191, 64,FALSE}, { 7,223, 32,FALSE},
          { 7,251,  4,FALSE}, { 6,223, 32,FALSE},
          { 6,239, 16,FALSE}, { 5,239, 16,FALSE},
          { 4,247,  8,FALSE}, { 5,223, 32,FALSE},

  /*K*/   { 4,223, 32,FALSE}, { 4,239, 16,FALSE},
          { 4,191, 64,FALSE}, { 5,191, 64,FALSE},
          { 4,251,  4,FALSE}, { 3,247,  8,FALSE},
          { 8,247,  8,FALSE}, { 6,251,  4,FALSE},
          { 7,239, 16,FALSE}, { 6,247,  8,FALSE},

  /*U*/   { 5,251,  4,FALSE}, { 6,127,128,FALSE},
          { 7,247,  8,FALSE}, { 7,127,128,FALSE},
          { 5,247,  8,FALSE}, { 8,127,128,FALSE},
          { 2,253,  2,TRUE }, { 3,251,  4,TRUE },
          { 2,247,  8,TRUE }
};



void InitKeyboard (void) {
  int i;
  for (i=0;i<=9;i++)
    Keyport[i]=255;
  LastReleasedKey = 0;
  CPCShiftPressed = FALSE;
  ShiftPressed = FALSE;
}


void DebugEmulation (void) {
  #ifdef DEBUG
  if (!NoDebug) {
    HelpOnDebugger();
    cpu.Trace = 1;
    cpu.Trap = 0x0000;
  }
  #endif
}


void CPCKeyPress(unsigned int k) {
  static byte row,col;
  LastReleasedKey = 0;
  row = col = 255;
  if (k>31 && k<126) {
    Keyport[KeyAsciiMask[k-32][0]] &= KeyAsciiMask[k-32][1];
    if (KeyAsciiMask[k-32][3]==TRUE) {
      Keyport[2] &= 223;
      CPCShiftPressed = TRUE;
    }
    else {
      Keyport[2] |= 32;
      CPCShiftPressed = FALSE;
    }
  }
  else {
    if (ShiftPressed)
      Keyport[2] &= 223;
    else
      Keyport[2] |= 32;

    switch (k) {
      case XK_BackSpace    : row=9; col=127; break;
      case XK_Return       : row=2; col=251; break;
      case XK_Control_L    : row=2; col=127; CtrlKeyPressed = TRUE; break;
      case XK_Control_R    : row=2; col=127; CtrlKeyPressed = TRUE; break;
      case XK_Shift_L      : ShiftPressed = TRUE; break;
      case XK_Shift_R      : ShiftPressed = TRUE; break;
      case XK_Meta_L       : row=1; col=253; break;  /* Left meta key (ALT) is the COPY-Key */
      case XK_Alt_L        : row=1; col=253; break;  /* Left meta key (ALT) is the COPY-Key */
      case XK_Delete       : row=2; col=254; break;
      case XK_Tab          : row=8; col=239; break;
      case XK_Left         : row=1; col=254; break;
      case XK_Up           : row=0; col=254; break;
      case XK_Right        : row=0; col=253; break;
      case XK_Down         : row=0; col=251; break;
      case XK_KP_1         : row=1; col=223; break;  /* Num block if NUMLOCK is switched on */
      case XK_KP_2         : row=1; col=191; break;
      case XK_KP_3         : row=0; col=223; break;
      case XK_KP_4         : row=2; col=239; break;
      case XK_KP_5         : row=1; col=239; break;
      case XK_KP_6         : row=0; col=239; break;
      case XK_KP_7         : row=1; col=251; break;
      case XK_KP_8         : row=1; col=247; break;
      case XK_KP_9         : row=0; col=247; break;
      case XK_KP_0         : row=1; col=127; break;
      case XK_KP_Enter     : row=0; col=191; break; /* Enter key on num block*/
      case XK_KP_Decimal   : row=0; col=127; break;
      case XK_KP_Right     : row=9; col=247; break; /* If NUMLOCK is swithced off, the    */
      case XK_KP_Left      : row=9; col=251; break; /* num block is the joystick          */
      case XK_KP_Up        : row=9; col=254; break; /* 2=Down, 4=Left, 6=Right, 8=Up      */
      case XK_KP_Down      : row=9; col=253; break;
      case XK_KP_Begin     : row=9; col=239; break; /* 5=Fire */
      case XK_KP_Insert    : row=9; col=223; break; /* 0=Fire2 */
      case XK_Escape       : row=8; col=251; break;
      case XK_F1           : InfoDialog (); break;
//    case XK_F2           : if (tmp1) tmp1=FALSE; else tmp1=TRUE; break;  /* I used this only as a code debug help */
      case XK_F3           : InsertDisk (0); break;
      case XK_F4           : InsertDisk (1); break;
      case XK_F5           : SoundOn = !SoundOn; break;
      case XK_F6           : SaveScreenImage(); break;
      case XK_F7           : SetupDialog (); break;
      case XK_F8           : ResetZ80 (&cpu); ResetFDC(); InitMem(); break;
      case XK_F9           : ClosePrinter (); break;
      case XK_F10          : DebugEmulation(); break;
      case XK_F11          : ExitCPC = TRUE; break;
      case XK_F12          : ExitCPC = TRUE; break;
    }
    if (row < 10) Keyport[row] &= col;
  }
}

void CPCKeyRelease (unsigned int k) {
  static byte row,col;
  row = col = 255;
  if (k>31 && k<126) {
    Keyport[KeyAsciiMask[k-32][0]] |= KeyAsciiMask[k-32][2];
    if (KeyAsciiMask[k-32][3]==TRUE)
      Keyport[2] |= 32;
  }
  else {
#ifdef PICO_BUILD
    /* On PS/2, auto-repeat only sends make codes (no release during
     * typematic), so the X11 LastReleasedKey guard is not needed.
     * It caused "sticky keys" when the same key was released twice
     * in succession (e.g. rapid arrow presses during gameplay). */
    switch (k) {
#else
    if (k == LastReleasedKey) {
      if (CtrlKeyPressed) {
        row = 2;
        col = 128;
        CtrlKeyPressed = FALSE;
      }
      if (CPCShiftPressed) {
        row = 2;
        col = 32;
        CPCShiftPressed = FALSE;
        ShiftPressed = FALSE;
      }
    }
    else {
      switch (k) {
#endif
        case XK_BackSpace    : row=9; col=128; break;
        case XK_Return       : row=2; col=  4; break;
        case XK_Control_L    : row=2; col=128; CtrlKeyPressed = FALSE; break;
        case XK_Control_R    : row=2; col=128; CtrlKeyPressed = FALSE; break;
        case XK_Shift_L      : row=2; col= 32; ShiftPressed = FALSE; CPCShiftPressed = FALSE; break;
        case XK_Shift_R      : row=2; col= 32; ShiftPressed = FALSE; CPCShiftPressed = FALSE; break;
        case XK_Meta_L       : row=1; col=  2; break;  /* Left meta key (ALT) is the COPY-Key */
        case XK_Alt_L        : row=1; col=  2; break;  /* Left meta key (ALT) is the COPY-Key */
        case XK_Delete       : row=2; col=  1; break;
        case XK_Tab          : row=8; col= 16; break;
        case XK_Left         : row=1; col=  1; break;
        case XK_Up           : row=0; col=  1; break;
        case XK_Right        : row=0; col=  2; break;
        case XK_Down         : row=0; col=  4; break;
        case XK_KP_1         : row=1; col= 32; break;
        case XK_KP_2         : row=1; col= 64; break;
        case XK_KP_3         : row=0; col= 32; break;
        case XK_KP_4         : row=2; col= 16; break;
        case XK_KP_5         : row=1; col= 16; break;
        case XK_KP_6         : row=0; col= 16; break;
        case XK_KP_7         : row=1; col=  4; break;
        case XK_KP_8         : row=1; col=  8; break;
        case XK_KP_9         : row=0; col=  8; break;
        case XK_KP_0         : row=1; col=128; break;
        case XK_KP_Enter     : row=0; col= 64; break;
        case XK_KP_Decimal   : row=0; col=128; break;
        case XK_KP_Right     : row=9; col=  8; break;
        case XK_KP_Left      : row=9; col=  4; break;
        case XK_KP_Up        : row=9; col=  1; break;
        case XK_KP_Down      : row=9; col=  2; break;
        case XK_KP_Begin     : row=9; col= 16; break;
        case XK_KP_Insert    : row=9; col= 32; break;
        case XK_Escape       : row=8; col=  4; break;
      }
#ifndef PICO_BUILD
    }
#endif
    if (row < 10) Keyport[row] |= col;
  }
  LastReleasedKey = k;
}
