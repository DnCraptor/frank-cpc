/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_settings.c — runtime settings store and persistence.
 */

#include "cpc_settings.h"
#include "tape.h"
#include "ff.h"

/* CPC subsystem headers needed for apply/reset */
#include "mem.h"
#include "io.h"
#include "Z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* CPC globals set by cpc_settings_apply(). */
extern int  CPCtype;
extern int  CPCMaxMem;
extern int  MonoScreen;
extern int  ay_stereo_mode;
extern Z80  cpu;
extern void ResetFDC(void);
extern void InitIO(void);
extern void InitColors(void);

/* ---- ROM list --------------------------------------------------------- */

char g_cpc_rom_list[CPC_ROM_MAX][32];
int  g_cpc_rom_count = 1;   /* always at least the "Auto" entry */

/* Simple insertion sort for the ROM filenames (index 1 onward). */
static void sort_rom_list(void) {
    for (int i = 2; i < g_cpc_rom_count; ++i) {
        char tmp[32];
        strncpy(tmp, g_cpc_rom_list[i], 31); tmp[31] = 0;
        int j = i - 1;
        while (j >= 1 && strcasecmp(g_cpc_rom_list[j], tmp) > 0) {
            strncpy(g_cpc_rom_list[j + 1], g_cpc_rom_list[j], 31);
            --j;
        }
        strncpy(g_cpc_rom_list[j + 1], tmp, 31);
    }
}

void cpc_settings_scan_roms(void) {
    strncpy(g_cpc_rom_list[0], "Auto", 31);
    g_cpc_rom_count = 1;

    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, "/cpc/rom") != FR_OK) return;

    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        size_t n = strlen(fi.fname);
        if (n < 5) continue;
        const char *ext = fi.fname + n - 4;
        if (strcasecmp(ext, ".rom") != 0) continue;
        if (g_cpc_rom_count >= CPC_ROM_MAX) break;
        strncpy(g_cpc_rom_list[g_cpc_rom_count], fi.fname, 31);
        g_cpc_rom_list[g_cpc_rom_count][31] = 0;
        ++g_cpc_rom_count;
    }
    f_closedir(&dir);
    sort_rom_list();
    printf("settings: found %d ROM(s) in /cpc/rom\n", g_cpc_rom_count - 1);
}

/* ---- defaults -------------------------------------------------------- */

cpc_settings_t g_cpc_settings = {
    .model    = 2,  /* CPC 6128 */
    .memory   = 1,  /* 128 KB   */
    .monitor  = 0,  /* Color    */
    .customer = 0,  /* Amstrad  */
    .rom_idx  = 0,  /* Auto     */
#if defined(HDMI_PIO_AUDIO)
    .audio_driver = CPC_AUDIO_HDMI,
#else
    .audio_driver = CPC_AUDIO_I2S,
#endif
};

bool g_cpc_settings_dirty = false;

/* ---- value tables ----------------------------------------------------- */

static const char *MODEL_LABELS[]    = { "CPC 464", "CPC 664", "CPC 6128" };
static const char *MEMORY_LABELS[]   = { "64 KB", "128 KB", "576 KB" };
static const char *MONITOR_LABELS[]  = { "Color", "Green" };
static const char *FAST_TAPE_LABELS[] = { "Off", "On" };
static const char *STEREO_LABELS[] = { "Mono", "ACB", "ABC" };

static const char *AUDIO_DRV_LABELS[CPC_AUDIO_COUNT] = {
    "I2S", "PWM", "HDMI",
};

/* Audio driver cycle per build configuration. */
#if defined(HDMI_PIO_AUDIO)
static const uint8_t AUDIO_DRV_CYCLE[] = { CPC_AUDIO_HDMI };
#else
static const uint8_t AUDIO_DRV_CYCLE[] = { CPC_AUDIO_I2S, CPC_AUDIO_PWM };
#endif
#define AUDIO_DRV_CYCLE_LEN ((int)(sizeof(AUDIO_DRV_CYCLE) / sizeof(AUDIO_DRV_CYCLE[0])))

