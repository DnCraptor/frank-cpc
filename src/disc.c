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
        TrackDataStart = ((FDCCommand[4] & 0x0F)-1) << 9;
        FDCDataLength = (dsk[FDCCurrDrv].Tracks[TrackIndex].BPS * dsk[FDCCurrDrv].Tracks[TrackIndex].SPT) << 9;
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
          TrackDataStart = ((FDCCommand[4] & 0x0F)-1) << 9;
          FDCDataLength = 512 + ((FDCCommand[4]-FDCCommand[6])<<9);
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
        TrackDataStart = ((FDCCommand[4] & 0x0F)-1) << 9;
        FDCDataLength = 512 + (((FDCCommand[4] & 0xF) - (FDCCommand[6] & 0xF))<<9);
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

    //case 9:
    //  /* Write deleted data */
    //  break;

    case 10:
      /* read ID of next sector */
      FDCCurrDrv = FDCCommand[1] & 3;
      FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
      st0_SE = 0x00;
      st0_IC = 0x00;
      GetRes7();
      if (dsk[FDCCurrDrv].fid > 0) {
        TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
        FDCResult[5] = dsk[FDCCurrDrv].Tracks[TrackIndex].sector[0].sector;   /* 0x01=IBM, 0x41=Data, 0xC1=System */
      }
      break;

    //case 12:
    //  /* Read deleted data */
    //  break;

    // case 13:
    //  /* Format a track */
    //  FDCCurrDrv = FDCCommand[1] & 3;
    //   FDCCurrSide[FDCCurrDrv] = (FDCCommand[1] >> 2) & 1;
    //  if (dsk[FDCCurrDrv].fid > 0) {
    //    TrackIndex = FDCCurrTrack[FDCCurrDrv] * dsk[FDCCurrDrv].DiskHeader.nbof_heads + FDCCurrSide[FDCCurrDrv];
    //    TrackDataStart = ((FDCCommand[4] & 0x0F)-1) << 9;
    //    FDCDataLength = (dsk[FDCCurrDrv].Tracks[TrackIndex].BPS * dsk[FDCCurrDrv].Tracks[TrackIndex].SPT) << 9;
    //    dsk[FDCCurrDrv].Tracks[TrackIndex].DiscData[TrackDataStart + FDCDataPointer];
    //  }
    //  GetRes7();
    //  break;

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
    for (i=0 ; i<1; i++) {
      FDCCurrTrack[i] = 0;
      FDCWrProtect[i] = FALSE;
    }
    for (i=0 ; i<7; i++) FDCCommand[i]=0;
    SeekTrack = FALSE;
    SeekTrackTime = 0.0;
    SeekTrackDrive = 0;
    st0_EC = 0;

}

