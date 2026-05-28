/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2005 Ulrich Doewich

   CPC Plus ASIC emulation — ported for frank-cpc RP2350 build.
   Based on caprice32 asic.cpp with CPCEC as secondary reference.
   SDL dependencies removed; adapted for 8bpp paletted framebuffer.
*/

#include "asic.h"
#include "crtc.h"
#include "z80.h"
#include <cstring>
#include <cstdio>

byte *pbRegisterPage;

extern t_GateArray GateArray;
extern t_CRTC CRTC;
extern t_CPC CPC;
extern t_PSG PSG;
extern t_z80regs z80;
extern byte *membank_write[4];
extern t_MemBankConfig membank_config;
void SetAYRegister(int Num, byte Value);

/* Frank-cpc uses a per-scanline 8bpp buffer; sprites are drawn into
 * the final framebuffer after the frame completes.  We need access
 * to the framebuffer pointer and stride for sprite overlay. */
extern byte *scanline_render_target;
extern int scanline_render_stride;
extern int fb_y_start;
extern int crtc_sl0_scrln;

asic_t asic;

/* ------------------------------------------------------------------ */
/* ASIC enhanced palette: 12-bit RGB → hardware palette index.        */
/* Frank-cpc uses a 32-entry hardware palette (0-31 = CPC colours).   */
/* ASIC colours 0-31 use palette entries 0-31.                        */
/* We extend the hardware palette for ASIC 4096-color support by      */
/* directly programming RGB values into the platform palette.         */
/* ------------------------------------------------------------------ */
extern "C" {
    void graphics_set_palette(uint8_t index, uint32_t rgb888);
    unsigned char *cpc_cartridge_get_page(int page);
}

uint32_t asic_rgb[32]; /* current RGB888 for each ASIC palette entry */

/* Dynamic palette allocation for per-scanline raster effects.
 * CPC Plus games reprogram palette entries mid-frame to create color
 * gradients. Since our 8bpp framebuffer uses indexed color, we allocate
 * a new hardware palette slot for each unique (pen, RGB) combination
 * written during the frame. Entries 0-31 = base ASIC palette,
 * 32-254 = dynamically allocated for mid-frame changes (double-buffered).
 * Double buffering: even frames use slots 32-143, odd frames use 144-254.
 * This prevents race conditions with the DVI output, which may still
 * be displaying the previous frame's pixels referencing old slot indices. */
static uint32_t dynamic_rgb[256];  /* RGB for each allocated slot */
static int dyn_next = 32;          /* next free dynamic slot */
static int dyn_max = 144;          /* upper limit for current frame's slots */
static int asic_frame_parity = 0;  /* alternates 0/1 each frame */
static uint32_t frame_first_rgb[32]; /* first color written per pen this frame */
static bool frame_first_set[32];    /* whether first write has been captured */
static uint8_t pen_last_slot[32];  /* last dynamic slot per pen (for overflow reuse) */
static bool asic_palette_ever_written = false; /* true once any ASIC palette write occurs */
static bool asic_pen_ever_written[32]; /* per-pen: has this pen EVER been written via ASIC? */

extern byte palette_byte[34];
extern uint32_t *active_halfwidth_lut;
void crtc_update_palette_cache();

static int pal_trace_frame = 0;
static bool pal_trace_enabled = false;

extern "C" void asic_enable_palette_trace(void) {
   pal_trace_frame = 3;
   pal_trace_enabled = true;
}

