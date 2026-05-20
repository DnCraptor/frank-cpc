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

#include <stdio.h>
#ifdef PICO_BUILD
#include "cpc_compat.h"

/* Dirty-row tracking: WrScreenMem marks the FB row it painted.
 * RedrawDirtyRows() does a targeted end-of-frame redraw of only those rows,
 * clearing then re-rendering each one from current video RAM.  This gives
 * ghost-free rendering without the cost of a full 16 K redraw every frame. */
#define CPC_FB_ROWS 200
static uint8_t g_dirty_rows[CPC_FB_ROWS];
/* Persistent per-row screen mode.  Updated on every WrScreenMem call so that
 * both the ChangeInk full-redraw and the dirty-row partial-redraw always use
 * the correct mode for each row, even when the game switches between Mode 0
 * (status bar) and Mode 1 (game area) within the same frame. */
static uint8_t g_row_mode[CPC_FB_ROWS];
static uint8_t g_in_redraw = 0;   /* suppresses dirty-row marking during redraw */

#endif
#include "cpc.h"
#include "Z80.h"
#include "mem.h"
#include "defines.h"
#include "screenpos.h"
#include "colors.h"

word HD6845Register[18];
byte HD6845RegisterPointer;

word ScreenAddr;
word ScreenBlock;
word ScreenBank;
word ScreenOffset;
unsigned int ScreenMode;
unsigned int RedrawScreen;
unsigned int ImageOffset;
unsigned int LineOffset;

unsigned int ScreenModified;
unsigned int ChangeInk;

/******************************************************************/

void InitScreen (void) {
   unsigned int x,y,i;
   for (x=0; x<=639; x++) {
     for (y=0; y<=399; y++) {
       XPutPixel(myimage,x,y,PixColor[4]);
     }
   }
   for (i=0;i<=17; i++) {
     HD6845Register[i]=0;
   }
   RedrawScreen = FALSE;
   ScreenAddr=0xC000;
   ScreenOffset = 0;
   ScreenBlock = 0xC000;
   ScreenMode = 1;
   ChangeInk=FALSE;
#ifdef PICO_BUILD
   /* Pre-populate with Mode 1 so the first ChangeInk full-redraw uses the
    * correct mode before the game has written any rows. */
   memset(g_row_mode, 1, sizeof(g_row_mode));
#endif
}

/******************************************************************/

