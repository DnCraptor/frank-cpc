#ifndef CPC_DANDANATOR_H
#define CPC_DANDANATOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a Dandanator EEPROM image (.dan file, up to 512KB).
 * Returns 0 on success. */
int dandanator_insert(const char *path);

/* Remove the Dandanator cartridge. */
void dandanator_eject(void);

/* Returns 1 if a Dandanator is inserted. */
int dandanator_is_inserted(void);

/* Called by Z80 executor on every trapped write instruction.
 * pc = opcode address, opcode = displacement byte, val = written value.
 * Returns 1 if the write was intercepted. */
int dandanator_trap_write(uint16_t pc, uint8_t opcode, uint8_t val);

/* Called by Z80 executor on RET instruction for delayed config. */
void dandanator_trap_ret(void);

/* Get pointer to the currently mapped 16KB bank (or NULL if not inserted). */
uint8_t *dandanator_get_mapped_bank(void);

/* Reset Dandanator state (called on CPC reset). */
void dandanator_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CPC_DANDANATOR_H */
