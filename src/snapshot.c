/*
 * CPC SNA Snapshot support for frank-cpc
 * Supports CPCEMU V3 SNA format (256-byte header + 64K/128K RAM dump)
 *
 * SNA format reference: http://cpctech.cpc-live.com/docs/snapshot.html
 *
 * Header (256 bytes):
 *   0x00  12  "MV - SNA" identifier
 *   0x10   1  Z80 reg F
 *   0x11   1  Z80 reg A
 *   0x12   1  Z80 reg C
 *   0x13   1  Z80 reg B
 *   0x14   1  Z80 reg E
 *   0x15   1  Z80 reg D
 *   0x16   1  Z80 reg L
 *   0x17   1  Z80 reg H
 *   0x18   1  Z80 reg R
 *   0x19   1  Z80 reg I
 *   0x1A   1  Z80 IFF0 (0=DI, 1=EI)
 *   0x1B   1  Z80 IFF1
 *   0x1C   1  Z80 IX low
 *   0x1D   1  Z80 IX high
 *   0x1E   1  Z80 IY low
 *   0x1F   1  Z80 IY high
 *   0x20   1  Z80 SP low
 *   0x21   1  Z80 SP high
 *   0x22   1  Z80 PC low
 *   0x23   1  Z80 PC high
 *   0x24   1  Z80 IM (0, 1 or 2)
 *   0x25   1  Z80 AF' low (F')
 *   0x26   1  Z80 AF' high (A')
 *   0x27   1  Z80 BC' low (C')
 *   0x28   1  Z80 BC' high (B')
 *   0x29   1  Z80 DE' low (E')
 *   0x2A   1  Z80 DE' high (D')
 *   0x2B   1  Z80 HL' low (L')
 *   0x2C   1  Z80 HL' high (H')
 *   0x2D   1  Gate Array: selected pen
 *   0x2E  17  Gate Array: palette (16 inks + border)
 *   0x2F   1  Gate Array: multi-config (screen mode, ROM config, IRQ)
 *   0x40   1  RAM config (for 128K machines)
 *   0x41   1  CRTC selected register
 *   0x42  18  CRTC register values (R0-R17)
 *   0x54   1  ROM select (upper ROM number)
 *   0x55   1  PPI port A
 *   0x56   1  PPI port B
 *   0x57   1  PPI port C
 *   0x58   1  PPI control
 *   0x59   1  PSG selected register
 *   0x5A  16  PSG register values (R0-R15)
 *   0x6A   2  Dump size (low, high) — 64=64K, 128=128K
 *   ...       rest of header is 0
 * Data (64K or 128K):
 *   Raw RAM dump
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <string.h>
#include "snapshot.h"
#include "Z80.h"
#include "cpc.h"
#include "mem.h"
#include "screen.h"
#include "colors.h"
#include "io.h"
#include "aysound.h"
#include "defines.h"

#ifdef PICO_BUILD
#include "cpc_compat.h"
#include "ff.h"
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* External state from io.c */
extern byte Pio_A, Pio_B, Pio_C;
extern byte AY_num_reg;
extern byte PioStatus;
extern byte Customer;

/* External state from cpc.c */
extern Z80 cpu;
extern char AYRegister[16];
extern int CPCMaxMem;

/* External state from mem.c */
extern byte *RAM;
extern int ROMNumber;
extern int LowerBlockIsRAM;
extern int UpperBlockIsRAM;

/* External state from screen.c */
extern word HD6845Register[18];
extern byte HD6845RegisterPointer;
extern unsigned int ScreenMode;
extern word ScreenAddr;
extern word ScreenOffset;
extern word ScreenBlock;
extern word ScreenBank;
extern unsigned int LineOffset;

/* External state from colors.c */
extern byte Ink[17];
extern byte AktInk[17];
extern byte InkNum;
extern int MonoScreen;

#ifdef PICO_BUILD

