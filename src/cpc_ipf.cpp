/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_ipf.cpp — IPF disk image loader.
 *
 * Adapted from caprice32's src/ipf.cpp (softpres.org contribution).
 * Loads an IPF file into PSRAM via FatFS, then uses the capsimg
 * library (via CAPSLockImageMemory) to parse it.  MFM track
 * decoding extracts sector data into the t_drive/t_track/t_sector
 * structures used by the FDC emulator.
 */

#include "cpc_ipf.h"
#include "cap32/cap32.h"
#include "cap32/disk.h"

#include <cstring>
#include <cstdio>

extern "C" {
#include "psram_allocator.h"
/* FatFS I/O is in cpc_ipf_io.c to avoid type conflicts */
uint8_t *ipf_read_file(const char *path, size_t *out_size);
}

/* capsimg library API.
 * Must define __cdecl before CapsLibAll.h since CapsFDC.h uses it
 * and stdafx.h (which defines it) is included later in the chain. */
#ifndef __cdecl
#define __cdecl
#endif
#include "capsimg/LibIPF/CapsLibAll.h"

/* ------------------------------------------------------------------ */
/* MFM Track Decoding (adapted from caprice32 ipf.cpp)                */
/* ------------------------------------------------------------------ */

/* Decode buffer — allocated in PSRAM on first use.
 * Original caprice32 uses a 2MB static buffer.  On the Pico we
 * allocate in PSRAM to avoid consuming internal SRAM. */
#define IPF_DECODE_BUF_SIZE  0x80000  /* 512KB — sufficient for CPC tracks */
static byte *abDecoded = nullptr;
static unsigned int uDecoded;

static bool fWrapped;
static unsigned int uPos, uLastPos;
static byte bLastData;
static word s_wCRC;

static struct CapsTrackInfoT1 cti;
static dword dwLockFlags = DI_LOCK_UPDATEFD | DI_LOCK_TYPE;

/* CRC-16 CCITT */
static void Crc(byte b_) {
    static word awCRC[256];
    if (!awCRC[1]) {
        for (int i = 0; i < 256; i++) {
            word w = i << 8;
            for (int j = 0; j < 8; j++)
                w = (w << 1) ^ ((w & 0x8000) ? 0x1021 : 0);
            awCRC[i] = w;
        }
    }
    s_wCRC = (s_wCRC << 8) ^ awCRC[((s_wCRC >> 8) ^ b_) & 0xff];
}

/* Read an MFM byte from the track */
static byte ReadByte() {
    unsigned int uOffset = uPos >> 3, uShift = uPos & 7;
    uPos += 8;
    byte b;
    if (!uShift)
        b = cti.trackbuf[uOffset];
    else
        b = (cti.trackbuf[uOffset] << uShift) | (cti.trackbuf[uOffset + 1] >> (8 - uShift));

    if (uPos >= cti.tracklen) {
        unsigned int uWrapBits = uPos - cti.tracklen;
        b &= ~(((1 << uWrapBits)) - 1);
        b |= cti.trackbuf[0] >> (8 - uWrapBits);
        uPos -= cti.tracklen;
        fWrapped = true;
    }
    return b;
}

/* Read an MFM word from the track */
static word ReadWord() {
    uLastPos = uPos;
    byte b1 = ReadByte(), b2 = ReadByte();

    byte bClock = ((b1 << 0) & 0x80) | ((b1 << 1) & 0x40) | ((b1 << 2) & 0x20) | ((b1 << 3) & 0x10) |
                  ((b2 >> 4) & 0x08) | ((b2 >> 3) & 0x04) | ((b2 >> 2) & 0x02) | ((b2 >> 1) & 0x01);

    byte bData = ((b1 << 1) & 0x80) | ((b1 << 2) & 0x40) | ((b1 << 3) & 0x20) | ((b1 << 4) & 0x10) |
                 ((b2 >> 3) & 0x08) | ((b2 >> 2) & 0x04) | ((b2 >> 1) & 0x02) | ((b2 >> 0) & 0x01);

    byte bGoodClock = 0;
    if (!(bData & 0x80) && !(bLastData & 1)) bGoodClock |= 0x80;
    if (!(bData & 0xc0)) bGoodClock |= 0x40;
    if (!(bData & 0x60)) bGoodClock |= 0x20;
    if (!(bData & 0x30)) bGoodClock |= 0x10;
    if (!(bData & 0x18)) bGoodClock |= 0x08;
    if (!(bData & 0x0c)) bGoodClock |= 0x04;
    if (!(bData & 0x06)) bGoodClock |= 0x02;
    if (!(bData & 0x03)) bGoodClock |= 0x01;

    bClock ^= bGoodClock;
    if (uDecoded < IPF_DECODE_BUF_SIZE)
        abDecoded[uDecoded++] = bLastData = bData;
    else
        bLastData = bData;

    return (bClock << 8) | bData;
}