static void asic_update_colour(int colour) {
   asic_palette_ever_written = true;
   asic_pen_ever_written[colour] = true;
   byte even = pbRegisterPage[0x2400 + colour * 2];
   byte odd  = pbRegisterPage[0x2400 + colour * 2 + 1];
   /* CPC Plus ASIC palette format (same as caprice32):
    * Even byte: bits 7-4 = Red, bits 3-0 = Blue
    * Odd byte:  bits 3-0 = Green */
   unsigned int r = (even >> 4) & 0x0F;
   unsigned int b = even & 0x0F;
   unsigned int g = odd & 0x0F;
   uint32_t rgb = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
   asic_rgb[colour] = rgb;

   /* Capture the first color written for each pen this frame.
    * This is used as the base palette for the NEXT frame's initial
    * scanlines (before any raster effect changes kick in). */
   if (!frame_first_set[colour]) {
      frame_first_rgb[colour] = rgb;
      frame_first_set[colour] = true;
   }

   if (pal_trace_enabled) {
      printf("PAL pen=%d even=0x%02X odd=0x%02X R=%d G=%d B=%d rgb=0x%06X slot=%d\n",
             colour, even, odd, r, g, b, rgb, dyn_next);
   }

   if (dyn_next < dyn_max) {
      int idx = dyn_next++;
      dynamic_rgb[idx] = rgb;
      graphics_set_palette((uint8_t)idx, rgb);  /* program immediately — avoids race with DVI */
      GateArray.palette[colour] = idx;
      palette_byte[colour] = (byte)idx;
      pen_last_slot[colour] = (uint8_t)idx;
      active_halfwidth_lut = nullptr;
   } else if (pen_last_slot[colour] >= 32) {
      /* Out of dynamic slots — reuse this pen's last slot.
       * Only this pen's previous scanlines lose their color;
       * other pens' dynamic slots are preserved. */
      int idx = pen_last_slot[colour];
      dynamic_rgb[idx] = rgb;
      graphics_set_palette((uint8_t)idx, rgb);
      GateArray.palette[colour] = idx;
      palette_byte[colour] = (byte)idx;
      active_halfwidth_lut = nullptr;
   } else {
      /* No dynamic slot ever allocated for this pen — use base entry */
      dynamic_rgb[colour] = rgb;
      graphics_set_palette((uint8_t)colour, rgb);
      GateArray.palette[colour] = colour;
      palette_byte[colour] = (byte)colour;
   }
}

/* ASIC 12-bit equivalents of the 32 CPC ink values, in 0GRB nibble format.
 * Matches CPCEC's video_asic_table[] exactly. */
static const uint16_t ink_to_asic_grb[32] = {
   0x666,0x666,0xF06,0xFF6,0x006,0x0F6,0x606,0x6F6,
   0x0F6,0xFF6,0xFF0,0xFFF,0x0F0,0x0FF,0x6F0,0x6FF,
   0x006,0xF06,0xF00,0xF0F,0x000,0x00F,0x600,0x60F,
   0x066,0xF66,0xF60,0xF6F,0x060,0x06F,0x660,0x66F,
};

void asic_ga_palette_write(int pen, byte ink_value) {
   /* On a real CPC Plus, GA palette writes also update the ASIC palette
    * registers immediately — this is how raster effects work via standard
    * GA port writes (OUT &7Fxx).  We write the 12-bit value into the
    * register page and then call asic_update_colour() which reads it back,
    * converts to RGB, and allocates a dynamic palette slot so the color
    * change takes effect on the current scanline. */
   if (pen > 16 || ink_value > 31) return;
   uint16_t grb = ink_to_asic_grb[ink_value];
   unsigned int r = (grb >> 4) & 0x0F;
   unsigned int b = grb & 0x0F;
   unsigned int g = (grb >> 8) & 0x0F;
   /* Write to ASIC register page: even byte = (R<<4)|B, odd byte = G */
   pbRegisterPage[0x2400 + pen * 2]     = (byte)((r << 4) | b);
   pbRegisterPage[0x2400 + pen * 2 + 1] = (byte)g;
   /* Now call asic_update_colour which reads from the register page,
    * updates asic_rgb[], and allocates a dynamic palette slot. */
   asic_update_colour(pen);
}

void asic_snapshot_palette(void) {
   /* Reset first-write tracking for the new frame */
   memset(frame_first_set, 0, sizeof(frame_first_set));
}

