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

#include "cpc.h"
#include "Z80.h"
#include "mem.h"
#include "screen.h"
#include "colors.h"
#include "keyboard.h"
#include "defines.h"
#include "disc.h"
#include "printer.h"
#include "aysound.h"
#include "tape.h"
#include "tape_rec.h"
#include <stdio.h>

byte Pio_A, Pio_B, Pio_C;
byte AY_num_reg;
byte PioStatus;
word AlterScrOffs;
byte Customer;

/* Tape timing: track how many T-states have been fed to the tape engine
 * within the current CRTC interrupt period so we can advance the tape
 * incrementally on each Port B read rather than in one bulk call. */
int tape_tstates_fed;

#ifdef PICO_BUILD
/* Display mode tracks the game's intended screen mode, filtering out
 * writes from the CPC firmware interrupt handler.  The firmware handler
 * (at RAM 0xB900-0xBA84 and ROM 0x0000-0x0043) writes its own tracked
 * mode on every interrupt, overwriting what the game set.  DisplayMode
 * is used for rendering; ScreenMode stays as the raw hardware register. */
unsigned int DisplayMode = 1;
#endif

/*********************************************************************************/

void InitIO (void) {
  Pio_B = 176 | Customer;
}

/*********************************************************************************/
/*** The main out handler:
/*** Filters the port adress bit by bit and do a IO action
/*********************************************************************************/
void OutZ80 (register word Port, register byte Value) {
  static word i, ok;
  int j;
  char c;

  ok = FALSE;
  /* 0x7Fxx: Gate-Array: ROM's, RAM, BORDER, INK's */
  if ((Port & 0x8000)==0x0000) {
    switch (Value & 0xC0) {
      case 0x80:
        UpperBlockIsRAM = Value & 8;
        LowerBlockIsRAM = Value & 4;
        ScreenMode = Value & 3;
#ifdef PICO_BUILD
        /* Track when the game has taken over from the firmware.
         * Once we see an MRER write with both ROMs disabled from a
         * non-firmware PC, we know the game owns the interrupt handler
         * and all subsequent MRER writes should update DisplayMode. */
        {
          word pc = cpu.PC.W;
          static int game_took_over = 0;
          if (!game_took_over && (Value & 0x0C) == 0x0C &&
              pc > 0x0043 && (pc < 0xB900 || pc > 0xBA84)) {
            game_took_over = 1;
          }
          int is_fw = !game_took_over &&
                      ((pc >= 0xB900 && pc <= 0xBA84) || pc <= 0x0043);
          if (!is_fw) {
            DisplayMode = Value & 3;
            pico_record_mode_event((uint8_t)(Value & 3));
          }
        }
#endif
        /* MRER bit 4: reset Gate Array IRQ timer (used by raster games). */
        if (Value & 0x10) irq_reset_pending = 1;
        ok = TRUE;
        break;

      case 0xC0:
        /* RAM-Verwaltung */
        SelectRamBank (Value & 0x3F);
        ok = TRUE;
        break;

      case 0x00:
        if (Value & 0x10)
          InkNum = 16;                 // Border
        else
          InkNum = Value & 0x0F;       // Color-Register
        ok = TRUE;
        break;

      case 0x40:
        Ink [InkNum] = (Value & 0x1F) + MonoScreen;
#ifdef PICO_BUILD
        pico_record_ink_event((uint8_t)InkNum, Ink[InkNum]);
#endif
        ok = TRUE;
        break;
    }
  }

  /* 0xDFxx: ROM number */
  if ((Port & 0x2000)==0x0000) {
    // printf ("ROM-No: %3i\n", Value);
    ROMNumber = Value;
    ok = TRUE;
  }

  /* 0xBCxx: Register adress of HD6845 */
  if ((Port & 0x4300)==0x0000) {
    HD6845RegisterPointer = Value;
    ok = TRUE;
  }

  /* 0xBDxx: Register data of HD6845 */
  if ((Port & 0x4300)==0x0100) {
    HD6845Register [HD6845RegisterPointer] = Value;
    if (HD6845RegisterPointer==12 || HD6845RegisterPointer==13) {
      ScreenAddr = (HD6845Register[12]<<8) + HD6845Register[13];
      ScreenOffset = (ScreenAddr & 1023)<<1;
      LineOffset= (((ScreenAddr & 1023)/40)<<4);
      ScreenBlock = (ScreenAddr<<2) & 0xC000;
      ScreenBank = ScreenBlock >> 14;
#ifdef PICO_BUILD
      /* Record mid-frame CRTC address change for per-row rendering.
       * Skip immediate redraw — frame-end handles splits correctly. */
      pico_record_crtc_event();
#else
      if (ScreenOffset == (((2048 | AlterScrOffs) - 80) & 0x7FF))
        RedrawFirstLine ();
      else
        RedrawLastLine ();
#endif
    }
    /* Force a full redraw when the display layout changes (overscan). */
    if (HD6845RegisterPointer == 1 || HD6845RegisterPointer == 6)
      ChangeInk = TRUE;
    AlterScrOffs = ScreenOffset;
    ok = TRUE;
  }

  /* 0xFAxx: Floppy motor */
  if ((Port & 0x0581) == 0x0000) {
    FloppyMotor = Value & 1;
    ok = TRUE;
  }

  /* 0xFBxx: FDC register */
  if ((Port & 0x0581) == 0x0101) {
    WriteFDCData(Value);
    ok = TRUE;
  }

  /* illegal FDC register access */
  if (Port == 0xFC7F) {
    WriteFDCData(Value);
    ok = TRUE;
  }

  /* 0xF4xx: 8255 PIO A (to sound chip) */
  if ((Port & 0x0B00)==0x0000) {
    //fprintf(debug_snd, "\n0xF4xx=%d\n", Value);
    Pio_A = Value;
    ok = TRUE;
  }

  /* 0xF6xx: 8255 PIO C, select keyboard row */
  if ((Port & 0x0B00)==0x0200) {
    byte old_c = Pio_C;
    KeyRow = Value & 15;
    Pio_C = Value;

    /* Cassette motor control: bit 4 of Port C */
    tape_set_motor((Value >> 4) & 1);

    /* Cassette write recording: bit 5 */
    if (tape_rec_active() && ((old_c ^ Pio_C) & 0x20)) {
      extern int IRQCount, CPUZyklenBisInt;
      unsigned long ts = (unsigned long)IRQCount * (unsigned long)CPUZyklenBisInt
                       + (unsigned long)(CPUZyklenBisInt - cpu.ICount);
      tape_rec_write_bit((Pio_C >> 5) & 1, ts);
    }

    switch (Value & 0xC0) {
      case 0xC0:   /* AY register SELECTION */
            AY_num_reg = Pio_A;
            break;
      case 0x80:   /* AY register WRITE */
            AYRegister[AY_num_reg] = Pio_A;
            break;
      case 0x40:   /* AY register READ */
            break;
      case 0x00:   /* AY register INACTIVE mode */
            break;
       }
    ok = TRUE;
  }

  /* 0xF7xx: 8255 PIO control register   */
  if ((Port & 0x0B00)==0x0300) {
    if (Value & 0x80) {
      /* Mode set — store control word */
      PioStatus = Value;
    } else {
      /* Bit Set/Reset (BSR) mode for Port C.
       * Bits 3-1 = bit number, bit 0 = set(1)/reset(0). */
      int bit = (Value >> 1) & 7;
      byte old_c = Pio_C;
      if (Value & 1)
        Pio_C |=  (1 << bit);
      else
        Pio_C &= ~(1 << bit);

      /* Update keyboard row from lower nibble */
      KeyRow = Pio_C & 15;

      /* Cassette motor control: bit 4 of Port C */
      tape_set_motor((Pio_C >> 4) & 1);

      /* Cassette write recording: bit 5 */
      if (tape_rec_active() && ((old_c ^ Pio_C) & 0x20)) {
        extern int IRQCount, CPUZyklenBisInt;
        unsigned long ts = (unsigned long)IRQCount * (unsigned long)CPUZyklenBisInt
                         + (unsigned long)(CPUZyklenBisInt - cpu.ICount);
        tape_rec_write_bit((Pio_C >> 5) & 1, ts);
      }
    }
    ok = TRUE;
  }

  /* 0xEF00: Printer port */
  if ((Port &0x1000)==0x0000) {
    ok = TRUE;
    /* Print only if STROBE is low. Use bit 5 of PIO C as printer bit 7 */
    if (Value & 0x80) PrintChar ((Value & 0x7F) + ((Pio_C & 32)<<2));
  }

  /* 0xF8FF: serial port (not supported now) */
  if ((Port &0x0700)==0x0000)
    ok = TRUE;

  if (ok==FALSE && !NoDebug)
    fprintf (DebugFP, "CPC4X - unknown write IO adress %05Xh \n", Port);

}

