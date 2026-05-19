#ifdef PICO_BUILD
#include <stdio.h>
#include "printer.h"

FILE *PrinterFile = 0;
unsigned int PrinterFileNo = 0;
char PrinterFileName[255];
char NoCR, NoLF, NoFF;

void InitPrinter(void) {}
void ClosePrinter(void) {}
void PrintChar(char c) { (void)c; }
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

/* printer.c */
#include <stdio.h>
#include <stdlib.h>
#include "cpc.h"

#define cpcLF 10
#define cpcFF 12
#define cpcCR 13


FILE         *PrinterFile;
unsigned int PrinterFileNo;
char         PrinterFileName [255];
char         NoCR, NoLF, NoFF;


void InitPrinter (void) {
  PrinterFile = NULL;
  PrinterFileNo = 0;
}


void OpenPrinterFile (void) {
  PrinterFileNo ++;
  sprintf (PrinterFileName, "%s/%06i.prn", WorkDirectory, PrinterFileNo);
  PrinterFile = fopen(PrinterFileName, "w");
}


void ClosePrinter (void) {
  static char cmd [255];
  if (PrinterFile != NULL) {
    fclose (PrinterFile);
    PrinterFile = NULL;
    sprintf (cmd, "%s %s", PrinterCmdLine, PrinterFileName);
    system (cmd);
  }
}


void PrintChar (char c) {
  if (PrinterFile == NULL) OpenPrinterFile();
  switch (c) {
    case cpcLF:   if (!NoLF) fprintf (PrinterFile, "%c", c); break;
    case cpcCR:   if (!NoCR) fprintf (PrinterFile, "%c", c); break;
    case cpcFF:   if (!NoFF) fprintf (PrinterFile, "%c", c); ClosePrinter ();break;
    default:      fprintf (PrinterFile, "%c", c); break;
  }
}

#endif
