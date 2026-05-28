#ifndef TAPE_FASTLOAD_H
#define TAPE_FASTLOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check if Z80 is in a known tape-reading loop and fast-load if possible.
 * Called from z80_execute() when tape motor is on.
 * Returns 1 if fast-load occurred (caller should skip normal execution).
 * Returns 0 if no pattern matched (continue normal execution). */
int tape_try_fastload(void);

#ifdef __cplusplus
}
#endif

#endif
