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
#ifdef PICO_BUILD
#include "cpc_compat.h"
#endif
#include "cpc.h"
#include "Z80.h"
#include "mem.h"
#include "defines.h"
#include "screenpos.h"
#include "colors.h"

word HD6845Register[18];
byte HD6845RegisterPointer;

word ScreenAddr;
word ScreenBlock;
word ScreenBank;
word ScreenOffset;
unsigned int ScreenMode;
unsigned int RedrawScreen;
unsigned int ImageOffset;
unsigned int LineOffset;

unsigned int ScreenModified;
unsigned int ChangeInk;

/******************************************************************/

void InitScreen (void) {
   unsigned int x,y,i;
   for (x=0; x<=639; x++) {
     for (y=0; y<=399; y++) {
       XPutPixel(myimage,x,y,PixColor[4]);
     }
   }
   for (i=0;i<=17; i++) {
     HD6845Register[i]=0;
   }
   RedrawScreen = FALSE;
   ScreenAddr=0xC000;
   ScreenOffset = 0;
   ScreenBlock = 0xC000;
   ScreenMode = 1;
   ChangeInk=FALSE;
}

/******************************************************************/

void WrScreenMem (register word Addr, register byte Value) {
    static int scrZeile, scrSpalte, scrBitNr;
    static int scrAddr, RowOffsAddr;
    static int i,j;
    static long farbe;
    static int mapdx;


    mapdx=0;  // 0: normal
    //mapdx=1024;


    RowOffsAddr = ((Addr & 0x07FF) + 2048 - ScreenOffset - mapdx) \
                  & 0x07FF;
    //RowOffsAddr = ((Addr & 0x07FF) + 2048 - ScreenOffset) & 0x07FF;  // ORIG
    scrAddr = (Addr & 0x3800) | RowOffsAddr;
    //scrAddr = (Addr & 0x3800) | RowOffsAddr;  // ORIG

    scrZeile  = PixelPosition [scrAddr & 2047][1];
    scrSpalte = PixelPosition [scrAddr & 2047][0];
    scrZeile  = ((scrAddr >> 11)<<1) + scrZeile + LineOffset;
    // -- memory to visible screen mapping (endless round map)
    // X: 0..400
    // Y: 0..???
    //if (scrSpalte>599) scrSpalte-=600;  // max. screen width
    if (scrZeile>399) scrZeile-=400;  // max. screen height

    ScreenModified = 1;

    switch (ScreenMode) {

      /************/
      /** MODE 0 **/
      /************/
      case 0 :
        for (i=0; i<=1; i++) {
          switch((Value<<i)&170) {
            case 0:
              farbe = PixColor [AktInk[0]];
              break;
            case 128:
              farbe = PixColor [AktInk[1]];
              break;
            case 8:
              farbe = PixColor [AktInk[2]];
              break;
            case 136:
              farbe = PixColor [AktInk[3]];
              break;
            case 32:
              farbe = PixColor [AktInk[4]];
              break;
            case 160:
              farbe = PixColor [AktInk[5]];
              break;
            case 40:
              farbe = PixColor [AktInk[6]];
              break;
            case 168:
              farbe = PixColor [AktInk[7]];
              break;
            case 2:
              farbe = PixColor [AktInk[8]];
              break;
            case 130:
              farbe = PixColor [AktInk[9]];
              break;
            case 10:
              farbe = PixColor [AktInk[10]];
              break;
            case 138:
              farbe = PixColor [AktInk[11]];
              break;
            case 34:
              farbe = PixColor [AktInk[12]];
              break;
            case 162:
              farbe = PixColor [AktInk[13]];
              break;
            case 42:
              farbe = PixColor [AktInk[14]];
              break;
            case 170:
              farbe = PixColor [AktInk[15]];
              break;
          }
          for (j=0; j<=3; j++) {
            XPutPixel(myimage,(scrSpalte)+(i*4)+j,scrZeile,farbe);
            XPutPixel(myimage,(scrSpalte)+(i*4)+j,scrZeile|1,farbe);
          }
        }
        break;

      /************/
      /** MODE 1 **/
      /************/
      case 1 :
        for (i=0; i<=3; i++) {
          switch ((Value>>i)&17) {
            case 0:
              farbe = PixColor [AktInk[0]];
              break;
            case 1:
              farbe = PixColor [AktInk[2]];
              break;
            case 16:
              farbe = PixColor [AktInk[1]];
              break;
            case 17:
              farbe = PixColor [AktInk[3]];
              break;
          }
          XPutPixel(myimage,(scrSpalte)+6-(i<<1),scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1),scrZeile|1,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1)|1,scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+6-(i<<1)|1,scrZeile|1,farbe);
        }
        break;

      /************/
      /** MODE 2 **/
      /************/
      case 2 :
        for (i=0; i<=7; i++) {
          if ((Value & (1<<i))>0)
            farbe = PixColor[AktInk[1]];
          else
            farbe = PixColor[AktInk[0]];

          XPutPixel(myimage,(scrSpalte)+7-i,scrZeile,farbe);
          XPutPixel(myimage,(scrSpalte)+7-i,scrZeile|1,farbe);
        }
        break;
    }
}


