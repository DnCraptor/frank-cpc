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

#ifndef DISK_H

#define DISK_H 1

#include "Z80.h"
// typedef unsigned char uchar;

/*******************************/ 
/** disk header of size 0x100 **/
/*******************************/

struct disc_header_type {
  byte   tag[0x30];     /* 00-21  MV - CPC ... or EXTENDED CPC DSK File             */
                        /* 22-2F  unused (0)                                        */
  byte   nbof_tracks;   /* 30     number of tracks (40)                             */
  byte   nbof_heads;    /* 31     number of heads (1) 2 not yet supported by cpcemu */
  short  tracksize;	/*        short must be 16bit integer                       */
                        /* 32-33  tracksize (including 0x100 bytes header)          */
                        /*        9 sectors * 0x200 bytes each + header = 0x1300    */
  byte   unused[0xcc];  /* 34-FF  unused (0)                                        */
                        /*        For extended DSK: bytes 34-FF contain per-track   */
                        /*        size table (high byte of track size for each track)*/
}; 
 

/*************************************/
/* sector info, used in track header */ 
/*************************************/
struct sector_info_type {
  /* Sector-Information (for each sector):                                  */ 
  byte 	track;     /* 18+i      tracknumber       \                         */
  byte 	head;      /* 19+i      headnumber        | Sector-ID-Information   */
  byte 	sector;    /* 1A+i      sectornumber      |                         */
  byte 	BPS;       /* 1B+i      BPS               /                         */
  byte 	status1;   /* 1C+i      status 1 errorcode (0)                      */
  byte 	status2;   /* 1D+i      status 2 errorcode (0)                      */
  byte 	data_len_lo; /* 1E+i    Extended DSK: actual data length low byte   */
  byte 	data_len_hi; /* 1F+i    Extended DSK: actual data length high byte  */
}; 
 


/* track header of size 0x100 */
struct track_type {
  byte                      tag[0x10];         /* 00-0C    Track-Info\r\n                          */
                                               /* 0D-0F    unused (0)                              */
  byte                      track;             /* 10       tracknumber (0 to number-of-tracks - 1) */
  byte                      head;              /* 11       headnumber (0)                          */
  byte                      unused[2];         /* 12-13    unused (0)                              */
  
  /* Format-Track-Parameter: */ 
  byte                      BPS;               /* 14       BPS (bytes per sector) (2 for 0x200 Bytes)    */
  byte                      SPT;               /* 15       SPT (sectors per track) (9, max. 29 possible) */
  byte                      GAP3;              /* 16       GAP#3 Format (gap for formatting: 0x4E)       */
  byte                      filler;            /* 17       Filling-Byte (filler for formatting: 0xE5)    */
  struct sector_info_type   sector[29];
  byte                      DiscData [0x3000]; /* Up to 12KB data per track (covers extended DSK)        */
} ; 



struct DskImg {
  char                    ImageName [80];    // Filename
  int                     fid;               // Unix-file-ID (returned by OPEN
  struct disc_header_type DiskHeader;
  struct track_type       *Tracks;
  unsigned long           TrackSize;
  int                     extended;          // 1 = Extended DSK format
};

extern byte FloppyMotor;
extern word FDCPointer;
extern float SeekTrackTime;
extern struct DskImg dsk[2];

/* Return the data offset within DiscData[] for a given sector index in a track.
 * For standard DSK: sector_index * (128 << BPS).
 * For extended DSK: sum of actual data lengths of preceding sectors. */
unsigned long GetSectorDataOffset(int DrvNum, int trackIdx, int sectorIdx);

/* Return the actual data length for a given sector in a track. */
unsigned long GetSectorDataLength(int DrvNum, int trackIdx, int sectorIdx);

/* Find the sector index within a track whose sector-ID field (R) matches
 * the requested value. Returns -1 if not found. Starts searching from
 * startIdx and wraps around. */
int FindSectorByID(int DrvNum, int trackIdx, byte sectorID, int startIdx);

void InsertDisk (int DrvNum);
void WriteDskImage (int DrvNum);

/* Create a blank formatted DSK image at the given path.
 * Standard format: 40 tracks, 1 head, 9 sectors/track (C1-C9), 512 bytes/sector.
 * Returns 0 on success, -1 on failure. */
int CreateBlankDsk(const char *path);

void InitDisc (void);

void ExitDisc (void);
void ResetFDC(void);
void WriteFDCData (register byte Value);
byte ReadFDCData (void);
byte ReadFDCStatus (void);
#endif
