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
#include "cpc_compat.h"
#endif
#include "cpc.h"
#include "Z80.h"
#include "defines.h"

byte Ink[17];
byte InkNum;
byte AktInk[17];

unsigned long PixColor[64];
unsigned long ToolBarColor;
int MonoScreen;

long GreenRGBs [32][3] = {
  { 0x00, 0x24, 0x00 },  { 0x00, 0x24, 0x00 },  { 0x00, 0x30, 0x00 },  { 0x00, 0x3C, 0x00 },
  { 0x00, 0x0C, 0x00 },  { 0x00, 0x18, 0x00 },  { 0x00, 0x1E, 0x00 },  { 0x00, 0x2A, 0x00 },
  { 0x00, 0x18, 0x00 },  { 0x00, 0x3c, 0x00 },  { 0x00, 0x3a, 0x00 },  { 0x00, 0x3e, 0x00 },
  { 0x00, 0x16, 0x00 },  { 0x00, 0x1a, 0x00 },  { 0x00, 0x28, 0x00 },  { 0x00, 0x2c, 0x00 },
  { 0x00, 0x0c, 0x00 },  { 0x00, 0x30, 0x00 },  { 0x00, 0x2e, 0x00 },  { 0x00, 0x32, 0x00 },
  { 0x00, 0x0a, 0x00 },  { 0x00, 0x0e, 0x00 },  { 0x00, 0x1c, 0x00 },  { 0x00, 0x20, 0x00 },
  { 0x00, 0x12, 0x00 },  { 0x00, 0x36, 0x00 },  { 0x00, 0x34, 0x00 },  { 0x00, 0x38, 0x00 },
  { 0x00, 0x10, 0x00 },  { 0x00, 0x14, 0x00 },  { 0x00, 0x22, 0x00 },  { 0x00, 0x26, 0x00 }
};

/* CPC Gate Array hardware colour table, indexed by hardware register value (0-31).
 * Values are 6-bit (0x00=0%, 0x20=50%, 0x3f=100%) per R/G/B channel.
 * Derived from Caprice32 colours_rgb[] table — the authoritative reference.
 * Reference: https://github.com/ColinPitrat/caprice32/blob/master/src/cap32.cpp */
long ColorRGBs [32][3] = {
  /* 0  White            */ { 0x20, 0x20, 0x20 },
  /* 1  White (dup)      */ { 0x20, 0x20, 0x20 },
  /* 2  Sea Green        */ { 0x00, 0x3f, 0x20 },
  /* 3  Pastel Yellow    */ { 0x3f, 0x3f, 0x20 },
  /* 4  Blue             */ { 0x00, 0x00, 0x20 },
  /* 5  Purple           */ { 0x3f, 0x00, 0x20 },
  /* 6  Cyan             */ { 0x00, 0x20, 0x20 },
  /* 7  Pink             */ { 0x3f, 0x20, 0x20 },
  /* 8  Purple (dup)     */ { 0x3f, 0x00, 0x20 },
  /* 9  Pastel Yel (dup) */ { 0x3f, 0x3f, 0x20 },
  /* 10 Bright Yellow    */ { 0x3f, 0x3f, 0x00 },
  /* 11 Bright White     */ { 0x3f, 0x3f, 0x3f },
  /* 12 Bright Red       */ { 0x3f, 0x00, 0x00 },
  /* 13 Bright Magenta   */ { 0x3f, 0x00, 0x3f },
  /* 14 Orange           */ { 0x3f, 0x20, 0x00 },
  /* 15 Pastel Magenta   */ { 0x3f, 0x20, 0x3f },
  /* 16 Blue (dup)       */ { 0x00, 0x00, 0x20 },
  /* 17 Sea Green (dup)  */ { 0x00, 0x3f, 0x20 },
  /* 18 Bright Green     */ { 0x00, 0x3f, 0x00 },
  /* 19 Bright Cyan      */ { 0x00, 0x3f, 0x3f },
  /* 20 Black            */ { 0x00, 0x00, 0x00 },
  /* 21 Bright Blue      */ { 0x00, 0x00, 0x3f },
  /* 22 Green            */ { 0x00, 0x20, 0x00 },
  /* 23 Sky Blue         */ { 0x00, 0x20, 0x3f },
  /* 24 Magenta          */ { 0x20, 0x00, 0x20 },
  /* 25 Pastel Green     */ { 0x20, 0x3f, 0x20 },
  /* 26 Lime             */ { 0x20, 0x3f, 0x00 },
  /* 27 Pastel Cyan      */ { 0x20, 0x3f, 0x3f },
  /* 28 Red              */ { 0x20, 0x00, 0x00 },
  /* 29 Mauve            */ { 0x20, 0x00, 0x3f },
  /* 30 Yellow           */ { 0x20, 0x20, 0x00 },
  /* 31 Pastel Blue      */ { 0x20, 0x20, 0x3f },
};

void InitColors(void) {
#ifdef PICO_BUILD
    for (int i = 0; i < 64; ++i)
        PixColor[i] = (unsigned long)i;
    ToolBarColor = 7;
    cpc_init_palette();
#else
    Colormap cmap;
    XColor color;
    int i;
    color.flags = DoRed | DoGreen | DoBlue;
    cmap = DefaultColormap(mydisplay, myscreen);
    for (i = 0; i <= 31; i++) {
        color.red   = ColorRGBs[i][0] * 0xFFFF / 0x3F;
        color.green = ColorRGBs[i][1] * 0xFFFF / 0x3F;
        color.blue  = ColorRGBs[i][2] * 0xFFFF / 0x3F;
        XAllocColor(mydisplay, cmap, &color);
        PixColor[i] = color.pixel;
    }
    for (i = 32; i <= 63; i++) {
        color.red   = GreenRGBs[i-32][0] * 0xFFFF / 0x3F;
        color.green = GreenRGBs[i-32][1] * 0xFFFF / 0x3F;
        color.blue  = GreenRGBs[i-32][2] * 0xFFFF / 0x3F;
        XAllocColor(mydisplay, cmap, &color);
        PixColor[i] = color.pixel;
    }
    color.red   = 0xC000;
    color.green = 0xC000;
    color.blue  = 0xC000;
    XAllocColor(mydisplay, cmap, &color);
    ToolBarColor = color.pixel;
#endif
}

unsigned long GetRGBColor(byte R, byte G, byte B) {
  return ((long)R<<16 | (long)G<<8 | (long)B);
}