int snapshot_save(const char *filename) {
    FIL fp;
    UINT bw;
    byte header[256];

    if (f_open(&fp, filename, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("SNA save: cannot open %s\n", filename);
        return -1;
    }

    memset(header, 0, sizeof(header));

    /* Identifier */
    memcpy(header, "MV - SNA", 8);

    /* Z80 registers */
    header[0x10] = cpu.AF.B.l;    /* F */
    header[0x11] = cpu.AF.B.h;    /* A */
    header[0x12] = cpu.BC.B.l;    /* C */
    header[0x13] = cpu.BC.B.h;    /* B */
    header[0x14] = cpu.DE.B.l;    /* E */
    header[0x15] = cpu.DE.B.h;    /* D */
    header[0x16] = cpu.HL.B.l;    /* L */
    header[0x17] = cpu.HL.B.h;    /* H */
    header[0x18] = cpu.R;
    header[0x19] = cpu.I;
    header[0x1A] = (cpu.IFF & IFF_1) ? 1 : 0;
    header[0x1B] = (cpu.IFF & IFF_2) ? 1 : 0;
    header[0x1C] = cpu.IX.B.l;
    header[0x1D] = cpu.IX.B.h;
    header[0x1E] = cpu.IY.B.l;
    header[0x1F] = cpu.IY.B.h;
    header[0x20] = cpu.SP.B.l;
    header[0x21] = cpu.SP.B.h;
    header[0x22] = cpu.PC.B.l;
    header[0x23] = cpu.PC.B.h;

    /* Interrupt mode */
    if (cpu.IFF & IFF_IM2)      header[0x24] = 2;
    else if (cpu.IFF & IFF_IM1) header[0x24] = 1;
    else                        header[0x24] = 0;

    /* Shadow registers */
    header[0x25] = cpu.AF1.B.l;   /* F' */
    header[0x26] = cpu.AF1.B.h;   /* A' */
    header[0x27] = cpu.BC1.B.l;   /* C' */
    header[0x28] = cpu.BC1.B.h;   /* B' */
    header[0x29] = cpu.DE1.B.l;   /* E' */
    header[0x2A] = cpu.DE1.B.h;   /* D' */
    header[0x2B] = cpu.HL1.B.l;   /* L' */
    header[0x2C] = cpu.HL1.B.h;   /* H' */

    /* Gate Array: selected pen */
    header[0x2D] = (byte)InkNum;

    /* Gate Array: palette — 16 inks + border (ink 16) */
    for (int i = 0; i < 17; i++)
        header[0x2E + i] = (byte)(Ink[i] - MonoScreen);

    /* Gate Array: multi-config register
     * bits 1-0: screen mode
     * bit 2: lower ROM disabled (1=RAM)
     * bit 3: upper ROM disabled (1=RAM)
     * bit 4: interrupt control (we store 0 here) */
    header[0x2F + 17] = (byte)((ScreenMode & 3) |
                                (LowerBlockIsRAM ? 4 : 0) |
                                (UpperBlockIsRAM ? 8 : 0));

    /* RAM config */
    header[0x40] = 0; /* default bank config */

    /* CRTC */
    header[0x41] = HD6845RegisterPointer;
    for (int i = 0; i < 18; i++)
        header[0x42 + i] = (byte)HD6845Register[i];

    /* ROM select */
    header[0x54] = (byte)ROMNumber;

    /* PPI */
    header[0x55] = Pio_A;
    header[0x56] = Pio_B;
    header[0x57] = Pio_C;
    header[0x58] = PioStatus;

    /* PSG */
    header[0x59] = AY_num_reg;
    for (int i = 0; i < 16; i++)
        header[0x5A + i] = (byte)AYRegister[i];

    /* Dump size: 64K = 64, 128K = 128 */
    int dump_kb = (CPCMaxMem >= 128) ? 128 : 64;
    header[0x6B] = (byte)dump_kb;
    header[0x6C] = 0;

    f_write(&fp, header, 256, &bw);

    /* RAM dump */
    unsigned long ram_size = (unsigned long)dump_kb * 1024;
    f_write(&fp, RAM, ram_size, &bw);

    f_close(&fp);
    printf("SNA saved: %s (%dK)\n", filename, dump_kb);
    return 0;
}

int snapshot_load(const char *filename) {
    FIL fp;
    UINT br;
    byte header[256];

    if (f_open(&fp, filename, FA_READ) != FR_OK) {
        printf("SNA load: cannot open %s\n", filename);
        return -1;
    }

    f_read(&fp, header, 256, &br);
    if (br < 256) {
        printf("SNA load: header too short\n");
        f_close(&fp);
        return -1;
    }

    /* Verify identifier */
    if (memcmp(header, "MV - SNA", 8) != 0 &&
        memcmp(header, "MV - CPC", 8) != 0) {
        printf("SNA load: invalid signature\n");
        f_close(&fp);
        return -1;
    }

    /* Z80 registers */
    cpu.AF.B.l = header[0x10];
    cpu.AF.B.h = header[0x11];
    cpu.BC.B.l = header[0x12];
    cpu.BC.B.h = header[0x13];
    cpu.DE.B.l = header[0x14];
    cpu.DE.B.h = header[0x15];
    cpu.HL.B.l = header[0x16];
    cpu.HL.B.h = header[0x17];
    cpu.R      = header[0x18];
    cpu.I      = header[0x19];

    cpu.IFF = 0;
    if (header[0x1A]) cpu.IFF |= IFF_1;
    if (header[0x1B]) cpu.IFF |= IFF_2;

    cpu.IX.B.l = header[0x1C];
    cpu.IX.B.h = header[0x1D];
    cpu.IY.B.l = header[0x1E];
    cpu.IY.B.h = header[0x1F];
    cpu.SP.B.l = header[0x20];
    cpu.SP.B.h = header[0x21];
    cpu.PC.B.l = header[0x22];
    cpu.PC.B.h = header[0x23];

    /* Interrupt mode */
    switch (header[0x24]) {
        case 2:  cpu.IFF |= IFF_IM2; break;
        case 1:  cpu.IFF |= IFF_IM1; break;
        default: break;
    }

    /* Shadow registers */
    cpu.AF1.B.l = header[0x25];
    cpu.AF1.B.h = header[0x26];
    cpu.BC1.B.l = header[0x27];
    cpu.BC1.B.h = header[0x28];
    cpu.DE1.B.l = header[0x29];
    cpu.DE1.B.h = header[0x2A];
    cpu.HL1.B.l = header[0x2B];
    cpu.HL1.B.h = header[0x2C];

    /* Gate Array: selected pen */
    InkNum = header[0x2D] & 0x1F;

    /* Gate Array: palette */
    for (int i = 0; i < 17; i++) {
        Ink[i] = header[0x2E + i] + MonoScreen;
        AktInk[i] = Ink[i];
    }

    /* Gate Array: multi-config */
    {
        byte ga = header[0x2E + 17]; /* offset 0x3F */
        ScreenMode = ga & 3;
        LowerBlockIsRAM = (ga & 4) ? TRUE : FALSE;
        UpperBlockIsRAM = (ga & 8) ? TRUE : FALSE;
#ifdef PICO_BUILD
        extern unsigned int DisplayMode;
        DisplayMode = ScreenMode;
#endif
    }

    /* RAM config */
    {
        byte ram_cfg = header[0x40];
        extern void SelectRamBank(byte Bank);
        SelectRamBank(ram_cfg);
    }

    /* CRTC */
    HD6845RegisterPointer = header[0x41];
    for (int i = 0; i < 18; i++)
        HD6845Register[i] = header[0x42 + i];

    /* Recalculate screen address from CRTC R12/R13 */
    ScreenAddr = (HD6845Register[12] << 8) + HD6845Register[13];
    ScreenOffset = (ScreenAddr & 1023) << 1;
    LineOffset = (((ScreenAddr & 1023) / 40) << 4);
    ScreenBlock = (ScreenAddr << 2) & 0xC000;
    ScreenBank = ScreenBlock >> 14;

    /* ROM select */
    ROMNumber = header[0x54];

    /* PPI */
    Pio_A = header[0x55];
    Pio_B = header[0x56];
    Pio_C = header[0x57];
    PioStatus = header[0x58];

    /* PSG */
    AY_num_reg = header[0x59];
    for (int i = 0; i < 16; i++)
        AYRegister[i] = (char)header[0x5A + i];

    /* Load RAM dump */
    int dump_kb = header[0x6B];
    if (dump_kb == 0) dump_kb = 64;   /* default if not set */
    unsigned long ram_size = (unsigned long)dump_kb * 1024;
    /* Clamp to available RAM */
    if (ram_size > (unsigned long)CPCMaxMem * 1024)
        ram_size = (unsigned long)CPCMaxMem * 1024;

    f_read(&fp, RAM, ram_size, &br);

    f_close(&fp);

    /* Force full screen redraw */
    extern unsigned int ChangeInk;
    ChangeInk = TRUE;

    printf("SNA loaded: %s (%dK)\n", filename, dump_kb);
    return 0;
}

#else
/* Non-Pico (Linux) build — uses POSIX file I/O */

int snapshot_save(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("SNA save: cannot open %s\n", filename);
        return -1;
    }

    byte header[256];
    memset(header, 0, sizeof(header));

    memcpy(header, "MV - SNA", 8);

    header[0x10] = cpu.AF.B.l;
    header[0x11] = cpu.AF.B.h;
    header[0x12] = cpu.BC.B.l;
    header[0x13] = cpu.BC.B.h;
    header[0x14] = cpu.DE.B.l;
    header[0x15] = cpu.DE.B.h;
    header[0x16] = cpu.HL.B.l;
    header[0x17] = cpu.HL.B.h;
    header[0x18] = cpu.R;
    header[0x19] = cpu.I;
    header[0x1A] = (cpu.IFF & IFF_1) ? 1 : 0;
    header[0x1B] = (cpu.IFF & IFF_2) ? 1 : 0;
    header[0x1C] = cpu.IX.B.l;
    header[0x1D] = cpu.IX.B.h;
    header[0x1E] = cpu.IY.B.l;
    header[0x1F] = cpu.IY.B.h;
    header[0x20] = cpu.SP.B.l;
    header[0x21] = cpu.SP.B.h;
    header[0x22] = cpu.PC.B.l;
    header[0x23] = cpu.PC.B.h;

    if (cpu.IFF & IFF_IM2)      header[0x24] = 2;
    else if (cpu.IFF & IFF_IM1) header[0x24] = 1;
    else                        header[0x24] = 0;

    header[0x25] = cpu.AF1.B.l;
    header[0x26] = cpu.AF1.B.h;
    header[0x27] = cpu.BC1.B.l;
    header[0x28] = cpu.BC1.B.h;
    header[0x29] = cpu.DE1.B.l;
    header[0x2A] = cpu.DE1.B.h;
    header[0x2B] = cpu.HL1.B.l;
    header[0x2C] = cpu.HL1.B.h;

    header[0x2D] = (byte)InkNum;
    for (int i = 0; i < 17; i++)
        header[0x2E + i] = (byte)(Ink[i] - MonoScreen);

    header[0x2E + 17] = (byte)((ScreenMode & 3) |
                                (LowerBlockIsRAM ? 4 : 0) |
                                (UpperBlockIsRAM ? 8 : 0));

    header[0x41] = HD6845RegisterPointer;
    for (int i = 0; i < 18; i++)
        header[0x42 + i] = (byte)HD6845Register[i];

    header[0x54] = (byte)ROMNumber;
    header[0x55] = Pio_A;
    header[0x56] = Pio_B;
    header[0x57] = Pio_C;
    header[0x58] = PioStatus;
    header[0x59] = AY_num_reg;
    for (int i = 0; i < 16; i++)
        header[0x5A + i] = (byte)AYRegister[i];

    int dump_kb = (CPCMaxMem >= 128) ? 128 : 64;
    header[0x6B] = (byte)dump_kb;

    fwrite(header, 1, 256, fp);
    fwrite(RAM, 1, (unsigned long)dump_kb * 1024, fp);
    fclose(fp);

    printf("SNA saved: %s (%dK)\n", filename, dump_kb);
    return 0;
}

