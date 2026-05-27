/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2005 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

// Ported for frank-cpc RP2350 build — emulation core only, SDL/GUI stripped.

#include <cstring>
#include <cstdio>
#include "cap32.h"
#include "crtc.h"
#include "z80.h"
#include "asic.h"

extern t_z80regs z80;
extern t_CPC CPC;
extern t_CRTC CRTC;
extern t_FDC FDC;
extern t_GateArray GateArray;
extern t_PPI PPI;
extern t_PSG PSG;
extern t_VDU VDU;

extern byte *membank_read[4];
extern byte *membank_write[4];
extern byte *membank_read_fast[4];
extern byte ram_shadow[];
extern void z80_invalidate_read_cache();

extern byte *pbRAM;
extern byte *pbROM;
extern byte *pbROMlo;
extern byte *pbROMhi;
extern byte *pbExpansionROM;
extern byte *memmap_ROM[256];

extern byte keyboard_matrix[16];

extern byte bTapeLevel;
extern dword freq_table[];

extern byte *pbSndBuffer;
extern byte *pbSndBufferEnd;

extern t_flags1 flags1;
extern t_new_dt new_dt;

void SetAYRegister(int Num, byte Value);
void ResetAYChipEmulation();
void InitAY();
byte fdc_read_status();
byte fdc_read_data();
void fdc_write_data(byte val);
void crtc_update_palette_cache();

extern "C" unsigned char *cpc_cartridge_get_page(int page);

t_MemBankConfig membank_config{};

#define MAX_FREQ_ENTRIES 6

namespace {

constexpr byte bit_values[8] = {
   0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
};

constexpr int kCmdPhase = 0;
constexpr int kStatusDrvAFlag = 128;
constexpr int kStatusDrvBFlag = 256;

void psg_write(byte psg_data)
{
   byte control = PSG.control & 0xc0;
   if (control == 0xc0) {
      PSG.reg_select = psg_data;
   }
   else if (control == 0x80) {
      if (PSG.reg_select < 16) {
         SetAYRegister(PSG.reg_select, psg_data);
      }
   }
}

void set_upper_rom()
{
   byte *upper_rom = memmap_ROM[GateArray.upper_ROM];
   if (upper_rom != nullptr) {
      pbExpansionROM = upper_rom;
   }
   else {
      pbExpansionROM = pbROMhi; // empty bank reverts to BASIC ROM
   }
}

} // namespace

dword freq_table[MAX_FREQ_ENTRIES] = {
   11025,
   22050,
   44100,
   48000,
   96000,
   32000
};

void ga_init_banking(t_MemBankConfig& banking, unsigned char RAM_bank)
{
   if (pbRAM == nullptr) {
      std::memset(banking, 0, sizeof(banking));
      return;
   }

   byte *romb0 = pbRAM;
   byte *romb1 = pbRAM + (1 * 16384);
   byte *romb2 = pbRAM + (2 * 16384);
   byte *romb3 = pbRAM + (3 * 16384);

   byte *pbRAMbank = pbRAM + ((RAM_bank + 1) * 65536);
   byte *romb4 = pbRAMbank;
   byte *romb5 = pbRAMbank + (1 * 16384);
   byte *romb6 = pbRAMbank + (2 * 16384);
   byte *romb7 = pbRAMbank + (3 * 16384);

   banking[0][0] = romb0;
   banking[0][1] = romb1;
   banking[0][2] = romb2;
   banking[0][3] = romb3;

   banking[1][0] = romb0;
   banking[1][1] = romb1;
   banking[1][2] = romb2;
   banking[1][3] = romb7;

   banking[2][0] = romb4;
   banking[2][1] = romb5;
   banking[2][2] = romb6;
   banking[2][3] = romb7;

   banking[3][0] = romb0;
   banking[3][1] = romb3;
   banking[3][2] = romb2;
   banking[3][3] = romb7;

   banking[4][0] = romb0;
   banking[4][1] = romb4;
   banking[4][2] = romb2;
   banking[4][3] = romb3;

   banking[5][0] = romb0;
   banking[5][1] = romb5;
   banking[5][2] = romb2;
   banking[5][3] = romb3;

   banking[6][0] = romb0;
   banking[6][1] = romb6;
   banking[6][2] = romb2;
   banking[6][3] = romb3;

   banking[7][0] = romb0;
   banking[7][1] = romb7;
   banking[7][2] = romb2;
   banking[7][3] = romb3;
}

