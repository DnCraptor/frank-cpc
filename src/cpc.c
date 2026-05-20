#ifdef PICO_BUILD
#include <stdio.h>
#include <string.h>
#include "pico/time.h"
#include "defines.h"
#include "Z80.h"
#include "mem.h"
#include "screen.h"
#include "io.h"
#include "keyboard.h"
#include "colors.h"
#include "printer.h"
#include "aysound.h"
#include "disc.h"
#include "dialogs.h"
#include "cpc.h"

char WorkDirectory[80];
Z80 cpu;
char AYRegister[16];
int CPUZyklenBisInt;
int IRQCount;
int ExitCPC;
char RCfilename[255];
char PrinterCmdLine[255];
int CPCMaxMem;
int CPCtype;
char ROMFile[8][80];
char DiscDir[2][80];
char Language[10];
int tmp1, tmp2, tmp3;
int NoDebug;
FILE *DebugFP;

void PatchZ80(register Z80 *R) { (void)R; }
void WriteRcFile(void) {}

void InitCPC(int Start) {
  (void)Start;
  CPCMaxMem = 128;
  CPCtype = 2;
  for (int i = 1; i <= 6; ++i) sprintf(ROMFile[i], "\n");
  sprintf(ROMFile[7], "amsdos.rom\n");
  sprintf(DiscDir[0], "disc\n");
  sprintf(DiscDir[1], "disc\n");
  MonoScreen = 0;
  Customer = 14;
  sprintf(Language, "eng");
  PrinterCmdLine[0] = 0;
}

word LoopZ80(register Z80 *R) {
  static int i;
  static uint64_t hb_last_us = 0;
  static uint32_t hb_frames = 0;
  static uint32_t hb_skips = 0;
  static uint32_t hb_max_work_us = 0;
  static uint32_t hb_max_redraw_us = 0;
  static uint32_t hb_redraw_count = 0;
  extern uint32_t g_frame_skips;
  (void)R;
  IRQCount++;
  if (SeekTrackTime > 0) SeekTrackTime -= 3.3333f;

  /* One CPC video frame = 6 CRTC interrupt periods (300Hz/50fps = 6). */
  if (IRQCount == 6) {
    uint64_t t_frame_start = time_us_64();

    cpc_ps2_feed_events();

    /* Palette update. */
    ChangeInk = FALSE;
    for (i = 0; i <= 13; i++) {
      if (AktInk[i] != Ink[i]) ChangeInk = TRUE;
      AktInk[i] = Ink[i];
    }
    AktInk[14] = Ink[14];
    AktInk[15] = Ink[15];
    if (Ink[16] != AktInk[16]) AktInk[16] = Ink[16];

    if (ChangeInk) {
      /* Palette changed — all pixels need recoloring, full redraw.
       * Exception: if all 16 inks are the same value the game is doing
       * VSYNC blanking (setting all inks to black to hide sprite updates).
       * Rendering with a blank palette would corrupt the display, so skip
       * the redraw and keep the previous frame on screen.  ChangeInk will
       * fire again when the real palette is restored → full redraw then. */
      int palette_blank = 1;
      for (i = 1; i <= 15; i++) {
        if (AktInk[i] != AktInk[0]) { palette_blank = 0; break; }
      }
      if (!palette_blank) {
        uint64_t t0 = time_us_64();
        RedrawScreenImage();
        uint32_t dt = (uint32_t)(time_us_64() - t0);
        if (dt > hb_max_redraw_us) hb_max_redraw_us = dt;
        hb_redraw_count++;
      }
    } else {
      /* Redraw only the rows that WrScreenMem dirtied this frame.
       * This clears each dirty row and re-renders it from current RAM,
       * guaranteeing no ghost pixels regardless of ScreenOffset changes. */
      uint64_t t0 = time_us_64();
      RedrawDirtyRows();
      uint32_t dt = (uint32_t)(time_us_64() - t0);
      if (dt > hb_max_redraw_us) hb_max_redraw_us = dt;
      if (dt > 0) hb_redraw_count++;
    }
    ChangeInk = FALSE;
    cpc_frame_present();
    IRQCount = 0;

    mix_notes(AYRegister);

    /* Measure work time before the sync wait. */
    uint32_t work_us = (uint32_t)(time_us_64() - t_frame_start);
    if (work_us > hb_max_work_us) hb_max_work_us = work_us;

    cpc_frame_sync();

    hb_frames++;
    uint32_t skips_now = g_frame_skips;
    uint32_t new_skips = skips_now - hb_skips;
    hb_skips = skips_now;

    uint64_t now_us = time_us_64();
    if (hb_last_us == 0) hb_last_us = now_us;
    if (now_us - hb_last_us >= 1000000u) {
      printf("[HB] fps=%lu  work_max=%lums  redraw=%lums(n=%lu)  skips=%lu\n",
             (unsigned long)hb_frames,
             (unsigned long)(hb_max_work_us / 1000u),
             (unsigned long)(hb_max_redraw_us / 1000u),
             (unsigned long)hb_redraw_count,
             (unsigned long)new_skips);
      hb_frames        = 0;
      hb_max_work_us   = 0;
      hb_max_redraw_us = 0;
      hb_redraw_count  = 0;
      hb_last_us       = now_us;
    }

    Pio_B = Pio_B | 1;
  } else {
    Pio_B = Pio_B & 254;
  }

  if (ExitCPC == TRUE) return INT_QUIT;
  return INT_IRQ;
}

