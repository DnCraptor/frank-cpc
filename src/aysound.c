#ifdef PICO_BUILD
#include <stdint.h>
#include <string.h>
#include "aysound.h"
#include "cpc_compat.h"

extern unsigned audio_ring_push_mono(const int16_t *samples, unsigned count);
extern unsigned audio_ring_push_stereo(const int16_t *samples, unsigned count);

/* AY stereo mode: 0=Mono, 1=ACB, 2=ABC */
extern int ay_stereo_mode;

int     NoSound=0;
int     SoundOn=1;
int     dspfd=0;
int     sample_res = 16;
int     sample_rate = 44100;
int     sample_channels = 1;
int     nb_samples = 882;
double  magic_number = 1.0;
int     note[4] = {0,0,0,0};
double  fq[4] = {0.,0.,0.,0.};
double  nsample[4] = {0.,0.,0.,0.};
signed char    phase[4] = {1,1,1,1};
signed char    new_note[4] = {0,0,0,0};
char    bits[8] = {1,2,4,8,16,32,64,128};
char    log_ampl[16] = {0,3,4,5,7,9,11,13,17,21,26,33,41,51,64,80};
int     max_volume = 0x7fff;
int     AY_rec = -1;
FILE    *debug_snd = NULL;
int     ay_stereo_mode = 0;  /* 0=Mono, 1=ACB, 2=ABC */

static uint32_t noise_seed = 1;

/* ---- AY-3-8912 hardware envelope generator ---- */
AYEnvelope ay_envelope;

/* Log-amplitude table for envelope (same scale as log_ampl[]) */
static const char env_ampl[16] = {0,3,4,5,7,9,11,13,17,21,26,33,41,51,64,80};

/* Advance the envelope cycle by one step and return the current volume (0-15).
 * Implements all 16 envelope shapes as per the AY datasheet. */
static int envelope_next_volume(AYEnvelope *env, int shape) {
    int vol;

    /* Held states: cycle==256 → held at 0, cycle==512 → held at 15 */
    if (env->cycle == 256) return 0;
    if (env->cycle == 512) return 15;

    shape &= 0x0F;

    switch (shape) {
        /* Shapes that count down: \_________ */
        case 0: case 1: case 2: case 3:
        case 8: case 9: case 11:
            vol = 15 - env->cycle;
            break;
        /* Shapes that count up: /---------- */
        case 4: case 5: case 6: case 7:
        case 12: case 13: case 15:
            vol = env->cycle;
            break;
        /* Alternating \/\/\/ */
        case 10:
            vol = (env->sign_10_14 == +1) ? (15 - env->cycle) : env->cycle;
            break;
        /* Alternating /\/\/\ */
        case 14:
            vol = (env->sign_10_14 == +1) ? env->cycle : (15 - env->cycle);
            break;
        default:
            vol = 15;
            break;
    }

    env->cycle++;
    if (env->cycle == 16) {
        switch (shape) {
            /* Hold at 0 after first period */
            case 0: case 1: case 2: case 3:
            case 4: case 5: case 6: case 7:
            case 9: case 15:
                env->cycle = 256;
                break;
            /* Hold at 15 after first period */
            case 11: case 13:
                env->cycle = 512;
                break;
            /* Repeat (shapes 8, 10, 12, 14) */
            default:
                env->cycle = 0;
                if (shape == 10 || shape == 14)
                    env->sign_10_14 = -env->sign_10_14;
                break;
        }
    }

    return vol;
}

static void envelope_reset(AYEnvelope *env) {
    env->cycle = 0;
    env->sign_10_14 = +1;
    env->counter = 0.0;
    env->period = 0.0;
    env->volume = 0;
    env->shape_written = 0;
}

int init_dsp(void) {
    resetAYRegister();
#ifdef HDMI_PIO_AUDIO
    sample_rate  = 32000;          /* HDMI data-island audio rate */
#endif
    nb_samples   = sample_rate / 50;       /* 22050/50 = 441 */
    magic_number = sample_rate / 125000.0;
    /* Full scale: worst case 3 channels at max volume ≈ (480/256)*32767 ≈ 61440,
     * clamped to int16. Sounds closer to real Amstrad output level. */
    max_volume   = (2 << (sample_res - 2)) - 1;  /* 32767 */
    return 1;
}

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
    envelope_reset(&ay_envelope);
}

