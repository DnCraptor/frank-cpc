#ifdef PICO_BUILD
#include <stdint.h>
#include <string.h>
#include "aysound.h"
#include "cpc_compat.h"

extern unsigned audio_ring_push_mono(const int16_t *samples, unsigned count);

int     NoSound=0;
int     SoundOn=1;
int     dspfd=0;
int     sample_res = 16;
int     sample_rate = 22050;
int     sample_channels = 1;
int     nb_samples = 441;
double  magic_number = 1.0;
int     note[4] = {0,0,0,0};
double  fq[4] = {0.,0.,0.,0.};
double  nsample[4] = {0.,0.,0.,0.};
char    phase[4] = {1,1,1,1};
char    new_note[4] = {0,0,0,0};
char    bits[8] = {1,2,4,8,16,32,64,128};
char    log_ampl[16] = {0,3,4,5,7,9,11,13,17,21,26,33,41,51,64,80};
int     max_volume = 0x7fff;
int     AY_rec = -1;
FILE    *debug_snd = NULL;

int init_dsp(void) { return 1; }
void exit_dsp(void) {}
void switchAYRec(void) {}
void resetAYRegister(void) {
    for (int i = 0; i < 4; ++i) {
        note[i] = 0;
        fq[i] = 0.;
        nsample[i] = 0.;
        phase[i] = 1;
        new_note[i] = 0;
    }
}
void mix_notes(char *AY_array_reg) {
    static int16_t silence[441];
    (void)AY_array_reg;
    audio_ring_push_mono(silence, 441);
}
int random_noise(void) { return 0; }
#else
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

/***** AY Sound Support *******************************************
 ** AY-3-8912 beta emulation (20000601)                          **
 ** Support of Channels A,B,C and Noise                          **
 ** No hardware envelope yet                                     **
 ** No stereo yet                                                **
 ** Some other problems : opposite wavelength (no sound, like in **
 **                       "Orphee")                              **
 **                       some noise problems...                 **
 ******************************************************************/

/*
 *  If you want to make changes, please do not(!) use TABs !!!!!
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#include "aysound.h"
#include "cpc.h"
#include "io.h"

int     NoSound=0;

#ifdef CPCSOUND
int     SoundOn=1;
#else
int     SoundOn=0;
#endif

/* Sound device driver : "/dev/dsp" */
int     dspfd=0;

/********************************************************************/
/* Sample Resolution and Sample rate. It's hard-coded. It would be  */
/* better if these settings were passed to init_dsp() function...   */
/********************************************************************/

int     sample_res      = 16;  // Do not change sample_res as I deleted support for 8 bit
int     sample_rate     = 44100;
int     sample_channels = 2;

int     nb_samples;
double  magic_number;
int     note[4]        = {0, 0, 0, 0 };
double  fq[4]          = {0.,0.,0.,0.};
double  nsample[4]     = {0.,0.,0.,0.};
char    phase[4]       = {1, 1, 1, 1 };
char    new_note[4]    = {0, 0, 0, 0 };
char    bits[8]        = {1,2,4,8,16,32,64,128};
char    log_ampl[16]   = {0,3,4,5,7,9,11,13,17,21,26,33,41,51,64,80};
int     max_volume;
int     AY_rec = -1;
FILE    *debug_snd;
short   *buffer;
int     taille;

/****************************************************************************/
/* I don't know how to do the same random generator as in the AY chip. This */
/* generator is a linux pseudo-random, not an AY pseudo-random ! I don't    */
/* know if there is adifference for human ears ...                          */
/****************************************************************************/
int random_noise() {
#ifdef CPCSOUND
    int x1, j;

    x1 = (((random()*1.0)/RAND_MAX)*32767)+1;
    for (j=0; x1 > 0; j++)
         x1>>=1;
    return (16-j);
#endif
}

/***************************************************************************/
/* you have only to give the adress of the AY registers, and this function */
/* will do the rest... I have a doubt about the mixer.                     */
/***************************************************************************/