void asic_flush_palette(void) {
   /* If no ASIC or GA palette writes ever occurred, don't override
    * the standard CPC hardware palette (which is set by setup_hw_palette). */
   if (!asic_palette_ever_written) return;
   /* Ensure index 255 stays black (reserved for border blanking) */
   graphics_set_palette(255, 0x000000);
   /* Program base slots 0-31 with current ASIC colors.
    * This is safe because during the frame, all rendered pixels use dynamic
    * slots (32+ or 144+).  No currently-displayed pixel references base slots.
    * Sprites (drawn next) write base slot indices 17-31 directly to the
    * framebuffer, so these MUST have the correct ASIC colors. */
   for (int i = 0; i < 32; i++) {
      if (asic_pen_ever_written[i]) {
         graphics_set_palette((uint8_t)i, asic_rgb[i]);
      }
   }
   /* Switch to the OTHER dynamic slot range for the next frame.
    * Double buffering: even frames use 32-143, odd frames use 144-254.
    * The DVI may still be displaying pixels from THIS frame that reference
    * the current range — the OTHER range is safe to overwrite. */
   asic_frame_parity ^= 1;
   if (asic_frame_parity) {
      dyn_next = 144;
      dyn_max = 255;
   } else {
      dyn_next = 32;
      dyn_max = 144;
   }
   /* Restore base palette mapping for start of next frame */
   for (int i = 0; i < 32; i++) {
      GateArray.palette[i] = i;
      palette_byte[i] = (byte)i;
      pen_last_slot[i] = (uint8_t)i; /* no dynamic slot yet */
   }
   crtc_update_palette_cache();
}

/* ------------------------------------------------------------------ */
/* Reset                                                              */
/* ------------------------------------------------------------------ */
void asic_reset() {
   asic.locked = true;
   asic.lockSeqPos = 0;
   asic_palette_ever_written = false;
   memset(asic_pen_ever_written, 0, sizeof(asic_pen_ever_written));

   asic.extend_border = false;
   asic.hscroll = 0;
   asic.vscroll = 0;

   for (int i = 0; i < 16; i++) {
      asic.sprites_x[i] = 0;
      asic.sprites_y[i] = 0;
      asic.sprites_mag_x[i] = 0;
      asic.sprites_mag_y[i] = 0;
      for (int j = 0; j < 16; j++) {
         for (int k = 0; k < 16; k++) {
            asic.sprites[i][j][k] = 0;
         }
      }
   }

   asic.raster_interrupt = false;
   asic.interrupt_vector = 1;
   asic.irq_cause = 0;

   for (auto &channel : asic.dma.ch) {
      channel.source_address = 0;
      channel.loop_address = 0;
      channel.prescaler = 0;
      channel.enabled = false;
      channel.interrupt = false;
      channel.pause_ticks = 0;
      channel.tick_cycles = 0;
      channel.loops = 0;
   }

   if (pbRegisterPage) {
      std::memset(pbRegisterPage, 0, 16384);
   }
}

/* ------------------------------------------------------------------ */
/* ASIC unlock sequence detection                                     */
/* Written to via CRTC register select port (active-low bit 14).      */
/* Sequence: 00 FF 77 B3 51 A8 D4 62 39 9C 46 2B 15 8A CD           */
/* Last byte 0xCD = unlock, anything else = lock.                     */
/* ------------------------------------------------------------------ */
void asic_poke_lock_sequence(byte val) {
   static constexpr byte lockSeq[] = {
      0x00, 0x00, 0xff, 0x77, 0xb3, 0x51, 0xa8, 0xd4,
      0x62, 0x39, 0x9c, 0x46, 0x2b, 0x15, 0x8a, 0xcd
   };
   static constexpr int lockSeqLength = sizeof(lockSeq) / sizeof(lockSeq[0]);

   if (asic.lockSeqPos == 0) {
      if (val > 0) {
         asic.lockSeqPos = 1;
      }
   } else {
      if (asic.lockSeqPos < lockSeqLength) {
         if (val == lockSeq[asic.lockSeqPos]) {
            asic.lockSeqPos++;
         } else {
            asic.lockSeqPos++;
            if (asic.lockSeqPos == lockSeqLength) {
               asic.locked = true;
            }
            if (val == 0) {
               asic.lockSeqPos = 2;
            } else {
               asic.lockSeqPos = 1;
            }
         }
      } else {
         if (asic.lockSeqPos == lockSeqLength) {
            asic.locked = false;
            asic.lockSeqPos = (val == 0) ? 0 : 1;
         }
      }
   }
}

