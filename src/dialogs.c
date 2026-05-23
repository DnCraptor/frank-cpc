#ifdef PICO_BUILD
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ff.h"
#include "dialogs.h"

char buf[255];
char cmd[255];
int PassDriveSelect;

static FIL g_dialog_file;

/* When non-empty, SelectDiskFile() uses this path instead of scanning. */
static char g_pending_disk_path[256] = "";

void SetPendingDiskPath(const char *path) {
    snprintf(g_pending_disk_path, sizeof(g_pending_disk_path), "%s", path);
}

static int open_selected_disk(const char *path, int *wrprotect) {
    if (f_open(&g_dialog_file, path, FA_READ | FA_WRITE) == FR_OK) {
        *wrprotect = 0;
        return (int)(uintptr_t)&g_dialog_file;
    }
    if (f_open(&g_dialog_file, path, FA_READ) == FR_OK) {
        *wrprotect = 1;
        return (int)(uintptr_t)&g_dialog_file;
    }
    return -1;
}

int SelectDiskFile(char *filename, int *DrvNum, int *WrProtect) {
    /* UI-selected path takes priority */
    if (g_pending_disk_path[0]) {
        int fid = open_selected_disk(g_pending_disk_path, WrProtect);
        snprintf(filename, 255, "%s", g_pending_disk_path);
        g_pending_disk_path[0] = '\0';
        if (fid > 0) {
            printf("Inserted disk[%d]: %s\n", *DrvNum, filename);
            return fid;
        }
        printf("Failed to open disk: %s\n", filename);
        return -1;
    }

    /* Fallback: try default filenames */
    const char *preferred = (*DrvNum == 0) ? "/cpc/disk/drivea.dsk" : "/cpc/disk/driveb.dsk";
    int fid = open_selected_disk(preferred, WrProtect);
    if (fid > 0) {
        snprintf(filename, 255, "%s", preferred);
        printf("Inserted disk[%d]: %s\n", *DrvNum, filename);
        return fid;
    }
    return -1;
}

int SetupDialog(void) { printf("Setup dialog not available on Pico build\n"); return 0; }
void InfoDialog(void) { printf("frank-cpc RP2350 build\n"); }
void FirstStartDialog(void) {}
void PrintCmdLinePars(void) { printf("Command-line UI not available on Pico build\n"); }
void SaveScreenImage(void) { printf("Screen save not available on Pico build\n"); }
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "cpc.h"
#include "defines.h"
#include "colors.h"
#include "io.h"
#include "rcfile.h"

char buf[255];
char cmd[255];
int  PassDriveSelect;


/*****************************************************/
/*** Show the copyright message in the text shell ***/
/***************************************************/
void TxtCopyRight (void) {
    /* If Tcl-Wish could not be started (may be because it is not installed) do a simple text help dialog */
    printf ("\n\n\n\n\n\n\n\n\n\n");
    system ("clear");
    printf ("             CPC4X - an AMSTRAD CPC emulator for UNIX/Linux and X11     \n");
    printf ("  -------------------------------------------------------------------------\n\n");
    printf ("   1999-2002\n");
    printf ("   GNU GENERAL PUBLIC LICENSE\n");
    printf ("   Ulrich Cordes                     Email:   ulrich.cordes@gmx.de\n");
    printf ("   Vor der Dorneiche 1            Homepage:   http://www.amstrad-cpc.de\n");
    printf ("   34317 HABICHTSWALD / Germany         or:   http://www.schneider-cpc.de\n\n");
    printf ("  Keys:\n  -----\n");
    printf ("  F3  - Inserts a disk image file and maps it as CPC drive A:\n");
    printf ("  F4  - Inserts a disk image file and maps it as CPC drive B:\n");
    printf ("  F8  - Resets the emulation (reset CPC)\n");
    printf ("  F9  - Close printer file and send it to the printer spooler\n");
    printf ("  F10 - Start the built-in Z80 debugger\n");
    printf ("  F12 - Exit emulation\n\n");
    printf ("  Please read HTML help document in the %s/html directory\n", InstallDir);
}