/*****************************************************/
/** After any color change, the screen image must   **/
/** be redrawed completely.                         **/
/*****************************************************/

void RedrawScreenImage(void) {
  static word Addr, z, s;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
}



/***************************************************************/
/** The last line of the screen image must be redrawed after  **/
/** a hardware scroll in upper direction via OUT commands.    **/
/***************************************************************/

void RedrawLastLine (void) {
  static word Addr, z, s;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
    //for (s=1920; s<2000; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
}


/****************************************************************/
/** The first line of the screen image must be redrawed after  **/
/** a hardware scroll in lower direction via OUT commands.     **/
/****************************************************************/

void RedrawFirstLine (void) {
  static word Addr, z, s;
  for (z=0; z<16384 ;z+=2048)
    for (s=0; s<2000; s++) {
    //for (s=0; s<80; s++) {
      Addr = ScreenBlock + ((ScreenOffset + z + s) & 0x3FFF);
      WrScreenMem (Addr, RAM[Addr]);
    }
}

/******************************************************************/

/**********************************************/
/**                                          **/
/** Save the screen as XPM pixmap            **/
/**                                          **/
/** XPM is C like coded.                     **/
/**                                          **/
/**********************************************/

#ifdef PICO_BUILD
void SaveScreenAsXPM (char *filename) {
  (void)filename;
}
#else
void SaveScreenAsXPM (char *filename) {
  FILE *fp;
  char txt[20];
  int ncolors, c, idx;
  int col, row, row2;
  unsigned long pixel;
  Colormap cmap;
  XColor color;
  /* Get the color map for XQueryColor function */
  color.flags = DoRed | DoGreen | DoBlue;
  cmap = DefaultColormap (mydisplay,myscreen);

  /* Open the XPM file */
  fp = fopen (filename, "w");
  if (fp!=NULL) {
    /* Print XPM file header */
    fprintf (fp, "/* XPM */\n");
    fprintf (fp, "static char *cpc4x_xpm[] = {\n");
    fprintf (fp, "/* width height ncolors chars_per_pixel */\n");
    /* Get number of colors */
    switch (ScreenMode) {
      case 2 : ncolors = 2; break;
      case 1 : ncolors = 4; break;
      case 0 : ncolors = 16; break;
    }
    /* Print width, height, number of colors and chars per pixel */
    fprintf (fp, "%c640 400 %d 1%c", 34, ncolors, 34);
    /* Print color informations to XPM file */
    for (c=0; c<ncolors; c++) {
      fprintf (fp, ",\n%c%c\t c #",34, 65+c);
      color.pixel =  PixColor[AktInk[c]];
      XQueryColor (mydisplay, cmap, &color);
      fprintf (fp, "%04X",   color.red);
      fprintf (fp, "%04X",   color.green);
      fprintf (fp, "%04X%c", color.blue, 34);
    }
    /* Print the image */
    for (row=0; row<399; row++) { //row<399
      /* Calculate image row with scroll row-offset */
      row2 = row + LineOffset;
      if (row2>399) row2=row2-400; //row2>=400
      /* Print one pixel row of the image */
      fprintf (fp, ",\n%c", 34);
      for (col=0; col<639; col++) {  //col<639
        pixel =  XGetPixel(myimage,col,row2);
        idx = 0;
        for (c=0; c<ncolors; c++)
          if (pixel == PixColor [AktInk[c]]) {
            idx=c;
            c=ncolors;
          }
        fprintf (fp, "%c",65+idx);
      }
      fprintf (fp, "%c",34);
    }
    fprintf (fp, "\n};");
    fclose (fp);
  }
}

#endif

/******************************************************************/

//  END OF  cpc.c

