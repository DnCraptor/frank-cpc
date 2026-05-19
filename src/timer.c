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
#include <signal.h>
#include <sys/time.h>

unsigned int TimerCount;
unsigned int Sekunden;
unsigned int Frequenz;
struct itimerval TimerValue, oldTimerValue;


/** MasterTimer() ********************************************/
/** The main timer handler which is called MAXTIMERFREQ     **/
/** times a second. It then calls user-defined timers.      **/
/*************************************************************/
static void MasterTimer(int Arg)
{
  TimerCount ++;
 // signal(Arg,MasterTimer);   // wozu?
}



/** TimerSignal() ********************************************/
/** Establish a signal handler called with given frequency  **/
/** (Hz). Returns 0 if failed.                              **/
/** Maximal Freq.= 100 Hz with Linux!!                      **/
/*************************************************************/
int TimerSignal(int Freq)
{
  TimerCount = 0;
  Sekunden = 0;
  Frequenz = Freq;
  TimerValue.it_interval.tv_sec  = 0; 
  TimerValue.it_value.tv_sec     = 0;
  TimerValue.it_interval.tv_usec = 1000000L/Freq;
  TimerValue.it_value.tv_usec    = 1000000L/Freq;
  if (setitimer(ITIMER_REAL,&TimerValue,&oldTimerValue)) return(0);
  if (signal(SIGALRM, MasterTimer)==SIG_ERR) { printf("CPC4X - Can not create timer handler\n"); return 0;}
  return(1);
}


/** DelTimer() ***********************************************/
/** Remove routine added with AddTimer().                   **/
/**   UC: Entnommen aus LibUnix.c von Marat Fayzullin       **/
/*************************************************************/
void DelTimer(void)
{
  printf ("Delete timer");
  setitimer(ITIMER_REAL,&oldTimerValue,NULL);
  signal(SIGALRM,SIG_DFL);
  printf (" ..... ok!\n");
}