static byte ReadDataByte() {
    return ReadWord() & 0xff;
}

/* Process MFM track data to extract sector headers and data fields */
static void ReadTrack(t_track *pt) {
    t_sector *ps = nullptr;
    unsigned int uHeaderOffset = 0;

    uPos = uDecoded = 0;
    fWrapped = false;
    bLastData = 0x00;

    if (!cti.tracklen) return;

    /* If track data already present and not flakey, skip re-decode */
    if (pt->data && !(cti.type & CTIT_FLAG_FLAKEY)) return;

    while (!fWrapped || ps) {
        byte bAM;

        if (ReadWord() != 0x04a1) { uPos -= 15; uDecoded--; continue; }
        if (ReadWord() != 0x04a1) continue;
        if (ReadWord() != 0x04a1) continue;

        s_wCRC = 0xcdb4;
        Crc(bAM = ReadDataByte());

        switch (bAM) {
            case 0xfe: {  /* ID address mark */
                if (pt->sectors >= DSK_SECTORMAX) continue;

                ps = &pt->sector[pt->sectors++];

                Crc(ps->CHRN[0] = ReadDataByte());
                Crc(ps->CHRN[1] = ReadDataByte());
                Crc(ps->CHRN[2] = ReadDataByte());
                Crc(ps->CHRN[3] = ReadDataByte());
                Crc(ReadDataByte());
                Crc(ReadDataByte());

                if (s_wCRC) {
                    pt->sectors--;
                    ps = nullptr;
                    continue;
                }

                uHeaderOffset = uLastPos;
                continue;
            }

            case 0xfb: case 0xfa:
            case 0xf8: case 0xf9: {  /* Data address mark */
                unsigned int uDataPos = uPos;
                bool fDataWrapped = fWrapped;

                if (!ps) continue;

                unsigned int uOffset = (uLastPos - uHeaderOffset) >> 4;

                if (uOffset < 32 || uOffset >= 64) {
                    ps->flags[1] &= ~0x01;
                    ps = nullptr;
                    continue;
                }

                if (bAM == 0xf8 || bAM == 0xf9)
                    ps->flags[1] |= 0x40;

                t_sector_setData(ps, abDecoded + uDecoded);
                unsigned int sector_size = (ps->CHRN[3] <= 7) ? (128 << ps->CHRN[3]) : 0x8000;
                t_sector_setSizes(ps, sector_size, sector_size);

                for (unsigned int u = 0; u < t_sector_getTotalSize(ps); u++)
                    Crc(ReadDataByte());

                Crc(ReadDataByte());
                Crc(ReadDataByte());

                if (s_wCRC) {
                    ps->flags[0] |= 0x20;
                    ps->flags[1] |= 0x20;
                }

                if (pt->sectors == 1 && t_sector_getTotalSize(ps) < 4096) {
                    for (unsigned int u = 0; u < (4096 - t_sector_getTotalSize(ps)); u++)
                        Crc(ReadDataByte());
                }

                ps = nullptr;
                uPos = uDataPos;
                fWrapped = fDataWrapped;
                continue;
            }
        }
    }

    if (!pt->data) {
        pt->data = (byte *)psram_malloc(uDecoded);
        if (pt->data) {
            std::memcpy(pt->data, abDecoded, uDecoded);
            pt->size = uDecoded;

            ptrdiff_t offset = pt->data - abDecoded;
            for (unsigned int u = 0; u < pt->sectors; u++) {
                byte *old_ptr = t_sector_getDataForWrite(&pt->sector[u]);
                t_sector_setData(&pt->sector[u], old_ptr + offset);
            }
        }
    }
}

/* Track hook — called each disk rotation for flakey data */
static void ipf_track_hook(t_drive *drive) {
    byte cyl = (byte)drive->current_track;
    byte head = (byte)drive->current_side;
    long id = drive->ipf_id;

    cti.type = 1;
    if (CAPSLockTrack(reinterpret_cast<CapsTrackInfo*>(&cti), id, cyl, head, dwLockFlags) == imgeOk) {
        t_track *pt = &drive->track[cyl][head];

        if (!cti.tracklen)
            std::memset(pt, 0, sizeof(*pt));
        else {
            if (!(dwLockFlags & DI_LOCK_TRKBIT)) cti.tracklen <<= 3;
            ReadTrack(pt);
        }
    }
}