/* Update membank_read_fast[n] to always point to SRAM.
 * For RAM banks in lower 64KB, point to ram_shadow.
 * For ROM or extended RAM, use the existing pointer (already SRAM or PSRAM). */
static void update_fast_banks() {
   for (int n = 0; n < 4; ++n) {
      byte *bank = membank_read[n];
      if (pbRAM && bank >= pbRAM && bank < pbRAM + 65536) {
         membank_read_fast[n] = ram_shadow + (bank - pbRAM);
      } else {
         membank_read_fast[n] = bank;
      }
   }
}

void ga_memory_manager()
{
   dword mem_bank = 0;

   if (CPC.ram_size <= 64) {
      GateArray.RAM_config = 0;
      GateArray.RAM_bank = 0;
   }
   else {
      dword requested_bank = GateArray.RAM_bank;
      dword encoded_bank = (GateArray.RAM_config >> 3) & 7;
      dword max_bank = (CPC.ram_size / 64) - 2;

      if (encoded_bank <= max_bank) {
         requested_bank = encoded_bank;
      }
      if (requested_bank > max_bank) {
         requested_bank = 0;
      }

      mem_bank = requested_bank;
      GateArray.RAM_bank = static_cast<byte>(mem_bank);
   }

   ga_init_banking(membank_config, GateArray.RAM_bank);

   for (int n = 0; n < 4; ++n) {
      membank_read[n] = membank_config[GateArray.RAM_config & 7][n];
      membank_write[n] = membank_config[GateArray.RAM_config & 7][n];
   }

   if ((GateArray.ROM_config & 0x04) == 0 && pbROMlo != nullptr) {
      membank_read[GateArray.lower_ROM_bank] = pbROMlo;
   }

   /* CPC Plus: map ASIC register page at 0x4000-0x7FFF */
   if (CPC.model > 2 && GateArray.registerPageOn && pbRegisterPage) {
      membank_read[1] = pbRegisterPage;
      membank_write[1] = pbRegisterPage;
   }

   if ((GateArray.ROM_config & 0x08) == 0) {
      set_upper_rom();
      if (pbExpansionROM != nullptr) {
         membank_read[3] = pbExpansionROM;
      }
   }
   update_fast_banks();
   z80_invalidate_read_cache();
}

__attribute__((section(".time_critical.cap32"))) byte z80_IN_handler(reg_pair port)
{
   byte ret_val = 0xff;

   if ((port.b.h & 0x40) == 0) {
      if ((port.b.h & 3) == 3) {
         if ((CRTC.reg_select > 11) && (CRTC.reg_select < 18)) {
            ret_val = CRTC.registers[CRTC.reg_select];
         }
         else {
            ret_val = 0;
         }
      }
      return ret_val;
   }

   if ((port.b.h & 0x08) == 0) {
      switch (port.b.h & 3) {
         case 0:
            if (PPI.control & 0x10) {
               if ((PSG.control & 0xc0) == 0x40) {
                  if (PSG.reg_select < 16) {
                     if (PSG.reg_select == 14) {
                        byte keyboard_line = static_cast<byte>(CPC.keyboard_line & 0x0f);
                        if ((PSG.RegisterAY.Index[7] & 0x40) == 0) {
                           ret_val = keyboard_matrix[keyboard_line];
                        }
                        else {
                           ret_val = PSG.RegisterAY.Index[14] & keyboard_matrix[keyboard_line];
                        }
                     }
                     else if (PSG.reg_select == 15) {
                        if (PSG.RegisterAY.Index[7] & 0x80) {
                           ret_val = PSG.RegisterAY.Index[15];
                        }
                     }
                     else {
                        ret_val = PSG.RegisterAY.Index[PSG.reg_select];
                     }
                  }
               }
            }
            else {
               ret_val = PPI.portA;
            }
            break;

         case 1:
            if (PPI.control & 0x02) {
               ret_val = (CPC.jumpers & 0x7f) | (CRTC.flag_invsync ? 0x01 : 0x00);
               if (bTapeLevel) {
                  ret_val |= 0x80;
               }
            }
            else {
               ret_val = PPI.portB;
            }
            break;

         case 2:
         {
            byte direction = PPI.control & 9;
            ret_val = PPI.portC;
            if (direction) {
               if (direction & 8) {
                  ret_val &= 0x0f;
                  byte val = PPI.portC & 0xc0;
                  if (val == 0xc0) {
                     val = 0x80;
                  }
                  ret_val |= val | 0x20;
                  if (CPC.tape_motor) {
                     ret_val |= 0x10;
                  }
               }
               if ((direction & 1) == 0) {
                  ret_val |= 0x0f;
               }
            }
            break;
         }
      }
      return ret_val;
   }

   if ((port.b.h == 0xfb) && ((port.b.l & 0x80) == 0)) {
      if ((port.b.l & 0x01) == 0) {
         return fdc_read_status();
      }
      return fdc_read_data();
   }

   return ret_val;
}

