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
#include <stdint.h>
#include <string.h>
#ifdef PICO_BUILD
#include "cpc_compat.h"
#include "ff.h"
#include "psram_allocator.h"
#define CPC_LARGE_ALLOC(sz) psram_malloc(sz)
#ifndef O_RDONLY
#define O_RDONLY  FA_READ
#endif
#ifndef O_RDWR
#define O_RDWR    (FA_READ | FA_WRITE)
#endif
static inline int pico_open(const char *path, int flags) {
    static FIL _disc_files[2];
    static unsigned _disc_slot = 0;
    FIL *fp = &_disc_files[_disc_slot++ & 1u];
    BYTE mode = (BYTE)(flags & 0xFF);
    return (f_open(fp, path, mode) == FR_OK) ? (int)(uintptr_t)fp : -1;
}
static inline int pico_close(int fd) {
    return (f_close((FIL*)(uintptr_t)fd) == FR_OK) ? 0 : -1;
}
static inline int pico_read(int fd, void *buf, unsigned n) {
    UINT br = 0;
    return (f_read((FIL*)(uintptr_t)fd, buf, (UINT)n, &br) == FR_OK) ? (int)br : -1;
}
static inline int pico_write(int fd, const void *buf, unsigned n) {
    UINT bw = 0;
    return (f_write((FIL*)(uintptr_t)fd, buf, (UINT)n, &bw) == FR_OK) ? (int)bw : -1;
}
#define open(path, flags) pico_open((path), (flags))
#define close(fd) pico_close((fd))
#define read(fd, buf, n) pico_read((fd), (buf), (n))
#define write(fd, buf, n) pico_write((fd), (buf), (n))
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define CPC_LARGE_ALLOC(sz) malloc(sz)
#endif

#include "cpc.h"
#include "Z80.h"
#include "disc.h"
#include "dialogs.h"
#include "defines.h"

//#define  DEBUGFDCCOMMANDS 1  /* Set this flag to true, if you want to print the number of every startet FDC command */




/**********************************************************/
/**                                                      **/
/** Es gibt max. zwei Diskettenlaufwerke, also auch max. **/
/** zwei Disk-Image-Dateien.                             **/
/**                                                      **/
/**********************************************************/
struct DskImg dsk [2];

/* Track which sector was last accessed for Read ID cycling */
static int FDCSectorIdx[2] = {0, 0};

byte FloppyMotor;               /* TRUE=Motor an, FALSE = Motor aus       */
byte FDCCurrDrv;                /* Aktuelles Laufwerk                     */
byte FDCWrProtect[2];           /* Write protection, not used so far      */
byte FDCCurrTrack[2];           /* Aktueller Track jedes Laufwerkes       */
byte FDCCurrSide[2];            /* Aktuelle Seite                         */
byte ExecCmdPhase;              /* TRUE=Kommandophase findet gerade statt */
byte ResultPhase;               /* TRUE=Result-Phase findet gerade statt  */
byte StatusRegister;            /* Das Statusregister des FDC 765         */
word StatusCounter;
byte st0, st1, st2, st3;
byte st0_EC, st0_SE, st0_IC;

/* Cause of an FDC interrupt:
 * 1 => Entering result phase of Read/Write/Format/Scan command
 * 2 => Ready for data transfer (in execution phase)
 * 4 => End of Seek or Recalibrate command
 */
static int int_cause;

byte FDCCommand [9];            /* Feld fr Kommandos  */
byte FDCResult  [7];            /* Feld fr Ergebnisse */

word FDCPointer;                /* Zeiger auf die akt. Variable im Komando-Feld (beim �ertragen) */
word FDCCmdPointer;             /* Zeiger auf das aktuell zu bertragende Zeichen (READ/WRITE)    */
word FDCResPointer;             /* Zeiger auf das akt. Result                                     */
word FDCResCounter;             /* Anzahl der Results, die Zurckgegeben werden                   */
unsigned long FDCDataPointer;   /* Sektor-Zeiger (Z�ler) */
unsigned long FDCDataLength;    /* Anzahl der zu lesenden Daten */
word TrackIndex;                /* Index auf dsk[].Tracks[....] */
unsigned long TrackDataStart;   /* Startposition der Daten des akt. Sektors im Track */

byte  SeekTrack;                /* Boolean */
float SeekTrackTime;            /* Time until Seek-End in ms */
int   SeekTrackDrive;
int   RmvSeekTrackDrive;
int   StepRate;
int   RealDiscSeekTime;

byte StepTime [16] = {
   32,30,28,26,
   24,22,20,18,
   16,14,12,10,
    8, 6, 4, 2
};