/* Eject hook — clean up CAPS image state */
static void ipf_eject_hook(t_drive *drive) {
    long id = drive->ipf_id;
    CAPSUnlockImage(id);
    CAPSRemImage(id);
    CAPSExit();
    drive->ipf_id = 0;
    drive->altered = 0;
    drive->track_hook = nullptr;
    drive->eject_hook = nullptr;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int cpc_ipf_load(const char *path, t_drive *drive) {
    printf("ipf: loading %s\n", path);

    /* Allocate MFM decode buffer in PSRAM if not already done */
    if (!abDecoded) {
        abDecoded = (byte *)psram_malloc(IPF_DECODE_BUF_SIZE);
        if (!abDecoded) {
            printf("ipf: failed to allocate decode buffer\n");
            return -1;
        }
    }

    /* Read entire IPF file into PSRAM via FatFS */
    size_t file_size = 0;
    byte *file_buf = ipf_read_file(path, &file_size);
    if (!file_buf) return -1;

    if (file_size < 4 || std::memcmp(file_buf, "CAPS", 4) != 0) {
        printf("ipf: not a CAPS/IPF file\n");
        psram_free(file_buf);
        return -1;
    }

    /* Initialize CAPS library */
    struct CapsVersionInfo vi = {};
    if (CAPSGetVersionInfo(&vi, 0) != imgeOk || vi.release < 4) {
        printf("ipf: capsimg library too old (release=%d)\n", vi.release);
        psram_free(file_buf);
        return -1;
    }

    dwLockFlags |= vi.flag & (DI_LOCK_OVLBIT | DI_LOCK_TRKBIT);

    if (CAPSInit() != imgeOk) {
        printf("ipf: CAPSInit failed\n");
        psram_free(file_buf);
        return -1;
    }

    long id = CAPSAddImage();
    if (id < 0) {
        printf("ipf: CAPSAddImage failed\n");
        CAPSExit();
        psram_free(file_buf);
        return -1;
    }

    /* Lock image from memory buffer — no disk file I/O needed */
    if (CAPSLockImageMemory(id, file_buf, (dword)file_size,
                            DI_LOCK_MEMREF) != imgeOk) {
        printf("ipf: CAPSLockImageMemory failed\n");
        CAPSRemImage(id);
        CAPSExit();
        psram_free(file_buf);
        return -1;
    }

    /* Get image info */
    struct CapsImageInfo cii;
    if (CAPSGetImageInfo(&cii, id) != imgeOk) {
        printf("ipf: CAPSGetImageInfo failed\n");
        CAPSUnlockImage(id);
        CAPSRemImage(id);
        CAPSExit();
        psram_free(file_buf);
        return -1;
    }

    /* Set up drive */
    drive->tracks = cii.maxcylinder + 1;
    drive->sides = cii.maxhead;
    drive->altered = 0;
    drive->track_hook = ipf_track_hook;
    drive->eject_hook = ipf_eject_hook;

    /* Load all tracks */
    for (byte cyl = (byte)cii.mincylinder; cyl <= cii.maxcylinder; cyl++) {
        for (byte head = (byte)cii.minhead; head <= cii.maxhead; head++) {
            cti.type = 1;
            if (CAPSLockTrack(reinterpret_cast<CapsTrackInfo*>(&cti), id, cyl, head, dwLockFlags) != imgeOk) {
                printf("ipf: failed to lock track %d/%d\n", cyl, head);
                CAPSUnlockImage(id);
                CAPSRemImage(id);
                CAPSExit();
                psram_free(file_buf);
                return -1;
            }

            t_track *pt = &drive->track[cyl][head];

            if (!cti.tracklen)
                std::memset(pt, 0, sizeof(*pt));
            else
                ReadTrack(pt);

            CAPSUnlockTrack(id, cyl, head);
        }
    }

    drive->ipf_id = id;
    std::strncpy(drive->filename, path, 255);
    drive->filename[255] = 0;

    /* Note: file_buf must remain allocated as capsimg references it
     * (DI_LOCK_MEMREF means no copy was made).
     * It will be needed until the image is ejected. */

    printf("ipf: loaded %s (%d tracks, %d sides)\n", path,
           drive->tracks, drive->sides);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FatFS file I/O wrapper — isolated to avoid BYTE/WORD/DWORD         */
/* typedef conflicts between ff.h and capsimg CommonTypes.h.           */
/* end of cpc_ipf.cpp */