/*****************************************************************/
/*** Calls a TCL/Tk dialog for disc image file selection and   ***/
/*** returns the FileID, filename, drive num and writeprotect. ***/
/*****************************************************************/

int SelectDiskFile (char *filename, int *DrvNum, int *WrProtect, int *Compressed) {
  static char  fname[80];
  static int   ok, fid, l, i;
  static char  drvletter;
  FILE *fp;
  fid = -1;
  /* Try to start the TCL/Tk dialog */
  sprintf (fname, "%s/.dlgfile", WorkDirectory);
  if (*DrvNum==0) drvletter='A'; else drvletter='B';
  fp = fopen (fname , "w");
    fprintf (fp, "%s/%s", WorkDirectory, DiscDir [0]);
    fprintf (fp, "%s/%s", WorkDirectory, DiscDir [1]);
    fprintf (fp, "%c\n",  drvletter);
  fclose (fp);

  if (PassDriveSelect==1)
    sprintf (cmd, "wish %s/menus/dlg_filedirect_%s.tcl", InstallDir, Language);
  else
    sprintf (cmd, "wish %s/menus/dlg_file_%s.tcl", InstallDir, Language);

  ok = system (cmd);
  if (ok>=0 && ok<127) {
    /* If TCL/Tk is installed, get selected file, drive */
    /* and write protection from a dialog handle file   */
    fp = fopen (fname , "r");
    /* Get file open status (OK or CANCEL) */
    fscanf (fp, "%s", buf);
    if (buf[0]=='O') {
      /* Get filename */
      fscanf (fp, "%s", filename);

/*      l=strlen(filename);
      if (l>4) {
        Compressed = 1;
	if (filename[l-4] != '.') Compressed = 0;
	if ((filename[l-3] | 32) != 'z') Compressed = 0;
	if ((filename[l-2] | 32) != 'i') Compressed = 0;
	if ((filename[l-1] | 32) != 'p') Compressed = 0;
      }
      if (Compressed>0) {
      }
*/
      /* Get drive num */
      fscanf (fp, "%s", buf);
      if (buf[0]=='A') *DrvNum=0; else *DrvNum=1;
      /* print filename */
      if (!NoDebug)
        printf ("%c: %s\n",65 + *DrvNum,filename);

      /* Get write protection information */
      fscanf (fp, "%s", buf);
      if (buf[0]=='0') *WrProtect=0; else *WrProtect=1;
      fid = open (filename, O_RDWR);
    }
  }
  else {
    /* If TCL/Tk is not installed do a text dialog via */
    /* stdout to select a disc image file.             */
    ok = 0;
    while (!ok) {
      system ("clear");
      printf ("\nInsert Disk in %c:\n", 65+DrvNum);
      printf ("-----------------\n\n");
      sprintf (cmd,"ls %s/disc -F", WorkDirectory);
      system (cmd);
      printf ("\nPlease enter a filename without [.dsk]:\n ");
      fscanf (stdin, "%s", fname );

      /***********************************************/
      /** Dateiname aus der Eingabe in die Struktur **/
      /** �bernehmen und Datei �ffen.               **/
      /***********************************************/
      sprintf (filename, "%s/disc/%s.dsk", WorkDirectory, fname);
      fid = open (filename, O_RDWR);
      if (fid<0) {
        printf ("File not found, try again (y/n)\n");
        scanf (" %c", fname);
        if (fname[0]=='N' || fname[0]=='n') ok=TRUE;
      }
      else
        ok = TRUE;
    }
    while (XEventsQueued (mydisplay, QueuedAfterReading) > 0)
      XNextEvent (mydisplay, &myevent);
  }
  XSetInputFocus (mydisplay, mywindow,0 ,0);
  return fid;
}