__attribute__((section(".time_critical.cap32"))) void z80_OUT_handler(reg_pair port, byte val)
{
   if ((port.b.h & 0xc0) == 0x40) {
      switch (val >> 6) {
         case 0:
            GateArray.pen = (val & 0x10) ? 0x10 : (val & 0x0f);
            break;

         case 1:
            GateArray.ink_values[GateArray.pen] = val & 0x1f;
            if (CPC.model > 2) {
               /* CPC Plus: identity mapping — ASIC palette is programmed
                * directly via register page writes. */
               GateArray.palette[GateArray.pen] = GateArray.pen;
            } else {
               GateArray.palette[GateArray.pen] = GateArray.ink_values[GateArray.pen];
            }
            GateArray.palette[33] = GateArray.palette[1];
            crtc_update_palette_cache();
            break;

         case 2:
            if (CPC.model > 2 && !asic.locked && (val & 0x20)) {
               /* CPC Plus RMR2 register */
               int membank = (val >> 3) & 3;
               if (membank == 3) {
                  GateArray.registerPageOn = true;
                  membank = 0;
               } else {
                  GateArray.registerPageOn = false;
               }
               int page = (val & 0x7);
               GateArray.lower_ROM_bank = membank;
               byte *cpr_page = cpc_cartridge_get_page(page);
               if (cpr_page) {
                  pbROMlo = cpr_page;
               }
               ga_memory_manager();
            } else {
               GateArray.ROM_config = val;
               GateArray.requested_scr_mode = val & 0x03;
               ga_memory_manager();
               if (val & 0x10) {
                  z80.int_pending = 0;
                  GateArray.sl_count = 0;
               }
            }
            break;

         case 3:
            break;
      }
   }

   if ((port.b.h & 0x80) == 0 && (val & 0xc0) == 0xc0) {
      GateArray.RAM_config = val;
      GateArray.RAM_bank = (val >> 3) & 7;
      ga_memory_manager();
   }

   if ((port.b.h & 0x40) == 0) {
      byte crtc_port = port.b.h & 3;
      if (crtc_port == 0) {
         if (CPC.model > 2) {
            asic_poke_lock_sequence(val);
         }
         CRTC.reg_select = val;
      }
      else if (crtc_port == 1) {
         if (CRTC.reg_select < 16) {
            switch (CRTC.reg_select) {
               case 0:
                  CRTC.registers[0] = val;
                  crtc_recompute_next_event();
                  break;

               case 1:
                  CRTC.registers[1] = val;
                  update_skew();
                  crtc_recompute_next_event();
                  break;

               case 2:
                  CRTC.registers[2] = val;
                  crtc_recompute_next_event();
                  break;

               case 3:
                  CRTC.registers[3] = val;
                  CRTC.hsw = val & 0x0f;
                  CRTC.vsw = val >> 4;
                  break;

               case 4:
                  CRTC.registers[4] = val & 0x7f;
                  if (CRTC.CharInstMR == CharMR2) {
                     if (CRTC.line_count == CRTC.registers[4]) {
                        if (CRTC.raster_count == CRTC.registers[9]) {
                           CRTC.flag_startvta = 1;
                        }
                     }
                  }
                  break;

               case 5:
                  CRTC.registers[5] = val & 0x1f;
                  break;

               case 6:
                  CRTC.registers[6] = val & 0x7f;
                  if (CRTC.line_count == CRTC.registers[6]) {
                     new_dt.NewDISPTIMG = 0;
                  }
                  break;

               case 7:
                  CRTC.registers[7] = val & 0x7f;
                  {
                     dword temp = 0;
                     if (CRTC.line_count == CRTC.registers[7]) {
                        temp++;
                        if (CRTC.r7match != temp) {
                           CRTC.r7match = temp;
                           if (CRTC.char_count >= 2) {
                              CRTC.flag_resvsync = 0;
                              if (!CRTC.flag_invsync) {
                                 CRTC.vsw_count = 0;
                                 CRTC.flag_invsync = 1;
                                 flags1.monVSYNC = 26;
                                 GateArray.hs_count = 2;
                              }
                           }
                        }
                     }
                     else {
                        CRTC.r7match = 0;
                     }
                  }
                  break;

               case 8:
                  CRTC.registers[8] = val;
                  update_skew();
                  break;

               case 9:
                  CRTC.registers[9] = val & 0x1f;
                  {
                     dword temp = 0;
                     if (CRTC.raster_count == CRTC.registers[9]) {
                        temp = 1;
                        CRTC.flag_resscan = 1;
                     }
                     if (CRTC.r9match != temp) {
                        CRTC.r9match = temp;
                        if (temp) {
                           CRTC.CharInstMR = CharMR1;
                           CRTC.charInstMR_state = 1;
                        }
                     }
                     if (CRTC.raster_count == CRTC.registers[9]) {
                        if (CRTC.char_count == CRTC.registers[1]) {
                           CRTC.next_addr = CRTC.addr + CRTC.char_count;
                        }
                        if (CRTC.char_count == CRTC.registers[0]) {
                           CRTC.flag_reschar = 1;
                        }
                        if (!CRTC.flag_startvta) {
                           CRTC.flag_resscan = 1;
                        }
                     }
                     else {
                        if (!CRTC.flag_invta) {
                           CRTC.flag_resscan = 0;
                        }
                     }
                  }
                  break;

               case 10:
                  CRTC.registers[10] = val & 0x7f;
                  break;

               case 11:
                  CRTC.registers[11] = val & 0x1f;
                  break;

               case 12:
                  CRTC.registers[12] = val & 0x3f;
                  CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
                  break;

               case 13:
                  CRTC.registers[13] = val;
                  CRTC.requested_addr = CRTC.registers[13] + (CRTC.registers[12] << 8);
                  break;

               case 14:
                  CRTC.registers[14] = val & 0x3f;
                  break;

               case 15:
                  CRTC.registers[15] = val;
                  break;
            }
         }
      }
   }

   if ((port.b.h & 0x20) == 0) {
      if (CPC.model > 2) {
         /* CPC Plus ROM select: page from cartridge */
         unsigned int page = 1; /* default to BASIC page */
         if (val == 7) {
            page = 3;
         } else if (val >= 128) {
            page = val & 31;
         }
         GateArray.upper_ROM = page;
         byte *cpr_page = cpc_cartridge_get_page(page);
         if (cpr_page) {
            pbExpansionROM = cpr_page;
         } else {
            pbExpansionROM = pbROMhi;
         }
      } else {
         GateArray.upper_ROM = val;
         set_upper_rom();
      }
      if ((GateArray.ROM_config & 0x08) == 0 && pbExpansionROM != nullptr) {
         membank_read[3] = pbExpansionROM;
      }
      z80_invalidate_read_cache();
   }

   if ((port.b.h & 0x08) == 0) {
      switch (port.b.h & 3) {
         case 0:
            PPI.portA = val;
            if ((PPI.control & 0x10) == 0) {
               psg_write(val);
            }
            break;

         case 1:
            PPI.portB = val;
            break;

         case 2:
            PPI.portC = val;
            if ((PPI.control & 1) == 0) {
               CPC.keyboard_line = val;
            }
            if ((PPI.control & 8) == 0) {
               CPC.tape_motor = val & 0x10;
               PSG.control = val;
               psg_write(PPI.portA);
            }
            break;

         case 3:
            if (val & 0x80) {
               PPI.control = val;
               PPI.portA = 0;
               PPI.portB = 0;
               PPI.portC = 0;
            }
            else {
               byte bit = (val >> 1) & 7;
               if (val & 1) {
                  PPI.portC |= bit_values[bit];
               }
               else {
                  PPI.portC &= ~bit_values[bit];
               }
               if ((PPI.control & 1) == 0) {
                  CPC.keyboard_line = PPI.portC;
               }
               if ((PPI.control & 8) == 0) {
                  CPC.tape_motor = PPI.portC & 0x10;
                  PSG.control = PPI.portC;
                  psg_write(PPI.portA);
               }
            }
            break;
      }
   }

   if ((port.b.h == 0xfa) && ((port.b.l & 0x80) == 0)) {
      FDC.motor = val & 0x01;
      FDC.flags |= kStatusDrvAFlag | kStatusDrvBFlag;
   }
   else if ((port.b.h == 0xfb) && ((port.b.l & 0x80) == 0)) {
      fdc_write_data(val);
   }
}

