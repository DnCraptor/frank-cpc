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

extern int PassDriveSelect;

int SelectDiskFile (char *filename, int *DrvNum, int *WrProtect);
int SetupDialog (void);
void InfoDialog (void);
void FirstStartDialog (void);
void PrintCmdLinePars(void);
void SaveScreenImage(void);

/* Set the path returned by the next SelectDiskFile() call.
 * Called by cpc_loader when the user picks a disk in the UI. */
void SetPendingDiskPath(const char *path);
