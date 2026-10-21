#ifndef Q2PSX_INSPECT_CMD_SYMS_H
#define Q2PSX_INSPECT_CMD_SYMS_H

#include "disc.h"

/*
 * `/MAIN.SYM` — the Psy-Q linker's own symbol table, which the North American
 * disc ships and the European one does not.
 *
 *   syms <disc>          the header, the counts, and a per-source-file census
 *   syms <disc> list     every function: PAL address, its build's address, file
 *   syms <disc> <what>   look one up by name, or by either address
 *
 * Addresses are reported in SLES-01534's space — the space every address in
 * this project's documents and loaders is written in — so a name read here can
 * be pasted straight into `disasm`, `xrefs` or a source comment.
 */
int cmd_syms(const disc *d, const char *what);

#endif /* Q2PSX_INSPECT_CMD_SYMS_H */
