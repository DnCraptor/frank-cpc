/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_settings.h — mutable runtime settings.
 *
 * Persisted to /cpc/cpc.ini on the SD card.
 * Model/Memory/Customer changes require a full CPC reset (InitMem + ResetZ80).
 * Monitor (colour filter) takes effect immediately.
 */
#ifndef CPC_SETTINGS_H
#define CPC_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CPC_SETTING_MODEL = 0,  /* CPC 464 / 664 / 6128          (needs reset) */
    CPC_SETTING_MEMORY,     /* 64K / 128K / 576K              (needs reset) */
    CPC_SETTING_MONITOR,    /* Color / Green                  (live)        */
    CPC_SETTING_CUSTOMER,   /* Amstrad / Schneider / ...      (needs reset) */
    CPC_SETTING_COUNT
} cpc_setting_id_t;

typedef struct {
    uint8_t model;    /* 0=CPC464, 1=CPC664, 2=CPC6128 */
    uint8_t memory;   /* 0=64K, 1=128K, 2=576K         */
    uint8_t monitor;  /* 0=Color, 1=Green               */
    uint8_t customer; /* 0..7 index (Amstrad..Orion)    */
} cpc_settings_t;

extern cpc_settings_t g_cpc_settings;

/* Number of choices for a given setting. */
int cpc_settings_choices(cpc_setting_id_t id);

/* Human-readable label for the setting itself. */
const char *cpc_settings_label(cpc_setting_id_t id);

/* Human-readable label for the current value. */
const char *cpc_settings_value_label(cpc_setting_id_t id);

/* Cycle the value by delta (+1 or -1). Wraps around. */
void cpc_settings_step(cpc_setting_id_t id, int delta);

/* True when the setting requires a CPC reset to take effect. */
bool cpc_settings_needs_reset(cpc_setting_id_t id);

/* Apply all settings to the CPC globals (CPCtype, CPCMaxMem, MonoScreen,
 * Customer).  Call before InitMem() / ResetZ80() on boot or after reset. */
void cpc_settings_apply(void);

/* Apply only the live (no-reset) settings — currently just the monitor. */
void cpc_settings_apply_visual(void);

/* Load /cpc/cpc.ini into g_cpc_settings.  Missing file → defaults. */
bool cpc_settings_load(void);

/* Serialise g_cpc_settings to /cpc/cpc.ini. */
bool cpc_settings_save(void);

#endif /* CPC_SETTINGS_H */