/* ------------------------------------------------------------------ */
/* Sprite magnification decode                                        */
/* ------------------------------------------------------------------ */
static inline unsigned short decode_magnification(byte val) {
   byte mag = (val & 0x3);
   if (mag == 3) mag = 4;
   return mag;
}

/* ------------------------------------------------------------------ */
/* DMA audio cycle — called once per HSYNC (at hsw_count == 3)        */
/* Reads 16-bit instructions from RAM and programs PSG registers.     */
/* ------------------------------------------------------------------ */
void asic_dma_cycle() {
   if (CPC.model <= 2) return;

   byte dcsr = 0;
   bool dcsr_changed = false;

   for (int c = 0; c < NB_DMA_CHANNELS; c++) {
      dma_channel &channel = asic.dma.ch[c];
      if (!channel.enabled) continue;

      if (channel.pause_ticks > 0) {
         if (channel.tick_cycles < channel.prescaler) {
            channel.tick_cycles++;
            continue;
         }
         channel.tick_cycles = 0;
         channel.pause_ticks--;
         continue;
      }

      int bank = ((channel.source_address & 0xC000) >> 14);
      int addr = (channel.source_address & 0x3FFF);
      word instruction = 0;
      instruction |= membank_config[GateArray.RAM_config & 7][bank][addr];
      instruction |= membank_config[GateArray.RAM_config & 7][bank][addr + 1] << 8;

      int opcode = ((instruction & 0x7000) >> 12);
      if (opcode == 0) {
         /* LOAD R,DD — write DD to PSG register R */
         int R = ((instruction & 0x0F00) >> 8);
         byte val = (instruction & 0x00FF);
         SetAYRegister(R, val);
      } else {
         if (opcode & 0x01) {
            /* PAUSE N */
            channel.pause_ticks = instruction & 0x0FFF;
            channel.tick_cycles = 0;
         }
         if (opcode & 0x02) {
            /* REPEAT NNN */
            channel.loops = instruction & 0x0FFF;
            channel.loop_address = channel.source_address;
         }
         if (opcode & 0x04) {
            /* NOP / LOOP / INT / STOP */
            if (instruction & 0x0001) {
               /* LOOP */
               if (channel.loops > 0) {
                  channel.source_address = channel.loop_address;
               }
            }
            if (instruction & 0x0010) {
               /* INT */
               channel.interrupt = true;
            }
            if (instruction & 0x0020) {
               /* STOP */
               channel.enabled = false;
            }
         }
      }
      channel.source_address += 2;

      /* Update DMA registers in register page */
      {
         word raddr = 0x6C00 + (c << 2);
         if (pbRegisterPage) {
            pbRegisterPage[(raddr - 0x4000)] = (byte)(channel.source_address & 0xFF);
            pbRegisterPage[(raddr - 0x4000) + 1] = (byte)((channel.source_address >> 8) & 0xFF);
         }
         if (channel.enabled) {
            dcsr |= (0x1 << c);
            dcsr_changed = true;
         }
         if (channel.interrupt) {
            dcsr |= (0x40 >> c);
            dcsr_changed = true;
         }
      }
   }

   if (dcsr_changed && pbRegisterPage) {
      pbRegisterPage[0x6C0F - 0x4000] = dcsr;
   }

   for (int c = 0; c < NB_DMA_CHANNELS; c++) {
      if (asic.dma.ch[c].interrupt) {
         z80.int_pending = 1;
         asic.irq_cause = 4 - c;  // ch0→4, ch1→3, ch2→2
         asic.dma.ch[c].interrupt = false;
         break;
      }
   }
}