byte bytes_in_cmd[32] = {
        1,  /*  0 = none                                */
	1,  /*  1 = none                                */
	9,  /*  2 = READ TRACK                          */
	3,  /*  3 = SPECIFY                             */
	2,  /*  4 = SENSE DRIVE STATUS                  */
	9,  /*  5 = WRITE DATA                          */
	9,  /*  6 = READ DATA                           */
	2,  /*  7 = RECALIBRATE                         */
	1,  /*  8 = SENSE INTERRUPT STATUS              */
	9,  /*  9 = WRITE DELETED DATA, not implemented */
	2,  /* 10 = READ SECTOR ID                      */
	1,  /* 11 = none                                */
	9,  /* 12 = READ DELETED DATA, not implemented  */
	6,  /* 13 = FORMAT A TRACK                      */
	1,  /* 14 = none                                */
	3,  /* 15 = SEEK                                */
	1,  /* 16 = none                                */
	9,  /* 17 = SCAN EQUAL                          */
	1,  /* 18 = none                                */
	1,  /* 19 = none                                */
	1,  /* 20 = none                                */
	1,  /* 21 = none                                */
	1,  /* 22 = none                                */
	1,  /* 23 = none                                */
	1,  /* 24 = none                                */
	9,  /* 25 = SCAN LOW OR EQUAL                   */
	1,  /* 26 = none                                */
	1,  /* 27 = none                                */
	1,  /* 28 = none                                */
	1,  /* 29 = none                                */
	9,  /* 30 = SCAN HIGH OR EQUAL                  */
	1   /* 31 = none                                */
};


/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
byte Get_st0(void) {
  st0 = FDCCurrDrv;
  if (dsk[FDCCurrDrv].fid <= 0)
    st0 |= 0x08; /* Drive not ready */
  st0 |= FDCCurrSide[FDCCurrDrv]<<2;
  st0 = st0 | st0_IC | st0_SE | st0_EC;
  return st0;
}


