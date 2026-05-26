// stdafx.h : Pico-compatible precompiled header for capsimg library
//
// When building for Pico (PICO_BUILD), we bypass POSIX-specific includes
// and stub out DiskFile / directory operations since IPF files are loaded
// via CAPSLockImageMemory() using pre-read PSRAM buffers.

#pragma once

#ifdef PICO_BUILD

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <vector>

#define MAX_PATH ( 260 )
#ifndef __cdecl
#define __cdecl
#endif
#define _lrotl(x,n) (((x) << (n)) | ((x) >> (sizeof(x)*8-(n))))
#define _lrotr(x,n) (((x) >> (n)) | ((x) << (sizeof(x)*8-(n))))
typedef const char *LPCSTR;
typedef const char *LPCTSTR;

// Stub POSIX functions not available on Pico
#define _access(p,m) (-1)
#define _mkdir(x)    (0)

// Stub dirent — not used when loading via memory buffer
struct dirent { char d_name[256]; unsigned char d_type; unsigned short d_reclen; };
#define DT_REG 8
#ifndef FF_DEFINED   /* FatFS defines its own DIR */
typedef struct { int _dummy; } DIR;
#endif
static inline DIR *opendir(const char *) { return NULL; }
static inline struct dirent *readdir(DIR *) { return NULL; }
static inline int closedir(DIR *) { return 0; }

#define INTEL
#define MAX_FILENAMELEN (MAX_PATH*2)

// external definitions
#include "CommonTypes.h"

// Core components
#include "BaseFile.h"
#include "DiskFile.h"
#include "MemoryFile.h"
#include "CRC.h"
#include "BitBuffer.h"

// IPF library public definitions
#include "CapsLibAll.h"

// CODECs
#include "DiskEncoding.h"
#include "CapsDefinitions.h"
#include "CTRawCodec.h"

// file support
#include "CapsFile.h"
#include "DiskImage.h"
#include "CapsLoader.h"
#include "CapsImageStd.h"
#include "CapsImage.h"
#include "StreamImage.h"
#include "StreamCueImage.h"
#include "DiskImageFactory.h"

// Device access
#include "C2Comm.h"

// system
#include "CapsCore.h"
#include "CapsFDCEmulator.h"
#include "CapsFormatMFM.h"

#define __assume(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)

// Stub SYSTEMTIME for capsimg date decoding
typedef struct _SYSTEMTIME {
        int16_t wYear;
        int16_t wMonth;
        int16_t wDayOfWeek;
        int16_t wDay;
        int16_t wHour;
        int16_t wMinute;
        int16_t wSecond;
        int16_t wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;
static inline void GetLocalTime(LPSYSTEMTIME t) {
    memset(t, 0, sizeof(SYSTEMTIME));
    t->wYear = 2026; t->wMonth = 1; t->wDay = 1;
}

#else /* !PICO_BUILD — original stdafx.h */

#ifdef WINDOWS
#include "targetver.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#include <io.h>
#include <direct.h>
#define _lrotl(x,n) (((x) << (n)) | ((x) >> (sizeof(x)*8-(n))))
#define _lrotr(x,n) (((x) >> (n)) | ((x) << (sizeof(x)*8-(n))))
#else
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define MAX_PATH ( 260 )
#ifndef __cdecl
#define __cdecl
#endif
#define _lrotl(x,n) (((x) << (n)) | ((x) >> (sizeof(x)*8-(n))))
#define _lrotr(x,n) (((x) >> (n)) | ((x) << (sizeof(x)*8-(n))))
typedef const char *LPCSTR;
typedef const char *LPCTSTR;
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <vector>
#include <dirent.h>

#define INTEL
#define MAX_FILENAMELEN (MAX_PATH*2)

#include "CommonTypes.h"
#include "BaseFile.h"
#include "DiskFile.h"
#include "MemoryFile.h"
#include "CRC.h"
#include "BitBuffer.h"
#include "CapsLibAll.h"
#include "DiskEncoding.h"
#include "CapsDefinitions.h"
#include "CTRawCodec.h"
#include "CapsFile.h"
#include "DiskImage.h"
#include "CapsLoader.h"
#include "CapsImageStd.h"
#include "CapsImage.h"
#include "StreamImage.h"
#include "StreamCueImage.h"
#include "DiskImageFactory.h"
#include "C2Comm.h"
#include "CapsCore.h"
#include "CapsFDCEmulator.h"
#include "CapsFormatMFM.h"

#define __assume(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)

#ifndef WINDOWS
#define _access access
#ifndef __MINGW32__
#define _mkdir(x) mkdir(x,0)
#else
#define _mkdir(x) mkdir(x)
#endif
#define d_namlen d_reclen

typedef struct _SYSTEMTIME {
        WORD wYear;
        WORD wMonth;
        WORD wDayOfWeek;
        WORD wDay;
        WORD wHour;
        WORD wMinute;
        WORD wSecond;
        WORD wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;
extern "C" void GetLocalTime(LPSYSTEMTIME lpSystemTime);
#endif

#endif /* PICO_BUILD */
