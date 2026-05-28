/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_settings.h — mutable runtime settings.
 *
 * Persisted to /cpc/cpc.ini on the SD card.
 * Model/Memory/Customer/Speed/Sound/Stereo/ROM changes require a full CPC
 * reset (InitMem + ResetZ80).
 * Monitor, Limit Speed, and Volume take effect immediately.
 */
#ifndef CPC_SETTINGS_H
#define CPC_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of ROM files shown (including the "Auto" entry). */
#define CPC_ROM_MAX 17

/* Scanned ROM filenames. Index 0 is always "Auto" (CPCtype-derived). */
extern char g_cpc_rom_list[CPC_ROM_MAX][32];
extern int  g_cpc_rom_count;   /* includes the "Auto" entry */

/* Audio-output backend. I2S targets the external DAC on GP9/10/11;
 * PWM drives GP10/11 through an RC filter. Both SHARE the same GPIOs,
 * so switching between them re-configures pin function. HDMI is only
 * reachable on HDMI_PIO_AUDIO builds. */
typedef enum {
    CPC_AUDIO_I2S = 0,
    CPC_AUDIO_PWM,
    CPC_AUDIO_HDMI,         /* HDMI_PIO_AUDIO builds only */
    CPC_AUDIO_COUNT
} cpc_audio_driver_t;

typedef enum {
    CPC_SETTING_MODEL = 0,  /* CPC 464 / 664 / 6128          (needs reset) */
    CPC_SETTING_MEMORY,     /* 64K / 128K / 576K              (needs reset) */
    CPC_SETTING_MONITOR,    /* Color / Green                  (live)        */
    CPC_SETTING_CUSTOMER,   /* Amstrad / Schneider / ...      (needs reset) */
    CPC_SETTING_SPEED,      /* 50%–400% emulation speed       (needs reset) */
    CPC_SETTING_LIMIT_SPEED,/* On / Off — cap to set speed    (live)        */
    CPC_SETTING_SND_ENABLED,/* On / Off — master sound toggle (needs reset) */
    CPC_SETTING_VOLUME,     /* 0–100% volume                  (live)        */
    CPC_SETTING_STEREO,     /* Mono / Stereo                  (needs reset) */
    CPC_SETTING_AUDIO_IN,   /* Off / GPIO22                   (live)        */
    CPC_SETTING_FAST_TAPE,  /* Off / On — skip frame sync     (live)        */
    CPC_SETTING_AUDIO_DRV,  /* I2S / PWM / HDMI               (live)        */
    CPC_SETTING_COUNT,      /* ---- visible settings end here ---- */
    CPC_SETTING_ROM,        /* Auto / <filename.rom>  — hidden for now      */
} cpc_setting_id_t;

typedef struct {
    uint8_t model;    /* 0=CPC464, 1=CPC664, 2=CPC6128 */
    uint8_t memory;   /* 0=64K, 1=128K, 2=576K         */
    uint8_t monitor;  /* 0=Color, 1=Green               */
    uint8_t customer; /* 0..7 index (Amstrad..Orion)    */
    uint8_t rom_idx;  /* index into g_cpc_rom_list      */
    char    autorun[64]; /* command to auto-type on boot, e.g. RUN"PRINCE */
    char    disk_a[128]; /* full path to disk image for drive A on boot   */
    char    disk_b[128]; /* full path to disk image for drive B on boot   */
    char    tape[128];   /* full path to tape image (.cdt/.cas) on boot   */
    uint8_t fast_tape;   /* 0=Off, 1=On — run unthrottled during tape load */
    uint8_t stereo;      /* 0=Mono, 1=Stereo                              */
    uint8_t audio_driver; /* cpc_audio_driver_t: I2S / PWM / HDMI         */
    uint8_t speed;       /* index into SPEED_PRESETS (0..6)                */
    uint8_t limit_speed; /* 0=Off, 1=On                                   */
    uint8_t snd_enabled; /* 0=Off, 1=On                                   */
    uint8_t volume;      /* 0..10 (×10 = 0%..100%)                        */
} cpc_settings_t;

extern cpc_settings_t g_cpc_settings;

/* True when a reset-requiring setting has been changed but the CPC
 * has not been reset yet.  Cleared by cpc_settings_do_reset(). */
extern bool g_cpc_settings_dirty;

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

/* Apply all settings to the CPC globals (model, ram, jumpers, speed,
 * sound, stereo).  Call before InitMem() / ResetZ80(). */
void cpc_settings_apply(void);

/* Apply only the live (no-reset) settings — currently just the monitor. */
void cpc_settings_apply_visual(void);

/* Scan /cpc/rom/ for *.rom files and populate g_cpc_rom_list.
 * Called automatically by cpc_settings_load(). */
void cpc_settings_scan_roms(void);

/* Load /cpc/cpc.ini into g_cpc_settings.  Missing file → defaults. */
bool cpc_settings_load(void);

/* Serialise g_cpc_settings to /cpc/cpc.ini. */
bool cpc_settings_save(void);

#endif /* CPC_SETTINGS_H */