int RunZ80_cpc(void) {
  ExitCPC = FALSE;
  ResetZ80(&cpu);
  cpu.TrapBadOps = 1;
  cpu.Trace = 0;

  while (ExitCPC == FALSE) {
    z80_arm_exec(CPUZyklenBisInt);
    word result = LoopZ80(&cpu);
    if (result == INT_QUIT) break;
    IntZ80(&cpu, result);
  }
  return 0;
}
#else
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

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "defines.h"
#include "Z80.h"
#include "mem.h"
#include "screen.h"
#include "timer.h"
#include "io.h"
#include "keyboard.h"
#include "colors.h"
#include "printer.h"
#include "aysound.h"
#include "disc.h"
#include "installdir.c"
#include "usersubdir.c"
#include "dialogs.h"

char              WorkDirectory [80];

char              XCPCTitel[] = "CPC-Emulator for X";
char              text[10];

Display           *mydisplay;
Drawable          mywindow;
XWindowAttributes mywindowattributes;
GC                mygc;
int               myscreen;
XEvent            myevent;
KeySym            mykey;
XSizeHints        myhint;
unsigned long     myforeground, mybackground;

unsigned int      depth;
int               format;
unsigned int      width, height;
int               bitmap_pad;
XImage            *myimage;
XRectangle        BorderRectangles[4];
XRectangle        DiscDrives;

Z80               Z80Register;
char              AYRegister[16];
int               CPUZyklenBisInt;
int               IRQCount;
int               ExitCPC;

char              RCfilename[255];  /* Full file name with path of .cpc4xrc-file */
char              PrinterCmdLine[255];

int               CPCMaxMem;
int               CPCtype;
char              ROMFile[8][80];
char              DiscDir [2][80];
char              Language [10];
int               tmp1, tmp2, tmp3;
int               NoDebug;
FILE              *DebugFP;

void PatchZ80     (register Z80 *R) {}

/*******************************************************/
/** Writes the .cpc4xrc-file which keeps the most     **/
/** important information aubout memory- and disc-    **/
/** configuration.                                    **/
/*******************************************************/
void WriteRcFile (void) {
  FILE *fp;
  int i;
  /* Write the current configuration to the .cpc4x file */
  fp = fopen(RCfilename, "w");
    /* Installation and working directories */
    fprintf (fp, "%s\n", InstallDir);
    fprintf (fp, "%s\n", WorkDirectory);

    /* 0 for CANCEL button */
    fprintf (fp, "%d\n", 0);

    /* CPC memory (64k, 128k or 576k */
    fprintf (fp, "%d\n", CPCMaxMem);

    /* Put the 7 upper ROM files names */
    for (i=1; i<=7; i++)
      fputs (ROMFile[i], fp);

    /* Put the working sub directories for dirve A and B */
    fputs (DiscDir[0], fp);
    fputs (DiscDir[1], fp);

    /* Put if color or mono screen is emulated now */
    fprintf (fp, "%d\n", MonoScreen);

    /* Put the current customer (AMSTRAD, SCHNEIDER, ...) */
    fprintf (fp, "%d\n", Customer);

    /* Put the current used language */
    fputs (Language, fp);

    /* Put the current CPC type (464, 664 or 6128) */
    fprintf (fp, "\n%d\n", CPCtype);

    /* Put the printer command line */
    fputs (PrinterCmdLine, fp);

    /* Put if CR, LF and FF should be printed ) */
    fprintf (fp, "\n%d\n", NoCR);
    fprintf (fp, "%d\n", NoLF);
    fprintf (fp, "%d\n", NoFF);

  fclose(fp);
}



