/*
 * frank-cpc — CPC emulator for RP2350
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cpc_settings.c — runtime settings store and persistence.
 */

#include "cpc_settings.h"
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
extern Z80  cpu;
extern void ResetFDC(void);
extern void InitIO(void);
extern void InitColors(void);

/* ---- defaults -------------------------------------------------------- */

cpc_settings_t g_cpc_settings = {
    .model    = 2,  /* CPC 6128 */
    .memory   = 1,  /* 128 KB   */
    .monitor  = 0,  /* Color    */
    .customer = 0,  /* Amstrad  */
};

/* ---- value tables ----------------------------------------------------- */

static const char *MODEL_LABELS[]    = { "CPC 464", "CPC 664", "CPC 6128" };
static const char *MEMORY_LABELS[]   = { "64 KB", "128 KB", "576 KB" };
static const char *MONITOR_LABELS[]  = { "Color", "Green" };
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
        default: return 0;
    }
}

const char *cpc_settings_label(cpc_setting_id_t id) {
    switch (id) {
        case CPC_SETTING_MODEL:    return "Model";
        case CPC_SETTING_MEMORY:   return "Memory";
        case CPC_SETTING_MONITOR:  return "Monitor";
        case CPC_SETTING_CUSTOMER: return "Customer";
        default: return "?";
    }
}

const char *cpc_settings_value_label(cpc_setting_id_t id) {
    switch (id) {
        case CPC_SETTING_MODEL:    return MODEL_LABELS[g_cpc_settings.model];
        case CPC_SETTING_MEMORY:   return MEMORY_LABELS[g_cpc_settings.memory];
        case CPC_SETTING_MONITOR:  return MONITOR_LABELS[g_cpc_settings.monitor];
        case CPC_SETTING_CUSTOMER: return CUSTOMER_LABELS[g_cpc_settings.customer];
        default: return "?";
    }
}

bool cpc_settings_needs_reset(cpc_setting_id_t id) {
    return id != CPC_SETTING_MONITOR;
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
    switch (id) {
        case CPC_SETTING_MODEL:    step_u8(&g_cpc_settings.model,    delta, n); break;
        case CPC_SETTING_MEMORY:   step_u8(&g_cpc_settings.memory,   delta, n); break;
        case CPC_SETTING_MONITOR:
            step_u8(&g_cpc_settings.monitor, delta, n);
            cpc_settings_apply_visual();
            break;
        case CPC_SETTING_CUSTOMER: step_u8(&g_cpc_settings.customer, delta, n); break;
        default: break;
    }
    cpc_settings_save();
}

void cpc_settings_apply_visual(void) {
    MonoScreen = g_cpc_settings.monitor ? 1 : 0;
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
}

/* Full CPC reset applying all settings.  Called from cpc_ui when the
 * user selects "Apply & Reset". */
void cpc_settings_do_reset(void) {
    cpc_settings_apply();
    printf("settings: reset CPCtype=%d CPCMaxMem=%d MonoScreen=%d Customer=%d\n",
           CPCtype, CPCMaxMem, MonoScreen, (int)(unsigned char)Customer);
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
        for (int i = 0; i < INI_FIELD_COUNT; ++i) {
            if (strcmp(key, INI_FIELDS[i].key) == 0) {
                char *endp = NULL;
                long v = strtol(val, &endp, 10);
                if (endp && endp != val && v >= 0 && v < INI_FIELDS[i].max)
                    *INI_FIELDS[i].slot = (uint8_t)v;
                break;
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
    f_close(&f);
    return true;
}