/*********************************/
/** Calls a TCL/Tk setup dialog **/
/*********************************/

int SetupDialog (void) {

  WriteRcFile();
  sprintf (cmd, "wish %s/menus/dlg_setup_%s.tcl", InstallDir, Language);
  system (cmd);

  InitCPC (0);
  printf ("setup dlg closed\n");

  //XSetInputFocus (mydisplay, mywindow,0 ,0);
  return 1;
}


/*********************************/
/** Calls a TCL/Tk info dialog  **/
/*********************************/

void InfoDialog (void) {
  FILE *fp;
  char txt[80];
  int ok, i, x;

  /* Try to start Tcl-Wish for the dialog */
  sprintf (cmd, "wish %s/menus/dlg_help.tcl",InstallDir);
  ok = system (cmd);

  /* If successfull test if user has selected READ HELP button */
  if (ok>=0 && ok<127) {
    sprintf (cmd,"%s/.dlghelp", WorkDirectory);
    fp = fopen(cmd, "r");
    if (fp != NULL) {
      fgets (txt, 10, fp);
      if (strncmp(txt, "CLOSE", 5)) {
        /* Open helpfile with a browser */
        fgets(txt, 10, fp);
        i = atoi (txt);
        switch (i) {
          case 1:
            sprintf (cmd, "konqueror %s/html/cpc4x_%s.html &", InstallDir, Language);
            break;
          case 2:
            sprintf (cmd, "netscape %s/html/cpc4x_%s.html &", InstallDir, Language);
            break;
          case 3:
            fgets (txt,80,fp);
            for (x=0; x<79; x++) if (txt[x]<32) txt[x]='\0';
            sprintf (cmd, "%s %s/html/cpc4x_%s.html &", txt, InstallDir, Language);
            printf ("%s\n",cmd);
            break;
        }
        sprintf (txt,"%s/html", InstallDir);
        chdir (txt);
        system (cmd);
        chdir (WorkDirectory);
      }
      fclose (fp);
    }
  }
  else
    TxtCopyRight ();
}


/***************************************************/
/*** Calls a TCL/Tk dialog 'dlg_firststert.tcl'  ***/
/***************************************************/

void FirstStartDialog (void) {
  int ok;
  sprintf (cmd, "wish %s/menus/dlg_firststart.tcl",InstallDir);
  ok = system (cmd);
}


/*******************************************************/
/*** Shows a help about the command line parameters  ***/
/*******************************************************/

void PrintCmdLinePars(void) {
  TxtCopyRight ();
  printf ("  \n\nUse command:  cpc [options]\n\n");
  printf ("  Options\n");
  printf ("    -noinfo                       Starts the emulator without splash window\n");
  printf ("    -cpc464, -cpc664, -cpc6128    Emulate a CPC 464, 664 or 6128\n");
  printf ("    -mem64, -mem128, -mem576      Use 64k, 128k or 576k RAM\n");
  printf ("    -ger, -eng                    Dialogs in german or english language\n");
  printf ("    -color, -mono                 Show a color or a green screen\n");
  printf ("    -passdriveselect              Opens disk image file dialog without drive select dialog\n");
  printf ("    -help                         Display this help\n\n\n");
}


/*******************************************************/
/*** Shows a file input dialog for screen XPM file   ***/
/*******************************************************/
void SaveScreenImage(void) {
  int ok;
  FILE *fp;

  sprintf (cmd, "wish %s/menus/dlg_image_%s.tcl", InstallDir, Language);
  ok=system (cmd);
  /* If successfull test if user has selected READ HELP button */
  if (ok>=0 && ok<127) {
    sprintf (cmd,"%s/.dlgimage", WorkDirectory);
    fp = fopen(cmd, "r");
    if (fp != NULL) {
      /* Get filename */
      fscanf (fp, "%s", buf);
      SaveScreenAsXPM (buf);
    }
  }
}

#endif