/*******************************************************/
/** Reads the .cpc4xrc-file which keeps the most      **/
/** important informations of the memory- and disc-   **/
/** configuration.                                    **/
/**                                                   **/
/** Use:  INITCPC(1) after starting cpc4x and         **/
/**       INITCPC(0) after changing configuration     **/
/**       with F7 key.                                **/
/*******************************************************/
void InitCPC (int Start) {
  FILE *fp;
  char txt[80];
  int i, ok;

  /* Get the full file name with path of .cpc4xrc file */
  sprintf (RCfilename, "%s/.cpc4xrc", getenv("HOME"));
  fp = fopen(RCfilename, "r");
  /* if .cpc4xrc file is present in the home directory */
  /* then read the configuration from it               */
  if (fp!=NULL) {
    /* Installation and work directory:                 */
    /* Read and ignore them, both are hard coded        */
    /* in CPC4X source and will be used by the dialoges */
    fgets(txt,79,fp);
    fgets(txt,79,fp);
    /* After configuration dialog the "applay and reset ..." or */
    /* the "CANCEL" button has been selected                    */
    fgets(txt,10,fp);
    ok = atoi (txt);

    /* If "Apply and reset ..." button has been selected, change configuration */
    if (ok || Start) {
      /* CPC memory (64k, 128k or 576k */
      fgets(txt,10,fp);
      CPCMaxMem = atoi (txt);

      /* Get the 7 upper ROM files */
      for (i=1; i<=7; i++)
        fgets (ROMFile[i], 99, fp);

      /* Get the working sub directories for dirve A and B */
      fgets (DiscDir[0], 255,fp);
      fgets (DiscDir[1], 255,fp);

      /* Get if color or mono screen has to be emulate */
      fgets(txt,10,fp);
      MonoScreen = atoi (txt);

      /* Get the customer (AMSTRAD, SCHNEIDER, AIWA, .... */
      fgets(txt,10,fp);
      Customer = atoi (txt);

      /* Get the working language */
      fgets (Language, 10, fp);
      Language [3] = '\0';      /* Use 3 letters, e.g. ger, eng, fra, esp, ... */

      /* Get the CPC type (464, 664, 6128) */
      fgets(txt,10,fp);
      CPCtype = atoi (txt);

      /* Get the printer command line and remove char 10 at the end of line */
      fgets(PrinterCmdLine, 254, fp);
      i = strlen(PrinterCmdLine);
      if (i>0 && PrinterCmdLine[i-1]<32) PrinterCmdLine[i-1] = '\0';

      /* Get if CR, LF and FF should be printed */
      fgets(txt,10,fp);
      NoCR = atoi (txt);
      fgets(txt,10,fp);
      NoLF = atoi (txt);
      fgets(txt,10,fp);
      NoFF = atoi (txt);

    }
    fclose (fp);
  }

  /* If the .cpc4xrc file is not present or an error has been detected */
  /* use a standard configuration (CPC6128 only)                        */
  if (fp==NULL || (CPCMaxMem!=64 && CPCMaxMem!=128 && CPCMaxMem!=576)) {
    CPCMaxMem=128;
    CPCtype = 2;
    for (i=1; i<=6; i++) sprintf (ROMFile[i], "\n");
    sprintf (ROMFile[7], "amsdos.rom\n");
    sprintf (DiscDir[0], "disc\n");
    sprintf (DiscDir[1], "disc\n");
    MonoScreen = 0;
    Customer = 14;
    sprintf (Language, "eng");
    sprintf (PrinterCmdLine, "lpr -Praw");
  }

  /* Only after changing setup, memory has to  */
  /* be reinitalize, not after program start   */
  if (ok && !Start) {
    InitIO();
    InitColors ();
    InitMem ();
    ResetZ80 (&Z80Register);
    ResetFDC();
  }
}