/* Matches the Linux random_noise() distribution: returns 1 with 50% probability,
 * 2 with 25%, 3 with 12.5%, etc.  Counts leading zeros of a 15-bit LFSR value. */
int random_noise(void) {
    noise_seed ^= noise_seed << 13;
    noise_seed ^= noise_seed >> 17;
    noise_seed ^= noise_seed << 5;
    uint32_t x = (noise_seed & 0x7FFF) + 1;   /* 1..32768 */
    int j = 0;
    while (x > 0) { x >>= 1; j++; }
    return 16 - j;                              /* 1..15, biased toward 1 */
}

void mix_notes(char *AY_array_reg) {
    static int16_t buffer[882];
    static int16_t stereo_buf[882 * 2];  /* interleaved L/R */
    int      i, j;
    double   level;
    float    amplitude, ampnoise;
    AYframe *cur_frame;

    /* Stereo panning weights: {L, R} for channels A, B, C.
     * ACB: A=left, C=center, B=right (most common CPC stereo)
     * ABC: A=left, B=center, C=right */
    static const double pan_acb[3][2] = {{1.0, 0.25}, {0.25, 1.0}, {0.65, 0.65}};
    static const double pan_abc[3][2] = {{1.0, 0.25}, {0.65, 0.65}, {0.25, 1.0}};
    const double (*pan)[2] = (ay_stereo_mode == 2) ? pan_abc : pan_acb;
    int stereo = (ay_stereo_mode != 0);

    if (!SoundOn) {
        if (stereo) {
            memset(stereo_buf, 0, nb_samples * 2 * sizeof(int16_t));
            audio_ring_push_stereo(stereo_buf, nb_samples);
        } else {
            memset(buffer, 0, nb_samples * sizeof(int16_t));
            audio_ring_push_mono(buffer, nb_samples);
        }
        return;
    }

    cur_frame = (AYframe *)AY_array_reg;

    for (j = 0; j < 3; j++) {
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

    /* Envelope period: update from registers. */
    {
        unsigned short fqe = cur_frame->fqe;
        int shape = cur_frame->she & 0x0F;
        static int last_shape = -1;
        if (last_shape != shape || !ay_envelope.shape_written) {
            ay_envelope.cycle = 0;
            ay_envelope.sign_10_14 = +1;
            ay_envelope.counter = 0.0;
            ay_envelope.shape_written = 1;
            last_shape = shape;
        }
        if (fqe == 0) fqe = 1;
        ay_envelope.period = ((double)fqe * 256.0 * sample_rate) / (1000000.0 * 16.0);
    }

    for (i = 0; i < nb_samples; i++) {
        double mix_l = 0, mix_r = 0, mix_mono = 0;

        ampnoise   = phase[3];
        nsample[3] = nsample[3] - 1;
        if (nsample[3] < 1) {
            phase[3]    = -phase[3];
            nsample[3] += fq[3] * random_noise();
        }

        /* Advance envelope counter */
        ay_envelope.counter += 1.0;
        while (ay_envelope.counter >= ay_envelope.period && ay_envelope.period > 0.0) {
            ay_envelope.counter -= ay_envelope.period;
            ay_envelope.volume = envelope_next_volume(&ay_envelope, cur_frame->she);
        }

        for (j = 0; j < 3; j++) {
            int vol;
            int use_envelope = cur_frame->lv[j] & 0x10;

            if (use_envelope) {
                vol = ay_envelope.volume;
            } else {
                vol = cur_frame->lv[j] & 0x0f;
            }

            double ch_level = 0;

            /* Noise channel contribution */
            if ((cur_frame->mix & bits[j + 3]) == 0) {
                ch_level += ampnoise * (use_envelope ? env_ampl[vol] : log_ampl[vol]);
            }

            /* Tone channel */
            if (fq[j] < 1.0 || vol == 0) {
                level = 0;
            } else {
                amplitude  = phase[j];
                nsample[j] = nsample[j] - 1;
                if (nsample[j] < 1) {
                    phase[j] = -phase[j];
                    if (new_note[j] == 1) {
                        new_note[j] = 0;
                        nsample[j]  = fq[j];
                    } else {
                        nsample[j] += fq[j];
                    }
                }

                level = 0;
                if (!(cur_frame->mix & bits[j]))
                    level = amplitude * (use_envelope ? env_ampl[vol] : log_ampl[vol]);
            }

            ch_level += level;

            if (stereo) {
                mix_l += ch_level * pan[j][0];
                mix_r += ch_level * pan[j][1];
            } else {
                mix_mono += ch_level;
            }
        }

        if (stereo) {
            mix_l = (mix_l / 256) * max_volume;
            mix_r = (mix_r / 256) * max_volume;
            if (mix_l > 32767.0)  mix_l = 32767.0;
            if (mix_l < -32768.0) mix_l = -32768.0;
            if (mix_r > 32767.0)  mix_r = 32767.0;
            if (mix_r < -32768.0) mix_r = -32768.0;
            stereo_buf[i * 2]     = (int16_t)mix_l;
            stereo_buf[i * 2 + 1] = (int16_t)mix_r;
        } else {
            mix_mono = (mix_mono / 256) * max_volume;
            if (mix_mono > 32767.0)  mix_mono = 32767.0;
            if (mix_mono < -32768.0) mix_mono = -32768.0;
            buffer[i] = (int16_t)mix_mono;
        }
    }

    /* Two-stage post-filter:
     * 1) DC-blocking high-pass (removes DC offset from asymmetric mixing)
     * 2) First-order low-pass at ~4 kHz to tame square-wave harmonics */
    if (stereo) {
        static double lpf_l = 0.0, lpf_r = 0.0;
        static double dc_pi_l = 0.0, dc_po_l = 0.0;
        static double dc_pi_r = 0.0, dc_po_r = 0.0;
        const double lp_alpha = 0.44;
        const double dc_alpha = 0.995;
        for (i = 0; i < nb_samples; i++) {
            double sl = (double)stereo_buf[i * 2];
            double dc_l = sl - dc_pi_l + dc_alpha * dc_po_l;
            dc_pi_l = sl; dc_po_l = dc_l;
            lpf_l += lp_alpha * (dc_l - lpf_l);
            if (lpf_l > 32767.0) lpf_l = 32767.0;
            if (lpf_l < -32768.0) lpf_l = -32768.0;
            stereo_buf[i * 2] = (int16_t)lpf_l;

            double sr = (double)stereo_buf[i * 2 + 1];
            double dc_r = sr - dc_pi_r + dc_alpha * dc_po_r;
            dc_pi_r = sr; dc_po_r = dc_r;
            lpf_r += lp_alpha * (dc_r - lpf_r);
            if (lpf_r > 32767.0) lpf_r = 32767.0;
            if (lpf_r < -32768.0) lpf_r = -32768.0;
            stereo_buf[i * 2 + 1] = (int16_t)lpf_r;
        }
        audio_ring_push_stereo(stereo_buf, nb_samples);
    } else {
        static double lpf_state = 0.0;
        static double dc_prev_in = 0.0, dc_prev_out = 0.0;
        const double lp_alpha = 0.44;
        const double dc_alpha = 0.995;
        for (i = 0; i < nb_samples; i++) {
            double s = (double)buffer[i];
            double dc_out = s - dc_prev_in + dc_alpha * dc_prev_out;
            dc_prev_in = s; dc_prev_out = dc_out;
            lpf_state += lp_alpha * (dc_out - lpf_state);
            if (lpf_state > 32767.0) lpf_state = 32767.0;
            if (lpf_state < -32768.0) lpf_state = -32768.0;
            buffer[i] = (int16_t)lpf_state;
        }
        audio_ring_push_mono(buffer, nb_samples);
    }
}
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
