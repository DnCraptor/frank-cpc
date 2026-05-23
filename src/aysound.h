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
    unsigned short fqx[3];     /* Tone period for channels A, B, C (regs 0-5) */
    unsigned char  fqn;        /* Noise period (reg 6) */
    unsigned char  mix;        /* Mixer control (reg 7) */
    unsigned char  lv[3];      /* Volume / envelope mode for A, B, C (regs 8-10) */
    unsigned short fqe;        /* Envelope period (regs 11-12) */
    unsigned char  she;        /* Envelope shape (reg 13) */
   };
typedef struct AYframe AYframe;

/* ---- AY-3-8912 hardware envelope generator state ---- */
typedef struct {
    int     cycle;         /* Current position in envelope (0-15, or 256/512 = held) */
    int     sign_10_14;    /* Direction for alternating shapes 10/14: +1 or -1 */
#ifdef PICO_BUILD
    float   counter;       /* Sample counter for envelope period */
    float   period;        /* Envelope period in samples */
#else
    double  counter;
    double  period;
#endif
    int     volume;        /* Current envelope output amplitude (0-15) */
    int     shape_written; /* Flag: non-zero once reg 13 has been written */
} AYEnvelope;

extern AYEnvelope ay_envelope;

extern  int     NoSound;
extern  int     SoundOn;
extern  int     sample_res;
extern  int     sample_rate;
extern  int     nb_samples;
#ifdef PICO_BUILD
extern  float   magic_number;
extern  float   fq[4];
extern  float   nsample[4];
#else
extern  double  magic_number;
extern  double  fq[4];
extern  double  nsample[4];
#endif
extern  int     note[4];
/* Use signed types for phase — ARM char is unsigned by default */
extern  signed char    phase[4];
extern  signed char    new_note[4];
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
