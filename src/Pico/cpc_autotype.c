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
#include "cpc_adapter.h"
#include <string.h>
#include <stdio.h>

/* Timing constants (all in 50fps frame units). */
#define FRAMES_PER_CHAR   5   /* total time budget per character */
#define PRESS_FRAMES      2   /* hold key pressed for this many frames */
#define FRAMES_PER_WAIT 500   /* \n = 10 second pause (no keypress)   */

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
 * or the ASCII/keysym to press. */
#define AUTOTYPE_WAIT_SENTINEL 0xFFFFFFFEu

/* Map ASCII/keysym → CPC keyboard matrix {row, bit, needs_shift}.
 * Based on the CPC keyboard layout. */
typedef struct { uint8_t row; uint8_t bit; uint8_t shift; } cpc_keymap_t;

static cpc_keymap_t ascii_to_cpc(unsigned int ks) {
    cpc_keymap_t k = {0xFF, 0, 0};
    if (ks >= 'a' && ks <= 'z') ks -= 32; /* uppercase */
    switch (ks) {
        case 'A': k.row=8; k.bit=5; break;
        case 'B': k.row=6; k.bit=6; break;
        case 'C': k.row=7; k.bit=6; break;
        case 'D': k.row=7; k.bit=5; break;
        case 'E': k.row=7; k.bit=2; break;
        case 'F': k.row=6; k.bit=5; break;
        case 'G': k.row=6; k.bit=4; break;
        case 'H': k.row=5; k.bit=4; break;
        case 'I': k.row=4; k.bit=3; break;
        case 'J': k.row=5; k.bit=5; break;
        case 'K': k.row=4; k.bit=5; break;
        case 'L': k.row=4; k.bit=4; break;
        case 'M': k.row=4; k.bit=6; break;
        case 'N': k.row=5; k.bit=6; break;
        case 'O': k.row=4; k.bit=2; break;
        case 'P': k.row=3; k.bit=3; break;
        case 'Q': k.row=8; k.bit=3; break;
        case 'R': k.row=6; k.bit=2; break;
        case 'S': k.row=7; k.bit=4; break;
        case 'T': k.row=6; k.bit=3; break;
        case 'U': k.row=5; k.bit=2; break;
        case 'V': k.row=6; k.bit=7; break;
        case 'W': k.row=7; k.bit=3; break;
        case 'X': k.row=7; k.bit=7; break;
        case 'Y': k.row=5; k.bit=3; break;
        case 'Z': k.row=8; k.bit=7; break;
        case '0': k.row=4; k.bit=0; break;
        case '1': k.row=8; k.bit=0; break;
        case '2': k.row=8; k.bit=1; break;
        case '3': k.row=7; k.bit=1; break;
        case '4': k.row=7; k.bit=0; break;
        case '5': k.row=6; k.bit=1; break;
        case '6': k.row=6; k.bit=0; break;
        case '7': k.row=5; k.bit=1; break;
        case '8': k.row=5; k.bit=0; break;
        case '9': k.row=4; k.bit=1; break;
        case ' ': k.row=5; k.bit=7; break;
        case 0xff0d:
            k.row=2; k.bit=2; break;
        case '"': k.row=8; k.bit=1; k.shift=1; break; /* Shift+2 */
        case '|': k.row=3; k.bit=2; k.shift=1; break; /* Shift+@ */
        case '@': k.row=3; k.bit=2; break;
        case '-': k.row=3; k.bit=1; break;
        case '.': k.row=3; k.bit=7; break;
        case ',': k.row=4; k.bit=7; break;
        case ';': k.row=3; k.bit=5; break;
        case ':': k.row=3; k.bit=4; break;
        case '/': k.row=3; k.bit=6; break;
        case '!': k.row=8; k.bit=0; k.shift=1; break; /* Shift+1 */
        case '?': k.row=3; k.bit=6; k.shift=1; break; /* Shift+/ */
        case '(': k.row=5; k.bit=0; k.shift=1; break; /* Shift+8 */
        case ')': k.row=4; k.bit=1; k.shift=1; break; /* Shift+9 */
        case '=': k.row=3; k.bit=1; k.shift=1; break; /* Shift+- */
        case '+': k.row=3; k.bit=4; k.shift=1; break; /* Shift+; (actually Shift+: on CPC layout) */
        case '*': k.row=3; k.bit=5; k.shift=1; break; /* Shift+: (actually Shift+; on CPC layout) */
        default: break;
    }
    return k;
}

static void autotype_press(unsigned int ks) {
    cpc_keymap_t k = ascii_to_cpc(ks);
    if (k.row == 0xFF) return;
    if (k.shift) cpc_key_matrix_set(2, 5, 1);
    cpc_key_matrix_set(k.row, k.bit, 1);
}

static void autotype_release(unsigned int ks) {
    cpc_keymap_t k = ascii_to_cpc(ks);
    if (k.row == 0xFF) return;
    cpc_key_matrix_set(k.row, k.bit, 0);
    if (k.shift) cpc_key_matrix_set(2, 5, 0);
}

static unsigned int char_to_keysym(char c) {
    if (c == '\r') return 0xff0d;
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
        autotype_press(ks);
    } else if (g_char_frame == PRESS_FRAMES) {
        autotype_release(ks);
    } else if (g_char_frame >= FRAMES_PER_CHAR - 1) {
        g_pos++;
        g_char_frame = 0;
        if (g_pos >= g_len) {
            printf("autotype: done\n");
        }
        return;
    }

    g_char_frame++;
}