static const char *CUSTOMER_LABELS[] = {
    "Amstrad", "Schneider", "ISP", "Triumph",
    "Saisho",  "Solavox",   "AWA", "Orion",
};

/* Raw Customer byte value for each index (PPI Port B bits [3:0]). */
static const uint8_t CUSTOMER_VALUES[] = {
    14,  /* Amstrad   */
    15,  /* Schneider */
    12,  /* ISP       */
    13,  /* Triumph   */
     0,  /* Saisho    */
     2,  /* Solavox   */
     4,  /* AWA       */
     6,  /* Orion     */
};

/* ---- API ------------------------------------------------------------- */

int cpc_settings_choices(cpc_setting_id_t id) {
    switch (id) {
        case CPC_SETTING_MODEL:    return 3;
        case CPC_SETTING_MEMORY:   return 3;
        case CPC_SETTING_MONITOR:  return 2;
        case CPC_SETTING_CUSTOMER: return 8;
        case CPC_SETTING_AUDIO_IN: return 2;
        case CPC_SETTING_FAST_TAPE: return 2;
        case CPC_SETTING_STEREO:  return 3;
        case CPC_SETTING_AUDIO_DRV: return AUDIO_DRV_CYCLE_LEN;
        case CPC_SETTING_ROM:      return g_cpc_rom_count > 0 ? g_cpc_rom_count : 1;
        default: return 0;
    }
}

const char *cpc_settings_label(cpc_setting_id_t id) {
    switch (id) {
        case CPC_SETTING_MODEL:    return "Model";
        case CPC_SETTING_MEMORY:   return "Memory";
        case CPC_SETTING_MONITOR:  return "Monitor";
        case CPC_SETTING_CUSTOMER: return "Customer";
        case CPC_SETTING_AUDIO_IN: return "Audio In";
        case CPC_SETTING_FAST_TAPE: return "Fast Tape";
        case CPC_SETTING_STEREO:  return "Audio Output";
        case CPC_SETTING_AUDIO_DRV: return "Audio Driver";
        case CPC_SETTING_ROM:      return "ROM";
        default: return "?";
    }
}

const char *cpc_settings_value_label(cpc_setting_id_t id) {
    switch (id) {
        case CPC_SETTING_MODEL:    return MODEL_LABELS[g_cpc_settings.model];
        case CPC_SETTING_MEMORY:   return MEMORY_LABELS[g_cpc_settings.memory];
        case CPC_SETTING_MONITOR:  return MONITOR_LABELS[g_cpc_settings.monitor];
        case CPC_SETTING_CUSTOMER: return CUSTOMER_LABELS[g_cpc_settings.customer];
        case CPC_SETTING_AUDIO_IN: return tape_get_gpio_mode() ? "GPIO22" : "Off";
        case CPC_SETTING_FAST_TAPE: return FAST_TAPE_LABELS[g_cpc_settings.fast_tape & 1];
        case CPC_SETTING_STEREO:  return STEREO_LABELS[g_cpc_settings.stereo % 3];
        case CPC_SETTING_AUDIO_DRV:
            if (g_cpc_settings.audio_driver < CPC_AUDIO_COUNT)
                return AUDIO_DRV_LABELS[g_cpc_settings.audio_driver];
            return "?";
        case CPC_SETTING_ROM: {
            uint8_t idx = g_cpc_settings.rom_idx;
            if (idx >= (uint8_t)g_cpc_rom_count) idx = 0;
            return g_cpc_rom_list[idx];
        }
        default: return "?";
    }
}

bool cpc_settings_needs_reset(cpc_setting_id_t id) {
    return id != CPC_SETTING_MONITOR && id != CPC_SETTING_AUDIO_IN
        && id != CPC_SETTING_FAST_TAPE && id != CPC_SETTING_STEREO
        && id != CPC_SETTING_AUDIO_DRV;
}

static void step_u8(uint8_t *v, int delta, int n) {
    int cur = (int)*v + delta;
    while (cur < 0)  cur += n;
    while (cur >= n) cur -= n;
    *v = (uint8_t)cur;
}

