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
#define CPC_FB_ROWS 200
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
/* Dirty-row tracking.  WrScreenMem marks the FB row it touched; rendering
 * is deferred to frame end where the correct DISPLAY-TIME mode is used.
 * On real CPC hardware, the Gate Array mode register only affects how the
 * CRTC interprets VRAM for display — VRAM writes are mode-independent.
 * Games use raster interrupts to change mode mid-frame for split screens
 * (e.g. Mode 0 game area + Mode 1 status bar).  Rendering at write-time
 * with ScreenMode would use the wrong mode for writes that happen while
 * the ISR has temporarily changed the mode register. */
static uint8_t g_dirty_rows[CPC_FB_ROWS];
static uint8_t g_in_redraw = 0;

/* Per-period mode snapshots, captured at each LoopZ80 interrupt boundary.
 * Used to determine the correct display-time mode for each screen region. */
static uint8_t g_period_mode_snap[6];

/* Per-period ink snapshots for palette splitting.
 * The status bar uses a different palette than the game area. */
static uint8_t g_period_ink[6][17];
static uint8_t g_split_ink[17];       /* palette for status bar rows */
static uint8_t g_prev_split_ink[17];  /* previous frame's split ink for change detection */
static int g_has_split_ink = 0;
static int g_split_ink_changed = 0;   /* set when split ink differs from previous frame */
static int g_split_row = -1;          /* first fb row of status bar region */

void pico_record_period_state(int period) {
  extern unsigned int DisplayMode;
  if (period >= 0 && period < 6) {
    g_period_mode_snap[period] = (uint8_t)DisplayMode;
    for (int k = 0; k < 17; k++)
      g_period_ink[period][k] = Ink[k];
  }
}

uint8_t pico_get_period_mode(int p) {
  return (p >= 0 && p < 6) ? g_period_mode_snap[p] : 255;
}

/* Determine the dominant display mode (used for most of the screen). */
static uint8_t pico_dominant_mode(void) {
  unsigned int counts[3] = {0, 0, 0};
  for (int p = 0; p < 6; p++)
    if (g_period_mode_snap[p] < 3) counts[g_period_mode_snap[p]]++;
  uint8_t best = 0;
  for (uint8_t m = 1; m < 3; m++)
    if (counts[m] > counts[best]) best = m;
  return best;
}

/* Build a per-row display mode map.  The dominant mode covers the whole
 * screen, then if period 0 has a different mode (status bar), apply it
 * to the bottom 8 rows (CPC char row 24 → fb rows 192-199).
 * Also compute the split palette from the alt-mode period's ink snapshot. */