/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void GetRes7 (void) {               /* Return 7 result bytes */
    FDCResult[0] = Get_st0();
    FDCResult[1] = st1;
    FDCResult[2] = st2;
    FDCResult[3] = FDCCommand[2];	    /* C, H, R, N */
    FDCResult[4] = FDCCommand[3];
    FDCResult[5] = FDCCommand[4];
    FDCResult[6] = FDCCommand[5];
    StatusRegister = 0xD0; /* Ready to return results */
    StatusCounter = 100;
    FDCResPointer = 0;
    FDCResCounter = 7;
    st0 = st1 = st2 = 0;
    ExecCmdPhase = FALSE;
    ResultPhase = TRUE;
    int_cause=1;
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void FDCExecWriteCommand (register byte Value) {
  word i;
  #ifdef DEBUGFDCCOMMANDS
    if (!NoDebug) fprintf (DebugFP, "FDC Write %d\n", FDCCommand [0]);
  #endif
  switch (FDCCommand [0]) {
    case 2:
      /* Read track */
      int_cause=2;
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
      if (dsk[FDCCurrDrv].fid <= 0) {
        GetRes7();
      }
      else {
        ExecCmdPhase = TRUE;
        int_cause=1;
        TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
        TrackDataStart = 0;
        /* Calculate total data in track */
        {
          unsigned long total = 0;
          int s;
          struct track_type *trk = &dsk[FDCCurrDrv].Tracks[TrackIndex];
          for (s = 0; s < trk->SPT; s++)
            total += GetSectorDataLength(FDCCurrDrv, TrackIndex, s);
          FDCDataLength = total;
        }
        FDCDataPointer = 0;
        StatusCounter = 100;
        StatusRegister = 0xF0;     /* RQM=1, DIO=FDC->CPU, EXM=1, CB=1 */
      }
      break;

    case 3:
      /* Specify */
      FDCResCounter = 0;       /* No result bytes */
      FDCResPointer = 0;
      StepRate = (FDCCommand[1] & 0xF0) >> 4;
      ExecCmdPhase = FALSE;
      ResultPhase = FALSE;
      st0_EC = 0x00;
      st0_IC = 0x00;
      st0_SE = 0x20;
      StatusRegister = 0x80;
      break;

    case 4:
      /* Sense drive status */
      FDCCurrDrv = FDCCommand[1] & 3;
      st3 = FDCCommand[1] & 7;
      if (FDCWrProtect[FDCCurrDrv] == TRUE) st3 |= 0x40;
      if (dsk[FDCCurrDrv].fid > 0) st3 |= 0x20;
      if (FDCCurrTrack[FDCCurrDrv]==0) st3 |= 0x10;
      st3 |= dsk[FDCCurrDrv].DiskHeader.nbof_heads<<4; /* Two side drive */
      FDCResCounter = 1;
      FDCResPointer = 0;
      FDCResult[0] = st3;
      ExecCmdPhase = FALSE;
      ResultPhase = TRUE;
      int_cause=1;
      StatusCounter = 100;
      StatusRegister = 0xD0;    /* Ready to return results */
      break;

    case 5:
      /* Write data */
      if (ExecCmdPhase == FALSE) {
        int_cause=2;
        FDCCurrDrv = FDCCommand[1] & 3;
        FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
        FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
        ExecCmdPhase = TRUE;
        if (dsk[FDCCurrDrv].fid <= 0) {
          GetRes7();
        }
        else {
          TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
          {
            int si = FindSectorByID(FDCCurrDrv, TrackIndex, FDCCommand[4], 0);
            if (si < 0) {
              st1 = 0x04; /* Sector not found (ND) */
              st0_IC = 0x40;
              GetRes7();
              break;
            }
            TrackDataStart = GetSectorDataOffset(FDCCurrDrv, TrackIndex, si);
            FDCDataLength = GetSectorDataLength(FDCCurrDrv, TrackIndex, si);
          }
          FDCDataPointer = 0;
          StatusCounter = 100;
          StatusRegister = 0xB0;     /* RQM=1, DIO=CPU->FDC, EXM=1, CB=1 */
          int_cause=1;
          st0_IC=0x40;
        }
      }
      else {
        dsk[FDCCurrDrv].Tracks[TrackIndex].DiscData[TrackDataStart + FDCDataPointer] = Value;
        FDCDataPointer ++;
        if (FDCDataPointer==FDCDataLength) {
          st0_IC=0x00;st0_SE=0x00;st0_EC=0;
          GetRes7();
        }
      }
      break;

    case 6:
      /* Read data */
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
      if (dsk[FDCCurrDrv].fid <= 0) {
        GetRes7();
      }
      else {
        ExecCmdPhase = TRUE;
        int_cause=2;
        TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
        {
          int si = FindSectorByID(FDCCurrDrv, TrackIndex, FDCCommand[4], 0);
          if (si < 0) {
            st1 = 0x04; /* Sector not found (ND) */
            st0_IC = 0x40;
            GetRes7();
            break;
          }
          /* Propagate ST1/ST2 from sector info (copy protection) */
          st1 |= dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status1;
          st2 |= dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status2;
          /* If sector has CM (deleted data mark) set in ST2 and this
           * is a normal read (not read deleted), signal it in ST2 */
          if (dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status2 & 0x40) {
            st2 |= 0x40; /* CM - control mark (deleted data) */
          }
          TrackDataStart = GetSectorDataOffset(FDCCurrDrv, TrackIndex, si);
          FDCDataLength = GetSectorDataLength(FDCCurrDrv, TrackIndex, si);
          /* Update result CHRN from the sector's actual ID fields */
          FDCCommand[2] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].track;
          FDCCommand[3] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].head;
          FDCCommand[4] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].sector;
          FDCCommand[5] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].BPS;
        }
        FDCDataPointer = 0;
        StatusCounter = 100;
        StatusRegister = 0xF0;     /* RQM=1, DIO=FDC->CPU, EXM=1, CB=1 */
      }
      break;

    case 7:
      /* Recalibrate (Spur 0 suchen) */
      st0 = st1 = st2 = 0;
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = 0;
      st0 = FDCCommand[1] & 7;
      if (dsk[FDCCurrDrv].fid <= 0) {
        StatusRegister = 0x80 ;
      }
      else {
        if (FDCCurrTrack[FDCCurrDrv] > 77) {
          FDCCurrTrack[FDCCurrDrv] -= 77;
          st0_EC = 0x10;
        }
        else {
          FDCCurrTrack[FDCCurrDrv] = 0;
          st0_EC = 0x00;
        }
        if (RealDiscSeekTime)
          SeekTrackTime = StepTime [StepRate];
        else
          SeekTrackTime = 0;
        SeekTrackDrive = 1<<(FDCCommand[1]&3);
        SeekTrack = TRUE;
        StatusCounter = 100;
        StatusRegister = 0x80;
      }
      ExecCmdPhase=FALSE;
      break;

    case 8:
      /* Sense Interrupt */
      FDCResPointer = 0;
      if (SeekTrack==TRUE && SeekTrackTime<=0) {
        RmvSeekTrackDrive = TRUE; /* Remove SEEK drive bit in the main status register while result phase */
        SeekTrackTime=0;
        SeekTrack=FALSE;
        st0_SE = 0x20;
        st0_IC = 0x00;
        FDCResCounter = 2;       /* Two result bytes */
        FDCResult[1] = FDCCurrTrack [FDCCurrDrv];
      }
      else {
        st0_IC = 0x80;
        FDCResCounter = 2;       /* Only one result byte */
        FDCResult[1] = 128;
      }

      StatusRegister = 0xD0;   /* RQM=1, DIO=FDC->CPU, EXM = 0, CB=1, DB0-DB3 = 0 */
      FDCResult[0] = Get_st0();
      ExecCmdPhase=FALSE;
      ResultPhase=TRUE;
      StatusCounter = 100;
      st0_EC = 0x00;
      break;

    case 9:
      /* Write deleted data - same as write data but mark sector as deleted */
      if (ExecCmdPhase == FALSE) {
        int_cause=2;
        FDCCurrDrv = FDCCommand[1] & 3;
        FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
        FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
        ExecCmdPhase = TRUE;
        if (dsk[FDCCurrDrv].fid <= 0) {
          GetRes7();
        }
        else {
          TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
          {
            int si = FindSectorByID(FDCCurrDrv, TrackIndex, FDCCommand[4], 0);
            if (si < 0) {
              st1 = 0x04;
              st0_IC = 0x40;
              GetRes7();
              break;
            }
            /* Mark sector as deleted data */
            dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status2 |= 0x40;
            TrackDataStart = GetSectorDataOffset(FDCCurrDrv, TrackIndex, si);
            FDCDataLength = GetSectorDataLength(FDCCurrDrv, TrackIndex, si);
          }
          FDCDataPointer = 0;
          StatusCounter = 100;
          StatusRegister = 0xB0;
          int_cause=1;
          st0_IC=0x40;
        }
      }
      else {
        dsk[FDCCurrDrv].Tracks[TrackIndex].DiscData[TrackDataStart + FDCDataPointer] = Value;
        FDCDataPointer ++;
        if (FDCDataPointer==FDCDataLength) {
          st0_IC=0x00;st0_SE=0x00;st0_EC=0;
          GetRes7();
        }
      }
      break;

    case 10:
      /* read ID of next sector — cycles through all sectors on the track */
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      st0_SE = 0x00;
      st0_IC = 0x00;
      if (dsk[FDCCurrDrv].fid > 0) {
        TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
        int spt = dsk[FDCCurrDrv].Tracks[TrackIndex].SPT;
        if (spt > 0) {
          int si = FDCSectorIdx[FDCCurrDrv] % spt;
          struct sector_info_type *sec = &dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si];
          FDCCommand[2] = sec->track;   /* C */
          FDCCommand[3] = sec->head;    /* H */
          FDCCommand[4] = sec->sector;  /* R */
          FDCCommand[5] = sec->BPS;     /* N */
          FDCSectorIdx[FDCCurrDrv] = (si + 1) % spt;
        }
      }
      GetRes7();
      break;

    case 12:
      /* Read deleted data - same as read data but only reads deleted sectors */
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
      if (dsk[FDCCurrDrv].fid <= 0) {
        GetRes7();
      }
      else {
        ExecCmdPhase = TRUE;
        int_cause=2;
        TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
        {
          int si = FindSectorByID(FDCCurrDrv, TrackIndex, FDCCommand[4], 0);
          if (si < 0) {
            st1 = 0x04;
            st0_IC = 0x40;
            GetRes7();
            break;
          }
          st1 |= dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status1;
          st2 |= dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status2;
          /* If sector does NOT have deleted mark, signal it */
          if (!(dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].status2 & 0x40)) {
            st2 |= 0x40; /* CM set = found non-deleted when looking for deleted */
          }
          TrackDataStart = GetSectorDataOffset(FDCCurrDrv, TrackIndex, si);
          FDCDataLength = GetSectorDataLength(FDCCurrDrv, TrackIndex, si);
          FDCCommand[2] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].track;
          FDCCommand[3] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].head;
          FDCCommand[4] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].sector;
          FDCCommand[5] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[si].BPS;
        }
        FDCDataPointer = 0;
        StatusCounter = 100;
        StatusRegister = 0xF0;
      }
      break;

    case 13:
      /* Format a track */
      if (ExecCmdPhase == FALSE) {
        FDCCurrDrv = FDCCommand[1] & 3;
        FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
        if (dsk[FDCCurrDrv].fid <= 0 || FDCWrProtect[FDCCurrDrv]) {
          if (FDCWrProtect[FDCCurrDrv]) st1 = 0x02; /* NW - not writable */
          st0_IC = 0x40;
          GetRes7();
        }
        else {
          ExecCmdPhase = TRUE;
          int_cause=2;
          TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
          /* FDCCommand[2] = BPS (N), FDCCommand[3] = sectors/track,
           * FDCCommand[4] = GAP3, FDCCommand[5] = filler byte */
          FDCDataPointer = 0;
          /* We expect 4 bytes per sector (C, H, R, N) from the host */
          FDCDataLength = (unsigned long)FDCCommand[3] * 4;
          StatusCounter = 100;
          StatusRegister = 0xB0;     /* RQM=1, DIO=CPU->FDC, EXM=1, CB=1 */
        }
      }
      else {
        /* Receive CHRN data for each sector: C, H, R, N per sector */
        static byte fmt_buf[29*4];
        if (FDCDataPointer < sizeof(fmt_buf))
          fmt_buf[FDCDataPointer] = Value;
        FDCDataPointer++;
        if (FDCDataPointer == FDCDataLength) {
          /* Apply format: update track header and fill sectors */
          struct track_type *trk = &dsk[FDCCurrDrv].Tracks[TrackIndex];
          int nsectors = FDCCommand[3];
          int sector_size = 128 << FDCCommand[2];
          byte filler = FDCCommand[5];
          trk->BPS = FDCCommand[2];
          trk->SPT = nsectors;
          trk->GAP3 = FDCCommand[4];
          trk->filler = filler;
          unsigned long data_off = 0;
          int s;
          for (s = 0; s < nsectors && s < 29; s++) {
            trk->sector[s].track  = fmt_buf[s*4 + 0];
            trk->sector[s].head   = fmt_buf[s*4 + 1];
            trk->sector[s].sector = fmt_buf[s*4 + 2];
            trk->sector[s].BPS    = fmt_buf[s*4 + 3];
            trk->sector[s].status1 = 0;
            trk->sector[s].status2 = 0;
            trk->sector[s].data_len_lo = sector_size & 0xFF;
            trk->sector[s].data_len_hi = (sector_size >> 8) & 0xFF;
            /* Fill sector data with filler byte */
            if (data_off + sector_size <= sizeof(trk->DiscData))
              memset(&trk->DiscData[data_off], filler, sector_size);
            data_off += sector_size;
          }
          st0_IC = 0x00; st0_SE = 0x00; st0_EC = 0;
          GetRes7();
        }
      }
      break;

    case 15:
      /* SEEK */
      StatusCounter = 100;
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = 0;
      //FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      if (dsk[FDCCurrDrv].fid > 0) {
        if (RealDiscSeekTime)
          if (FDCCurrTrack[FDCCurrDrv] > FDCCommand[2])
            SeekTrackTime = (FDCCurrTrack[FDCCurrDrv] - FDCCommand[2]) * StepTime [StepRate];
          else
            SeekTrackTime = (FDCCommand[2] - FDCCurrTrack[FDCCurrDrv]) * StepTime [StepRate];
          if (SeekTrackTime == 0) SeekTrackTime = StepTime [StepRate];
        else
          SeekTrackTime=0;
        SeekTrackDrive = 1<<(FDCCommand[1]&3);
        SeekTrack = TRUE;
        FDCCurrTrack[FDCCurrDrv] = FDCCommand[2];
        ExecCmdPhase=FALSE;
        StatusRegister = 0x80;
        st0_EC = 0;
      }
      break;

    //case 17:
    //  /* Scan equal */
    //  break;

    //case 25:
    //  /* Scan low or equal */
    //  break;

    //case 30:
    //  /* Scan high or equal */
    //  break;

    default:
      if (!NoDebug) fprintf (DebugFP, "CPC4X - unknown disc write command %3i\n", FDCCommand [0]);
      break;
  }
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
byte FDCExecReadCommand (void) {

  byte ret;
  ret=0;
  #ifdef DEBUGFDCCOMMANDS
    if (!NoDebug) fprintf (DebugFP, "FDC Read %d\n", FDCCommand [0]);
  #endif

  switch (FDCCommand [0]) {
    case 2:
      ret = dsk[FDCCurrDrv].Tracks[TrackIndex].DiscData[TrackDataStart + FDCDataPointer];
      FDCDataPointer ++;
      if (FDCDataPointer==FDCDataLength) {
        st0_IC =0x40;   /* Unit, head, command canceled */
        st0_SE = 0x00;
        st1 = 0x80;     /* End of track error           */
        GetRes7();
      }
      break;

    case 6:
      ret = dsk[FDCCurrDrv].Tracks[TrackIndex].DiscData[TrackDataStart + FDCDataPointer];
      FDCDataPointer ++;
      if (FDCDataPointer==FDCDataLength) {
        st0_IC = 0x40;   /* Unit, head, command canceled */
        st0_SE = 0x00;
        st1 = 0x80;      /* End of track error           */
        GetRes7();
      }
      break;

    default:
      if (!NoDebug) fprintf (DebugFP, "CPC4X - unknown disc read command %3i\n", FDCCommand [0]);
      break;
  }
  return ret;
}