/* ------------------------------------------------------------------ */
/* ASIC register page write handler                                   */
/* Called when Z80 writes to 0x4000-0x7FFF while register page is     */
/* mapped. Returns true if byte should also be written to backing RAM.*/
/* ------------------------------------------------------------------ */
bool asic_register_page_write(word addr, byte val) {
   if (addr < 0x4000 || addr > 0x7FFF) {
      return true;
   }

   /* Sprite pixel data: 0x4000-0x4FFF */
   if (addr >= 0x4000 && addr < 0x5000) {
      int id = ((addr & 0xF00) >> 8);
      int y = ((addr & 0xF0) >> 4);
      int x = (addr & 0xF);
      byte color = (val & 0xF);
      pbRegisterPage[(addr & 0x3FFF)] = color;
      if (color > 0) {
         color += 16; /* sprite colours use palette entries 16-31 */
      }
      asic.sprites[id][x][y] = color;
      return false;
   }

   /* Sprite coordinates: 0x6000-0x607F */
   if (addr >= 0x6000 && addr < 0x607D) {
      int id = ((addr - 0x6000) >> 3);
      int type = (addr & 0x7);
      switch (type) {
         case 0:
            asic.sprites_x[id] = (asic.sprites_x[id] & 0xFF00) | val;
            pbRegisterPage[(addr & 0x3FFF) + 4] = val;
            break;
         case 1:
            val = val & 0x3;
            asic.sprites_x[id] = (asic.sprites_x[id] & 0x00FF) | (val << 8);
            pbRegisterPage[(addr & 0x3FFF) + 4] = val;
            break;
         case 2:
            asic.sprites_y[id] = ((asic.sprites_y[id] & 0xFF00) | val);
            pbRegisterPage[(addr & 0x3FFF) + 4] = val;
            break;
         case 3:
            val = val & 0x1;
            asic.sprites_y[id] = ((asic.sprites_y[id] & 0x00FF) | (val << 8));
            pbRegisterPage[(addr & 0x3FFF) + 4] = val;
            break;
         case 4:
            asic.sprites_mag_x[id] = decode_magnification(val >> 2);
            asic.sprites_mag_y[id] = decode_magnification(val);
            return false; /* write-only */
         default:
            break;
      }
   }

   /* Enhanced palette: 0x6400-0x643F */
   if (addr >= 0x6400 && addr < 0x6440) {
      int colour = (addr & 0x3F) >> 1;
      if ((addr % 2) == 1) {
         pbRegisterPage[(addr & 0x3FFF)] = (val & 0x0F);
         /* Allocate dynamic slot on the SECOND byte write only —
          * avoids wasting slots on intermediate states when both
          * bytes are written sequentially. */
         asic_update_colour(colour);
      } else {
         pbRegisterPage[(addr & 0x3FFF)] = val;
         /* For even byte, just update the cached RGB value without
          * allocating a new dynamic slot. If the game only writes
          * the even byte, the color changes on next frame's base. */
         byte even = pbRegisterPage[0x2400 + colour * 2];
         byte odd  = pbRegisterPage[0x2400 + colour * 2 + 1];
         unsigned int r = (even >> 4) & 0x0F;
         unsigned int b = even & 0x0F;
         unsigned int g = odd & 0x0F;
         asic_rgb[colour] = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
      }
      return false;
   }

   /* Scanline events: 0x6800-0x6805 */
   if (addr >= 0x6800 && addr < 0x6806) {
      if (addr == 0x6800) {
         /* Programmable Raster Interrupt scan line */
         CRTC.interrupt_sl = val;
      } else if (addr == 0x6801) {
         /* Screen Split Scan Line */
         CRTC.split_sl = val;
      } else if (addr == 0x6802) {
         CRTC.split_addr &= 0x00FF;
         CRTC.split_addr |= (val << 8);
      } else if (addr == 0x6803) {
         CRTC.split_addr &= 0x3F00;
         CRTC.split_addr |= val;
      } else if (addr == 0x6804) {
         /* Soft Scroll Control Register */
         asic.hscroll = (val & 0xf);
         asic.vscroll = ((val >> 4) & 0x7);
         asic.extend_border = (val >> 7);
         update_skew();
      } else if (addr == 0x6805) {
         /* Interrupt Vector Register */
         asic.interrupt_vector = val & 0xF8;
      }
   }

   /* Analog inputs: 0x6808-0x680F (ignore) */
   if (addr >= 0x6808 && addr < 0x6810) {
      return true;
   }

   /* DMA channel source addresses: 0x6C00-0x6C0B */
   if (addr >= 0x6C00 && addr < 0x6C0B) {
      int c = ((addr & 0xc) >> 2);
      dma_channel *channel = &asic.dma.ch[c];
      switch (addr & 0x3) {
         case 0:
            channel->source_address &= 0xFF00;
            channel->source_address |= (val & 0xFE); /* word-aligned */
            break;
         case 1:
            channel->source_address &= 0x00FF;
            channel->source_address |= (val << 8);
            break;
         case 2:
            channel->prescaler = val;
            break;
         default:
            break;
      }
   }

   /* DMA control/status register: 0x6C0F */
   if (addr == 0x6C0F) {
      for (int c = 0; c < NB_DMA_CHANNELS; c++) {
         asic.dma.ch[c].enabled = (val & (0x1 << c));
      }
   }

   return true;
}