static void pico_build_display_modes(uint8_t *display_mode) {
  uint8_t dom = pico_dominant_mode();
  memset(display_mode, dom, CPC_FB_ROWS);

  g_has_split_ink = 0;
  g_split_row = -1;

  /* Check if period 0 uses a different mode (status bar split). */
  if (g_period_mode_snap[0] != dom && g_period_mode_snap[0] < 3) {
    int split_row = (384 + (int)LineOffset) >> 1;
    if (split_row >= CPC_FB_ROWS) split_row -= CPC_FB_ROWS;
    g_split_row = split_row;
    for (int r = split_row; r < CPC_FB_ROWS; r++)
      display_mode[r] = g_period_mode_snap[0];

    /* Use period 0's ink snapshot as the status bar palette.
     * Track changes so we can force a redraw when the split palette
     * changes (e.g. after a red flash effect). */
    g_split_ink_changed = 0;
    for (int k = 0; k < 17; k++) {
      uint8_t new_ink = g_period_ink[0][k];
      if (new_ink != g_split_ink[k]) g_split_ink_changed = 1;
      g_prev_split_ink[k] = g_split_ink[k];
      g_split_ink[k] = new_ink;
    }
    g_has_split_ink = 1;
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
#ifdef PICO_BUILD
   { extern unsigned int DisplayMode; DisplayMode = 1; }
#endif
   ChangeInk=FALSE;
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
    /* Mark the row as dirty for deferred rendering at frame end.
     * Do NOT render pixels here — ScreenMode may be temporarily wrong
     * due to a raster interrupt changing the mode register. */
    {
        int fb_r = scrZeile >> 1;
        if ((unsigned)fb_r < CPC_FB_ROWS && !g_in_redraw)
            g_dirty_rows[fb_r] = 1;
    }
    return;
#else
    ScreenModified = 1;
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

#ifdef PICO_BUILD
/***************************************************************/
/** Render a single VRAM byte into cpc_fb[] using the         **/
/** specified screen mode and current AktInk[].                **/
/***************************************************************/
static void pico_render_byte(word Addr, byte Value, unsigned int mode,
                             const uint8_t *ink) {
    int RowOffsAddr = ((Addr & 0x07FF) + 2048 - ScreenOffset) & 0x07FF;
    if (RowOffsAddr >= 2000) return;
    int scrAddr = (Addr & 0x3800) | RowOffsAddr;
    int scrZeile  = PixelPosition[scrAddr & 2047][1];
    int scrSpalte = PixelPosition[scrAddr & 2047][0];
    scrZeile = ((scrAddr >> 11) << 1) + scrZeile + LineOffset;
    if (scrZeile > 399) scrZeile -= 400;

    uint8_t * const row = cpc_fb[scrZeile >> 1];
    const int base = scrSpalte >> 1;
    int i;
    long farbe;

    switch (mode) {
      case 0:
        for (i = 0; i <= 1; i++) {
            switch ((Value << i) & 170) {
                case   0: farbe = PixColor[ink[0]];  break;
                case 128: farbe = PixColor[ink[1]];  break;
                case   8: farbe = PixColor[ink[2]];  break;
                case 136: farbe = PixColor[ink[3]];  break;
                case  32: farbe = PixColor[ink[4]];  break;
                case 160: farbe = PixColor[ink[5]];  break;
                case  40: farbe = PixColor[ink[6]];  break;
                case 168: farbe = PixColor[ink[7]];  break;
                case   2: farbe = PixColor[ink[8]];  break;
                case 130: farbe = PixColor[ink[9]];  break;
                case  10: farbe = PixColor[ink[10]]; break;
                case 138: farbe = PixColor[ink[11]]; break;
                case  34: farbe = PixColor[ink[12]]; break;
                case 162: farbe = PixColor[ink[13]]; break;
                case  42: farbe = PixColor[ink[14]]; break;
                default:  farbe = PixColor[ink[15]]; break;
            }
            row[base + i*2]     = (uint8_t)farbe;
            row[base + i*2 + 1] = (uint8_t)farbe;
        }
        break;
      case 1:
        for (i = 0; i <= 3; i++) {
            switch ((Value >> i) & 17) {
                case  0: farbe = PixColor[ink[0]]; break;
                case  1: farbe = PixColor[ink[2]]; break;
                case 16: farbe = PixColor[ink[1]]; break;
                default: farbe = PixColor[ink[3]]; break;
            }
            row[base + 3 - i] = (uint8_t)farbe;
        }
        break;
      case 2:
        row[base + 0] = (Value & 0xC0) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 1] = (Value & 0x30) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 2] = (Value & 0x0C) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 3] = (Value & 0x03) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        break;
    }
}