/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
byte FDCGetResult (void) {
  static byte ret;
  ret = FDCResult[FDCResPointer];
  FDCResPointer ++;

  /* Remove SEEK-Track drive bit in main status rebister */
  if (RmvSeekTrackDrive) {
     RmvSeekTrackDrive = FALSE;
     SeekTrackDrive = 0;
     st0_SE = 0x00;
  }

  /* All results transmitted ? */
  if (FDCResPointer==FDCResCounter) {
    StatusRegister = 0x80;
    ResultPhase=FALSE;
  }
  return ret;
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void ResetFDC (void) {
    static int i;
    FloppyMotor = 0;
    FDCPointer = 0;
    ExecCmdPhase = 0;
    ResultPhase = 0;
    StatusRegister = 128;
    int_cause=0;
    for (i=0 ; i<2; i++) {
      FDCCurrTrack[i] = 0;
      FDCWrProtect[i] = FALSE;
      FDCSectorIdx[i] = 0;
    }
    for (i=0 ; i<7; i++) FDCCommand[i]=0;
    SeekTrack = FALSE;
    SeekTrackTime = 0.0;
    SeekTrackDrive = 0;
    st0_EC = 0;
}

/*********************************************************************/
/* Extended DSK sector helper functions                              */
/*********************************************************************/
unsigned long GetSectorDataLength(int DrvNum, int trackIdx, int sectorIdx) {
    struct track_type *trk = &dsk[DrvNum].Tracks[trackIdx];
    if (dsk[DrvNum].extended) {
        /* In extended DSK, the actual data length is in the sector info */
        unsigned long len = trk->sector[sectorIdx].data_len_lo |
                           (trk->sector[sectorIdx].data_len_hi << 8);
        if (len == 0) {
            /* Fallback to BPS field */
            len = 128u << trk->sector[sectorIdx].BPS;
        }
        return len;
    } else {
        /* Standard DSK: all sectors same size from track header BPS */
        return 128u << trk->BPS;
    }
}

unsigned long GetSectorDataOffset(int DrvNum, int trackIdx, int sectorIdx) {
    unsigned long offset = 0;
    int i;
    for (i = 0; i < sectorIdx; i++) {
        offset += GetSectorDataLength(DrvNum, trackIdx, i);
    }
    return offset;
}

int FindSectorByID(int DrvNum, int trackIdx, byte sectorID, int startIdx) {
    struct track_type *trk = &dsk[DrvNum].Tracks[trackIdx];
    int spt = trk->SPT;
    if (spt <= 0) return -1;
    int i;
    for (i = 0; i < spt; i++) {
        int idx = (startIdx + i) % spt;
        if (trk->sector[idx].sector == sectorID)
            return idx;
    }
    return -1;
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void InitDisc (void) {
    int i;
    dsk[0].TrackSize = 194560;
    dsk[0].fid       = -1;   // No file loaded now
    dsk[0].Tracks    = (void *)CPC_LARGE_ALLOC (194560);
    dsk[0].extended  = 0;
    dsk[1].TrackSize = 778240;
    dsk[1].fid       = -1;   // No file loaded now
    dsk[1].Tracks    = (void *)CPC_LARGE_ALLOC (778240);
    dsk[1].extended  = 0;
    ResetFDC();
    RealDiscSeekTime=0;
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void ExitDisc (void) {
  WriteDskImage (0);
  WriteDskImage (1);
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/

void WriteFDCData (register byte Value) {
  if (ExecCmdPhase==FALSE) {
    if (FDCPointer == 0) {
      FDCCommand [0] = Value & 0x1F;  /* Neues Kommando*/
      FDCPointer ++;
      StatusRegister |= 0x10;         /* FDC Busy*/
    }
    else
      if (FDCPointer < bytes_in_cmd[FDCCommand[0]]) {
        FDCCommand[FDCPointer] = Value;                  /* Parameter fr das Kommando */
        FDCPointer ++;
      }

    if (FDCPointer == bytes_in_cmd[FDCCommand[0]]) {
      FDCPointer = 0;
      StatusRegister |= 0x20;
      FDCExecWriteCommand (Value);                     /* Kommando ausfhren */
    }
  }

  else
    FDCExecWriteCommand (Value);                     /* Kommando ausfhren */
}



/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
byte ReadFDCData (void) {
  if (ExecCmdPhase==TRUE) return FDCExecReadCommand ();
  if (ResultPhase==TRUE) return FDCGetResult ();
  return 255;
}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
byte ReadFDCStatus (void) {
  return StatusRegister | SeekTrackDrive;
}


/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void WriteDskImage (int DrvNum) {
  if (dsk[DrvNum].ImageName[0] == '\0') {
    dsk[DrvNum].fid = -1;
    return;
  }
  printf("Write disc image: %s", dsk[DrvNum].ImageName);
  dsk[DrvNum].fid = open(dsk[DrvNum].ImageName, O_RDWR);
  if (dsk[DrvNum].fid > 0) {
    write(dsk[DrvNum].fid, &dsk[DrvNum].DiskHeader, 0x100);

    int total_tracks = dsk[DrvNum].DiskHeader.nbof_tracks * dsk[DrvNum].DiskHeader.nbof_heads;
    int track;

    if (dsk[DrvNum].extended) {
      byte *track_size_table = &dsk[DrvNum].DiskHeader.unused[0];
      for (track = 0; track < total_tracks; track++) {
        unsigned int track_total_size = (unsigned int)track_size_table[track] * 256;
        if (track_total_size == 0) continue;

        struct track_type* trackHeader = &dsk[DrvNum].Tracks[track];
        write(dsk[DrvNum].fid, trackHeader, 0x100);

        unsigned int data_size = track_total_size - 0x100;
        if (data_size > sizeof(trackHeader->DiscData))
          data_size = sizeof(trackHeader->DiscData);
        write(dsk[DrvNum].fid, trackHeader->DiscData, data_size);
      }
    } else {
      for (track = 0; track < total_tracks; track++) {
        struct track_type* trackHeader = &dsk[DrvNum].Tracks[track];
        write(dsk[DrvNum].fid, trackHeader, 0x100);

        unsigned int data_size = dsk[DrvNum].DiskHeader.tracksize - 0x100;
        if (data_size > sizeof(trackHeader->DiscData))
          data_size = sizeof(trackHeader->DiscData);
        write(dsk[DrvNum].fid, trackHeader->DiscData, data_size);
      }
    }

#ifdef PICO_BUILD
    f_truncate((FIL*)(uintptr_t)dsk[DrvNum].fid);
#else
    ftruncate(dsk[DrvNum].fid, lseek(dsk[DrvNum].fid, 0, SEEK_CUR));
#endif
    close(dsk[DrvNum].fid);
    printf(" .... ok!");
  } else {
    printf(" .... FAILED to open for writing");
  }
  printf("\n");
  dsk[DrvNum].fid = -1;
}

/*********************************************************************/
/**                                                                 **/
/** InsertDisk (DrvNum);                                            **/
/**                                                                 **/
/** Zeigt einen Datei-Dialog zur Auswahl von Disk-Image-Dateien an, **/
/** schlie� eine evtl. bereits ge�fnete Datei, �fnet die ausge-  **/
/** w�lte Image-Datei und liest diese in den Speicher ein.         **/
/** Der fr die Track-Informationen und die Daten des Disk-Images   **/
/** ben�igte Speicher wird jedoch nur einmal mit MALLOC vom Syste  **/
/** angefordert und immer wieder verwendet, bis die Emulation be-   **/
/** endet wird.                                                     **/
/**                                                                 **/
/** DRVNUM = Laufwerksnummer (0 fr A: und 1 fr B:)                **/
/**                                                                 **/
/*********************************************************************/

void InsertDisk (int DrvNum) {
  static unsigned long readbytes;
  static int      fid, wrprotect;
  static char     fname [255];
  static int      track;
  /**************************************************/
  /** Select a file via file dialog                **/
  /**************************************************/
  fid = SelectDiskFile (fname , &DrvNum, &wrprotect);

  if ( fid > 0 ) {
    WriteDskImage (DrvNum);

    dsk[DrvNum].fid = fid;
    sprintf (dsk[DrvNum].ImageName, "%s", fname);
    FDCWrProtect[DrvNum] = wrprotect;

    /***********************************/
    /** Read Disk-Header (256 Bytes)  **/
    /***********************************/
    read ( dsk[DrvNum].fid , &dsk[DrvNum].DiskHeader  , 0x100);

    /* Detect Extended DSK format by checking the tag */
    dsk[DrvNum].extended = 0;
    if (dsk[DrvNum].DiskHeader.tag[0] == 'E' &&
        dsk[DrvNum].DiskHeader.tag[1] == 'X' &&
        dsk[DrvNum].DiskHeader.tag[2] == 'T') {
      dsk[DrvNum].extended = 1;
    }

    int total_tracks = dsk[DrvNum].DiskHeader.nbof_tracks * dsk[DrvNum].DiskHeader.nbof_heads;

    if (dsk[DrvNum].extended) {
      /*******************************************************/
      /** Extended DSK: per-track size table is at offset   **/
      /** 0x34 in header. Each byte = high byte of track    **/
      /** size (multiply by 256 to get actual size).        **/
      /*******************************************************/
      /* Calculate total size needed */
      readbytes = (unsigned long)total_tracks * sizeof(struct track_type);

      if ( dsk[DrvNum].TrackSize < readbytes ) {
          /* On Pico, PSRAM allocations can't be freed/realloc'd easily.
           * Only re-allocate if truly needed. */
#ifndef PICO_BUILD
          free (dsk[DrvNum].Tracks);
#endif
          dsk[DrvNum].TrackSize = 0;
      }
      if ( dsk[DrvNum].TrackSize == 0 ) {
          dsk[DrvNum].Tracks = (void *)CPC_LARGE_ALLOC (readbytes);
      }
      dsk[DrvNum].TrackSize = readbytes;

      /* Track size table starts at offset 0x34 in the disk header */
      byte *track_size_table = &dsk[DrvNum].DiskHeader.unused[0];

      for (track = 0; track < total_tracks; ++track) {
        struct track_type* trackHeader = &dsk[DrvNum].Tracks[track];
        unsigned int track_total_size = (unsigned int)track_size_table[track] * 256;

        if (track_total_size == 0) {
          /* Unformatted track */
          memset(trackHeader, 0, sizeof(struct track_type));
          continue;
        }

        /* Read track header (0x100 bytes) */
        read(dsk[DrvNum].fid, trackHeader, 0x100);

        /* Read all sector data sequentially (track_total_size - 0x100 bytes) */
        unsigned int data_size = track_total_size - 0x100;
        if (data_size > sizeof(trackHeader->DiscData))
          data_size = sizeof(trackHeader->DiscData);
        read(dsk[DrvNum].fid, trackHeader->DiscData, data_size);

        /* Skip any remaining data that doesn't fit in our buffer */
        if (track_total_size - 0x100 > data_size) {
          unsigned int skip = (track_total_size - 0x100) - data_size;
          /* Seek forward by reading and discarding */
          byte discard[256];
          while (skip > 0) {
            unsigned int chunk = skip > 256 ? 256 : skip;
            read(dsk[DrvNum].fid, discard, chunk);
            skip -= chunk;
          }
        }
      }
    }
    else {
      /*******************************************************/
      /** Standard DSK: fixed track size                    **/
      /*******************************************************/
      /* Allocate based on struct size since we index Tracks[] as an array */
      readbytes = (unsigned long)total_tracks * sizeof(struct track_type);

      if ( dsk[DrvNum].TrackSize < readbytes ) {
#ifndef PICO_BUILD
          free (dsk[DrvNum].Tracks);
#endif
          dsk[DrvNum].TrackSize = 0;
      }
      if ( dsk[DrvNum].TrackSize == 0 ) {
          dsk[DrvNum].Tracks = (void *)CPC_LARGE_ALLOC (readbytes);
      }
      dsk[DrvNum].TrackSize = readbytes;

      for (track = 0; track < total_tracks; ++track) {
        struct track_type* trackHeader = &dsk[DrvNum].Tracks[track];

        /* Read track header */
        read(dsk[DrvNum].fid, trackHeader, 0x100);

        /* Read sector data: use actual sector count and sizes */
        int spt = trackHeader->SPT;
        if (spt > 29) spt = 29;
        int sector_size = 128 << trackHeader->BPS;
        unsigned long data_offset = 0;

        int sector;
        for (sector = 0; sector < spt; sector++) {
          /* Read sector data directly at sequential offset
           * (no reordering — keep sectors in image order) */
          if (data_offset + sector_size <= sizeof(trackHeader->DiscData)) {
            read(dsk[DrvNum].fid, &trackHeader->DiscData[data_offset], sector_size);
          }
          data_offset += sector_size;
        }

        /* Skip any padding bytes to align with tracksize */
        unsigned int expected_data = dsk[DrvNum].DiskHeader.tracksize - 0x100;
        if (data_offset < expected_data) {
          byte discard[256];
          unsigned int skip = expected_data - data_offset;
          while (skip > 0) {
            unsigned int chunk = skip > 256 ? 256 : skip;
            read(dsk[DrvNum].fid, discard, chunk);
            skip -= chunk;
          }
        }
      }
    }

    close ( dsk[DrvNum].fid );
    FDCSectorIdx[DrvNum] = 0;
  }
  int_cause=4;
}

/* ---- Create a blank formatted DSK image -------------------------------- */

int CreateBlankDsk(const char *path) {
  /* Standard AMSDOS format: 40 tracks, 9 sectors/track (C1-C9), 512 B/sector */
  const int num_tracks = 40;
  const int num_sectors = 9;
  const int sector_size = 512;
  const int track_data_size = num_sectors * sector_size; /* 0x1200 */
  const int track_total_size = 0x100 + track_data_size;  /* 0x1300 */

  /* -- Disk header (0x100 bytes) -- */
  byte header[0x100];
  memset(header, 0, sizeof(header));
  memcpy(header, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
  header[0x30] = (byte)num_tracks;
  header[0x31] = 1;  /* 1 head */
  header[0x32] = (byte)(track_total_size & 0xFF);
  header[0x33] = (byte)((track_total_size >> 8) & 0xFF);

#ifdef PICO_BUILD
  FIL fp;
  UINT bw = 0;
  if (f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
    return -1;
  if (f_write(&fp, header, 0x100, &bw) != FR_OK || bw != 0x100) {
    f_close(&fp);
    return -1;
  }

  /* -- Track data -- */
  byte track_hdr[0x100];
  byte sector_data[512];
  memset(sector_data, 0xE5, sizeof(sector_data));

  for (int t = 0; t < num_tracks; t++) {
    memset(track_hdr, 0, sizeof(track_hdr));
    memcpy(track_hdr, "Track-Info\r\n", 12);
    track_hdr[0x10] = (byte)t;   /* track number */
    track_hdr[0x11] = 0;         /* head */
    track_hdr[0x14] = 2;         /* BPS: 2 = 512 bytes */
    track_hdr[0x15] = (byte)num_sectors;
    track_hdr[0x16] = 0x4E;      /* GAP3 */
    track_hdr[0x17] = 0xE5;      /* filler */

    /* Sector info table */
    for (int s = 0; s < num_sectors; s++) {
      int off = 0x18 + s * 8;
      track_hdr[off + 0] = (byte)t;          /* C - track */
      track_hdr[off + 1] = 0;                /* H - head */
      track_hdr[off + 2] = (byte)(0xC1 + s); /* R - sector ID */
      track_hdr[off + 3] = 2;                /* N - BPS */
      track_hdr[off + 4] = 0;                /* ST1 */
      track_hdr[off + 5] = 0;                /* ST2 */
    }

    f_write(&fp, track_hdr, 0x100, &bw);
    for (int s = 0; s < num_sectors; s++)
      f_write(&fp, sector_data, (UINT)sector_size, &bw);
  }

  f_close(&fp);
  return 0;
#else
  FILE *fp = fopen(path, "wb");
  if (!fp) return -1;
  fwrite(header, 1, 0x100, fp);

  byte track_hdr[0x100];
  byte sector_data[512];
  memset(sector_data, 0xE5, sizeof(sector_data));

  for (int t = 0; t < num_tracks; t++) {
    memset(track_hdr, 0, sizeof(track_hdr));
    memcpy(track_hdr, "Track-Info\r\n", 12);
    track_hdr[0x10] = (byte)t;
    track_hdr[0x11] = 0;
    track_hdr[0x14] = 2;
    track_hdr[0x15] = (byte)num_sectors;
    track_hdr[0x16] = 0x4E;
    track_hdr[0x17] = 0xE5;

    for (int s = 0; s < num_sectors; s++) {
      int off = 0x18 + s * 8;
      track_hdr[off + 0] = (byte)t;
      track_hdr[off + 1] = 0;
      track_hdr[off + 2] = (byte)(0xC1 + s);
      track_hdr[off + 3] = 2;
    }

    fwrite(track_hdr, 1, 0x100, fp);
    for (int s = 0; s < num_sectors; s++)
      fwrite(sector_data, 1, (size_t)sector_size, fp);
  }

  fclose(fp);
  return 0;
#endif
}
