/*
 * frank-cpc — Amstrad CPC for RP2350
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cpc_dandanator.cpp — Dandanator cartridge emulation.
 *
 * The Dandanator is a 512KB EEPROM (32 × 16KB banks) that replaces the
 * lower ROM when enabled. This implementation traps LD (IX/IY+d),A writes
 * for d = 0..3 and applies delayed configuration changes on RET.
 */

#include "cpc_dandanator.h"
#include "cpc_cartridge.h"
#include "cap32/cap32.h"

extern "C" {
#include "ff.h"
#include "psram_allocator.h"
}

#include <cstdio>
#include <cstring>

extern void ga_memory_manager();

namespace {

constexpr size_t kDandanatorBankSize = 16 * 1024;
constexpr size_t kDandanatorBankCount = 32;
constexpr size_t kDandanatorSize = kDandanatorBankSize * kDandanatorBankCount;

struct dandanator_zone_t {
    uint8_t config;
    uint8_t bank;
};

uint8_t *dan_eeprom = nullptr;
bool dan_inserted = false;
dandanator_zone_t dan_pending[2] = {};
dandanator_zone_t dan_current[2] = {};

void dandanator_apply_pending(void) {
    std::memcpy(dan_current, dan_pending, sizeof(dan_current));
    ga_memory_manager();
}

} // namespace

int dandanator_insert(const char *path) {
    if (!path || !*path) {
        return -1;
    }

    if (!dan_eeprom) {
        dan_eeprom = static_cast<uint8_t *>(psram_malloc(kDandanatorSize));
        if (!dan_eeprom) {
            std::printf("dandanator: PSRAM alloc failed\n");
            return -1;
        }
    }

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        std::printf("dandanator: cannot open %s\n", path);
        return -1;
    }

    cpc_cartridge_eject();
    dandanator_eject();
    std::memset(dan_eeprom, 0xFF, kDandanatorSize);

    UINT br = 0;
    FRESULT fr = f_read(&f, dan_eeprom, kDandanatorSize, &br);
    f_close(&f);
    if (fr != FR_OK) {
        std::printf("dandanator: read failed for %s\n", path);
        return -1;
    }

    dan_inserted = true;
    dandanator_reset();
    std::printf("dandanator: loaded %u bytes from %s\n", static_cast<unsigned>(br), path);
    return 0;
}

void dandanator_eject(void) {
    if (!dan_inserted) {
        return;
    }

    dan_inserted = false;
    std::memset(dan_pending, 0, sizeof(dan_pending));
    std::memset(dan_current, 0, sizeof(dan_current));
    ga_memory_manager();
    std::printf("dandanator: ejected\n");
}

int dandanator_is_inserted(void) {
    return dan_inserted ? 1 : 0;
}

int dandanator_trap_write(uint16_t pc, uint8_t opcode_offset, uint8_t val) {
    (void)pc;

    if (!dan_inserted || opcode_offset > 3) {
        return 0;
    }

    switch (opcode_offset) {
        case 0:
            dan_pending[0].config = val;
            break;
        case 1:
            dan_pending[1].config = val;
            break;
        case 2:
            dan_pending[0].bank = val & 31;
            break;
        case 3:
            dan_pending[1].bank = val & 31;
            break;
        default:
            return 0;
    }

    if ((dan_pending[1].config & 0x40) == 0) {
        dandanator_apply_pending();
    }
    return 1;
}

void dandanator_trap_ret(void) {
    if (!dan_inserted || (dan_pending[1].config & 0x40) == 0) {
        return;
    }

    dan_pending[1].config &= static_cast<uint8_t>(~0x40u);
    dandanator_apply_pending();
}

uint8_t *dandanator_get_mapped_bank(void) {
    if (!dan_inserted || (dan_current[0].config & 1) == 0) {
        return nullptr;
    }

    return dan_eeprom + (static_cast<size_t>(dan_current[0].bank & 31) * kDandanatorBankSize);
}

void dandanator_reset(void) {
    dan_pending[0].config = 1;
    dan_pending[0].bank = 0;
    dan_pending[1].config = 0;
    dan_pending[1].bank = 0;
    std::memcpy(dan_current, dan_pending, sizeof(dan_current));

    if (dan_inserted) {
        ga_memory_manager();
    }
}