/***************************************************************/
/** Render a VRAM byte at explicit framebuffer coordinates,    **/
/** bypassing PixelPosition[].  Used for CRTC overscan modes   **/
/** where the VRAM-to-screen mapping differs from the standard **/
/** 40-char, 25-row layout.                                    **/
/***************************************************************/
static void pico_render_byte_at(byte Value, unsigned int mode,
                                const uint8_t *ink, int fb_x, int fb_y) {
    if ((unsigned)fb_y >= CPC_FB_ROWS || (unsigned)fb_x > CPC_FB_WIDTH - 4)
        return;

    uint8_t * const row = cpc_fb[fb_y];
    const int base = fb_x;
    int i;
    uint8_t farbe;

    switch (mode) {
      case 0:
        for (i = 0; i <= 1; i++) {
            switch ((Value << i) & 170) {
                case   0: farbe = (uint8_t)PixColor[ink[0]];  break;
                case 128: farbe = (uint8_t)PixColor[ink[1]];  break;
                case   8: farbe = (uint8_t)PixColor[ink[2]];  break;
                case 136: farbe = (uint8_t)PixColor[ink[3]];  break;
                case  32: farbe = (uint8_t)PixColor[ink[4]];  break;
                case 160: farbe = (uint8_t)PixColor[ink[5]];  break;
                case  40: farbe = (uint8_t)PixColor[ink[6]];  break;
                case 168: farbe = (uint8_t)PixColor[ink[7]];  break;
                case   2: farbe = (uint8_t)PixColor[ink[8]];  break;
                case 130: farbe = (uint8_t)PixColor[ink[9]];  break;
                case  10: farbe = (uint8_t)PixColor[ink[10]]; break;
                case 138: farbe = (uint8_t)PixColor[ink[11]]; break;
                case  34: farbe = (uint8_t)PixColor[ink[12]]; break;
                case 162: farbe = (uint8_t)PixColor[ink[13]]; break;
                case  42: farbe = (uint8_t)PixColor[ink[14]]; break;
                default:  farbe = (uint8_t)PixColor[ink[15]]; break;
            }
            row[base + i*2]     = farbe;
            row[base + i*2 + 1] = farbe;
        }
        break;
      case 1:
        for (i = 0; i <= 3; i++) {
            switch ((Value >> i) & 17) {
                case  0: farbe = (uint8_t)PixColor[ink[0]]; break;
                case  1: farbe = (uint8_t)PixColor[ink[2]]; break;
                case 16: farbe = (uint8_t)PixColor[ink[1]]; break;
                default: farbe = (uint8_t)PixColor[ink[3]]; break;
            }
            row[base + 3 - i] = farbe;
        }
        break;
      case 2:
        row[base + 0] = (Value & 0xC0) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 1] = (Value & 0x30) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 2] = (Value & 0x0C) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        row[base + 3] = (Value & 0x03) ? (uint8_t)PixColor[ink[1]] : (uint8_t)PixColor[ink[0]];
        break;
    }
}
#endif
/** After any color change, the screen image must   **/
/** be redrawed completely.                         **/
/*****************************************************/