/*******************************************************/
/**                                                   **/
/** LoopZ80:                                          **/
/**                                                   **/
/** This function will be called by the Z80 emulator  **/
/** every 13333 CPU cycles. This is equal to the      **/
/** 300 Hz interrupt of the original CPC.             **/
/**                                                   **/
/** A counter filters every 6th interrupt for the     **/
/** 50 Hz interupt. In this interrupt the event       **/
/** handler of X11 will be checked and the emulation  **/
/** be stopped until a 50 Hz UNIX timer give a        **/
/** signal. This is the real time synchronisation.    **/
/**                                                   **/
/** If the CPC screen has been changed, the screen    **/
/** image will be updated in the X11 window (max.     **/
/** 25 times per second).                             **/
/**                                                   **/
/** The return value is either INT_IRQ or INT_QUIT,   **/
/** if the emulator has been ended with the F12 key.  **/
/**                                                   **/
/*******************************************************/

word LoopZ80 (register Z80 *R) {
  static int i;
  IRQCount ++;
  if (SeekTrackTime>0) SeekTrackTime -= 3.3333;
  if (IRQCount==6 || IRQCount==12 || IRQCount==18 || IRQCount==24) {
    while (XEventsQueued (mydisplay, QueuedAfterReading) > 0) {
      XNextEvent (mydisplay, &myevent);

      switch (myevent.type) {
        case Expose:
          if (LineOffset>0) {
            XPutImage (mydisplay, mywindow, mygc,myimage,0,LineOffset,8,8,640,400-LineOffset);
            XPutImage (mydisplay, mywindow, mygc,myimage,0,0,8,408-LineOffset,640,LineOffset);
          }
          else
            XPutImage (mydisplay, mywindow, mygc,myimage,0,0,8,8,640,400);

          //XSetForeground(mydisplay, mygc, ToolBarColor);
          //XFillRectangle(mydisplay, mywindow, mygc,0,416,656,28);
          XSetForeground(mydisplay,mygc,PixColor[Ink[16]]);
          XFillRectangles (mydisplay, mywindow, mygc, BorderRectangles,4);
          ScreenModified = FALSE;
          break;

        case EnterNotify:
          if (((XCrossingEvent *)&myevent)->detail != NotifyInferior)
             XAutoRepeatOff (mydisplay);
          break;

        case LeaveNotify:
          if (((XCrossingEvent *)&myevent)->detail != NotifyInferior)
             XAutoRepeatOn (mydisplay);
          break;

        case DestroyNotify :
            printf ("Destroy!!\n");
            XAutoRepeatOn(mydisplay);
            ExitCPC = TRUE;
            break;

        case ConfigureNotify:
            XResizeWindow (mydisplay, mywindow,656,416);  //444);
            break;

        case MappingNotify: break;

        case FocusIn:
            XAutoRepeatOff(mydisplay);
            InitKeyboard();
            break;

        case FocusOut:
            XAutoRepeatOn(mydisplay);
            break;

        case ButtonPress:
            break;

        case KeyPress:
            i=XLookupString (&myevent.xkey, text, 2, &mykey, 0);
            //printf ("%d\n",mykey);
            CPCKeyPress (mykey);
            break;

        case KeyRelease:
            i=XLookupString (&myevent.xkey, text, 2, &mykey, 0);
            //printf ("!%d\n",mykey);
            CPCKeyRelease (mykey);
            break;

      } /* Event-switch */
    }

    /*************************************************************/
    /** Update 12.5 times per second the colors from Gate-Array **/
    /** in the CPC screen image and redraw this image           **/
    /*************************************************************/
    if (IRQCount==24) {
      ChangeInk == FALSE;
      for (i=0; i<=13; i++) {
        if (AktInk[i]!=Ink[i]) ChangeInk=TRUE;
        AktInk[i]=Ink[i];
      }
      AktInk[14]=Ink[14];
      AktInk[15]=Ink[15];

      /************/
      /** Border **/
      /************/
      if (Ink[16]!= AktInk[16]) {
        AktInk[16]=Ink[16];
        XSetForeground(mydisplay,mygc,PixColor[Ink[16]]);
        XFillRectangles (mydisplay, mywindow, mygc, BorderRectangles,4);
      }
      if (ChangeInk==TRUE) RedrawScreenImage();
      ChangeInk = FALSE;
    }

    /**************************************************************/
    /** If the CPC screen has been changed, it must be send to   **/
    /** the X11 window (max. 12.5 times per second)              **/
    /**************************************************************/
    if (ScreenModified && IRQCount==24){
      if (LineOffset>0) {
        XPutImage (mydisplay, mywindow, mygc,myimage,0,LineOffset,8,8,640,400-LineOffset);
        XPutImage (mydisplay, mywindow, mygc,myimage,0,0,8,408-LineOffset,640,LineOffset);
      }
      else
        XPutImage (mydisplay, mywindow, mygc,myimage,0,0,8,8,640,400);
      ScreenModified = FALSE;
    }

    /***************************************************************/
    /** Real time synchronisation with 50 Hz Linux timer          **/
    /** Wait for the 50 Hz timer with each emulated fast ticker   **/
    /** interrupt.                                                **/
    /***************************************************************/
    mix_notes(AYRegister);
    do {
       XFlush (mydisplay); // i = XEventsQueued (mydisplay, QueuedAfterReading);
    } while (TimerCount < 1);
    TimerCount = 0;

    Pio_B = Pio_B | 1; // *** set VSYNC
    if (IRQCount == 24) IRQCount=0;
  }
  else
    Pio_B = Pio_B & 254;  // *** reset VSYNC

  if (ExitCPC == TRUE) return INT_QUIT;
  else return INT_IRQ;
}