void WrScreenMem (register word Addr, register byte Value) {
#ifdef PICO_BUILD
    /* Non-static locals: compiler can keep them in registers (faster). */
    int scrZeile, scrSpalte;
    int scrAddr, RowOffsAddr;
    int i, j;
    long farbe;
#else
    static int scrZeile, scrSpalte, scrBitNr;
    static int scrAddr, RowOffsAddr;
    static int i, j;
    static long farbe;
    static int mapdx;
    mapdx = 0;
#endif

    RowOffsAddr = ((Addr & 0x07FF) + 2048 - ScreenOffset) & 0x07FF;

#ifdef PICO_BUILD
    /* Bytes 2000–2047 in each 2 KB bank are the horizontal-blank region.
     * PixelPosition[] only has valid entries for indices 0–1999; higher
     * indices zero-initialise to {0,0} and would paint garbage at the
     * top-left corner.  Skip them. */
    if (RowOffsAddr >= 2000) return;
#endif

    scrAddr = (Addr & 0x3800) | RowOffsAddr;

    scrZeile  = PixelPosition [scrAddr & 2047][1];
    scrSpalte = PixelPosition [scrAddr & 2047][0];
    scrZeile  = ((scrAddr >> 11)<<1) + scrZeile + LineOffset;
    if (scrZeile>399) scrZeile-=400;

#ifdef PICO_BUILD
    {
        int fb_r = scrZeile >> 1;
        if ((unsigned)fb_r < CPC_FB_ROWS) {
            /* Always keep the persistent per-row mode up to date so that
             * both RedrawDirtyRows and RedrawScreenImage can replay the row
             * with the correct mode (game may switch between Mode 0 / Mode 1
             * for different screen regions within the same frame). */
            g_row_mode[fb_r] = (uint8_t)ScreenMode;
            if (!g_in_redraw)
                g_dirty_rows[fb_r] = 1;
        }
    }
#else
    ScreenModified = 1;
#endif

#ifdef PICO_BUILD
    /* Fast Pico pixel path.
     * cpc_fb is 320x200 (half of CPC's 640x400), so every pair of adjacent
     * CPC pixels maps to the same fb cell.  Rather than 4–16 XPutPixel calls
     * (each with a bounds-check), write the 4 fb columns directly. */
    {
        uint8_t * const row = cpc_fb[scrZeile >> 1];
        const int base = scrSpalte >> 1;
        switch (ScreenMode) {

          /* Mode 0: 2 macro-pixels × 4 CPC-px wide → 4 fb columns */
          case 0:
            for (i = 0; i <= 1; i++) {
                switch ((Value << i) & 170) {
                    case   0: farbe = PixColor[AktInk[0]];  break;
                    case 128: farbe = PixColor[AktInk[1]];  break;
                    case   8: farbe = PixColor[AktInk[2]];  break;
                    case 136: farbe = PixColor[AktInk[3]];  break;
                    case  32: farbe = PixColor[AktInk[4]];  break;
                    case 160: farbe = PixColor[AktInk[5]];  break;
                    case  40: farbe = PixColor[AktInk[6]];  break;
                    case 168: farbe = PixColor[AktInk[7]];  break;
                    case   2: farbe = PixColor[AktInk[8]];  break;
                    case 130: farbe = PixColor[AktInk[9]];  break;
                    case  10: farbe = PixColor[AktInk[10]]; break;
                    case 138: farbe = PixColor[AktInk[11]]; break;
                    case  34: farbe = PixColor[AktInk[12]]; break;
                    case 162: farbe = PixColor[AktInk[13]]; break;
                    case  42: farbe = PixColor[AktInk[14]]; break;
                    default:  farbe = PixColor[AktInk[15]]; break;
                }
                row[base + i*2]     = (uint8_t)farbe;
                row[base + i*2 + 1] = (uint8_t)farbe;
            }
            break;

          /* Mode 1: 4 pixels × 2 CPC-px wide → 4 fb columns */
          case 1:
            for (i = 0; i <= 3; i++) {
                switch ((Value >> i) & 17) {
                    case  0: farbe = PixColor[AktInk[0]]; break;
                    case  1: farbe = PixColor[AktInk[2]]; break;
                    case 16: farbe = PixColor[AktInk[1]]; break;
                    default: farbe = PixColor[AktInk[3]]; break;
                }
                row[base + 3 - i] = (uint8_t)farbe;
            }
            break;

          /* Mode 2: 8 CPC pixels (1 bit each) → 4 fb columns.
           * Our fb is 320 wide (half of CPC's 640), so each fb cell covers
           * two adjacent CPC pixels.  OR the pair: if either pixel is set,
           * show ink[1].  CPC bit order: px0=bit7, px1=bit6, ..., px7=bit0. */
          case 2:
            row[base + 0] = (Value & 0xC0) ? (uint8_t)PixColor[AktInk[1]] : (uint8_t)PixColor[AktInk[0]];
            row[base + 1] = (Value & 0x30) ? (uint8_t)PixColor[AktInk[1]] : (uint8_t)PixColor[AktInk[0]];
            row[base + 2] = (Value & 0x0C) ? (uint8_t)PixColor[AktInk[1]] : (uint8_t)PixColor[AktInk[0]];
            row[base + 3] = (Value & 0x03) ? (uint8_t)PixColor[AktInk[1]] : (uint8_t)PixColor[AktInk[0]];
            break;
        }
    }
    return;
#endif

    switch (ScreenMode) {

      /************/
      /** MODE 0 **/
      /************/
      case 0 :
        for (i=0; i<=1; i++) {
          switch((Value<<i)&170) {
            case 0:
              farbe = PixColor [AktInk[0]];
              break;
            case 128:
              farbe = PixColor [AktInk[1]];
              break;
            case 8:
              farbe = PixColor [AktInk[2]];
              break;
            case 136:
              farbe = PixColor [AktInk[3]];
              break;
            case 32:
              farbe = PixColor [AktInk[4]];
              break;
            case 160:
              farbe = PixColor [AktInk[5]];
              break;
            case 40:
              farbe = PixColor [AktInk[6]];
              break;
            case 168:
              farbe = PixColor [AktInk[7]];
              break;
            case 2:
              farbe = PixColor [AktInk[8]];
              break;
            case 130:
              farbe = PixColor [AktInk[9]];
              break;
            case 10:
              farbe = PixColor [AktInk[10]];
              break;
            case 138:
              farbe = PixColor [AktInk[11]];
              break;
            case 34:
              farbe = PixColor [AktInk[12]];
              break;
            case 162:
              farbe = PixColor [AktInk[13]];
              break;
            case 42:
              farbe = PixColor [AktInk[14]];
              break;
            case 170:
              farbe = PixColor [AktInk[15]];
              break;
          }
          for (j=0; j<=3; j++) {
            XPutPixel(myimage,(scrSpalte)+(i*4)+j,scrZeile,farbe);
            XPutPixel(myimage,(scrSpalte)+(i*4)+j,scrZeile|1,farbe);
          }
        }
        break;

      /************/
      /** MODE 1 **/
      /************/
      case 1 :
        for (i=0; i<=3; i++) {
          switch ((Value>>i)&17) {
            case 0:
              farbe = PixColor [AktInk[0]];
              break;
            case 1:
              farbe = PixColor [AktInk[2]];
              break;
            case 16:
              farbe = PixColor [AktInk[1]];
              break;
            case 17:
              farbe = PixColor [AktInk[3]];
              break;
          }
          XPutPixel(myimage,(scrSpalte)+6-(i<<1),scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1),scrZeile|1,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1)|1,scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1)|1,scrZeile|1,farbe);
        }
        break;

      /************/
      /** MODE 2 **/
      /************/
      case 2 :
        for (i=0; i<=7; i++) {
          if ((Value & (1<<i))>0)
            farbe = PixColor[AktInk[1]];
          else
            farbe = PixColor[AktInk[0]];

          XPutPixel(myimage,(scrSpalte)+7-i,scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+7-i,scrZeile|1,farbe);
        }
        break;
    }
}