/* ------------------------------------------------------------------ */
/* Sprite rendering                                                   */
/* Draws sprites over the completed framebuffer.                      */
/* Frank-cpc framebuffer is 320x240 8bpp with palette indices.        */
/* Sprite coordinates are in CPC pixel space (relative to border).    */
/* ------------------------------------------------------------------ */
void asic_draw_sprites() {
   if (CPC.model <= 2) return;
   if (asic.locked) return;
   if (!scanline_render_target) return;

   /* CPC Plus sprite coordinate system:
    * Sprite Y is relative to CRTC sl_count=0 (frame restart).
    * crtc_sl0_scrln records VDU.scrln at that moment.
    * fb row = (sprite_y + crtc_sl0_scrln) - fb_y_start
    *
    * Sprite X=0 is the left edge of the active display.
    * scanline_complete() copies from scanline_buf + crtc_active_display_offset,
    * so fb column 0 = active display start. No X adjustment needed. */
   const int fb_w = 320;
   const int fb_h = 240;
   const int y_offset = crtc_sl0_scrln - fb_y_start;

   for (int i = 15; i >= 0; i--) {
      int sx = asic.sprites_x[i];
      int mx = asic.sprites_mag_x[i];
      if (mx == 0) continue;

      int sy = asic.sprites_y[i];
      int my = asic.sprites_mag_y[i];
      if (my == 0) continue;

      for (int x = 0; x < 16; x++) {
         /* Early column clip in CPC coords */
         int cpc_x_base = sx + x * mx;
         if (cpc_x_base + mx <= 0) continue;
         if (cpc_x_base / 2 >= fb_w) break;

         for (int y = 0; y < 16; y++) {
            byte p = asic.sprites[i][x][y];
            if (p == 0) continue; /* transparent */

            for (int dy = 0; dy < my; dy++) {
               int py = sy + y * my + dy + y_offset;
               if (py < 0) continue;
               if (py >= fb_h) break;

               for (int dx = 0; dx < mx; dx++) {
                  int px = (cpc_x_base + dx) / 2;
                  if (px < 0) continue;
                  if (px >= fb_w) break;

                  scanline_render_target[py * scanline_render_stride + px] = p;
               }
            }
         }
      }
   }
}
