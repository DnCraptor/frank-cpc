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
#include <stdlib.h>
#include <string.h>
#ifndef PICO_BUILD
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include "cpc_compat.h"
#include "ff.h"
#include "psram_allocator.h"
/* Route large CPC allocations (RAM, ROM buffers) to PSRAM. */
#define CPC_LARGE_ALLOC(sz)  psram_malloc(sz)
#endif

#ifndef CPC_LARGE_ALLOC
#define CPC_LARGE_ALLOC(sz)  malloc(sz)
#endif

#include "Z80.h"
#include "defines.h"
#include "cpc.h"
#include "screen.h"

byte SystemROM[16384];
byte *RAM;
byte *UpperROM [8];
byte ROMPresent[256];

unsigned long BankAddr[4];

int ROMNumber;
int LowerBlockIsRAM;
int UpperBlockIsRAM;
int RS_UpBlockRAM;
int RS_LoBlockRAM;

int InitMem (void){
  static int file, length;
  static unsigned long i,j;
  static char filename [128];

  j = CPCMaxMem * 1024;
  if (RAM==NULL)
    RAM = (byte *)CPC_LARGE_ALLOC(j);
  if (RAM == NULL)
    return 0;
  for (i=0; i<=65535; i++)
    RAM[i]=0;

  for (i=0; i<=255; i++)
      ROMPresent[i]=FALSE;

  for (i=1; i<=7; i++) {
    length = strlen (ROMFile[i]);
    if (length>1) {
      if (UpperROM[i]==NULL)
        UpperROM[i] = (byte *)CPC_LARGE_ALLOC(16384);
#ifdef PICO_BUILD
      char romname[80];
      FIL fp;
      UINT bytes_read = 0;
      strncpy(romname, ROMFile[i], sizeof(romname) - 1);
      romname[sizeof(romname) - 1] = 0;
      length = strlen(romname);
      while (length > 0 && romname[length - 1] < 32)
        romname[--length] = 0;
      snprintf(filename, sizeof(filename), "/cpc/rom/%s", romname);
      if (f_open(&fp, filename, FA_READ) == FR_OK) {
        f_read(&fp, UpperROM[i], 16384, &bytes_read);
        if (bytes_read >= 2 && UpperROM[i][1] > 31) {
          f_lseek(&fp, 128);
          f_read(&fp, UpperROM[i], 16384, &bytes_read);
        }
        f_close(&fp);
        ROMPresent[i] = TRUE;
        printf("ROM[%lu] loaded: %s (%u bytes)\n", i, filename, (unsigned)bytes_read);
      } else {
        printf("ROM[%lu] not found: %s\n", i, filename);
      }
#else
      sprintf (filename, "%s/rom/%s", WorkDirectory, ROMFile[i]);
      length = strlen (filename) - 1;
      if (filename[length] < 32)
         filename [length] = 0;
      file = open (filename, O_RDONLY);
      if (file != -1) {
        read(file, UpperROM[i], 16384);
        close (file);
        if (UpperROM[i][1]>31) {
          file = open (filename, O_RDONLY);
          read(file, UpperROM[i], 128);
          read(file, UpperROM[i], 16384);
          close (file);
        }
        ROMPresent[i]=TRUE;
      }
#endif
    }
  }

  if (UpperROM[0]==NULL)
    UpperROM[0] = (byte *)CPC_LARGE_ALLOC(16384);
#ifdef PICO_BUILD
  const char *basic_rom;
  FIL fp2;
  UINT bytes_read_lo = 0, bytes_read_hi = 0;
  switch (CPCtype) {
    case 2  : basic_rom = "/cpc/rom/cpc6128.rom"; break;
    case 1  : basic_rom = "/cpc/rom/cpc664.rom"; break;
    default : basic_rom = "/cpc/rom/cpc464.rom"; CPCtype=0; break;
  }
  memset(SystemROM, 0, sizeof(SystemROM));
  memset(UpperROM[0], 0, 16384);
  if (f_open(&fp2, basic_rom, FA_READ) == FR_OK) {
    f_read(&fp2, SystemROM, 16384, &bytes_read_lo);
    f_read(&fp2, UpperROM[0], 16384, &bytes_read_hi);
    if (bytes_read_hi == 0)
      memcpy(UpperROM[0], SystemROM, 16384);
    f_close(&fp2);
    printf("BASIC ROM loaded: %s (%u/%u bytes)\n", basic_rom,
           (unsigned)bytes_read_lo, (unsigned)bytes_read_hi);
    ROMPresent[0]=TRUE;
    ROMNumber = 0;
    LowerBlockIsRAM = FALSE;
    UpperBlockIsRAM = FALSE;
    ScreenBlock = 0xC000;
    return 1;
  }
  printf("BASIC ROM not found: %s\n", basic_rom);
  return 0;
#else
  switch (CPCtype) {
    case 2  : sprintf (filename, "%s/rom/cpc6128.rom", WorkDirectory); break;
    case 1  : sprintf (filename, "%s/rom/cpc664.rom", WorkDirectory); break;
    default : sprintf (filename, "%s/rom/cpc464.rom", WorkDirectory); CPCtype=0; break;
  }
  file = open (filename, O_RDONLY);
  if (file != -1) {
    read(file, SystemROM, 16384);
    read(file, UpperROM[0], 16384);
    ROMPresent[0]=TRUE;
    close (file);
    ROMNumber = 0;
    LowerBlockIsRAM = FALSE;
    UpperBlockIsRAM = FALSE;
    ScreenBlock = 0xC000;
    return 1;
  }
  else return 0;
#endif
}