/*****************************************************/
/** After any color change, the screen image must   **/
/** be redrawed completely.                         **/
/*****************************************************/

void RedrawScreenImage(void) {
#ifdef PICO_BUILD
  /* Re-render every row using the persistent per-row screen mode so that
   * rows rendered in Mode 0 (status bar) are not incorrectly re-rendered
   * in Mode 1 (game area) or vice versa.  AktInk has already been updated
   * to the new palette by the caller, which is what we want here. */
  unsigned int saved_mode = ScreenMode;
  memset(cpc_fb, 0, sizeof(cpc_fb));
  memset(g_dirty_rows, 0, sizeof(g_dirty_rows));
  g_in_redraw = 1;
  for (int bank = 0; bank < 8; bank++) {
    int z = bank << 11;
    for (int C = 0; C < 25; C++) {
      int scrZ = bank * 2 + C * 16 + (int)LineOffset;
      if (scrZ > 399) scrZ -= 400;
      int fb_r = scrZ >> 1;
      if ((unsigned)fb_r < CPC_FB_ROWS)
        ScreenMode = g_row_mode[fb_r];
      int s_base = C * 80;
      for (int s = s_base; s < s_base + 80; s++) {
        word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
        WrScreenMem(Addr, RAM[Addr]);
      }
    }
  }
  ScreenMode = saved_mode;
  g_in_redraw = 0;
#else
  static word Addr, z, s;
  g_in_redraw = 1;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
  g_in_redraw = 0;
#endif
}



/***************************************************************/
/** The last line of the screen image must be redrawed after  **/
/** a hardware scroll in upper direction via OUT commands.    **/
/***************************************************************/

void RedrawLastLine (void) {
#ifdef PICO_BUILD
  RedrawScreenImage();
#else
  static word Addr, z, s;
  g_in_redraw = 1;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
  g_in_redraw = 0;
#endif
}


/****************************************************************/
/** The first line of the screen image must be redrawed after  **/
/** a hardware scroll in lower direction via OUT commands.     **/
/****************************************************************/

void RedrawFirstLine (void) {
#ifdef PICO_BUILD
  RedrawScreenImage();
#else
  static word Addr, z, s;
  g_in_redraw = 1;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
  g_in_redraw = 0;
#endif
}

