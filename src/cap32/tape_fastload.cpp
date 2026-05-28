/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * tape_fastload.cpp — Fast-load pattern detection for tape loading.
 *
 * Detects when the Z80 is executing known tape-reading firmware routines
 * and bypasses most hardware timing by advancing the CDT/CAS tape state
 * directly.
 */

#include "tape_fastload.h"

#include "Pico/cpc_settings.h"
#include "cap32.h"
#include "tape.h"
#include "z80.h"

extern t_CPC CPC;
extern t_z80regs z80;
extern byte *membank_read[4];
extern int iTapeCycleCount;

namespace {

constexpr word kFirmwareMinPc = 0x2800;
constexpr word kFirmwareMaxPc = 0x2c80;
constexpr int kFastloadPulseBurst = 32;

static inline byte read_z80(word addr)
{
   return membank_read[addr >> 14][addr & 0x3fff];
}

static bool match_signature(word pc, const byte *bytes, const byte *mask, unsigned len)
{
   for (unsigned i = 0; i < len; ++i) {
      if (mask[i] && read_z80(pc + i) != bytes[i]) {
         return false;
      }
   }
   return true;
}

static bool is_amstrad_firmware_loader(word pc)
{
   if (pc < kFirmwareMinPc || pc > kFirmwareMaxPc) {
      return false;
   }

   static const byte entry_bytes[] = {0xcd, 0x00, 0x00, 0xf5, 0x21, 0x00, 0x00, 0x18, 0x19};
   static const byte entry_mask[] =  {0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x01};
   static const byte poll_bytes[] =  {0x06, 0xf4, 0xed, 0x78, 0xe6, 0x04, 0xc8};
   static const byte poll_mask[] =   {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
   static const byte wait_bytes[] =  {0xed, 0x78, 0xad, 0xe6, 0x80, 0x20, 0xf3};
   static const byte wait_mask[] =   {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
   static const byte timing_bytes[] = {0xed, 0x5f, 0xcb, 0x3f, 0x91, 0x30, 0x03, 0x3c, 0x20, 0xfd};
   static const byte timing_mask[] =  {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};

   return match_signature(pc, entry_bytes, entry_mask, sizeof(entry_bytes)) ||
          match_signature(pc, poll_bytes, poll_mask, sizeof(poll_bytes)) ||
          match_signature(pc, wait_bytes, wait_mask, sizeof(wait_bytes)) ||
          match_signature(pc, timing_bytes, timing_mask, sizeof(timing_bytes));
}

static void fast_forward_tape()
{
   for (int i = 0; i < kFastloadPulseBurst && CPC.tape_play_button; ++i) {
      iTapeCycleCount = 0;
      Tape_UpdateLevel();
   }
}

} // namespace

__attribute__((section(".time_critical.z80"))) int tape_try_fastload(void)
{
   if (!g_cpc_settings.fast_tape) {
      return 0;
   }
   if (!CPC.tape_motor || !CPC.tape_play_button) {
      return 0;
   }

   const word pc = z80.PC.w.l;
   if (!is_amstrad_firmware_loader(pc)) {
      return 0;
   }

   fast_forward_tape();
   return 0;
}