void ExitMem (void) {
  int i;
  printf ("Free ROM memory no ");
  for (i=0; i<=7; i++) {
    printf ("%i  ",i);
    free (UpperROM[i]);
  }
  printf (" ..... ok!\nFree RAM memory");
  free (RAM);
  printf (" ..... ok!\n");
}

void SelectRamBank (byte Bank) {
  static int block;
  if (CPCMaxMem==576)
    block = (Bank >> 3) & 7;
  else
    block = 0;
  switch (Bank & 0x07) {
    case 0:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x000000;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x000000;
      break;
    case 1:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x000000;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x010000 + 0x010000 * block;
      break;
    case 2:
      BankAddr[0]=0x010000 + 0x010000 * block;
      BankAddr[1]=0x010000 + 0x010000 * block;
      BankAddr[2]=0x010000 + 0x010000 * block;
      BankAddr[3]=0x010000 + 0x010000 * block;
      break;
    case 3:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x008000;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x010000 + 0x010000 * block;
      break;
    case 4:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x00C000 + 0x010000 * block;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x000000;
      break;
    case 5:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x010000 + 0x010000 * block;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x000000;
      break;
    case 6:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x014000 + 0x010000 * block;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x000000;
      break;
    case 7:
      BankAddr[0]=0x000000;
      BankAddr[1]=0x018000 + 0x010000 * block;
      BankAddr[2]=0x000000;
      BankAddr[3]=0x000000;
      break;
    default: break;
  }
}

void WrZ80 (register word Addr, register byte Value) {
  static byte Bank;
  Bank = (Addr>>14) & 0x0003;
  RAM [BankAddr[Bank] + (unsigned long)Addr] = Value;
  if (((Addr & 0xC000) == ScreenBlock) && (BankAddr[ScreenBank]==0)) WrScreenMem (Addr,Value);
}

byte RdZ80(register word Addr) {
  static byte Bank;
  Bank = Addr>>14;
  if ((Bank==0) && (LowerBlockIsRAM==FALSE))
    return SystemROM [Addr & 0x3FFF];
  if ((Bank==3) && (UpperBlockIsRAM==FALSE)) {
    if (ROMPresent[ROMNumber])
      return UpperROM[ROMNumber][Addr & 0x3FFF];
    else
      return UpperROM[0][Addr & 0x3FFF];
  }
  return RAM [BankAddr[Bank] + (unsigned long)Addr];
}