void cpc_settings_step(cpc_setting_id_t id, int delta) {
    int n = cpc_settings_choices(id);
    if (n <= 0) return;
    if (cpc_settings_needs_reset(id))
        g_cpc_settings_dirty = true;
    switch (id) {
        case CPC_SETTING_MODEL:    step_u8(&g_cpc_settings.model,    delta, n); break;
        case CPC_SETTING_MEMORY:   step_u8(&g_cpc_settings.memory,   delta, n); break;
        case CPC_SETTING_MONITOR:
            step_u8(&g_cpc_settings.monitor, delta, n);
            cpc_settings_apply_visual();
            break;
        case CPC_SETTING_CUSTOMER: step_u8(&g_cpc_settings.customer, delta, n); break;
        case CPC_SETTING_AUDIO_IN:
            tape_set_gpio_mode(!tape_get_gpio_mode());
            break;
        case CPC_SETTING_FAST_TAPE:
            step_u8(&g_cpc_settings.fast_tape, delta, n);
            break;
        case CPC_SETTING_STEREO:
            step_u8(&g_cpc_settings.stereo, delta, n);
            ay_stereo_mode = g_cpc_settings.stereo;
            break;
        case CPC_SETTING_AUDIO_DRV: {
            int idx = 0;
            for (int i = 0; i < AUDIO_DRV_CYCLE_LEN; i++)
                if (AUDIO_DRV_CYCLE[i] == g_cpc_settings.audio_driver) { idx = i; break; }
            idx = (idx + AUDIO_DRV_CYCLE_LEN + delta) % AUDIO_DRV_CYCLE_LEN;
            g_cpc_settings.audio_driver = AUDIO_DRV_CYCLE[idx];
            break;
        }
        case CPC_SETTING_ROM:      step_u8(&g_cpc_settings.rom_idx,  delta, n); break;
        default: break;
    }
    cpc_settings_save();
}

void cpc_settings_apply_visual(void) {
    MonoScreen = g_cpc_settings.monitor ? 32 : 0;
}

void cpc_settings_apply(void) {
    static const int MEM_MAP[] = { 64, 128, 576 };

    /* Model */
    CPCtype = (int)g_cpc_settings.model;

    /* Memory */
    int idx = g_cpc_settings.memory;
    if (idx > 2) idx = 2;
    CPCMaxMem = MEM_MAP[idx];

    /* Monitor */
    cpc_settings_apply_visual();

    /* Customer — raw PPI Port B value */
    Customer = (char)CUSTOMER_VALUES[g_cpc_settings.customer & 7];

    /* AY stereo mode */
    ay_stereo_mode = g_cpc_settings.stereo;

    /* ROM override — forced Auto until ROM selection is re-enabled */
    g_basic_rom_override[0] = '\0';
}

/* Full CPC reset applying all settings.  Called from cpc_ui when the
 * user selects "Apply & Reset". */
void cpc_settings_do_reset(void) {
    g_cpc_settings_dirty = false;
    cpc_settings_apply();
    printf("settings: reset CPCtype=%d CPCMaxMem=%d MonoScreen=%d Customer=%d rom=%s\n",
           CPCtype, CPCMaxMem, MonoScreen, (int)(unsigned char)Customer,
           g_basic_rom_override[0] ? g_basic_rom_override : "(auto)");
    InitIO();
    InitColors();
    InitMem();
    ResetFDC();
    ResetZ80(&cpu);
}

/* ---- INI persistence ------------------------------------------------- */

#define CPC_INI_PATH "/cpc/cpc.ini"

typedef struct {
    const char *key;
    uint8_t    *slot;
    uint8_t     max;
} ini_field_t;

static const ini_field_t INI_FIELDS[] = {
    { "model",    &g_cpc_settings.model,    3 },
    { "memory",   &g_cpc_settings.memory,   3 },
    { "monitor",  &g_cpc_settings.monitor,  2 },
    { "customer", &g_cpc_settings.customer, 8 },
    { "fast_tape", &g_cpc_settings.fast_tape, 2 },
    { "stereo",    &g_cpc_settings.stereo,    3 },
    { "audio_drv", &g_cpc_settings.audio_driver, CPC_AUDIO_COUNT },
};
#define INI_FIELD_COUNT ((int)(sizeof(INI_FIELDS)/sizeof(INI_FIELDS[0])))