#ifdef PICO_BUILD
/***************************************************************/
/** Redraw only the rows that were touched by WrScreenMem     **/
/** this frame.  Each dirty row is cleared in cpc_fb then     **/
/** re-rendered from current video RAM using the persistent   **/
/** per-row g_row_mode so mode switches (Mode 0 status bar /  **/
/** Mode 1 game area) are correctly replayed.                 **/
/***************************************************************/
void RedrawDirtyRows(void) {
  /* Snapshot and clear dirty flags. g_in_redraw prevents re-marking during
   * the redraw loop so the snapshot stays clean. */
  static uint8_t was_dirty[CPC_FB_ROWS];
  memcpy(was_dirty, g_dirty_rows, CPC_FB_ROWS);
  memset(g_dirty_rows, 0, CPC_FB_ROWS);

  /* Quick exit if nothing changed this frame. */
  int any = 0;
  for (int r = 0; r < CPC_FB_ROWS; r++) if (was_dirty[r]) { any = 1; break; }
  if (!any) return;

  /* Clear only the dirty rows in the framebuffer. */
  for (int r = 0; r < CPC_FB_ROWS; r++)
    if (was_dirty[r]) memset(cpc_fb[r], 0, CPC_FB_WIDTH);

  /* Re-render dirty rows.  ChangeInk is FALSE here so AktInk is already the
   * final palette for the frame.  Use g_row_mode[fb_r] so each row is
   * rendered in the correct screen mode (Mode 0 / Mode 1 / Mode 2). */
  unsigned int saved_mode = ScreenMode;
  g_in_redraw = 1;
  for (int bank = 0; bank < 8; bank++) {
    int z = bank << 11;
    for (int C = 0; C < 25; C++) {
      int scrZ = bank * 2 + C * 16 + (int)LineOffset;
      if (scrZ > 399) scrZ -= 400;
      int fb_r = scrZ >> 1;
      if ((unsigned)fb_r >= CPC_FB_ROWS || !was_dirty[fb_r]) continue;

      ScreenMode = g_row_mode[fb_r];

      int s_base = C * 80;
      for (int s = s_base; s < s_base + 80; s++) {
        word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
        WrScreenMem(Addr, RAM[Addr]);
      }
    }
  }
  ScreenMode = saved_mode;
  g_in_redraw = 0;
}
#endif

/******************************************************************/

/**********************************************/
/**                                          **/
/** Save the screen as XPM pixmap            **/
/**                                          **/
/** XPM is C like coded.                     **/
/**                                          **/
/**********************************************/

#ifdef PICO_BUILD
void SaveScreenAsXPM (char *filename) {
  (void)filename;
}
#else
void SaveScreenAsXPM (char *filename) {
  FILE *fp;
  char txt[20];
  int ncolors, c, idx;
  int col, row, row2;
  unsigned long pixel;
  Colormap cmap;
  XColor color;
  /* Get the color map for XQueryColor function */
  color.flags = DoRed | DoGreen | DoBlue;
  cmap = DefaultColormap (mydisplay,myscreen);

  /* Open the XPM file */
  fp = fopen (filename, "w");
  if (fp!=NULL) {
    /* Print XPM file header */
    fprintf (fp, "/* XPM */\n");
    fprintf (fp, "static char *cpc4x_xpm[] = {\n");
    fprintf (fp, "/* width height ncolors chars_per_pixel */\n");
    /* Get number of colors */
    switch (ScreenMode) {
      case 2 : ncolors = 2; break;
      case 1 : ncolors = 4; break;
      case 0 : ncolors = 16; break;
    }
    /* Print width, height, number of colors and chars per pixel */
    fprintf (fp, "%c640 400 %d 1%c", 34, ncolors, 34);
    /* Print color informations to XPM file */
    for (c=0; c<ncolors; c++) {
      fprintf (fp, ",\n%c%c\t c #",34, 65+c);
      color.pixel =  PixColor[AktInk[c]];
      XQueryColor (mydisplay, cmap, &color);
      fprintf (fp, "%04X",   color.red);
      fprintf (fp, "%04X",   color.green);
      fprintf (fp, "%04X%c", color.blue, 34);
    }
    /* Print the image */
    for (row=0; row<399; row++) { //row<399
      /* Calculate image row with scroll row-offset */
      row2 = row + LineOffset;
      if (row2>399) row2=row2-400; //row2>=400
      /* Print one pixel row of the image */
      fprintf (fp, ",\n%c", 34);
      for (col=0; col<639; col++) {  //col<639
        pixel =  XGetPixel(myimage,col,row2);
        idx = 0;
        for (c=0; c<ncolors; c++)
          if (pixel == PixColor [AktInk[c]]) {
            idx=c;
            c=ncolors;
          }
        fprintf (fp, "%c",65+idx);
      }
      fprintf (fp, "%c",34);
    }
    fprintf (fp, "\n};");
    fclose (fp);
  }
}

#endif

/******************************************************************/

//  END OF  cpc.c

