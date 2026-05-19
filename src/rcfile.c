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
#include "cpc.h"

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
  fclose(fp);
}