/*******************************************************/
/** The main program                                  **/
/**                                                   **/
/**                                                   **/
/**                                                   **/
/**                                                   **/
/**                                                   **/
/*******************************************************/

main (argc,argv) int argc;char **argv; {
  int i, noinfo, foundargs;
  char text [10];

  char cmd[255];

  /* Build the full working directory name */
  sprintf (WorkDirectory,"%s%s", getenv("HOME"), UserSubDir);

#ifdef CPCLOCAL
  sprintf (InstallDir,"%s%s", getenv("HOME"), UserSubDir);
#endif

  DebugFP=stdout;
  /* Test if the working directory exists. If not, create it */
  /* with some different sub directories and copy the most   */
  /* important files to them.                                */

  if (chdir(WorkDirectory) < 0) {
    FirstStartDialog();
    mkdir (WorkDirectory, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    sprintf (cmd, "%s/rom", WorkDirectory);
    mkdir (cmd, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    sprintf (cmd, "%s/disc", WorkDirectory);
    mkdir (cmd, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    sprintf (cmd, "%s/icons", WorkDirectory);
    mkdir (cmd, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    sprintf (cmd, "%s/prn", WorkDirectory);
    mkdir (cmd, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

    sprintf (cmd, "cp %s/rom/* %s/rom", InstallDir, WorkDirectory);
    system (cmd);
    sprintf (cmd, "cp %s/disc/* %s/disc", InstallDir, WorkDirectory);
    system (cmd);
    sprintf (cmd, "cp %s/icons/* %s/icons", InstallDir, WorkDirectory);
    system (cmd);
  }

  InitCPC (1);

  noinfo = FALSE;

  /* Get command line arguments */
  foundargs = 0;
  if (argc > 1) {
    for (i=1; i<argc; i++) {
      if (strncmp ("-noinfo", argv[i],7)== 0) {
        foundargs ++;
        noinfo=TRUE;
      }
      if (strncmp ("-cpc464", argv[i],7)== 0) {
        foundargs ++;
        CPCtype = 0;
      }
      if (strncmp ("-cpc664", argv[i],7)== 0) {
        foundargs ++;
        CPCtype = 1;
      }
      if (strncmp ("-cpc6128", argv[i],8)== 0) {
        foundargs ++;
        CPCtype = 2;
      }
      if (strncmp ("-mem64", argv[i],6)== 0) {
        foundargs ++;
        CPCMaxMem = 64;
      }
      if (strncmp ("-mem128", argv[i],7)== 0) {
        foundargs ++;
        CPCMaxMem = 128;
      }
      if (strncmp ("-mem576", argv[i],7)== 0) {
        foundargs ++;
        CPCMaxMem = 576;
      }
      if (strncmp ("-ger", argv[i],4)== 0) {
        foundargs ++;
        sprintf (Language,"ger");
      }
      if (strncmp ("-eng", argv[i],4)== 0) {
        foundargs ++;
        sprintf (Language,"eng");
      }
      if (strncmp ("-fra", argv[i],4)== 0) {
        foundargs ++;
        sprintf (Language,"fra");
      }
      if (strncmp ("-mono", argv[i],5)== 0) {
        foundargs ++;
        MonoScreen = 32;
      }
      if (strncmp ("-color", argv[i],6)== 0) {
        foundargs ++;
        MonoScreen = 0;
      }
      NoDebug = FALSE;
      if (strncmp ("-nodebug", argv[i],8)== 0) {
        foundargs ++;
        NoDebug = TRUE;
      }
      NoSound = 0;
      if (strncmp ("-nosound", argv[i],8)== 0) {
        foundargs ++;
        NoSound = 1;
      }
      PassDriveSelect = 0;
      if (strncmp ("-passdriveselect", argv[i],16)== 0) {
        foundargs ++;
        PassDriveSelect = 1;
      }
    }
  }
  if (foundargs==(argc-1)) {
    if (noinfo==FALSE) InfoDialog();
    WriteRcFile ();

    CPUZyklenBisInt = 13333;
    RAM = NULL;
    for (i=0;i<=7; i++)
      UpperROM[i]=NULL;
    
    if (InitMem()) {
      InitDisc ();
      InitPrinter ();

      ExitCPC = FALSE;
      ResetZ80 (&Z80Register);
      TimerSignal(50);

      Z80Register.IPeriod=CPUZyklenBisInt;
      Z80Register.TrapBadOps = 1;
      Z80Register.Trace = 0;

      IRQCount = 0;

      mydisplay = XOpenDisplay (":0.0");
      myscreen  = DefaultScreen (mydisplay);

      myhint.x=200;
      myhint.y=300;
      myhint.width=656;
      myhint.height=416; //444;
      myhint.flags=PPosition | PSize;

      /*********************************************/
      /** Prepare CPC screen image for X11 window **/
      /*********************************************/
      width=640;
      height=400;
      format = ZPixmap;
      depth = DefaultDepth (mydisplay, myscreen);
      bitmap_pad=8;
      if (depth > 8) bitmap_pad=8;

      myimage=XCreateImage (mydisplay, DefaultVisual (mydisplay,myscreen),
                            depth, format, 0, 0, width, height, bitmap_pad,0);

      myimage->data = (void *)malloc (myimage->bytes_per_line * height);

      /***************************************/
      /** The four CPC border rectangles    **/
      /***************************************/
      BorderRectangles[0].x=0;
      BorderRectangles[0].y=0;
      BorderRectangles[0].width = 656;
      BorderRectangles[0].height= 8;
      BorderRectangles[1].x=0;
      BorderRectangles[1].y=8;
      BorderRectangles[1].width =8;
      BorderRectangles[1].height=400;
      BorderRectangles[2].x=648;
      BorderRectangles[2].y=8;
      BorderRectangles[2].width =8;
      BorderRectangles[2].height=400;
      BorderRectangles[3].x=0;
      BorderRectangles[3].y=408;
      BorderRectangles[3].width =656;
      BorderRectangles[3].height=8;


      /*******************************************/
      /** Initialize RGB colors and draw X11    **/
      /** window with blue background           **/
      /*******************************************/
      InitColors();
      if (MonoScreen) mybackground = PixColor[36]; else mybackground = PixColor[4];
      myforeground = BlackPixel (mydisplay, myscreen);
      mywindow = XCreateSimpleWindow (mydisplay, DefaultRootWindow (mydisplay),
                                      myhint.x, myhint.y,
                                      myhint.width, myhint.height, 5,
                                      myforeground, mybackground);

      XSetStandardProperties (mydisplay, mywindow, XCPCTitel, XCPCTitel, None, argv, argc, &myhint);

      mygc = XCreateGC (mydisplay, mywindow, 0, 0);
      XSetBackground(mydisplay, mygc, mybackground);

      XSetForeground(mydisplay, mygc, myforeground);

      XSelectInput (mydisplay, mywindow,
                    KeyPressMask    |
                    KeyReleaseMask  |
                    ExposureMask    |
                    FocusChangeMask |
                    EnterWindowMask |
                    LeaveWindowMask
                    );

      XMapRaised (mydisplay, mywindow);
      XAutoRepeatOff (mydisplay);
      XGetWindowAttributes(mydisplay,mywindow, &mywindowattributes);
      InitScreen ();

      InitIO();
      InitKeyboard();
      init_dsp();

      RunZ80 (&Z80Register);

      ExitDisc();
      DelTimer();
      ExitMem ();
      ClosePrinter ();
      if (PrinterFileNo>0)
        for (i=1; i<=PrinterFileNo; i++) {
          sprintf (cmd, "%s/%06i.prn", WorkDirectory, i);
          remove (cmd);
        }
      XDestroyImage (myimage);
      XAutoRepeatOn (mydisplay);
      XFreeGC (mydisplay, mygc);
      XDestroyWindow (mydisplay, mywindow);
      XCloseDisplay (mydisplay);
      exit_dsp();
    }
  }
  else
    PrintCmdLinePars();
  exit (0);
}



#endif
