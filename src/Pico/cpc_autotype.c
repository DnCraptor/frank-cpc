/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_autotype.c — automatic key injection for unattended boot / testing.
 *
 * Injects a sequence of keypresses into the CPC keyboard matrix, one character
 * every FRAMES_PER_CHAR frames (5 frames = 100ms at 50fps).
 * Each character is pressed for PRESS_FRAMES then released for RELEASE_FRAMES.
 */
#include "cpc_autotype.h"
#include "cpc.h"
#include "keyboard.h"
#include <string.h>
#include <stdio.h>

/* Timing constants (all in 50fps frame units). */
#define FRAMES_PER_CHAR   5   /* total time budget per character */
#define PRESS_FRAMES      2   /* hold key pressed for this many frames */
#define FRAMES_PER_WAIT 500   /* \n = 10 second pause (no keypress)   */

/* XK_Return matches keyboard.c */
#define XK_Return 0xff0d

/* ---- state ------------------------------------------------------------- */

static char     g_text[256];    /* enlarged for long sequences with pauses */
static int      g_len = 0;
static int      g_pos = -1;       /* -1 = not yet started */
static unsigned g_boot_delay = 0;
static unsigned g_frame = 0;      /* frames since cpc_autotype_set() */
static int      g_char_frame = 0; /* frame within current character */
static int      g_wait_frames = 0;/* countdown for \n wait */

/* ---- helpers ---------------------------------------------------------- */

/* Returns 0 for "skip instantly", AUTOTYPE_WAIT_SENTINEL for a 10s pause,
 * or the XK_ keysym to press. */
#define AUTOTYPE_WAIT_SENTINEL 0xFFFFFFFEu

static unsigned int char_to_keysym(char c) {
    if (c == '\r' || c == '\n' && 0) return XK_Return; /* \r = Enter */
    if (c == '\r') return XK_Return;
    /* \n (0x0A) used as "10 second pause" in autotype strings */
    if (c == '\n') return AUTOTYPE_WAIT_SENTINEL;
    if ((unsigned char)c >= 32 && (unsigned char)c < 126)
        return (unsigned int)(unsigned char)c;
    return 0;
}

/* ---- API -------------------------------------------------------------- */

void cpc_autotype_set(const char *text, unsigned int boot_delay_frames) {
    strncpy(g_text, text, sizeof(g_text) - 1);
    g_text[sizeof(g_text) - 1] = 0;
    g_len         = (int)strlen(g_text);
    g_pos         = -1;
    g_boot_delay  = boot_delay_frames;
    g_frame       = 0;
    g_char_frame  = 0;
    g_wait_frames = 0;
    printf("autotype: armed %d chars, boot_delay=%u frames\n", g_len, boot_delay_frames);
}

void cpc_autotype_tick(void) {
    if (g_pos >= g_len) return;   /* done or never started */
    if (g_len == 0) return;

    g_frame++;

    /* Wait for boot delay before starting. */
    if (g_pos < 0) {
        if (g_frame < g_boot_delay) return;
        g_pos = 0;
        g_char_frame  = 0;
        g_wait_frames = 0;
        printf("autotype: starting\n");
    }

    /* Counting down a \n pause — no keypress, just wait. */
    if (g_wait_frames > 0) {
        g_wait_frames--;
        return;
    }

    unsigned int ks = char_to_keysym(g_text[g_pos]);

    if (ks == AUTOTYPE_WAIT_SENTINEL) {
        /* Start a 10-second pause then advance. */
        g_wait_frames = FRAMES_PER_WAIT;
        g_pos++;
        g_char_frame = 0;
        printf("autotype: 10s pause at pos %d\n", g_pos);
        return;
    }

    if (!ks) {
        /* Skip unknown character instantly */
        g_pos++;
        g_char_frame = 0;
        return;
    }

    if (g_char_frame == 0) {
        CPCKeyPress(ks);
    } else if (g_char_frame == PRESS_FRAMES) {
        CPCKeyRelease(ks);
    } else if (g_char_frame >= FRAMES_PER_CHAR - 1) {
        g_pos++;
        g_char_frame = 0;
        if (g_pos >= g_len)
            printf("autotype: done\n");
        return;
    }

    g_char_frame++;
}