/*********************************************************************/
/**                                                                 **/
/*********************************************************************/
void InitDisc (void) {
    int i;
    dsk[0].TrackSize = 194560;
    dsk[0].fid       = -1;   // No file loaded now
    dsk[0].Tracks    = (void *)CPC_LARGE_ALLOC (194560);
    dsk[1].TrackSize = 778240;
    dsk[1].fid       = -1;   // No file loaded now
    dsk[1].Tracks    = (void *)CPC_LARGE_ALLOC (778240);
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
#ifdef PICO_BUILD
  /* Disk images are read-only on Pico — skip write-back to prevent
   * SD card corruption. */
  printf("WriteDskImage: skipped (read-only mode)\n");
  dsk[DrvNum].fid = -1;
  return;
#endif
  if (dsk[DrvNum].fid>0) {
    printf ("Write disc image: %s", dsk[DrvNum].ImageName);
    dsk[DrvNum].fid = open (dsk[DrvNum].ImageName, O_RDWR);
    /***************************************/
    /** Disk-Header speichern (256 Bytes) **/
    /***************************************/
    if (dsk[DrvNum].fid>0) {
      write ( dsk[DrvNum].fid , &dsk[DrvNum].DiskHeader  , 0x100);

      /***********************************/
      /** Tracks und Daten speichern    **/
      /***********************************/
      write ( dsk[DrvNum].fid , dsk[DrvNum].Tracks , dsk[DrvNum].TrackSize );

      close ( dsk[DrvNum].fid );
      printf(" .... ok!");
    }
    printf ("\n");
  }
  dsk[DrvNum].fid=-1;
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
  static byte     track,sector,sectorID;
  /**************************************************/
  /** Select a file via file dialog                **/
  /**************************************************/
  fid = SelectDiskFile (fname , &DrvNum, &wrprotect);

  /*************************************************/
  /** Wenn Datei ge�fnet werden konnte, dann den **/
  /** Disk-header, alle Track-header (z.B. 40)    **/
  /** sowie natrlich die Daten lesen.            **/
  /*************************************************/
  if ( fid > 0 ) {
    /**************************************************/
    /** Prfen, ob bereits eine Image-Datei ge�fnet **/
    /** ist. Wenn ja, dann diese schlie�n           **/
    /**************************************************/
    WriteDskImage (DrvNum);

    /**************************************************/
    /** Dateiname, FileID und Schreibschtuz-Flag in  **/
    /** die Struktur bnehmen.                       **/
    /**************************************************/
    dsk[DrvNum].fid = fid;
    sprintf (dsk[DrvNum].ImageName, "%s", fname);
    FDCWrProtect[DrvNum] = wrprotect;

    /***********************************/
    /** Disk-Header lesen (256 Bytes) **/
    /***********************************/
    read ( dsk[DrvNum].fid , &dsk[DrvNum].DiskHeader  , 0x100);

    /*************************************************/
    /** Anzahl der zu lesenden Bytes fr die Tracks **/
    /** berechnet sich aus Tracks*Heads*(256+4608)  **/
    /*************************************************/
    readbytes = 0x1300 * dsk[DrvNum].DiskHeader.nbof_tracks * dsk[DrvNum].DiskHeader.nbof_heads;

    /*************************************************/
    /** Wenn der fr die Track-Header vom System    **/
    /** angeforderte Speicherbereich zu klein ist,  **/
    /** dann diesen frei geben und dies vermerken.  **/
    /*************************************************/
    if ( dsk[DrvNum].TrackSize < readbytes ) {
        free (dsk[DrvNum].Tracks);
        dsk[DrvNum].TrackSize = 0;
    }

    /**************************************************/
    /** Wenn fr die Track-Header kein Speicher vor- **/
    /** handen ist oder dieser zuvor freigegeben     **/
    /** wurde, weil er zu klein war, dann erforder-  **/
    /** lichen Speicher vom System anfordern.        **/
    /**************************************************/
    if ( dsk[DrvNum].TrackSize == 0 ) {
        printf ("0 %7d\n", readbytes);
        dsk[DrvNum].Tracks = (void *)CPC_LARGE_ALLOC (readbytes);
        printf ("1\n");
    }
    dsk[DrvNum].TrackSize = readbytes;
    //read ( dsk[DrvNum].fid , dsk[DrvNum].Tracks , readbytes);

    /*******************************************************/
    /** Some disk images have interleaving sectors, so we **/
    /** need to read tracks and sectors one by one and    **/
    /** re-arrange the sectors within each track.         **/
    /*******************************************************/
    for (track=0; track < dsk[DrvNum].DiskHeader.nbof_tracks * dsk[DrvNum].DiskHeader.nbof_heads; ++track) {

      /* Where does this track start? */
      struct track_type* trackHeader = & dsk[DrvNum].Tracks[track];

      /* Read track header */
      read (dsk[DrvNum].fid, trackHeader, 0x100);

      /*
       * Read sectors and re-arrange. We assume that the first sector
       * in the disk image is the one with the smallest ID within the track
       */
      for (sector=0; sector<9; sector++)
        read (dsk[DrvNum].fid, (void*)trackHeader + 0x100 + 0x200 * ( trackHeader->sector[sector].sector - trackHeader->sector[0].sector ), 0x200);

    }

    close ( dsk[DrvNum].fid );
  }
  int_cause=4;
}