void emulator_reset()
{
   if (pbROMlo == nullptr && pbROM != nullptr) {
      pbROMlo = pbROM;
   }
   if (pbROMhi == nullptr && pbROM != nullptr) {
      pbROMhi = pbROM + 16384;
   }
   if (pbExpansionROM == nullptr) {
      pbExpansionROM = pbROMhi;
   }

   z80_reset();
   crtc_reset();

   CPC.cycle_count = static_cast<int>(CPC.speed * FRAME_PERIOD_MS * 1000.0);
   CPC.keyboard_line = 0;
   CPC.tape_motor = 0;
   CPC.tape_play_button = 0;
   CPC.printer_port = 0xff;

   std::memset(keyboard_matrix, 0xff, sizeof(keyboard_matrix));
   std::memset(&VDU, 0, sizeof(VDU));
   VDU.flag_drawing = 1;

   std::memset(&GateArray, 0, sizeof(GateArray));
   GateArray.scr_mode = 1;
   GateArray.requested_scr_mode = 1;
   GateArray.lower_ROM_bank = 0;
   GateArray.registerPageOn = false;

   std::memset(&PPI, 0, sizeof(PPI));
   std::memset(&PSG, 0, sizeof(PSG));
   ResetAYChipEmulation();
   InitAY(); // re-initialize PSG Synthesizer function pointer

   std::memset(&FDC, 0, sizeof(FDC));
   FDC.phase = kCmdPhase;
   FDC.flags = kStatusDrvAFlag | kStatusDrvBFlag;

   if (pbRAM != nullptr && CPC.ram_size != 0) {
      std::memset(pbRAM, 0, CPC.ram_size * 1024);
   }

   ga_init_banking(membank_config, GateArray.RAM_bank);
   video_set_palette();
   ga_memory_manager();
   asic_reset();
}

