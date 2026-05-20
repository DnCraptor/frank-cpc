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
/* Persistent row-mode hints.  Z80 writes can happen while a game is briefly
 * changing the Gate Array mode inside an interrupt, so end-of-frame redraws
 * use the dominant value as the stable frame mode instead of trusting one
 * transient write to define a whole row. */
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

#ifdef PICO_BUILD
/* Per-row bit flags tracking which screen modes have written to each row
 * during the current frame.  Bit 0 = Mode 0 writes, Bit 1 = Mode 1, etc.
 * Set in WrScreenMem during Z80 execution (not during redraw passes).
 * Reset every frame so anchors can re-emerge after screen transitions.
 * Used by pico_compute_display_modes() to distinguish genuine mode-split
 * regions from raster-interrupt contamination. */
static uint8_t g_row_mode_flags[CPC_FB_ROWS];

/* Per-period mode snapshots, captured at each LoopZ80 interrupt boundary.
 * Used by pico_stable_redraw_mode() for reliable dominant mode detection
 * (immune to g_row_mode contamination during screen transitions). */
static uint8_t g_period_mode_snap[6];

void pico_reset_row_mode_flags(void) {
  memset(g_row_mode_flags, 0, sizeof(g_row_mode_flags));
}

static uint8_t pico_stable_redraw_mode(void) {
   /* Use per-period mode snapshots (captured at each LoopZ80 call) for a
    * reliable dominant mode.  g_row_mode[] can be massively contaminated
    * during screen transitions when the ISR runs while VRAM is rewritten.
    * The period_mode data (5/6 periods = Mode 0 for PoP) is authoritative. */
   unsigned int counts[3] = { 0, 0, 0 };
   for (int p = 0; p < 6; p++) {
     if (g_period_mode_snap[p] < 3) counts[g_period_mode_snap[p]]++;
   }

   uint8_t best = (ScreenMode < 3) ? (uint8_t)ScreenMode : 1;
   for (uint8_t m = 0; m < 3; m++) {
     if (counts[m] > counts[best]) best = m;
   }
   return best;
}

/* Compute per-row display modes by detecting raster-interrupt contamination.
 *
 * CPC games use raster interrupts to split the screen into regions with
 * different modes (e.g. Mode 0 game area + Mode 1 status bar).  Our deferred
 * renderer records WRITE-time modes, but game code may update status bar VRAM
 * during Mode 0 (main loop), then display it in Mode 1 (raster interrupt).
 *
 * Algorithm: find "anchor" rows (pure alt-mode-only writes) in the bottom
 * portion of the screen.  Once a split region is detected, PERSIST it across
 * frames — anchors may flicker frame-to-frame as game code intermittently
 * writes to status bar rows in Mode 0 (timer updates).  The persistent split
 * region is only cleared on full redraws (level transitions). */
#define ANCHOR_MIN_ROW  188   /* Only search for anchors in the last ~12 rows */
static int g_split_first = -1, g_split_last = -1;
static uint8_t g_split_mode = 0;
static int g_split_age = 0;    /* frames since last anchor detection */
#define SPLIT_MAX_AGE  32       /* max frames to persist without anchor refresh */

/* Per-period ink snapshots for palette splitting.
 * Captured in LoopZ80 at each interrupt period boundary.
 * The split palette is the palette from the period with the alt mode. */
static uint8_t g_period_ink[6][17];
static uint8_t g_split_ink[17];       /* palette for split-region rows */
static int g_has_split_ink = 0;       /* 1 if split ink is valid */

void pico_record_period_state(int period) {
  if (period >= 0 && period < 6) {
    g_period_mode_snap[period] = (uint8_t)ScreenMode;
    for (int k = 0; k < 17; k++)
      g_period_ink[period][k] = Ink[k];
  }
}

void pico_compute_split_palette(void) {
  /* Find the dominant mode (most common across periods). */
  unsigned int mcnt[3] = {0, 0, 0};
  for (int p = 0; p < 6; p++)
    if (g_period_mode_snap[p] < 3) mcnt[g_period_mode_snap[p]]++;
  uint8_t dom = 0;
  for (uint8_t m = 1; m < 3; m++)
    if (mcnt[m] > mcnt[dom]) dom = m;

  /* Find the first period with a non-dominant mode — that's the split palette. */
  g_has_split_ink = 0;
  for (int p = 0; p < 6; p++) {
    if (g_period_mode_snap[p] != dom) {
      for (int k = 0; k < 17; k++)
        g_split_ink[k] = g_period_ink[p][k];
      g_has_split_ink = 1;
      break;
    }
  }
}

void pico_reset_split_region(void) {
  g_split_first = -1;
  g_split_last = -1;
  g_split_age = 0;
}

