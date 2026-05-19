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

#ifndef CPCTIMER_H
#define CPCTIMER_H 1
#include <signal.h>
#include <sys/time.h>

extern unsigned int TimerCount;
extern unsigned int Sekunden;
extern unsigned int Frequenz;
extern struct itimerval TimerValue;

int TimerSignal(int Freq);
void DelTimer(void);

#endif