void mix_notes(char *AY_array_reg) {
#ifdef CPCSOUND
    int      i, j;
    double   mix,mix_a,mix_b,level;
    float    amplitude, ampnoise;
    short    ampl16,ampl16_a,ampl16_b;
    AYframe *cur_frame;

    if (!SoundOn) return;

    cur_frame = (AYframe*)AY_array_reg;

    if (AY_rec == 1)
        for (i=0; i <14; i++)
           fprintf(debug_snd, "%c", AY_array_reg[i]);

    for (j=0; j < 3; j++) {
        cur_frame->fqx[j] = cur_frame->fqx[j] & 0x0fff;
        if (cur_frame->fqx[j] != note[j]) {
            note[j]     = cur_frame->fqx[j];
            fq[j]       = note[j] * magic_number;
            new_note[j] = 1;
        }
    }

    cur_frame->fqn = cur_frame->fqn & 0x1f;
    if (cur_frame->fqn != note[3]) {
        note[3]    = cur_frame->fqn;
        fq[3]      = note[3] * magic_number;
        nsample[3] = fq[3] * random_noise();
    }

    buffer = malloc(nb_samples*sample_channels);

    for (i=0; i < nb_samples; i++) {
         mix   = 0;
         mix_a = 0;
         mix_b = 0;

         ampnoise   = phase[3];
         nsample[3] = nsample[3] - 1;

         if (nsample[3] < 1) {
             phase[3]    = -phase[3];
             nsample[3] += fq[3]*random_noise();
         }

         for (j=0; j<3; j++) {

             /* This line disable the hardware envelope (try it with "commando"  */
             /* and you will understand                                          */
             cur_frame->lv[j] = cur_frame->lv[j] & 0x0f;;

             /* when the hardware envelope will be coded, this line should be deleted */
             if ((cur_frame->mix & bits[j+3]) == 0) {
                mix   += ampnoise * log_ampl[cur_frame->lv[j]];
                mix_a += ampnoise * log_ampl[cur_frame->lv[j]] / 2;
                mix_b += ampnoise * log_ampl[cur_frame->lv[j]] / 2;
             }

             amplitude  = phase[j];
             nsample[j] = nsample[j] - 1;

             if (nsample[j] < 1) {
                 phase[j]    = -phase[j];
                 if (new_note[j] == 1) {
                     new_note[j] = 0;
                     nsample[j]  = fq[j];
                 } else
                     nsample[j] += fq[j];
             }

             if (!(cur_frame->mix & bits[j]))
                 level = amplitude * log_ampl[cur_frame->lv[j]];

             mix += level;

             // I can't remember which channel outputs left, right and left/right
             // It's not offense if you change this order :)

             switch (j) {
                case 0 : mix_a += level;
                         break;
                case 1 : mix_a += level/2;
                         mix_b += level/2;
                         break;
                case 2 : mix_b += level;
                         break;
             }
         }

         mix   = (mix / 256) * max_volume;
         mix_a = (mix_a / 256) * max_volume;
         mix_b = (mix_b / 256) * max_volume;

         ampl16   = mix;
         ampl16_a = mix_a;
         ampl16_b = mix_b;

         if (sample_channels==1)
             buffer[i] = ampl16;
         else {
             buffer[i*2] = ampl16_a;
             buffer[i*2+1] = ampl16_b;
         }
    }
    write(dspfd, buffer, taille);
    free(buffer);
#endif
}

void switchAYRec() {
#ifdef CPCSOUND
   char FName[80];
   AY_rec = -AY_rec;
   if (AY_rec == -1) {
      fclose(debug_snd);
      printf("Sound debug file closed\n");
   }
   else {
      sprintf (FName, "%s/sound_logfile.txt", WorkDirectory);
      debug_snd = fopen(FName, "w");
      printf("sound debug file %s opened\n", FName);
   }
#endif
}

void resetAYRegister() {
   int i;
   for (i=0; i <4; i++) {
        note[i]    = 0;
        fq[i]      = 0.;
        nsample[i] = 0.;
        phase[i]   = 1;
   }
}

int init_dsp() {
#ifdef CPCSOUND
    if (dspfd != 0)
        close (dspfd);
    //dspfd = open ("/dev/dsp", O_WRONLY);
    dspfd = open ("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (dspfd <= 0) {
        printf ("\n*** ERROR can't open /dev/dsp ***\n");
        SoundOn = 0;
        return (-1);
    }

    //fcntl (dspfd, F_SETFL, fcntl(dspfd,F_GETFL) & ~O_NONBLOCK);
    if (ioctl(dspfd, SOUND_PCM_WRITE_BITS, &sample_res)==-1) {
        printf ("\n*** ERROR while initializing sample resolution ***");
        return (-1);
    }

    if (ioctl(dspfd, SOUND_PCM_WRITE_RATE, &sample_rate)==-1) {
       printf ("\\n*** ERROR while initializing sample rate ***");
       return (-1);
    }

    if (ioctl(dspfd, SOUND_PCM_WRITE_CHANNELS, &sample_channels)==-1) {
       printf("\\n*** ERROR while initializing sample channels ***");
       return (-1);
    }

    resetAYRegister();
    nb_samples   = sample_rate / 50;
    magic_number = sample_rate / 125000.0;
    max_volume   = (2<<(sample_res-2)) - 1;
    taille       = nb_samples*sizeof(short)*sample_channels;
    return (0);
#else
    printf ("\n*** No sound support ***\n");
#endif
}

void exit_dsp (void) {
#ifdef CPCSOUND
  if (NoSound==0) {
    if (dspfd > 0){
      printf ("Close dsp ....\n");
      close (dspfd);
      printf ("ok, dsp closed\n");
    }
  }
#endif
}

#endif
