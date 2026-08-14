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
int cmd_moddisasm(const disc *d, const char *map, const char *addr, int count);
int cmd_levdisasm(const disc *d, const char *map, const char *addr, int count);

#endif /* Q2PSX_INSPECT_CMD_EXE_H */
