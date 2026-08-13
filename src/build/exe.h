/*
 * exe.h — the boot executable as an addressable image.
 *
 * Nearly every remaining unknown in this project is a question about code, not
 * about bytes on the disc: where the GPU primitive word is assembled, what the
 * per-frame handlers do, how a bit-packed chunk is read. Answering those needs
 * the executable mapped by *address*, because every reference in FORMATS.md is
 * written as a PSX virtual address.
 *
 * A PS-X EXE is a 2 KiB header followed by one flat segment loaded at `t_addr`.
 * There are no sections and no relocations — the file *is* the memory image — so
 * the address map is a single add, and any address the header does not cover
 * (scratchpad, hardware registers, the heap) simply does not exist here.
 *
 * KUSEG/KSEG0/KSEG1 alias the same physical RAM, so `0x80068A58`, `0x00068A58`
 * and `0xA0068A58` are one address. Everything below normalises before mapping.
 */
#ifndef Q2PSX_EXE_H
#define Q2PSX_EXE_H

#include "disc.h"
#include "q2psx.h"

#define Q2_EXE_HEADER_SIZE 0x800

typedef struct q2_exe {
    q2_buf file;         /* the whole file, header included               */

    u32 pc0;             /* entry point                                   */
    u32 gp0;             /* initial $gp — needed to resolve gp-relative
                          * accesses, which this compiler emits heavily   */
    u32 text_addr;       /* load address of the single segment            */
    u32 text_size;       /* its size in bytes                             */
    u32 bss_addr;        /* zero-filled region, present in the image only
                          * as an address range                           */
    u32 bss_size;
    u32 sp_base;
    u32 sp_size;

    char name[32];       /* "SLES_015.34"                                 */
} q2_exe;

/* Load by explicit filename, or let it ask the disc which executable boots. */
q2_result q2_exe_load(q2_exe *out, const disc *d, const char *exe_name);
void      q2_exe_free(q2_exe *e);

/* Strip the KSEG/KUSEG selector. */
Q2PSX_INLINE u32 q2_exe_norm(u32 addr) { return addr & 0x1FFFFFFFu; }

/* True when [addr, addr+len) lies inside the loaded segment. */
bool q2_exe_contains(const q2_exe *e, u32 addr, u32 len);

/* Pointer to `len` bytes at `addr`, or NULL if that escapes the segment. */
const u8 *q2_exe_ptr(const q2_exe *e, u32 addr, u32 len);

/* Typed reads. Each returns false and leaves *out alone when out of range. */
bool q2_exe_u8 (const q2_exe *e, u32 addr, u8  *out);
bool q2_exe_u16(const q2_exe *e, u32 addr, u16 *out);
bool q2_exe_u32(const q2_exe *e, u32 addr, u32 *out);
bool q2_exe_s16(const q2_exe *e, u32 addr, s16 *out);

/* First and last mapped address, for callers that want to sweep the image. */
Q2PSX_INLINE u32 q2_exe_begin(const q2_exe *e) { return e->text_addr; }
Q2PSX_INLINE u32 q2_exe_end(const q2_exe *e)   { return e->text_addr + e->text_size; }

#endif /* Q2PSX_EXE_H */
