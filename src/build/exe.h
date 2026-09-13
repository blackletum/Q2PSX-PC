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
 *
 * TWO ADDRESS SPACES. The readers below take the address as it is in THIS
 * image. The addresses this project writes down are SLES-01534's, and a table
 * loader that means one of those asks `q2_exe_addr` for where this build keeps
 * it. What it then reads is this build's own bytes — a pointer inside a table is
 * already this image's address and is followed as it stands.
 */
#ifndef Q2PSX_EXE_H
#define Q2PSX_EXE_H

#include "disc.h"
#include "q2psx.h"

#define Q2_EXE_HEADER_SIZE 0x800

struct q2_build_desc;

typedef struct q2_exe {
    q2_buf file;         /* the whole file, header included               */

    /* The catalogued build these bytes hash to — or, failing that, the one
     * whose boot file this is, with `build_exact` clear — or NULL. Decided
     * from the image at load, so a caller cannot pair an executable with the
     * wrong build's addresses. */
    const struct q2_build_desc *build;
    bool build_exact;

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

/*
 * Where this image keeps what SLES-01534 keeps at `pal`. The identity on
 * SLES-01534; the catalogued layout on any other known build (ident.h).
 * Returns 0 when there is nothing to translate to — an address inside code
 * that changed between the builds, or any address at all in an executable no
 * catalogue entry describes. 0 is not in any image, so a read through it fails
 * the way a read of a bad address should.
 */
u32  q2_exe_addr(const q2_exe *e, u32 pal);

/* ...and back: the SLES-01534 address of this image's `addr`, or 0. */
u32  q2_exe_pal(const q2_exe *e, u32 addr);

/* True when SLES-01534 addresses mean something in this image. */
bool q2_exe_has_layout(const q2_exe *e);

/*
 * For a checker comparing an instruction's immediate against SLES-01534's:
 * true when `pal_lo`, read as the low half of some SLES-01534 address, is
 * `got` once that address is translated into this image — a global's `%lo`
 * that moved with the global. Always false on SLES-01534 itself.
 */
bool q2_exe_lo_relocates(const q2_exe *e, s32 pal_lo, s32 got);

/*
 * The same for a whole word: true when `got`, read from this image, is
 * SLES-01534's `pal_word` with every address in it translated — a pointer in
 * data, a `j`/`jal` target, an I-type whose immediate is a moved `%lo`. Equal
 * words agree trivially.
 */
bool q2_exe_word_relocates(const q2_exe *e, u32 pal_word, u32 got);

#endif /* Q2PSX_EXE_H */
