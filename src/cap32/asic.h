/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2005 Ulrich Doewich

   CPC Plus ASIC emulation.
   The ASIC replaces the Gate Array in CPC Plus/GX4000 and adds:
   - 16 hardware sprites (16x16, 4-bit color, magnification)
   - 4096-color enhanced palette (12-bit RGB)
   - 3 DMA sound channels (programmed via RAM)
   - Programmable Raster Interrupt
   - Screen split (change CRTC address at scanline)
   - Soft scroll (pixel-level H/V scrolling)

   Ported for frank-cpc RP2350 build.
*/

#ifndef ASIC_H
#define ASIC_H

#include "cap32.h"

#define NB_DMA_CHANNELS 3

struct dma_channel {
   unsigned int source_address;
   unsigned int loop_address;
   byte prescaler;
   bool enabled;
   bool interrupt;
   int pause_ticks;
   byte tick_cycles;
   int loops;
};

struct dma_t {
   dma_channel ch[NB_DMA_CHANNELS];
};

struct asic_t {
   bool locked;
   int lockSeqPos;

   bool extend_border;
   unsigned int hscroll;
   unsigned int vscroll;
   byte sprites[16][16][16];
   int16_t sprites_x[16];
   int16_t sprites_y[16];
   short int sprites_mag_x[16];
   short int sprites_mag_y[16];

   bool raster_interrupt;
   byte interrupt_vector;

   dma_t dma;
};

extern asic_t asic;
extern byte *pbRegisterPage;

void asic_reset();
void asic_poke_lock_sequence(byte val);
void asic_dma_cycle();
bool asic_register_page_write(word addr, byte val);
void asic_draw_sprites();

#endif
