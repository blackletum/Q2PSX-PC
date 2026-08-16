#ifndef Q2PSX_INSPECT_CMD_EXE_H
#define Q2PSX_INSPECT_CMD_EXE_H

#include "disc.h"

int cmd_exe(const disc *d, const char *save_path);
int cmd_disasm(const disc *d, const char *addr, int count);
int cmd_xrefs(const disc *d, const char *addr);
int cmd_funcs(const disc *d, const char *filter);
int cmd_bytes(const disc *d, const char *addr, int count);
int cmd_find(const disc *d, const char *pattern);
int cmd_access(const disc *d, const char *imm, const char *mnemonic);
/* `cre` selects one module out of a map's CreAIBin by name, relocating it on
 * its own so its addresses match what `creatures` prints. NULL relocates the
 * whole chunk, which only agrees with `creatures` on a single-module map. */
int cmd_moddisasm(const disc *d, const char *map, const char *addr, int count,
                  const char *cre);
int cmd_levdisasm(const disc *d, const char *map, const char *addr, int count);
int cmd_modstrings(const disc *d, const char *map, bool level);
int cmd_modxrefs(const disc *d, const char *map, const char *addr, bool level);
/* Hex dump a relocated module's image by address. `bytes` reads the executable
 * and so cannot see a module at all; a module's data — its object tables, its
 * static records — was only reachable by disassembling the code that walked
 * it. This reads it directly. */
int cmd_modbytes(const disc *d, const char *map, const char *addr, int count,
                 bool level);

#endif /* Q2PSX_INSPECT_CMD_EXE_H */