/*********************************************************************************/

byte InZ80 (register word Port) {

  /* 0xBFxx: Register data of HD6845 */
  if ((Port & 0x4300)==0x0300) return HD6845Register [HD6845RegisterPointer];

  /* 0xBExx: Register adress of HD6845 */
  //if ((Port & 0x4300)==0x0200) return 255;

  /* 0xFB7E: FDC status register */
  if ((Port & 0x0581) == 0x0100) return ReadFDCStatus();
  /* 0xFB7F: FDC data register */
  if ((Port & 0x0581) == 0x0101) return ReadFDCData();

  /* 0xF4xx: 8255 PIO A (from sound chip) */
  if ((Port & 0x0B00)==0x0000) return Keyport[KeyRow];

  /* 0xF5xx: 8255 PIO B */
  if ((Port & 0x0B00)==0x0100) {
    /* Bit 7 = cassette data input (active-high when signal present) */
    byte b = Pio_B;
    if (tape_is_loaded() && tape_get_motor()) {
      /* Advance tape to current T-state position so the signal is fresh. */
      extern int CPUZyklenBisInt;
      int elapsed = CPUZyklenBisInt - cpu.ICount;
      int delta = elapsed - tape_tstates_fed;
      if (delta > 0) {
        tape_main(delta);
        tape_tstates_fed = elapsed;
      }
      b = (b & 0x7F) | (tape_get_status() ? 0x80 : 0x00);
    }
    return b;
  }

  /* 0xF5xx: 8255 PIO C */
  if ((Port & 0x0B00)==0x0200) return Pio_C;

  /* 0xF7xx: 8255 PIO Status */
  if ((Port & 0x0B00)==0x0300) return PioStatus;

  if (!NoDebug) fprintf (DebugFP, "CPC4X - unknown read IO adress %05Xh \n", Port);
  return 255;
}

/*********************************************************************************/
