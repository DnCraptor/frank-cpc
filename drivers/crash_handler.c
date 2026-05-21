/*
 * frank-cpc — CPC emulator for RP2350
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * crash_handler.c — Persistent crash reporting for RP2350 (ARM Cortex-M33).
 *
 * On HardFault, BusFault, or MemManage exceptions the handler saves key
 * registers into watchdog scratch registers 0-3 (which survive soft
 * resets) and forces a watchdog reboot.  On the next boot,
 * crash_handler_check_and_print() detects the saved record and prints
 * a one-shot crash report to stdout.
 *
 * Watchdog scratch layout:
 *   scratch[0] = magic (0xDEAD_C0DE)
 *   scratch[1] = fault type  (3=HardFault, 4=MemManage, 5=BusFault)
 *   scratch[2] = stacked PC  (return address at time of fault)
 *   scratch[3] = CFSR (combined fault status register)
 */

#include "crash_handler.h"

#include "pico/stdlib.h"
#include "hardware/exception.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include <stdio.h>

#define CRASH_MAGIC 0xDEADC0DEu
#define CRASH_SENT  0xDEAD5E41u  /* "sent" marker — prevents reboot loops */
#define HANG_MAGIC  0xDEAD4A46u  /* watchdog timeout (hang detected)      */

/* ------------------------------------------------------------------ */
/* Fault handlers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Common handler body.  'fault_num' matches the ARM exception number
 * (3 = HardFault, 4 = MemManage, 5 = BusFault).
 *
 * The stacked exception frame on Cortex-M33 is:
 *   sp[0]=R0, sp[1]=R1, sp[2]=R2, sp[3]=R3,
 *   sp[4]=R12, sp[5]=LR, sp[6]=PC, sp[7]=xPSR
 *
 * We need the PSP or MSP depending on EXC_RETURN (LR).
 */
static void __attribute__((used))
crash_save_and_reboot(uint32_t fault_num, uint32_t *frame) {
    /* Disable running watchdog first to prevent it firing during save */
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);

    /* If we already reported a crash/hang (CRASH_SENT), don't reset
     * again — just hang.  Prevents infinite reboot loops. */
    if (watchdog_hw->scratch[0] == CRASH_SENT) {
        while (1) __asm volatile("nop");
    }

    watchdog_hw->scratch[0] = CRASH_MAGIC;
    watchdog_hw->scratch[1] = fault_num | (scb_hw->bfar & 0xFFFF0000u);
    watchdog_hw->scratch[2] = frame[6]; /* stacked PC  */
    watchdog_hw->scratch[3] = scb_hw->cfsr; /* combined fault status */

    /* Use SDK watchdog_reboot() to trigger a proper reset.  It sets
     * psm_hw->wdsel (so the PSM actually resets the chip) and
     * configures the watchdog timer correctly.  Scratch registers 0-3
     * survive watchdog resets; scratch[4-7] are used by the SDK. */
    watchdog_reboot(0, 0, 1 /* 1 ms */);
    while (1) __asm volatile("nop");
}

/*
 * Naked fault entry: figure out which stack pointer was in use at the
 * time of the fault and pass it alongside the fault number to the
 * C handler.
 */
#define FAULT_ENTRY(name, num)                                           \
    static void __attribute__((naked)) name(void) {                      \
        __asm volatile(                                                   \
            "mov  r0, #" #num  "\n"  /* fault_num                     */ \
            "tst  lr, #4       \n"  /* bit2 of EXC_RETURN: PSP or MSP*/ \
            "ite  eq           \n"                                       \
            "mrseq r1, msp     \n"                                       \
            "mrsne r1, psp     \n"                                       \
            "b  %[handler]     \n"                                       \
            : : [handler] "i" (crash_save_and_reboot)                    \
        );                                                               \
    }

FAULT_ENTRY(crash_hardfault,  3)
FAULT_ENTRY(crash_memmanage,  4)
FAULT_ENTRY(crash_busfault,   5)

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void crash_handler_install(void) {
    /* Enable MemManage and BusFault as separate exceptions (otherwise
     * they escalate to HardFault).  We clear any pending bits first
     * to avoid immediately triggering on stale faults. */
    scb_hw->cfsr  = 0xFFFFFFFFu;   /* W1C: clear all fault status bits */
    scb_hw->hfsr  = 0xFFFFFFFFu;   /* W1C: clear HardFault status      */
    scb_hw->shcsr |= (1u << 16)   /* MEMFAULTENA  */
                    | (1u << 17);  /* BUSFAULTENA  */

    exception_set_exclusive_handler(HARDFAULT_EXCEPTION,  crash_hardfault);
    exception_set_exclusive_handler(MEMMANAGE_EXCEPTION,  crash_memmanage);
    exception_set_exclusive_handler(BUSFAULT_EXCEPTION,   crash_busfault);

    /* Start a watchdog timer to catch hangs (not just faults).
     * Must be fed periodically via crash_handler_feed().
     * Don't overwrite CRASH_SENT — that prevents infinite reboot loops
     * when the same hang recurs on every boot. */
    if (watchdog_hw->scratch[0] != CRASH_SENT) {
        watchdog_hw->scratch[0] = HANG_MAGIC;
        watchdog_enable(8000, true);  /* 8 second timeout, pause on debug */
    }
}

void crash_handler_feed(void) {
    watchdog_update();
}

void crash_handler_check_and_print(void) {
    uint32_t magic = watchdog_hw->scratch[0];
    if (magic != CRASH_MAGIC && magic != HANG_MAGIC) return;

    if (magic == HANG_MAGIC) {
        printf("\n!!! PREVIOUS HANG DETECTED !!!\n");
        printf("  Watchdog timeout — system was unresponsive\n");
        printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
    } else {
        uint32_t raw1   = watchdog_hw->scratch[1];
        uint32_t fault  = raw1 & 0xFF;
        uint32_t bfar_hi = raw1 & 0xFFFF0000u;
        uint32_t pc     = watchdog_hw->scratch[2];
        uint32_t cfsr   = watchdog_hw->scratch[3];

        const char *name;
        switch (fault) {
            case 3:  name = "HardFault";  break;
            case 4:  name = "MemManage";  break;
            case 5:  name = "BusFault";   break;
            default: name = "Unknown";    break;
        }

        printf("\n!!! PREVIOUS CRASH DETECTED !!!\n");
        printf("  Fault : %s (exception %lu)\n", name, (unsigned long)fault);
        printf("  PC    : 0x%08lX\n", (unsigned long)pc);
        printf("  CFSR  : 0x%08lX\n", (unsigned long)cfsr);
        if (cfsr & 0xFF)
            printf("  MMFSR : 0x%02lX (MemManage)\n", (unsigned long)(cfsr & 0xFF));
        if (cfsr & 0xFF00) {
            printf("  BFSR  : 0x%02lX (BusFault)\n", (unsigned long)((cfsr >> 8) & 0xFF));
            if ((cfsr >> 8) & 0x80)
                printf("  BFAR  : 0x%04lXxxxx (upper 16 bits)\n", (unsigned long)(bfar_hi >> 16));
        }
        if (cfsr & 0xFFFF0000)
            printf("  UFSR  : 0x%04lX (UsageFault)\n", (unsigned long)((cfsr >> 16) & 0xFFFF));
        printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
    }

    /* Mark as "sent" so the fault handler knows we already rebooted
     * once for a crash — if the same fault recurs, it will just hang
     * instead of looping. */
    watchdog_hw->scratch[0] = CRASH_SENT;
    watchdog_hw->scratch[1] = 0;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[3] = 0;
}
