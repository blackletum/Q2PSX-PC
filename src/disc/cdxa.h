/*
 * cdxa.h — building a raw 2352-byte CD-XA sector.
 *
 * The rest of this project reads sectors; this makes them, and it exists for
 * one reason: a `.STX` is not a byte stream, it is a SECTOR stream, and the
 * difference is not cosmetic. Video rides in Mode 2 Form 1 sectors with 2048
 * bytes of user data, audio rides in Form 2 with 2324, and which form a sector
 * is lives in a subheader that a flat file has thrown away. Every `.STX` sitting
 * extracted in a working tree is 2048 bytes per sector for BOTH — so every one
 * of its audio sectors has lost 276 bytes, which is the warning FORMATS.md §6
 * opens with and the reason an encoder that writes flat files would be writing
 * silent ones.
 *
 * ---------------------------------------------------------------------------
 * How this is known to be right
 * ---------------------------------------------------------------------------
 * Not by trusting the standard: by rebuilding the disc's own sectors. Hand
 * `cd_sector_check` a raw sector off `Quake II (Europe)` and it recomputes the
 * EDC and the P/Q parity from the payload and compares. The disc has 32,442
 * `.STX` sectors and they all have to come back byte-identical, which is a
 * check no amount of reading the specification can substitute for.
 *
 * ---------------------------------------------------------------------------
 * The layout
 * ---------------------------------------------------------------------------
 *     0x000  12  sync            00 FF FF FF FF FF FF FF FF FF FF 00
 *     0x00C   4  address + mode  MIN SEC FRAC in BCD, then 2
 *     0x010   8  subheader       file, channel, submode, coding — TWICE
 *     0x018      user data       2048 (Form 1) or 2324 (Form 2)
 *
 *   Form 1:  0x818   4  EDC over 0x010..0x817
 *            0x81C 172  P parity      0x8C8 104  Q parity
 *   Form 2:  0x92C   4  EDC over 0x010..0x92B, and no parity at all
 *
 * The ECC is computed with the four address bytes treated as ZERO, which is
 * Mode 2's rule and not Mode 1's — get that wrong and every sector's parity is
 * wrong in a way only a real drive complains about.
 */
#ifndef Q2PSX_DISC_CDXA_H
#define Q2PSX_DISC_CDXA_H

#include "disc.h"
#include "q2psx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build one sector at `lba` (the absolute sector number; the MSF stamp is
 * `lba + 150`).
 *
 * `payload` is CD_SECTOR_FORM1 bytes when `form2` is false and CD_SECTOR_FORM2
 * when it is true. `submode` should carry CD_SUBMODE_FORM2 in the second case;
 * it is forced either way, because a subheader that disagrees with the sector's
 * actual size is the one error that produces a file which mounts and plays
 * nothing.
 */
void cd_sector_build(u8 out[CD_SECTOR_RAW], u32 lba,
                     u8 file, u8 channel, u8 submode, u8 coding,
                     const u8 *payload, bool form2);

/*
 * Check a raw sector's own EDC and parity against what this module computes.
 *
 * Returns a bitmask: 1 = the EDC disagrees, 2 = P parity, 4 = Q parity. Zero
 * means this module reproduces that sector exactly. Form 2 sectors are checked
 * on the EDC alone, and a Form 2 EDC of zero is accepted — the standard allows
 * it to be left blank, and a disc that leaves it blank is not wrong.
 */
u32 cd_sector_check(const u8 raw[CD_SECTOR_RAW]);

/* The two primitives, exposed so the check above and a caller can share them. */
u32  cd_edc(const u8 *p, u32 n);
void cd_ecc_generate(u8 sector[CD_SECTOR_RAW]);

#ifdef __cplusplus
}
#endif

#endif /* Q2PSX_DISC_CDXA_H */