void RedrawScreenImage(void) {
#ifdef PICO_BUILD
  /* Full-screen redraw using per-row display modes from period snapshots.
   * This correctly handles raster interrupt mode splits. */
  static uint8_t display_mode[CPC_FB_ROWS];
  pico_build_display_modes(display_mode);

  memset(cpc_fb, AktInk[16], sizeof(cpc_fb));
  memset(g_dirty_rows, 0, sizeof(g_dirty_rows));
  g_in_redraw = 1;

  /* Use CRTC registers to determine the display layout.
   * Standard CPC: reg1=40 chars/row, reg6=25 rows.
   * Overscan games set wider/taller values (e.g. 48×34). */
  int chars_per_row = HD6845Register[1] ? HD6845Register[1] : 40;
  int rows_displayed = HD6845Register[6] ? HD6845Register[6] : 25;

  if (chars_per_row != 40 || rows_displayed != 25) {
    /* ---- Overscan / non-standard rendering path ---- */
    int bytes_per_row = chars_per_row * 2;
    int display_chars = (chars_per_row > 40) ? 40 : chars_per_row;
    int skip_chars_left = (chars_per_row > 40) ? (chars_per_row - 40) / 2 : 0;
    int skip_bytes_left = skip_chars_left * 2;
    int fb_x_offset = (chars_per_row < 40) ? ((40 - chars_per_row) / 2) * 8 : 0;
    int display_rows = (rows_displayed > 25) ? 25 : rows_displayed;
    int skip_rows_top = (rows_displayed > 25) ? (rows_displayed - 25) / 2 : 0;

    for (int bank = 0; bank < 8; bank++) {
      for (int row = 0; row < display_rows; row++) {
        int crtc_row = row + skip_rows_top;
        int fb_y_400 = bank * 2 + row * 16 + (int)LineOffset;
        if (fb_y_400 > 399) fb_y_400 -= 400;
        int fb_r = fb_y_400 >> 1;
        if ((unsigned)fb_r >= CPC_FB_ROWS) continue;

        unsigned int mode = display_mode[fb_r];
        const uint8_t *ink = (g_has_split_ink && fb_r > g_split_row)
                             ? g_split_ink : AktInk;

        for (int b = 0; b < display_chars * 2; b++) {
          int row_byte = b + skip_bytes_left;
          int s = (crtc_row * bytes_per_row + row_byte) & 0x7FF;
          word Addr = (word)(ScreenBlock +
                     ((ScreenOffset + (bank << 11) + s) & 0x3FFF));
          int fb_x = fb_x_offset + b * 4;
          pico_render_byte_at(RAM[Addr], mode, ink, fb_x, fb_r);
        }
      }
    }
  } else {
    /* ---- Standard 40×25 rendering path ---- */
    for (int bank = 0; bank < 8; bank++) {
      int z = bank << 11;
      for (int C = 0; C < 25; C++) {
        int scrZ = bank * 2 + C * 16 + (int)LineOffset;
        if (scrZ > 399) scrZ -= 400;
        int fb_r = scrZ >> 1;
        unsigned int mode = ((unsigned)fb_r < CPC_FB_ROWS) ? display_mode[fb_r] : ScreenMode;
        const uint8_t *ink = (g_has_split_ink && fb_r > g_split_row) ? g_split_ink : AktInk;

        int s_base = C * 80;
        for (int s = s_base; s < s_base + 80; s++) {
          word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
          pico_render_byte(Addr, RAM[Addr], mode, ink);
        }
      }
    }
  }
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
/** Redraw only the rows touched by WrScreenMem this frame,   **/
/** using display-time modes from period snapshots.            **/
/***************************************************************/
void RedrawDirtyRows(void) {
  /* For overscan modes, dirty row tracking uses the standard PixelPosition
   * mapping which gives wrong positions.  Fall back to full redraw. */
  int chars_per_row = HD6845Register[1] ? HD6845Register[1] : 40;
  int rows_displayed = HD6845Register[6] ? HD6845Register[6] : 25;
  if (chars_per_row != 40 || rows_displayed != 25) {
    /* Check if any rows were dirtied. */
    int any = 0;
    for (int r = 0; r < CPC_FB_ROWS; r++) if (g_dirty_rows[r]) { any = 1; break; }
    memset(g_dirty_rows, 0, sizeof(g_dirty_rows));
    if (any) RedrawScreenImage();
    return;
  }

  static uint8_t was_dirty[CPC_FB_ROWS];
  memcpy(was_dirty, g_dirty_rows, CPC_FB_ROWS);
  memset(g_dirty_rows, 0, CPC_FB_ROWS);

  /* Build display modes first — this also detects split ink changes. */
  static uint8_t display_mode[CPC_FB_ROWS];
  pico_build_display_modes(display_mode);

  /* If the split palette changed (e.g. after a red flash effect),
   * promote to a full redraw so stale palette rows get refreshed. */
  if (g_split_ink_changed) {
    RedrawScreenImage();
    return;
  }

  int any = 0;
  for (int r = 0; r < CPC_FB_ROWS; r++) if (was_dirty[r]) { any = 1; break; }
  if (!any) return;

  for (int r = 0; r < CPC_FB_ROWS; r++)
    if (was_dirty[r]) memset(cpc_fb[r], AktInk[16], CPC_FB_WIDTH);

  g_in_redraw = 1;
  for (int bank = 0; bank < 8; bank++) {
    int z = bank << 11;
    for (int C = 0; C < 25; C++) {
      int scrZ = bank * 2 + C * 16 + (int)LineOffset;
      if (scrZ > 399) scrZ -= 400;
      int fb_r = scrZ >> 1;
      if ((unsigned)fb_r >= CPC_FB_ROWS || !was_dirty[fb_r]) continue;

      unsigned int mode = display_mode[fb_r];
      const uint8_t *ink = (g_has_split_ink && fb_r > g_split_row) ? g_split_ink : AktInk;
      int s_base = C * 80;
      for (int s = s_base; s < s_base + 80; s++) {
        word Addr = (word)(ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF));
        pico_render_byte(Addr, RAM[Addr], mode, ink);
      }
    }
  }
  g_in_redraw = 0;
}
#endif


/******************************************************************/
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