static void ini_trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

bool cpc_settings_load(void) {
    /* Scan ROM directory first so rom_idx can be matched. */
    cpc_settings_scan_roms();

    FIL f;
    if (f_open(&f, CPC_INI_PATH, FA_READ) != FR_OK) {
        printf("settings: no cpc.ini, using defaults\n");
        return false;
    }
    char line[128];
    while (f_gets(line, sizeof(line), &f)) {
        ini_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ini_trim(key); ini_trim(val);

        /* autorun — freeform string, stored verbatim */
        if (strcmp(key, "autorun") == 0) {
            strncpy(g_cpc_settings.autorun, val, sizeof(g_cpc_settings.autorun) - 1);
            g_cpc_settings.autorun[sizeof(g_cpc_settings.autorun) - 1] = 0;
            continue;
        }

        /* disk_a / disk_b — full path or filename relative to /cpc/disk/ */
        if (strcmp(key, "disk_a") == 0 || strcmp(key, "disk_b") == 0) {
            char *dst = (key[5] == 'a') ? g_cpc_settings.disk_a : g_cpc_settings.disk_b;
            size_t dsz = sizeof(g_cpc_settings.disk_a);
            /* If no leading '/', prepend /cpc/disk/ */
            if (val[0] != '/') snprintf(dst, dsz, "/cpc/disk/%s", val);
            else                strncpy(dst, val, dsz - 1), dst[dsz - 1] = 0;
            continue;
        }

        /* tape — full path or filename relative to /cpc/disk/ */
        if (strcmp(key, "tape") == 0) {
            if (val[0] != '/') snprintf(g_cpc_settings.tape, sizeof(g_cpc_settings.tape), "/cpc/disk/%s", val);
            else                strncpy(g_cpc_settings.tape, val, sizeof(g_cpc_settings.tape) - 1), g_cpc_settings.tape[sizeof(g_cpc_settings.tape) - 1] = 0;
            continue;
        }

        /* Numeric fields */
        for (int i = 0; i < INI_FIELD_COUNT; ++i) {
            if (strcmp(key, INI_FIELDS[i].key) == 0) {
                char *endp = NULL;
                long v = strtol(val, &endp, 10);
                if (endp && endp != val && v >= 0 && v < INI_FIELDS[i].max)
                    *INI_FIELDS[i].slot = (uint8_t)v;
                break;
            }
        }

        /* ROM filename — find in scanned list */
        if (strcmp(key, "rom") == 0) {
            for (int j = 0; j < g_cpc_rom_count; ++j) {
                if (strcasecmp(val, g_cpc_rom_list[j]) == 0) {
                    g_cpc_settings.rom_idx = (uint8_t)j;
                    break;
                }
            }
        }
    }
    f_close(&f);
    printf("settings: loaded %s\n", CPC_INI_PATH);
    return true;
}

bool cpc_settings_save(void) {
    FIL f;
    if (f_open(&f, CPC_INI_PATH, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("settings: save failed\n");
        return false;
    }
    char buf[64];
    UINT bw;
    int n = snprintf(buf, sizeof(buf), "# frank-cpc settings\n");
    f_write(&f, buf, (UINT)n, &bw);
    for (int i = 0; i < INI_FIELD_COUNT; ++i) {
        n = snprintf(buf, sizeof(buf), "%s=%u\n",
                     INI_FIELDS[i].key, (unsigned)*INI_FIELDS[i].slot);
        f_write(&f, buf, (UINT)n, &bw);
    }
    /* ROM: store filename (robust against re-ordering) */
    uint8_t ridx = g_cpc_settings.rom_idx;
    if (ridx >= (uint8_t)g_cpc_rom_count) ridx = 0;
    n = snprintf(buf, sizeof(buf), "rom=%s\n", g_cpc_rom_list[ridx]);
    f_write(&f, buf, (UINT)n, &bw);
    f_close(&f);
    return true;
}