int snapshot_load(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("SNA load: cannot open %s\n", filename);
        return -1;
    }

    byte header[256];
    if (fread(header, 1, 256, fp) < 256) {
        printf("SNA load: header too short\n");
        fclose(fp);
        return -1;
    }

    if (memcmp(header, "MV - SNA", 8) != 0 &&
        memcmp(header, "MV - CPC", 8) != 0) {
        printf("SNA load: invalid signature\n");
        fclose(fp);
        return -1;
    }

    cpu.AF.B.l = header[0x10];
    cpu.AF.B.h = header[0x11];
    cpu.BC.B.l = header[0x12];
    cpu.BC.B.h = header[0x13];
    cpu.DE.B.l = header[0x14];
    cpu.DE.B.h = header[0x15];
    cpu.HL.B.l = header[0x16];
    cpu.HL.B.h = header[0x17];
    cpu.R      = header[0x18];
    cpu.I      = header[0x19];

    cpu.IFF = 0;
    if (header[0x1A]) cpu.IFF |= IFF_1;
    if (header[0x1B]) cpu.IFF |= IFF_2;

    cpu.IX.B.l = header[0x1C];
    cpu.IX.B.h = header[0x1D];
    cpu.IY.B.l = header[0x1E];
    cpu.IY.B.h = header[0x1F];
    cpu.SP.B.l = header[0x20];
    cpu.SP.B.h = header[0x21];
    cpu.PC.B.l = header[0x22];
    cpu.PC.B.h = header[0x23];

    switch (header[0x24]) {
        case 2:  cpu.IFF |= IFF_IM2; break;
        case 1:  cpu.IFF |= IFF_IM1; break;
        default: break;
    }

    cpu.AF1.B.l = header[0x25];
    cpu.AF1.B.h = header[0x26];
    cpu.BC1.B.l = header[0x27];
    cpu.BC1.B.h = header[0x28];
    cpu.DE1.B.l = header[0x29];
    cpu.DE1.B.h = header[0x2A];
    cpu.HL1.B.l = header[0x2B];
    cpu.HL1.B.h = header[0x2C];

    InkNum = header[0x2D] & 0x1F;
    for (int i = 0; i < 17; i++) {
        Ink[i] = header[0x2E + i] + MonoScreen;
        AktInk[i] = Ink[i];
    }

    {
        byte ga = header[0x2E + 17];
        ScreenMode = ga & 3;
        LowerBlockIsRAM = (ga & 4) ? TRUE : FALSE;
        UpperBlockIsRAM = (ga & 8) ? TRUE : FALSE;
    }

    {
        byte ram_cfg = header[0x40];
        extern void SelectRamBank(byte Bank);
        SelectRamBank(ram_cfg);
    }

    HD6845RegisterPointer = header[0x41];
    for (int i = 0; i < 18; i++)
        HD6845Register[i] = header[0x42 + i];

    ScreenAddr = (HD6845Register[12] << 8) + HD6845Register[13];
    ScreenOffset = (ScreenAddr & 1023) << 1;
    LineOffset = (((ScreenAddr & 1023) / 40) << 4);
    ScreenBlock = (ScreenAddr << 2) & 0xC000;
    ScreenBank = ScreenBlock >> 14;

    ROMNumber = header[0x54];
    Pio_A = header[0x55];
    Pio_B = header[0x56];
    Pio_C = header[0x57];
    PioStatus = header[0x58];

    AY_num_reg = header[0x59];
    for (int i = 0; i < 16; i++)
        AYRegister[i] = (char)header[0x5A + i];

    int dump_kb = header[0x6B];
    if (dump_kb == 0) dump_kb = 64;
    unsigned long ram_size = (unsigned long)dump_kb * 1024;
    if (ram_size > (unsigned long)CPCMaxMem * 1024)
        ram_size = (unsigned long)CPCMaxMem * 1024;

    fread(RAM, 1, ram_size, fp);
    fclose(fp);

    extern unsigned int ChangeInk;
    ChangeInk = TRUE;

    printf("SNA loaded: %s (%dK)\n", filename, dump_kb);
    return 0;
}

#endif