int emulator_init()
{
   if (CPC.model > 3) {
      CPC.model = 2;
   }
   if (CPC.ram_size == 0) {
      CPC.ram_size = 128;
   }
   if (CPC.model == 2 && CPC.ram_size < 128) {
      CPC.ram_size = 128;
   }
   if (CPC.speed < MIN_SPEED_SETTING || CPC.speed > MAX_SPEED_SETTING) {
      CPC.speed = DEF_SPEED_SETTING;
   }
   if (CPC.jumpers == 0) {
      CPC.jumpers = 0x1e;
   }
   if (CPC.snd_playback_rate >= MAX_FREQ_ENTRIES) {
      CPC.snd_playback_rate = 2;
   }
   if (CPC.snd_volume > 100) {
      CPC.snd_volume = 80;
   }

   if (pbROM != nullptr) {
      pbROMlo = pbROM;
      if (pbROMhi == nullptr) {
         pbROMhi = pbROM + 16384;
      }
   }
   if (pbExpansionROM == nullptr) {
      pbExpansionROM = pbROMhi;
   }
   if (memmap_ROM[0] == nullptr) {
      memmap_ROM[0] = pbROMhi;
   }

   ga_init_banking(membank_config, GateArray.RAM_bank);
   video_set_palette();
   update_cpc_speed();
   crtc_init();
   z80_init_tables();
   return 0;
}

int video_set_palette()
{
   for (int n = 0; n < 34; ++n) {
      GateArray.palette[n] = 0;
   }

   if (CPC.model > 2) {
      /* CPC Plus: identity palette mapping.
       * ASIC palette entries are programmed directly via graphics_set_palette().
       * GateArray.palette[ink] = ink so the renderer outputs the ink number
       * as the framebuffer pixel value. */
      for (int n = 0; n < 33; ++n) {
         GateArray.palette[n] = n;
      }
   } else {
      for (int ink = 0; ink < 17; ++ink) {
         GateArray.palette[ink] = GateArray.ink_values[ink] & 0x1f;
      }
   }

   GateArray.palette[33] = GateArray.palette[1];
   crtc_update_palette_cache();
   return 0;
}

void update_timings()
{
   CPC.cycle_count = static_cast<int>(CPC.speed * FRAME_PERIOD_MS * 1000.0);
}

void update_cpc_speed()
{
   update_timings();
   InitAY();
}
