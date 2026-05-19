/***************************************/
/**                                   **/
/** AMSTRAD/Schneider CPC-Emulator    **/
/** for Linux and X11                 **/
/**                                   **/
/** GNU GENERAL PUBLIC LICENSE        **/
/** 1999-2002                         **/
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

#ifndef AYSOUND_H
#define AYSOUND_H 1

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#ifndef PICO_BUILD
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#endif

struct AYframe
   {
    unsigned short fqx[3];
    unsigned char  fqn;
    unsigned char  mix;
    unsigned char  lv[3];
    unsigned short fqe;
    unsigned char  she;
   };
typedef struct AYframe AYframe;

extern  int     NoSound;
extern  int     SoundOn;
extern  int     sample_res;
extern  int     sample_rate;
extern  int     nb_samples;
extern  double  magic_number;
extern  int     note[4];
extern  double  fq[4];
extern  double  nsample[4];
extern  char    phase[4];
extern  char    new_note[4];
extern  char    bits[8];
extern  char    log_ampl[16];
extern  int     max_volume;
extern  int     AY_rec;
extern  int     dspfd;
extern  FILE    *debug_snd;

void  mix_notes(char *AY_array_reg);
void  resetAYRegister(void);
int   init_dsp(void);
void  switchAYRec(void);
void  exit_dsp(void);

#endif