static void pico_compute_display_modes(uint8_t *display_mode) {
  uint8_t dominant = pico_stable_redraw_mode();
  memset(display_mode, dominant, CPC_FB_ROWS);

  uint8_t alt_mode = dominant ^ 1;
  uint8_t alt_bit = (uint8_t)(1u << alt_mode);

  /* Look for anchor rows in the bottom portion of the screen. */
  int first_anchor = -1, last_anchor = -1;
  for (int r = ANCHOR_MIN_ROW; r < CPC_FB_ROWS; r++) {
    if (g_row_mode_flags[r] == alt_bit) {
      if (first_anchor < 0) first_anchor = r;
      last_anchor = r;
    }
  }

  /* If anchors found this frame, update the persistent split region. */
  if (first_anchor >= 0) {
    g_split_first = first_anchor;
    g_split_last = last_anchor;
    g_split_mode = alt_mode;
    g_split_age = 0;
  } else {
    g_split_age++;
  }

  /* Apply the persistent split region (expires after SPLIT_MAX_AGE frames). */
  if (g_split_first >= 0 && g_split_age < SPLIT_MAX_AGE) {
    for (int r = g_split_first; r <= g_split_last; r++)
      display_mode[r] = g_split_mode;
  }
}
#endif

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
            g_row_mode[fb_r] = (uint8_t)ScreenMode;
            if (!g_in_redraw) {
                g_dirty_rows[fb_r] = 1;
                if (ScreenMode < 3)
                    g_row_mode_flags[fb_r] |= (1u << ScreenMode);
            }
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
  /* Full-screen redraws happen at frame/palette boundaries.  Use the
   * dominant row-mode for all rows — contaminated rows get the correct
   * dominant mode, and any legitimate mode-split region (status bar)
   * will self-correct on the next RedrawDirtyRows via the display-mode
   * heuristic once the Z80 rewrites those rows with the correct mode.
   *
   * Reset g_row_mode_flags and the persistent split region so the
   * heuristic re-learns from fresh Z80 writes after level transitions. */
  pico_reset_row_mode_flags();
  pico_reset_split_region();
  unsigned int saved_mode = ScreenMode;
  ScreenMode = pico_stable_redraw_mode();
  memset(cpc_fb, 0, sizeof(cpc_fb));
  memset(g_dirty_rows, 0, sizeof(g_dirty_rows));
  g_in_redraw = 1;
  for (int bank = 0; bank < 8; bank++) {
    int z = bank << 11;
    for (int C = 0; C < 25; C++) {
      int s_base = C * 80;
      for (int s = s_base; s < s_base + 80; s++) {
        word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
        WrScreenMem(Addr, RAM[Addr]);
      }
    }
  }
  g_in_redraw = 0;
  ScreenMode = saved_mode;
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
/** this frame.  Uses pico_compute_display_modes() to get     **/
/** the correct per-row screen mode, filtering out scattered  **/
/** Mode 1 contamination while preserving legitimate mode     **/
/** splits (e.g. Mode 1 status bar).                          **/
/***************************************************************/
void RedrawDirtyRows(void) {
  static uint8_t was_dirty[CPC_FB_ROWS];
  memcpy(was_dirty, g_dirty_rows, CPC_FB_ROWS);
  memset(g_dirty_rows, 0, CPC_FB_ROWS);

  int any = 0;
  for (int r = 0; r < CPC_FB_ROWS; r++) if (was_dirty[r]) { any = 1; break; }
  if (!any) return;

  /* Compute decontaminated display modes for this frame. */
  static uint8_t display_mode[CPC_FB_ROWS];
  pico_compute_display_modes(display_mode);

  for (int r = 0; r < CPC_FB_ROWS; r++)
    if (was_dirty[r]) memset(cpc_fb[r], 0, CPC_FB_WIDTH);

  unsigned int saved_mode = ScreenMode;
  uint8_t saved_ink[17];
  memcpy(saved_ink, AktInk, sizeof(saved_ink));

  g_in_redraw = 1;
  int in_split = 0;   /* track current AktInk state to minimize swaps */
  for (int bank = 0; bank < 8; bank++) {
    int z = bank << 11;
    for (int C = 0; C < 25; C++) {
      int scrZ = bank * 2 + C * 16 + (int)LineOffset;
      if (scrZ > 399) scrZ -= 400;
      int fb_r = scrZ >> 1;
      if ((unsigned)fb_r >= CPC_FB_ROWS || !was_dirty[fb_r]) continue;

      ScreenMode = display_mode[fb_r];

      /* Swap AktInk for split-region rows (palette splitting). */
      int need_split = (g_has_split_ink && g_split_first >= 0 &&
                        fb_r >= g_split_first && fb_r <= g_split_last);
      if (need_split && !in_split) {
        memcpy(AktInk, g_split_ink, sizeof(saved_ink));
        in_split = 1;
      } else if (!need_split && in_split) {
        memcpy(AktInk, saved_ink, sizeof(saved_ink));
        in_split = 0;
      }

      int s_base = C * 80;
      for (int s = s_base; s < s_base + 80; s++) {
        word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
        WrScreenMem(Addr, RAM[Addr]);
      }
    }
  }
  /* Restore AktInk and ScreenMode. */
  memcpy(AktInk, saved_ink, sizeof(saved_ink));
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
