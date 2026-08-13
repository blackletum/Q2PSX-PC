# Quake II (PlayStation) — On-Disc Data Format Specification

**Target:** *Quake II*, PAL, `SLES-01534`, ported by Hammerhead / published by Activision, build stamped
1999‑09‑20. This document describes the file and in-memory structures of the retail disc for the purpose of
writing a **native PC reimplementation** (in the tradition of ScummVM / OpenMW / devilutionX). It is an
interoperability and preservation document: it records *structure only* — offsets, widths, counts and
invariants. It contains **no game asset bytes, no decoded audio or video, no level text, and no name lists
lifted from the game data**. A legally owned disc is required to use any of it.

Every claim below was derived by parsing the actual files and, where the meaning depended on runtime
behaviour, by disassembling the MIPS R3000 boot executable. Each claim carries a confidence marker. Claims
that an earlier analysis pass got wrong and that a later adversarial pass overturned are called out inline as
**CORRECTION**, because a reimplementation written to the original wording would have broken.

---

## Status legend

| Marker | Meaning |
| --- | --- |
| **CONFIRMED** | Independently re-derived from raw bytes across the whole corpus (all 49 map directories, all 115 zone files, all 8 media extents, etc.) with **zero** counter-examples, and/or read directly out of the disassembly at a named instruction address. Safe to implement against. |
| **INFERRED** | The layout parses and every cross-check passes, but the *semantics* rest on a plausible reading rather than proof — typically because only one sample exists, or because the consuming code has not been located. Implement it, but keep it isolated behind an interface and expect to revise. |
| **UNKNOWN** | The field exists and its observed value range is recorded, but nothing about its meaning is established. Preserve the bytes; do not act on them. |

Two additional conventions:

* A field described as *"always N (x/y)"* means the value N was observed in x of y instances examined, and the
  denominator is the full population, not a sample, unless stated.
* Where two analysis passes disagreed, the document states the *verified* value and marks the superseded one.

---

## Conventions

**Endianness.** All integers are **little endian** (MIPS R3000 in the PSX runs LE). The single exception is
the 4-byte integer fields inside a Sony `VAG` header, which are **big endian** by Sony's own format definition.

**Fixed point.** The PSX GTE uses 1.3.12 / 1.19.12 fixed point: **1.0 == 4096**. This is confirmed
empirically, not assumed — every model vertex normal, every trigger plane normal, and every collision plane
normal in the game has magnitude 4094…4096.

**Angles.** A full turn appears to be 4096 units (so 1024 == 90°). CONFIRMED as *consistent with* the data
(dominant spawn angles cluster at 0 / 1015 / 2041 / 3068), but not proven from code — INFERRED.

**EXE address translation.**

```
file_offset = 0x800 + (vaddr - 0x80018000)
```

valid for `vaddr` in `[0x80018000, 0x800B2800)`. Any address at or above `0x800B2800` is BSS and has no
backing bytes in the file.

**Alignment.** Every chunk in every `.DAT` container is 4-byte aligned and every chunk *size* is a multiple of
4. There is **no** 2048-byte sector padding anywhere in the level data (census over 2263 directory entries:
0 not-4-aligned, 1013 not-8-aligned, 2257 not-2048-aligned).

---

## Disc layout

```
SYSTEM.CNF                       boot descriptor (68 bytes, CRLF text)
SLES_015.34                      PS-EXE, MIPS R3000, 634,880 bytes
Q2DATA/LEVELS/<MAP>/COMMON.DAT   per-map static data          (49 maps)
Q2DATA/LEVELS/<MAP>/ZONE<N>.DAT  per-zone streamed world data (115 files, N = 0..5)
Q2DATA/LEVELS/<MAP>/SNDVRAM.DAT  per-map VRAM images + SPU sound bank (49)
Q2DATA/LEVELS/LAB/MAP.ALL        one file only, on one map    (editor leftover)
Q2DATA/AUD/QUAKE_[A-E].XAI       CD-XA music, 4 channels each (5)
Q2DATA/MOVIES/*.STX              Sony STR video + XA audio    (3)
```

Payload census (CONFIRMED): 224 files, 276,287,936 bytes; 49 level directories; exactly 214 `.DAT`/`.ALL`
files, all under `Q2DATA/LEVELS`. Zone-file histogram: `ZONE0` ×49, `ZONE1` ×33, `ZONE2` ×17, `ZONE3` ×11,
`ZONE4` ×4, `ZONE5` ×1.

Media extents are contiguous on the disc: LBA 337 → 104650 covers `QUAKE_A..E.XAI` then the three `.STX`
files in one unbroken 104,314-sector run; level data begins at LBA 104654.

> **Warning about pre-extracted media.** If your working tree contains already-extracted `.STX` / `.XAI`
> files, they are **lossy**. The `.STX` extractions are byte-identical to the 2048-byte Form 1 user area of
> *every* sector, meaning each Form 2 audio sector was truncated 2324 → 2048 and movie audio is
> unrecoverable from them. The `.XAI` extractions are exact multiples of 2324 but are short by 147 / 546 /
> 1364 / 1114 / 98 sectors versus the true Form 2 payload concatenation. **All sector-level work must be done
> against the raw Track 1 `.bin`.**

---

## 1. The `.DAT` chunked container

`COMMON.DAT` and `ZONE<N>.DAT` share one container format. It is a flat directory of fixed 16-byte entries at
file offset 0, terminated by a sentinel entry whose name is 12 zero bytes and whose offset equals the file
size. There is no magic number, no version, no per-chunk size field, no compression and no checksum. The
format is self-describing from its first 16 bytes.

```c
/* q2dat_dir_entry_t — CONFIRMED. sizeof == 16. Array begins at file offset 0. */
typedef struct {
    char     name[12];  /* CONFIRMED: NUL-padded ASCII. NOT NUL-terminated when the
                         * name is exactly 12 chars ("PrimaryRemap", "SecondaryCol").
                         * Parse as name[:12].split('\0')[0]. This is a field ID,
                         * not a filename. */
    uint32_t offset;    /* CONFIRMED: absolute byte offset from file start. Always a
                         * multiple of 4. MUST be u32 — 1144 of 2263 observed offsets
                         * exceed 0xFFFF (max 444372), so a u16 read breaks on the
                         * majority of files. */
} q2dat_dir_entry_t;
```

| Offset | Type | Name | Meaning | Confidence |
| --- | --- | --- | --- | --- |
| `0x00` | `char[12]` | `name` | Chunk identifier, NUL-padded, not NUL-terminated at 12 chars | CONFIRMED |
| `0x0C` | `uint32_t` | `offset` | Absolute offset of chunk data; sentinel entry holds the file size | CONFIRMED |

### Derived quantities

```
entry_count = dir[0].offset / 16      /* dir[0].offset is also the directory byte size */
chunk_count = entry_count - 1         /* the last entry is the EOF sentinel           */
size(i)     = dir[i+1].offset - dir[i].offset
```

Chunks are strictly contiguous: `dir[0].offset == entry_count * 16`, and
`dir[i].offset + size(i) == dir[i+1].offset` for all *i*. No inter-chunk gaps, no trailing bytes after the
last chunk.

**Verified over all 164 containers (49 `COMMON.DAT` + 115 `ZONE*.DAT`):** 0 parse failures, 0 monotonicity
violations, 0 sentinel-name violations, 0 sentinel-offset ≠ filesize, 0 non-printable or badly NUL-padded
names, 0 duplicate names within a file, 0 inter-chunk gaps, 0 chunk sizes not a multiple of 4.

Observed `entry_count` (including the sentinel):

| Value | Population | Notes |
| --- | --- | --- |
| 15 | 48 `COMMON.DAT` | 14 chunks |
| 16 | 1 `COMMON.DAT` | one map carries one extra chunk |
| 15 | 98 `ZONE*.DAT` | 14 chunks, AI pair present |
| 13 | 17 `ZONE*.DAT` | 12 chunks, AI pair absent |

### ⚠ Resolve chunks by NAME, never by index

Directory order is **not** stable. Two independent reasons, both CONFIRMED:

1. In `ZONE<N>.DAT` the relative order of the `SecondaryCol` / `PrimaryRemap` / `AreaConx` trio permutes as a
   function of the zone index *N* (three distinct orders for `N==0`, `N==1`, `N>=2`).
2. The optional `CreAIRel` / `CreAIBin` pair is absent from 17 zone files, shifting every subsequent index
   down by one.

Only 5 distinct directory orders exist across the 115 zone files, but there is no reason to rely on that.

```c
typedef struct {
    const char *name;   /* points into a caller-owned NUL-terminated copy */
    uint32_t    offset;
    uint32_t    size;   /* == next.offset - offset; always a multiple of 4 */
} q2dat_chunk_t;

typedef struct {
    const uint8_t *raw;
    uint32_t       raw_size;
    uint32_t       chunk_count;   /* entry_count - 1 */
    q2dat_chunk_t *chunks;        /* [chunk_count], in directory order */
} q2dat_archive_t;
```

### A 4-byte chunk is *usually* — not always — an empty list

`CreAIBin`, `CreAIRel`, `MapNames`, `SpaceLights` and `UserFuncs` always hold `0x00000000` when they are 4
bytes long. `PrimaryRemap` does **not**: a 4-byte `PrimaryRemap` holds `0x00000001` (4 files) or `0x38000001`
(11 files), i.e. a live 1-element record. **Do not special-case `size == 4` as "empty".** (CONFIRMED.)

### The engine's chunk-name literal pool — INFERRED, do not use as an enum

A run of 12-byte NUL-padded string literals sits in the EXE at vaddr `0x800AD414` (file `0x95C14`), at a
12-byte stride, containing 26 chunk names plus the file label that precedes them. The **byte layout is
CONFIRMED** — every literal is at its stated address with the stated padding.

The **array framing is REFUTED**, on four grounds:

* The 12-byte stride runs unbroken from at least `0x800AD3E4` through `0x800AD570` with no structural
  terminator, so the table's true base and extent are unproven.
* The string `ModelNames` occurs **zero** times in the entire 634,880-byte EXE, yet a chunk of that name is
  present in all 49 `COMMON.DAT` files. A canonical table cannot be missing a universally present chunk.
* `TriggerRemap` and `SecondaryRem` exist in the EXE exactly once each and are emitted by **no** `.DAT` on
  the disc (cut features; a parser should tolerate them appearing).
* The only located code reference (`0x8007A4A8`) materialises the address and copies 12 bytes of *one*
  element to the stack as a single string literal — that is by-name use, not an array base with a computed
  index.

**Consequence for the port:** key chunks by name string. Do not persist any ordinal derived from this pool.

---

## 2. `COMMON.DAT` — per-map static data

14 chunks on 48 maps; one map inserts a 15th (`GlintMod`) after `Strings`. Canonical order:

```
CreAIRel  LevelRel  Resources  CastList  TrigBounds  Lights  ModelNames
StartPos  Population  Strings  [GlintMod]  CreAIBin  LevelBin  UserFuncs  Events
```

Zero-length chunks in `COMMON.DAT`: only `LevelRel` and `LevelBin`, and only on the two front-end stub maps.
A loader **must** handle a 0-byte chunk.

### 2.1 `Resources` — zone declaration table

Array of 32-byte records terminated by 12 NUL bytes. `len % 32 == 12` on 49/49.

```c
typedef struct {           /* 32 bytes */
    char    name[12];      /* CONFIRMED: zone name, e.g. "Zone0" -> ZONE0.DAT      */
    int32_t unk0;          /* UNKNOWN: 49 distinct values, -3000..6600, incl. the
                            * sentinels -1 and -3000. Mostly round numbers. Does
                            * not track zone file size. Draw distance is a guess.  */
    int32_t always0;       /* CONFIRMED: 0 in every record of every map            */
    int32_t alwaysM1;      /* CONFIRMED: -1 in every record; none/invalid sentinel */
    int32_t unk3;          /* INFERRED: 64 in 47/49 maps; two records hold 80.
                            * NOT a constant.                                      */
    int32_t unk4;          /* UNKNOWN: 17 distinct values in 40..180. Per-zone
                            * budget or limit. Varies between zones of one map.    */
} q2p_resource;
```

| Offset | Type | Name | Meaning | Confidence |
| --- | --- | --- | --- | --- |
| `0x00` | `char[12]` | `name` | Zone name → `ZONE<n>.DAT` | CONFIRMED |
| `0x0C` | `int32_t` | `unk0` | Scalar, −3000…6600 | UNKNOWN |
| `0x10` | `int32_t` | `always0` | 0 everywhere | CONFIRMED |
| `0x14` | `int32_t` | `alwaysM1` | −1 everywhere | CONFIRMED |
| `0x18` | `int32_t` | `unk3` | 64, occasionally 80 | INFERRED |
| `0x1C` | `int32_t` | `unk4` | Per-zone budget, 40…180 | UNKNOWN |

> **CORRECTION.** The record count is the map's **declared** zone count and is *not* the number of
> `ZONE<n>.DAT` files present. Twelve maps declare 5 zones but ship only 2 zone files. **Never size the zone
> array from the filesystem.**

### 2.2 `CastList` — model / mesh bank

A back-to-back chain of models. Walk: read a 64-byte header at `p`, next model at `p + ofsEnd`; `ofsEnd == 0`
marks the last model. All seven sub-block offsets are relative to the model start, and are always sorted:
`verts(0x40) <= parts <= faces <= blockA <= blockB <= blockC <= blockD <= end`.

Verified over **965 models / 116,814 vertices / 76,320 faces** in the whole game, with zero violations of
every identity below.

```c
typedef struct {            /* 64 bytes; ALL offsets are model-relative */
    uint8_t  magic;         /* CONFIRMED: always 6 (965/965)                        */
    uint8_t  unk1[3];       /* UNKNOWN: 24-bit LE value, range 261..333367          */
    uint32_t always3;       /* CONFIRMED: always 3 (965/965)                        */
    char     name[12];      /* CONFIRMED: present in this map's ModelNames on 49/49.
                             * NOT unique — 11 maps contain duplicate model names.  */
    uint16_t numFaces;      /* CONFIRMED                                            */
    uint16_t numParts;      /* CONFIRMED                                            */
    int16_t  ext0;          /* UNKNOWN: always >= 0; no bbox correlate found        */
    int16_t  ext1;          /* UNKNOWN: negative in 72/965                          */
    int16_t  ext2;          /* INFERRED: always >= 0; == vertex max-Y for 83% of
                             * single-part models. Probable top-of-bounds.          */
    int16_t  ext3;          /* INFERRED: always <= 0; == vertex min-Y for 88% of
                             * single-part models. Probable bottom-of-bounds.       */
    uint32_t ofsFaces;      /* CONFIRMED: ofsBlockA - ofsFaces == 16*numFaces       */
    uint32_t ofsVerts;      /* CONFIRMED: ALWAYS 0x40. nVerts = (ofsParts-0x40)/12  */
    uint32_t ofsParts;      /* CONFIRMED: ofsFaces - ofsParts == 8*numParts         */
    uint32_t ofsBlockA;     /* CONFIRMED (container): 8 x {u16 count; u16 offset;
                             * u32 0} directory, offsets ascending, offsets[0]==64,
                             * max offset <= block size. Contents UNDECODED.        */
    uint32_t ofsBlockB;     /* INFERRED: exactly 16 zero bytes in 821/965; 144
                             * articulated models use 12 distinct larger sizes.
                             * Same name can carry 3 sizes across maps, so this is
                             * per-instance, not per-class. Skeleton? UNDECODED.    */
    uint32_t ofsBlockC;     /* INFERRED: == 12 + 8*numParts for 659/965; 306 are
                             * larger. Derive the size from ofsBlockD - ofsBlockC.
                             * Prime suspect for a per-part vertex base.            */
    uint32_t ofsBlockD;     /* CONFIRMED: size = (ofsEnd ? ofsEnd : chunkEnd) - this */
    uint32_t ofsEnd;        /* CONFIRMED: total model size; 0 == LAST model. The
                             * bytes after that header are the last model's OWN
                             * blocks, not padding — blockD ends at chunk end.      */
} q2p_model;

typedef struct {            /* 12 bytes. CONFIRMED: |n| in 4094..4096 for all
                             * 116,814 vertices — unit normals in 1.0 == 4096.      */
    int16_t x, y, z;        /* CONFIRMED: position, model-local units               */
    int16_t nx, ny, nz;     /* CONFIRMED: normal, 1.0 == 4096                       */
} q2p_vertex;

typedef struct {            /* 8 bytes */
    uint16_t numFaces;      /* CONFIRMED: sums exactly to header numFaces (965/965) */
    uint8_t  flags;         /* INFERRED: 48 distinct values, max 66, OR of all == 127 */
    uint8_t  numVerts;      /* CONFIRMED: sums exactly to the vertex count; max 82  */
    uint32_t zero;          /* CONFIRMED: 0 in every part of every model            */
} q2p_part;

typedef struct {            /* 16 bytes == a PSX GPU POLY_GT4/FT4 payload */
    uint8_t  v[4];          /* UNKNOWN BASE — see the blocking note below           */
    uint8_t  uv[4][2];      /* CONFIRMED: per-corner texture coords, full 0..255    */
    uint8_t  flags;         /* UNKNOWN: 12 distinct values 0..11, multi-bit field   */
    uint8_t  texture;       /* INFERRED: 0..175, 172 distinct. Target texture set is
                             * NOT in COMMON.DAT.                                   */
    uint16_t zero;          /* CONFIRMED: 0 in all 76,320 faces in the game         */
} q2p_face;
```

> **BLOCKING — the `CastList` face vertex-index base is unresolved.** Two candidate readings both fail:
> * *Part-relative* (indices relative to the owning part's vertex base): **26,237 of 76,320 faces (34 %)**
>   carry an index ≥ their part's `numVerts`, and 257 of 965 models admit **no** consistent per-part index
>   window at all.
> * *Whole-model absolute*: every index is in `[0, vertexCount)` for 965/965 models, but most vertices of
>   large models are then never referenced (one 745-vertex model has a maximum face index of 48).
>
> Coplanarity scoring over 13k faces mildly favours a cumulative part-base reading (median planarity error
> 0.011 vs 0.090) but it still fails outright on a third of parts. **Do not ship a mesh loader until this is
> resolved from the renderer in the EXE.** `blockC`, whose 8-byte-per-part shape is the obvious home for a
> per-part vertex base, is the place to look.

Face geometry note: 416 faces are degenerate quads (`v3 == v2` or `v3 == v0`) representing triangles; 75,904
are true quads.

### 2.3 `TrigBounds` — trigger volumes

```
{u16 numTrig; u16 numPlanes}
Trigger[numTrig + 1]           /* the last is a fill sentinel; its planeStart == numPlanes */
Plane[numPlanes]               /* one shared pool */
```

Size identity `len == 4 + 36*(numTrig+1) + 12*numPlanes` holds on 49/49.

```c
typedef struct { uint16_t numTrig, numPlanes; } q2p_trigbounds_hdr;

typedef struct {            /* 36 bytes */
    int32_t  min[3];        /* CONFIRMED: AABB min, world units (min <= max always) */
    int32_t  max[3];        /* CONFIRMED                                            */
    uint16_t planeStart;    /* CONFIRMED: index of this trigger's first plane;
                             * monotonically non-decreasing on 49/49                */
    uint16_t eventOfs;      /* CONFIRMED: BYTE offset into the Events chunk of the
                             * event this trigger fires; 0xFFFF == none. All 886
                             * real links land exactly on a walked record boundary. */
    uint32_t zero;          /* CONFIRMED: 0 in every trigger of every map           */
    uint16_t id;            /* UNKNOWN: 9..75 plus 255. 255 (not 0xFFFF) is "none".
                             * Byte +0x21 is 0 everywhere, so the u16 read is safe. */
    uint16_t flags;         /* UNKNOWN: 14 distinct values up to 10240              */
} q2p_trigger;

typedef struct {            /* 12 bytes */
    int16_t px, py, pz;     /* CONFIRMED: a point on the plane, RELATIVE to the
                             * owning trigger's min[] (8226/8228 within +/-1)       */
    int16_t nx, ny, nz;     /* CONFIRMED: outward normal; |n| == 4095 or 4096 for
                             * all 8228 planes                                      */
} q2p_plane;
```

> **CORRECTION.** `numPlanes != 6 * numTrig` on **eleven** maps, and two further maps contain non-box
> triggers whose totals coincidentally equal `6 * numTrig`. **Always** compute a trigger's plane range as
> `[planeStart[i], planeStart[i+1])`, never as `[6i, 6i+6)`.

### 2.4 `Lights` — coloured point lights

Flat array, no header, no terminator. `len % 28 == 0` on 49/49. 7,814 lights in the game.

```c
typedef struct {            /* 28 bytes */
    int32_t  x, y, z;       /* CONFIRMED: world position                            */
    uint8_t  r, g, b;       /* CONFIRMED: colour 0..255                             */
    uint8_t  pad0;          /* CONFIRMED: 0 in all 7814                             */
    uint8_t  always255;     /* CONFIRMED: 0xFF in all 7814. "Intensity" is a guess —
                             * the value never varies, so the meaning is unproven.  */
    uint8_t  type;          /* CONFIRMED: exactly 5 values game-wide, 7/15/23/31/39
                             * == ((n<<3)|7) for n = 0..4. Style semantics UNKNOWN. */
    uint16_t radius;        /* CONFIRMED: == isqrt(radiusSq) for all 7814           */
    uint32_t innerRadiusSq; /* CONFIRMED: always <= radiusSq                        */
    uint32_t radiusSq;      /* CONFIRMED: authoritative cut-off radius squared      */
} q2p_light;
```

### 2.5 `ModelNames`

`char[12]` array followed by a `uint32_t 0` terminator; `len % 12 == 4` on 49/49. The array index is the
map-local model ID.

CONFIRMED: every `CastList` model name appears somewhere in `ModelNames` on 49/49 maps (zero misses).
**REFUTED**: the shortcut "the first *K* `ModelNames` entries correspond 1:1 to the first *K* `CastList`
models" — on one map *K* is 0. Name lookup alone is also ambiguous: 11 maps contain duplicate `CastList`
names.

### 2.6 `StartPos`

28-byte records terminated by 12 NUL bytes; `(len-12) % 28 == 0` on 49/49.

```c
typedef struct {            /* 28 bytes */
    char    name[12];       /* CONFIRMED: every map has a default entry             */
    int32_t x, y, z;        /* CONFIRMED: world position                            */
    int16_t angle;          /* INFERRED: observed -2047..+2047, consistent with
                             * 4096 == 360 deg stored signed. Unit not proven.      */
    int16_t zone;           /* CONFIRMED: always < the map's Resources record count */
} q2p_startpos;
```

### 2.7 `Population` — spawn / placement lists

A header table of 24-byte group records terminated by 4 zero bytes, then per-group lists located by
chunk-relative offset (`0` == group has no such list). 76 distinct group names game-wide (zone names, script
batch names, item categories, and one special group). An empty `Population` chunk is 8 bytes.

```c
typedef struct {            /* 24 bytes; table terminated by uint32_t 0 */
    char     name[12];      /* CONFIRMED                                            */
    uint32_t ofsSpawnList;  /* CONFIRMED: chunk-relative offset of list A; 0 = none */
    uint32_t ofsPlaceList;  /* CONFIRMED: chunk-relative offset of list B; 0 = none */
    uint32_t zero;          /* CONFIRMED: 0 for every group of every map            */
} q2p_popgroup;
```

**List A has two layouts, selected by the group name.** This is a correction that takes the chunk from 76 %
decodable to fully decodable.

```c
/* List A for every group EXCEPT the path-node group. Terminator: uint32_t 0.
 * CONFIRMED: all 673 records place inside the map's own light-coordinate envelope. */
typedef struct {            /* 24 bytes */
    uint32_t classId;       /* UNKNOWN: 25 distinct values 0..37. NOT a plain
                             * ModelNames index — 15 of 673 exceed the map's name
                             * count, and name resolution yields nonsense.          */
    int32_t  x, y, z;       /* CONFIRMED                                            */
    uint16_t angle;         /* INFERRED: 0..3958; 0/1015/2041/3068 dominate         */
    uint16_t link;          /* UNKNOWN: 0xFFFF == none in 482/673                   */
    uint16_t flags;         /* UNKNOWN: 47 distinct values 0..12293                 */
    uint16_t index;         /* UNKNOWN: slot index 0..323, not strictly monotonic   */
} q2p_spawn;

/* List A for the path-node group only. Terminator: 0xFFFFFFFF, NOT zero.
 * CONFIRMED: 466/466 records place inside the map envelope with xyz at +0x00
 * (only 192/466 with xyz at +0x04). A stride/offset sweep gives (24, +0) as the
 * unique 100% solution. This is a patrol/path graph; 19 maps carry one. */
typedef struct {            /* 24 bytes */
    int32_t  x, y, z;       /* CONFIRMED                                            */
    uint16_t unk0;          /* UNKNOWN: 0 in 464/466                                */
    uint16_t zero;          /* CONFIRMED: 0 in all 466                              */
    uint32_t link0;         /* INFERRED: neighbour node index; 0xFFFFFFFF == none   */
    uint32_t link1;         /* INFERRED: second neighbour; 0xFFFFFFFF == none       */
} q2p_pathcorner;

/* List B. Terminator: 0xFFFFFFFF (CONFIRMED 174/174 arrays), NOT zero. */
typedef struct {            /* 16 bytes */
    int32_t  x, y, z;       /* CONFIRMED: 1009/1013 inside the map envelope         */
    uint16_t unk;           /* UNKNOWN: NOT a 0..4095 angle — values include 4096,
                             * 32768, 36864, 53248. Looks like flag bits OR'd with
                             * an angle.                                            */
    uint16_t id;            /* INFERRED: 0..56, 41 distinct — the same numeric range
                             * as the EXE class-table id byte, so this is the most
                             * likely link to the global pickup table.              */
} q2p_place;
```

Alignment slack: a group table normally ends 4 bytes before the first list, but two maps leave a 4-byte gap,
as do 18 list-A and 34 list-B arrays. Treat the region between a list's terminator and the next declared
offset as slack, not data.

### 2.8 `Strings`

A directory of `{char key[12]; uint32_t offset}` records terminated by a 16-byte all-zero record, followed by
a blob of NUL-terminated ASCII. CONFIRMED on 49/49: the first offset equals the byte just past the terminator
record; all offsets strictly ascending and in bounds.

### 2.9 `UserFuncs`

`uint32_t count` then `count` × `char[12]` NUL-padded script-primitive names. `len == 4 + 12*count` on 49/49;
`count` observed 0…17; 38 distinct names game-wide.

### 2.10 `Events` — compiled trigger scripts

```
uint32_t count;
{char name[12]; uint32_t offset}[K];   /* named entry points; K implicit          */
uint32_t 0;                            /* directory terminator                    */
record[count];                         /* variable length, packed back-to-back    */
```

**CONFIRMED on 49/49 `COMMON.DAT` and 115/115 `ZONE*.DAT`:** walking `p += rec.size` from the computed data
start yields exactly `count` records and lands *exactly* on chunk end. All 145 `COMMON` directory offsets and
all 886 `TrigBounds.eventOfs` links coincide with a walked record boundary. Stub maps carry an 8-byte chunk
(count 0, empty directory).

```c
typedef struct { char name[12]; uint32_t offset; } q2p_eventent;

typedef struct {            /* variable length record header */
    uint16_t size;          /* CONFIRMED: total record bytes INCLUDING this field.
                             * Range 4..240, always a multiple of 4.               */
    uint8_t  sub;           /* INFERRED: small sub-opcode, 0..13; 1/2/3 dominate   */
    uint8_t  cls;           /* INFERRED: class byte, ALWAYS a multiple of 8;
                             * observed exactly {0x08,0x10,0x18,0x20,0x28,
                             * 0x48,0x50,0x58}                                     */
    /* uint8_t body[size - 4];   UNKNOWN: operand stream, not decoded              */
} q2p_event_rec;
```

> Earlier passes read `sub`/`cls` as a single `uint16_t type` (giving values such as `0x1001`). The two-byte
> split is the better model — the high byte's strict multiple-of-8 structure is not a coincidence — but note
> the two readings are byte-equivalent, so either parses.

### 2.11 `LevelBin` / `CreAIBin` — relocatable MIPS modules, and `LevelRel` / `CreAIRel` — fixups

These four chunks hold **compiled MIPS R3000 code**. CONFIRMED by opcode-field histograms that are textbook
MIPS (SPECIAL dominant, then `addiu`, `lw`, `lui`, `sw`, `lbu`, `beq`/`bne`) and by the byte pattern for
`jr $ra` recurring dozens of times per chunk.

```c
typedef struct { char name[12]; uint32_t nextModuleOfs; } q2p_module_hdr;
```

* `CreAIBin` is a chain of per-monster AI modules: header, then `uint32_t segSizes[4]` (UNKNOWN), then code,
  literals, a `{char[12]; uint32_t}` export table and zeroed BSS. Seven distinct AI module names occur across
  the 13 maps that ship AI.
* `LevelBin` starts `uint32_t ofsX` (INFERRED: plausibly the code-segment start — the opcode histogram from
  there to chunk end is clean MIPS), then two zero words, then a 12-byte symbol-name table, C string
  literals, code, and an export table.
* `CreAIRel` mirrors `CreAIBin`'s module chain; `LevelRel` is a bare fixup array.
* Fixups: `uint32_t[]` terminated by `0xFFFFFFFF`, then a `uint32_t 0` chain terminator in `CreAIRel`.

CONFIRMED: **all 25,381** `LevelRel` fixup values are `< sizeof(LevelBin)` on 49/49 maps; `CreAIRel` fixups
are 100 % in-range on 13/13 maps; module *names* match `Rel` ↔ `Bin` one-for-one on 13/13; the last module's
`nextModuleOfs == chunkSize - 4`.

> **CORRECTION.** `nextModuleOfs` is **chunk-local**, so the `Rel` and `Bin` values *differ* for the same
> module. Only the names match.
> **CORRECTION.** An "empty" `CreAIRel`/`CreAIBin` is a 4-byte zero, but `LevelRel`/`LevelBin` can be
> genuinely **zero length** on stub maps.
> Only 31 % of `LevelRel` fixup values are 4-aligned, so the array is **not** a plain word-address list. The
> encoding is unresolved and needs the loader routine.

### 2.12 `GlintMod` (one map only)

2608 bytes, single sample, purpose UNKNOWN. Uses 234 of 256 possible byte values across the full range; only
the opening bytes are small ascending indices, the bulk is high-entropy. (An earlier description of it as
"small ascending byte indices throughout" is REFUTED.)

### 2.13 `COMMON.DAT` chunk size envelope

Per-chunk `min / median / max` byte sizes over 49 maps, for buffer sizing:

| Chunk | min | median | max |
| --- | --- | --- | --- |
| `CreAIRel` | 4 | 4 | 3292 |
| `LevelRel` | 0 | 1136 | 13008 |
| `Resources` | 44 | 108 | 204 |
| `CastList` | 10764 | 72044 | 190528 |
| `TrigBounds` | 40 | 856 | 11272 |
| `Lights` | 28 | 2604 | 19516 |
| `ModelNames` | 28 | 424 | 568 |
| `StartPos` | 40 | 152 | 320 |
| `Population` | 8 | 584 | 2996 |
| `Strings` | 64 | 168 | 1472 |
| `GlintMod` | — | — | 2608 (1 file) |
| `CreAIBin` | 4 | 4 | 14864 |
| `LevelBin` | 0 | 5608 | 118216 |
| `UserFuncs` | 4 | 40 | 220 |
| `Events` | 8 | 152 | 2664 |

---

## 3. `ZONE<N>.DAT` — streamed per-zone world data

14 chunk names (12 when the AI pair is absent). The name set is constant; the *order* is not (§1).

Universal invariant, verified on 115/115 files for all 14 chunk names: **every chunk size is a multiple of 4.**

### 3.1 `Scene` — convex world nodes

Flat array of 52-byte records; `chunkSize % 52 == 0` on 115/115; the node count equals the `Points` group
count on 115/115. 17,035 nodes game-wide.

```c
typedef struct {                /* 52 bytes */
    uint32_t mapmodOffset;      /* CONFIRMED: byte offset of this node's polygon
                                 * record inside MapMod. offs[0]==0, strictly
                                 * increasing, no duplicates, so node i spans
                                 * [off[i], off[i+1]) and the last ends at chunk end. */
    uint32_t unk04;             /* CONFIRMED: always 0 (0 of 17035 nonzero)          */
    uint16_t flags08;           /* INFERRED: bitfield. Observed {0, 0x400, 0x800,
                                 * 0x1000, 0x1400, 0x4000, 0x4400, 0x4800}. Zero in
                                 * 69.8% of nodes. Meaning UNKNOWN.                  */
    uint16_t pad0A;             /* CONFIRMED: always 0 (upper half of the dword)     */
    uint8_t  unk0C;             /* UNKNOWN: 0..4, nonzero in 6 nodes                 */
    uint8_t  unk0D;             /* UNKNOWN: 0..3, nonzero in 5 nodes                 */
    uint8_t  unk0E;             /* UNKNOWN: 0..197, 119 distinct; 3 nodes are 0      */
    uint8_t  unk0F;             /* CONFIRMED: always 0                               */
    int32_t  bboxMin[3];        /* CONFIRMED value, INFERRED semantics — see note    */
    int32_t  bboxMax[3];        /* CONFIRMED value, INFERRED semantics — see note    */
    int32_t  origin[3];         /* CONFIRMED: world vertex = Points.xyz + origin     */
} Q2P_SceneNode;
```

> **CORRECTION — the `Scene` AABB does not contain its own geometry.** Over 51,105 axis tests on all 115
> files, `stored_min - computed_min` is 0…3 (never negative) and `stored_max - computed_max` is −2…+1. In
> **43,696 of 51,105** axis tests the node's own points lie outside the stored box on at least one side.
> **Inflate the box by ~4 units before using it for culling or point-in-node tests**, or geometry will pop.
> Probable cause: coordinates are quantised one unit *below* the authoring grid (dominant world-Y values
> arrive in ±1 pairs around multiples of 640).

### 3.2 `Points` — per-node vertex lists

```
uint32_t numGroups;                                  /* == Scene node count, 115/115 */
{uint32_t byteOffset; uint32_t count}[numGroups];    /* offsets relative to 4+8*n    */
Q2P_Point points[...];
```

CONFIRMED on 115/115: groups strictly contiguous, last group ends exactly at chunk end, zero empty groups.
**Hard constraint:** `1 <= count <= 117`, never ≥ 256 — this is what makes the `uint8_t` vertex indices in
`MapMod` safe.

```c
typedef struct {                /* 12 bytes */
    int16_t x, y, z;            /* CONFIRMED: node-LOCAL; add SceneNode.origin.
                                 * Observed local range -25032..24711.               */
    uint8_t slot[6];            /* INFERRED: reverse map into GPU quad corners.
                                 * 0xFF == unused. slot = polyIndex*4 + s, and
                                 * MapMod poly.vtx[{0,1,3,2}[s]] == this point index.
                                 * Holds for 1,099,415 of 1,099,732 entries (99.971%).
                                 * 317 exceptions, 170 of them in NON-degenerate
                                 * quads. REBUILD THIS FROM MapMod AT LOAD TIME;
                                 * treat the stored map as advisory only.            */
} Q2P_Point;
```

### 3.3 `MapMod` — per-node renderable quads

One variable-length record per scene node, located via `SceneNode.mapmodOffset`. This is the best-verified
structure in the whole format: **17,035 records / 274,936 polygons across all 115 files, zero violations** of
every identity below.

```c
typedef struct {                /* record header */
    uint16_t numPolys;          /* CONFIRMED: HARD RANGE 0..63, NEVER >= 64 — forced
                                 * by the u8 corner-slot encoding (poly*4+corner with
                                 * 0xFF reserved). 5 nodes have 0; handle empties.   */
    uint16_t colourOffset;      /* CONFIRMED: == 8 + numPolys*12 (17035/17035)       */
    uint32_t uvOffset;          /* CONFIRMED: colourOffset <= uvOffset <= recordEnd  */
    /* Q2P_Poly poly[numPolys];
     * uint8_t  rgb[][3];     size == align4(3*(maxColIdx+1))   (17035/17035)
     * uint8_t  uv[nUvSets][8];  nUvSets = (nextRecOff - uvOffset)/8, exact multiple
     *                           of 8 in 17035/17035; nUvSets never exceeds numPolys */
} Q2P_MapModRec;

typedef struct {                /* 12 bytes; maps onto a PSX POLY_GT4 */
    uint8_t  vtx[4];            /* CONFIRMED: indices into this node's Points group,
                                 * PSX quad (Z) order. 0 out-of-range in 274,936      */
    uint8_t  col[4];            /* CONFIRMED: per-corner index into the record's RGB
                                 * table (Gouraud). Proven by the colour-table size
                                 * identity holding in 17035/17035.                  */
    uint16_t clut;              /* CONFIRMED: PSX CLUT id. VRAM x = (clut & 0x3F)*16,
                                 * observed 0..816, every value a multiple of 16
                                 * (4bpp). y = clut >> 6, observed 0..340. 486
                                 * distinct CLUTs across all 115 files.              */
    uint8_t  tpage;             /* CONFIRMED: PSX texture-page attribute, 0..11       */
    uint8_t  uvIdxFlags;        /* CONFIRMED: bits 0-5 index the record's UV table
                                 * (max index used 56 vs max nUvSets 57 — almost no
                                 * headroom). Bits 6-7 are render flags, meaning
                                 * UNKNOWN; distribution 0x00 88.3%, 0x80 4.55%,
                                 * 0x40 3.64%, 0xC0 3.51%.                           */
} Q2P_Poly;
```

### 3.4 `PrimaryColl` / `SecondaryCol` — collision hulls

Both chunks share one layout. Size equation
`4 + (numNodes+1)*36 + numPlanes*12 + numExtra*4` is **exact on all 230 chunks** (115 files × 2), with
`sentinel.firstPlane == numPlanes` and `sentinel.firstExtra == numExtra`.

```c
/* uint16_t numNodes; uint16_t numPlanes;
 * Q2P_CollNode  node[numNodes + 1];   last entry is a totals sentinel
 * Q2P_CollPlane plane[numPlanes];
 * uint32_t      extra[numExtra];                                                     */

typedef struct {                /* 36 bytes */
    int32_t  bboxMin[3];        /* CONFIRMED                                          */
    int32_t  bboxMax[3];        /* CONFIRMED                                          */
    uint16_t firstPlane;        /* CONFIRMED: per-node count = node[i+1] - node[i]     */
    uint16_t firstExtra;        /* CONFIRMED: same derivation                          */
    uint32_t c;                 /* UNKNOWN: 0..65,077,433. NOT monotonic (609 of
                                 * 23,003 node-to-node steps decrease).                */
    uint32_t d;                 /* UNKNOWN: 0..75                                      */
} Q2P_CollNode;

typedef struct {                /* 12 bytes */
    uint16_t a, b, c;           /* INFERRED: a point on the plane as an UNSIGNED offset
                                 * from the owning node's bboxMin. Range 0..29439,
                                 * never negative. 46,968 of 49,148 tested planes
                                 * (95.6%) land inside their node's bbox under that
                                 * reading; 91% of nodes are convex-consistent.        */
    int16_t  nx, ny, nz;        /* CONFIRMED: 1.3.12 UNIT normal. |n| == 4096 in
                                 * 120,911 planes, 4095 in 18,321 and 4094 in 8, out
                                 * of 139,240 across all 115 files and both chunks —
                                 * 100% within 2 LSB of unity. No other byte offset in
                                 * the record comes close (s16@+0 hits unit magnitude
                                 * in 0.4%). Diagonals appear as 2896 == round(4096/√2). */
} Q2P_CollPlane;
```

Node-count relation: `SecondaryCol > PrimaryColl` in 89/115 files, equal in 17, **smaller in 9**. Do not
assume Secondary is the finer hull. `PrimaryColl / Scene` node ratio spans 0.287…0.874, mean 0.537.

### 3.5 `PrimaryRemap`

`uint16_t remap[PrimaryColl.numNodes]`, zero-padded to 4 bytes. `len == align4(2 * numNodes)` on 115/115.
The maximum value **exceeds the `Scene` node count in 100 of 115 files**, so it is definitively *not* a
scene-node index — probably a polygon or surface id in a shared table. UNKNOWN.

### 3.6 `AreaConx` — area connection (portal) graph

```c
/* uint16_t numAreas;                    at +0x00
 * uint16_t areaOffset[numAreas];        at +0x02  <-- NOT +0x04
 * ... link records ...                                                                */
```

> **CORRECTION — this is the one field-offset error that silently corrupts a parser.** An earlier pass
> declared a `uint16_t pad` at `+0x02` and placed the offset table at `+0x04`. Parsing at `+0x04` puts area
> offsets out of bounds on **100 of 115 files** (one 588-byte chunk yields an offset of 2561). The correct
> base is `+0x02`: `areaOffset[0] == 0` in 115/115 because area 0 never has links, and that constant zero is
> what was mistaken for padding. With the correct base, the first non-zero offset equals `2 + 2*numAreas` in
> 115/115, all offsets are in bounds, and 1,725 records recover cleanly.

Link record: `uint8_t nLinks;` then `nLinks` × 9 bytes.
`size == 9*nLinks + 2 - (nLinks & 1)`, exact on all **1,675 interior** records. (The last record in a chunk
is followed by 0 or 2 trailing bytes because the chunk is padded to 4 — so the formula cannot be checked that
way for the final record.) `nLinks` observed 1…7 and 9, never 8.

The 9-byte link payload is **UNKNOWN**. No fixed byte offset yields a 1.3.12 unit normal in more than 39 % of
3,494 links; byte histograms show `0x10`/`0xF0` clustering consistent with *unaligned* `int16_t` values of
`0x1000`/`0xF000`, but links start at `record + 1 + 9*L` so their parity alternates and no single struct
layout can express it. Byte `+3` is 0 in 2,132 of 3,494 links with small values otherwise, and is the best
neighbour-index candidate.

### 3.7 `Events` (zone)

Same format as §2.10, and **UPGRADED to CONFIRMED** here: the `uint32_t` at `+0` equals the walked record
count in 115/115 files (3,283 records total), the walk tiles the record area and ends exactly at chunk end in
115/115, the post-directory `uint32_t 0` separator is present in 115/115, and all 495 named record offsets
land exactly on record starts.

### 3.8 `MapNames`

`{char name[12]; uint32_t id}[N]` then a `uint32_t 0` terminator; `N = (size-4)/16`. Exact on 115/115; 671
entries total. `id` equals the sequential entry index in 428 entries and *differs* in 243 (max 274) — so it
is a real id, not a redundant index. CONFIRMED.

### 3.9 `CastList` (zone) — per-zone actor asset bank

A zone's `CastList` is an actor/model asset bank, not a spawn list. 17 zones have a **zero-length**
`CastList`; the largest is 139,100 bytes.

CONFIRMED on **98/98** non-empty chunks: `u32@0x00` low byte == `0x06` (magic/type tag; upper 3 bytes vary,
UNKNOWN); `u32@0x04` == 3 (version); `char[12]` at `+0x08` is a NUL-padded printable actor name;
`u32@0x24` == `0x40`, i.e. the first sub-section begins at `0x40`. The claim of an 8-entry offset table at
`+0x20` remains INFERRED — only the `0x40` entry is confirmed constant. A 20-byte-stride animation name table
follows; INFERRED.

### 3.10 `CreAIRel` / `CreAIBin` (zone)

Present in the 98 files with 15-entry directories. CONFIRMED: in exactly 29 of those, **both** chunks are
exactly 4 bytes of zero (no creatures). In the other 69, the first 12 bytes of both chunks are the *same*
NUL-padded ASCII actor name. Bodies as §2.11.

### 3.11 `SortData` / `SpaceLights` — undecoded

* `SortData`: opaque, bit-packed, no offset table, no byte-aligned record structure. Size is a multiple of 4
  on 115/115; bytes-per-scene-node spans **4.0…88.6** (a 22× spread), which rules out any fixed per-node
  record. UNKNOWN.
* `SpaceLights`: flat `uint16_t` array of small indices, no length prefix or offset table. Size a multiple of
  4 on 115/115; `uint16` count per scene node spans **0.68…7.12**, so it is not a fixed multiple of the node
  count either. UNKNOWN.

Both require the EXE's consumer to decode.

### 3.12 Zone-level test vectors

**Stub zones (CONFIRMED).** Exactly 15 non-gameplay zone files are **exactly 840 bytes**, each with 2 scene
nodes / 8 points / 2 quads / 1 `PrimaryColl` node / 1 `SecondaryCol` node. They are ideal minimal fixtures.
They fall into three byte-identical groups (of 4, of 5, and of 2 files) plus 4 unique files.

**Duplicate zones (CONFIRMED).** On 13 maps, `ZONE0` and `ZONE1` are *not* byte-identical, yet **every one of
their 15 chunks compares byte-for-byte equal**; only the directory ordering differs, shifting chunks to
different file offsets. A port may load `ZONE0` and alias `ZONE1` to it for those maps.

### 3.13 Zone chunk size envelope

| Chunk | min | median | max |
| --- | --- | --- | --- |
| `Events` | 8 | 1036 | 2664 |
| `CreAIRel` (n=98) | 4 | 2984 | 8176 |
| `Scene` | 104 | 7748 | 19760 |
| `CastList` | 0 | 38536 | 139100 |
| `MapNames` | 4 | 20 | 644 |
| `SpaceLights` | 4 | 592 | 1992 |
| `SortData` | 8 | 5868 | 18608 |
| `MapMod` | 64 | 50604 | 127220 |
| `Points` | 116 | 53012 | 129128 |
| `PrimaryColl` | 148 | 9524 | 23696 |
| `SecondaryCol` | 148 | 11852 | 49392 |
| `PrimaryRemap` | 4 | 156 | 384 |
| `AreaConx` | 24 | 400 | 884 |
| `CreAIBin` (n=98) | 4 | 13168 | 35684 |

---

## 4. `SNDVRAM.DAT` — per-map VRAM images + SPU sound bank

Despite the name this file is **two** sections: VRAM/texture images first, the audio bank second. It is
**not** the named-chunk container; it uses an anonymous `uint32_t` offset table with the same EOF-sentinel
idiom.

```c
typedef struct {
    uint32_t ofs_vram_section;  /* CONFIRMED: always 0x0000000C (49/49). Doubles as
                                 * the table byte size (N=3) AND as the BASE that the
                                 * image records' offsets are relative to.           */
    uint32_t ofs_sound_bank;    /* CONFIRMED: file-absolute. Observed 3544..406280.   */
    uint32_t total_size;        /* CONFIRMED: == stat() size (49/49). EOF sentinel.   */
} SndVramHeader;
```

Verified on 49/49: `t[0] == 12`, strictly monotonic, `t[2] == file size`, all offsets 4-aligned, zero
violations. Reading the table as 16-byte named entries fails immediately since `12 % 16 != 0`.

### 4.1 Section A — VRAM images (at file offset `0x0C`)

```
uint8_t  count_texpages;      /* at 0x0C */
uint8_t  count_images;        /* at 0x0D */
uint8_t  unknown_0E;          /* at 0x0E */
uint8_t  unknown_0F;          /* at 0x0F */
VramImageRec images[count_texpages + count_images];   /* at 0x10 */
uint16_t clut_or_pad[256];    /* exactly 512 bytes, every u16 == 0x8000 */
... unidentified section-A data ...
char image_name_list[];       /* packed NUL-terminated names */
... compressed pixel payloads, up to ofs_sound_bank ...
```

```c
typedef struct {                /* 8 bytes */
    uint32_t ofs_pixels_rel;    /* CONFIRMED: offset of this image's (compressed)
                                 * pixel data RELATIVE TO 0x0C, i.e. absolute =
                                 * 0x0C + value. See the correction below.           */
    uint16_t width;             /* INFERRED: only 128, 256, 512 observed             */
    uint16_t height;            /* INFERRED: only 128, 224, 240, 256 observed        */
} VramImageRec;
```

| Offset | Type | Name | Meaning | Confidence |
| --- | --- | --- | --- | --- |
| `0x0C` | `uint8_t` | `count_texpages` | Number of texture-page records, 1…12 | CONFIRMED |
| `0x0D` | `uint8_t` | `count_images` | Number of image records, 0…12 | CONFIRMED |
| `0x0E` | `uint8_t` | `unknown_0E` | 17 for every front-end / cutscene map; 29…86 for gameplay maps | UNKNOWN |
| `0x0F` | `uint8_t` | `unknown_0F` | 1…181; constant within some map families but not others | UNKNOWN |

> **CORRECTION — the image offsets are section-relative, not file-absolute**, and the difference is exactly
> 12 bytes in **49 of 49** files. Proof: `0x0C + images[0].ofs_pixels_rel` lands exactly on the byte after
> the final NUL of the packed image-name list in 49/49, whereas the raw value lands 12 bytes *inside* the
> name strings. A loader using the absolute reading feeds 12 bytes of ASCII filename into every first
> image's pixel decoder.

**Record count is self-checking.** A run of 512 bytes of `uint16 0x8000` begins at exactly
`0x10 + 8*(count_texpages + count_images)` in 49/49, and the `uint16` immediately after it is never `0x8000`
— so the count is pinned, not assumed. The 512-byte block is most likely an unused 256-entry RGB1555 CLUT
slot (`0x8000` = opaque black / transparent bit set).

**Image name list (CONFIRMED, and a useful cross-check).** Immediately before the first pixel payload sits a
packed run of NUL-terminated ASCII names: `count_texpages` texture-page names followed by `count_images`
image names, contiguous with no padding. Scanning the whole section for those two filename extensions yields
counts matching the two header bytes in 49/49 — which is what upgrades `count_texpages` / `count_images` from
inferred to confirmed. The list's end address equals `0x0C + images[0].ofs_pixels_rel`.

Observed dimension pairs across all 553 records: 128×256 (477), 128×128 (47), 512×240 (13), 256×256 (10),
256×224 (6). Payload span is always less than `width*height*2`, consistent with a compressed rectangle.

> **Width/height vs VRAM (X,Y) is not settled by the bytes.** Every observed pair is also a legal VRAM
> coordinate. "Size" is the better reading because 437 of 553 records duplicate an earlier pair *within the
> same file* — duplicate page dimensions are normal, duplicate placement coordinates would be a bug. Marked
> INFERRED until one payload is decompressed and its pixels counted.

**The pixel compression codec is UNKNOWN.** This is the single biggest blocker in the whole format set.

### 4.2 Section B — the VAG sound bank

This section *is* the SPU RAM content. All offsets are relative to `ofs_sound_bank`.

```
uint32_t offsets[num_entries];   /* num_entries = offsets[0] / 4                     */
                                 /* entries 0..n-2 point at VagHeader structs        */
                                 /* the final entry == section byte size (sentinel)  */
```

CONFIRMED across 49 files / 2,524 table entries / 2,475 VAG instances: `offsets[0]/4 == entry count` in
49/49; the final entry equals the section byte size in 49/49; every entry resolves to a `VAGp` magic.
`num_sounds = num_entries - 1`, observed 2…87, never above the runtime clamp of 96 (the clamp is visible in
the disassembly as `slti 97 / addiu 96`). Exactly 54 duplicate (aliased) offsets exist disc-wide, and exactly
those 54 entries have a span ≠ `48 + data_size`.

**Runtime sound IDs are 1-based:** index *i* maps to id *i+1*, because slot 0 holds a null-sound placeholder.
Read directly out of the `(i+1)*12` slot arithmetic in the loader.

```c
typedef struct {                /* 48 bytes; INTEGER FIELDS ARE BIG ENDIAN */
    char     magic[4];          /* CONFIRMED: 'VAGp' (2475/2475)                     */
    uint32_t version_be;        /* CONFIRMED: 0x00000020 (2475/2475)                 */
    uint32_t reserved0;         /* CONFIRMED: 0 (2475/2475)                          */
    uint32_t data_size_be;      /* CONFIRMED: ADPCM body bytes, always a multiple of
                                 * 16; observed 320..36464                           */
    uint32_t sample_rate_be;    /* CONFIRMED: only 11025 (1159x) and 22050 (1316x).
                                 * SPU pitch = (rate << 12)/44100 -> 0x400 / 0x800.  */
    uint8_t  reserved1[12];     /* CONFIRMED: all zero (2475/2475)                   */
    char     name[16];          /* CONFIRMED: NUL-padded ASCII, up to 15 chars.
                                 * Runtime slots are only 12 bytes wide, so names are
                                 * truncated to 11 chars + NUL at load. Exactly one
                                 * genuine truncation collision exists disc-wide.    */
    /* uint8_t body[data_size];  raw SPU-ADPCM, uploaded verbatim to SPU RAM         */
} VagHeader;
```

Big-endianness is proven by construction: the BE reading yields exactly two plausible sample rates and
16-aligned sizes; the LE reading yields values in the hundreds of millions.

```c
typedef struct {                /* 16 bytes */
    uint8_t shift_filter;       /* CONFIRMED: bits 0-3 right-shift, observed 0..12
                                 * (NOT 2..12 — shift 0 and 1 both occur); bits 4-7
                                 * predictor filter index, observed 0..4 only.       */
    uint8_t flags;              /* CONFIRMED: bit0 End, bit1 Repeat, bit2 LoopStart.
                                 * Only 0,1,2,3,6,7 occur.                           */
    uint8_t nibbles[14];        /* CONFIRMED: 28 signed 4-bit residuals, low nibble
                                 * first. s = (n<<12)>>shift;
                                 * out = s + (f0*prev1 + f1*prev2)/64, clamped s16.  */
} SpuAdpcmBlock;

static const int SPU_F0[5] = { 0, 60, 115,  98, 122 };  /* both /64 */
static const int SPU_F1[5] = { 0,  0, -52, -55, -60 };
```

Verified over **1,167,540 blocks**: zero invalid flag bytes, zero invalid shifts, zero invalid filters.

**Loop detection, exactly as the engine does it** (it reads the flag bytes of body blocks 1 and 2):

```c
is_looping = (body[17] & 2) && (body[33] & 2);
```

This agrees with "contains a `LoopStart|Repeat` block" in **2475/2475** VAGs. Loop start is always body block
index 1. Looping samples end with an `End|Repeat` block at `nb-1`; there are exactly 118 looping VAGs.

**One-shot tails come in two forms** — a decoder that stops only on the `0x07` terminator overruns by one
block on 176 samples: 2,181 one-shots have `flags==1` at `nb-2` followed by a `0x07` terminator, and 176 have
`flags==1` at `nb-2` followed by a plain `flags==0` block with no terminator at all.

**SPU RAM budget.** On the worst-case map the VAG *bodies* total 522,000 bytes against 522,240 usable
(0x80000 − 0x800) — a 240-byte margin. Section B *including* the 48-byte headers is 525,124, which exceeds
SPU RAM. **The loader must upload bodies only.** Whether reverb is disabled given that margin is an open
question.

Section-size statistics: section A 3,532…406,268 bytes (49 distinct values, i.e. genuinely per-map); section
B 1,340…525,124 bytes but only **33 distinct** values across 49 files (one size recurs 8×, another 6×),
implying a shared/common bank rather than purely per-map data.

---

## 5. `MAP.ALL` — single-file editor artefact (INFERRED)

Exactly one `MAP.ALL` exists on the disc, in one level directory. It uses the anonymous `uint32_t` offset
table idiom.

```c
#define Q2MAPALL_TABLE_COUNT 16
typedef struct {
    uint32_t offsets[16];   /* CONFIRMED (values): 15 sections + EOF sentinel.
                             * offsets[15] == file size. Strictly monotonic, all
                             * 4-aligned. Two sections are zero-length.
                             * INFERRED (framing): that 16 is the right table length
                             * is asserted by offsets[15]==filesize, not encoded.
                             * Reading 20 entries fails immediately (offsets[16]==0,
                             * non-monotonic, later values > 3.2e9).                */
    uint32_t extra[4];      /* UNKNOWN: first word 0; the other three decode as
                             * plausible IEEE-754 floats near -1.93..-1.97.         */
} q2mapall_header_t;        /* sizeof == 80 == offsets[0] */
```

`offsets[0] == 0x50 == 80 == sizeof(this header)`, so the file **does** encode its total header size; what it
does *not* encode is the offset-table entry count.

**Evidence it is not runtime data:** the strings `MAP.ALL` / `Map.All` / `map.all` occur **zero** times
anywhere in the 634,880-byte EXE (whereas `SndVram`, `.Dat` and the levels path all occur), and the file's
last 44 bytes are `0xCD` — the MSVC uninitialised-heap fill pattern. A port can almost certainly ignore this
file.

> Two supporting arguments from an earlier pass were overstated and are corrected here: the `0xCD` fill is
> **confined** to those trailing 44 bytes (there are exactly two contiguous runs, both inside the last
> declared section — not eight runs scattered through the file), so it is **not** evidence that the four
> header words at `+0x40` are garbage. Those three float-like values deserve investigation rather than
> dismissal.

---

## 6. `.STX` — Sony STR video (MDEC) + multiplexed XA audio

Three files. Stock Sony STR v2. No Hammerhead container.

### 6.1 Container interleave

> **CORRECTION.** An `.STX` file is **not** a pure video stream. It is an **8-sector interleave**: slots 0–6
> carry MDEC video, slot 7 carries XA ADPCM audio. An earlier pass that assumed a frame header at the start
> of *every* sector misread ADPCM shift/filter bytes as bogus magic values.

```c
#define STX_SECTOR_SIZE        2048u
#define STX_INTERLEAVE_PERIOD     8u
#define STX_AUDIO_SLOT            7u
#define STX_VIDEO_PAYLOAD      2016u   /* 2048 - sizeof(StrFrameHeader) */
#define STX_WIDTH               320u
#define STX_HEIGHT              192u
#define STX_FPS                  25u

static inline bool stx_sector_is_audio(uint32_t i)
{ return (i % STX_INTERLEAVE_PERIOD) == STX_AUDIO_SLOT; }
```

CONFIRMED across all 32,442 sectors of all three files: the audio-sector gap histogram is exactly `{8}`;
`audio_index % 8 == 7` for **4,055 / 4,055** audio sectors; there is **not one** video sector at slot 7, and
in one file there is not one off-slot-7 non-video sector either. Video and audio share `file# == 1` and
`channel# == 1`; they are separated only by the Form 2 submode bit.

### 6.2 Frame header (first 32 bytes of each video sector's 2048-byte user area)

Verified on **all 27,831 video sectors / 5,301 frames**.

```c
typedef struct {                /* 32 bytes */
    uint16_t magic;             /* CONFIRMED: 0x0160 (27831/27831)                   */
    uint16_t sub_type;          /* CONFIRMED: 0x8001 = MDEC video                    */
    uint16_t chunk_index;       /* CONFIRMED: 0-based sector index within the frame.
                                 * Per-frame coverage is exactly 0..count-1 in
                                 * 5301/5301 frames. (Only reaches 5, so the high
                                 * byte is always zero — u16 vs u8+pad is not
                                 * falsifiable from this data.)                      */
    uint16_t chunk_count;       /* CONFIRMED: 5 or 6, NEVER a constant 6. The cadence
                                 * is strictly 6,5,5,5 keyed to (frame_number-1)%4,
                                 * with zero violations in any file. Average 5.25
                                 * video sectors/frame; +0.75 audio = exactly 6.000
                                 * sectors/frame.                                    */
    uint32_t frame_number;      /* CONFIRMED: 1-based, strictly monotonic, no gaps    */
    uint32_t frame_size_bytes;  /* CONFIRMED: valid bitstream bytes for the whole
                                 * frame; identical across all sectors of a frame;
                                 * always a multiple of 4; always <= chunk_count*2016.
                                 * Every byte of the assembled buffer BEYOND this
                                 * value is zero in 5301/5301 frames.                */
    uint16_t width;             /* CONFIRMED: 320 (27831/27831)                      */
    uint16_t height;            /* CONFIRMED: 192 (27831/27831)                      */
    /* --- mirror of the frame bitstream's own first 8 bytes; do NOT re-prepend --- */
    uint16_t bs_num_codes;      /* INFERRED: "number of 16-bit MDEC code words" is
                                 * plausible but unvalidated. Byte facts: always a
                                 * multiple of 32, range 1440..9568, and
                                 * bs_num_codes*2 >= frame_size_bytes-8 in 5301/5301.
                                 * It is NOT frame_size/4.                           */
    uint16_t bs_magic;          /* CONFIRMED: 0x3800 (27831/27831)                   */
    uint16_t bs_qscale;         /* CONFIRMED: PER-FRAME quantization scale, observed
                                 * 1..20 across the three movies. MUST be read per
                                 * frame — it is not a constant.                     */
    uint16_t bs_version;        /* CONFIRMED: 2 (BS v2)                              */
    uint32_t reserved;          /* CONFIRMED: 0 (27831/27831)                        */
    /* uint8_t payload[2016]; */
} StrFrameHeader;
```

| Offset | Type | Name | Meaning | Confidence |
| --- | --- | --- | --- | --- |
| `0x00` | `uint16_t` | `magic` | `0x0160` | CONFIRMED |
| `0x02` | `uint16_t` | `sub_type` | `0x8001`, MDEC video | CONFIRMED |
| `0x04` | `uint16_t` | `chunk_index` | Sector index within frame | CONFIRMED |
| `0x06` | `uint16_t` | `chunk_count` | 5 or 6; cadence 6,5,5,5 | CONFIRMED |
| `0x08` | `uint32_t` | `frame_number` | 1-based, contiguous | CONFIRMED |
| `0x0C` | `uint32_t` | `frame_size_bytes` | Valid bitstream bytes this frame | CONFIRMED |
| `0x10` | `uint16_t` | `width` | 320 | CONFIRMED |
| `0x12` | `uint16_t` | `height` | 192 | CONFIRMED |
| `0x14` | `uint16_t` | `bs_num_codes` | Multiple of 32; meaning unvalidated | INFERRED |
| `0x16` | `uint16_t` | `bs_magic` | `0x3800` | CONFIRMED |
| `0x18` | `uint16_t` | `bs_qscale` | Per-frame quant scale, 1…20 | CONFIRMED |
| `0x1A` | `uint16_t` | `bs_version` | 2 | CONFIRMED |
| `0x1C` | `uint32_t` | `reserved` | 0 | CONFIRMED |

> **Critical decoder note (CONFIRMED 5301/5301).** For `chunk_index == 0`, `payload[0:8]` is byte-identical
> to header bytes `0x14…0x1B`. The concatenated demux buffer therefore **already carries its own 8-byte
> bitstream header** — do not prepend one.

### 6.3 Timing derivation

The frame rate is *forced*, not assumed. Audio occupies 1 sector in 8, and one XA sector is exactly
`2016 / 37800 s = 53.3333 ms`, so real-time audio requires `8 × 18.75 = 150` sectors/s (2× drive speed). At
6.000 sectors per frame that is exactly **25.000 fps**.

Per-file structure (CONFIRMED): 1,559 / 2,459 / 1,283 frames respectively; 8,185 / 12,910 / 6,736 video
sectors; 1,247 / 1,844 / 964 audio sectors; 544 / 0 / 12 null sectors.

> **CORRECTION.** The null sectors are *not* a contiguous trailing block — audio keeps streaming at
> `i % 8 == 7` right through the tail. The null-index-mod-8 histogram covers residues 0…6 only. The video
> region ends exactly at `frames × 6` sectors, after which the video slots are nulled while audio continues
> for a few more seconds. Consequently "6.000 sectors per frame" is exact *file-wide* only for the one movie
> with no tail; the other two are 6.399 and 6.011 file-wide.

STX null sectors (submode `0x00`, channel 0) are **fully zero-filled** — 0 of 556 contain a non-zero byte.
(This does *not* generalise to `.XAI` null sectors; see §7.)

---

## 7. `.XAI` — CD-XA ADPCM music

Five files, pure XA audio, zero video/data sectors. Four independent stereo streams per file, multiplexed by
CD-XA channel in **strict one-sector round-robin**: `sector_index % 4 == channel_num`, with **zero**
violations across 66,608 audio sectors.

```c
#define XAI_FILE_COUNT        5
#define XAI_CHANNELS          4
#define XAI_SLOT_COUNT        20   /* (file, channel) pairs                        */
#define XAI_DISTINCT_STREAMS  19   /* one file's channels 2 and 3 are byte-identical */
```

Total: 66,608 audio sectors = **3,552.4 s (59 min 12 s)** of stream time, but only **3,279.5 s** of distinct
content, because two channels of one file carry the same audio (all 5,118 payload pairs byte-identical,
identical stream hash). Individual track lengths run 1:59 to 4:33.

After a channel emits its EOF sector, its slots are filled with null sectors. **`.XAI` null sectors are NOT
zero-filled** — on one file all 1,154 have non-zero user data, 479 of them byte-duplicate the payload 4
sectors earlier, and only 1 in 18 blocks passes the ADPCM parameter-duplication test. They are stale
mastering filler. **A player must gate on submode / channel and never decode them.**

Coding info is `0x01` on **100 %** of the 70,663 audio sectors disc-wide → stereo, 37800 Hz, 4-bit, no
emphasis. One audio sector = 2016 stereo frames = 53.3333 ms; one channel gets 1 sector in 4 at 75 sectors/s
= 18.75 sectors/s, exactly real-time at **1× drive speed**.

### 7.1 Raw CD-XA sector (2352 bytes) — applies to both `.STX` and `.XAI`

```c
#define CDXA_SECTOR_RAW      2352
#define CDXA_FORM1_USER      2048
#define CDXA_FORM2_USER      2324
#define CDXA_FORM2_ADPCM     2304   /* 18 * 128; the last 20 bytes are UNUSED       */

#define CDXA_OFF_SYNC        0x000  /* 12 bytes: 00 FF*10 00                        */
#define CDXA_OFF_MSF         0x00C  /* 3 bytes BCD; decodes to LBA+150 in 100%      */
#define CDXA_OFF_MODE        0x00F  /* always 2                                     */
#define CDXA_OFF_SUBHDR      0x010  /* 8 = 4 fields + an exact duplicate            */
#define CDXA_OFF_USER        0x018
#define CDXA_OFF_F1_EDC      0x818  /* Mode2/Form1 EDC lives HERE                   */
#define CDXA_OFF_F1_ECC      0x81C  /* 276 bytes (P 172 + Q 104)                    */
#define CDXA_OFF_F2_EDC      0x92C  /* always 0x00000000 on this disc               */

typedef struct {                    /* raw bytes 0x10..0x17 */
    uint8_t file_num;               /* CONFIRMED: always 1 on STX and XAI sectors    */
    uint8_t channel_num;            /* CONFIRMED: STX 1; XAI 0..3 round-robin        */
    uint8_t submode;                /* CONFIRMED: see the bit table below            */
    uint8_t coding_info;            /* CONFIRMED: 0x01 on 100% of audio sectors      */
    uint8_t file_num_copy;          /* CONFIRMED: exact duplicate (104314/104314)    */
    uint8_t channel_num_copy;
    uint8_t submode_copy;
    uint8_t coding_info_copy;
} XaSubheader;
```

> **CORRECTION — two format errors worth stating explicitly.**
> 1. **Mode 2 Form 1 has no 8 reserved zero bytes** — that is Mode 1. The layout is 2048 user bytes at raw
>    `0x018`, a 4-byte EDC at `0x818`, then 276 bytes of ECC. The ECMA-130 EDC recomputed over raw bytes
>    `0x10…0x817` matched the stored word at `0x818` in **28,342 / 28,342** Form 1 sectors; reading it at any
>    other offset gave zero matches.
> 2. **A 2324-byte Form 2 payload is not 18 × 128.** `18 × 128 == 2304`. The trailing **20 bytes are unused
>    and are zero in 70,663 / 70,663** audio sectors. A decoder must consume 2304, not 2324, bytes per sector.

Submode bits and the complete observed set:

| Bit | Name | | Value | Meaning | Count |
| --- | --- | --- | --- | --- | --- |
| 0x01 | EOR | | `0x48` | RT \| Data — STR video Form 1 | 27,829 |
| 0x02 | Video (never set) | | `0xC8` | EOF \| RT \| Data — one file's last sector | 1 |
| 0x04 | Audio | | `0x64` | RT \| Form2 \| Audio | 70,643 |
| 0x08 | Data | | `0xE4` | EOF \| RT \| Form2 \| Audio | 20 |
| 0x10 | Trigger | | `0x80` | EOF only — last sector of each `.XAI` | 5 |
| 0x20 | Form 2 | | `0x00` | null / padding | 5,815 |
| 0x40 | Real-time | | | | |
| 0x80 | EOF | | | | |

### 7.2 XA ADPCM sound group

```c
typedef struct {                /* 128 bytes; 18 per Form 2 sector */
    uint8_t  param_copy_lo[4];  /* CONFIRMED: == params_0_3 in 1,271,934/1,271,934  */
    uint8_t  params_0_3[4];     /* CONFIRMED: low nibble shift (0..12), high nibble
                                 * filter (0..3)                                     */
    uint8_t  param_copy_hi[4];  /* CONFIRMED: == params_4_7 in 1,271,934/1,271,934  */
    uint8_t  params_4_7[4];     /* CONFIRMED                                         */
    uint32_t nibbles[28];       /* CONFIRMED: block b, sample n =
                                 * (nibbles[n] >> (4*b)) & 0xF                        */
} XaSoundGroup;

static const int XA_K0[4] = { 0, 60, 115,  98 };   /* both /64 */
static const int XA_K1[4] = { 0,  0, -52, -55 };
/* out = (n << (12 - shift)) + K0*prev1 + K1*prev2 */
```

**Stereo channel assignment (INFERRED as to left/right, CONFIRMED as to separation):** even block index →
one channel, odd → the other. Verified by DSP: splitting even/odd blocks into two channels gives a
per-channel Nyquist-energy ratio of 0.013…0.032 (smooth), whereas reading the same blocks as one interleaved
mono stream gives 0.56…0.74 (per-sample zigzag); inter-channel correlation is +0.25…+0.44 on all five files,
i.e. genuine stereo rather than duplicated mono. Which of the two is *left* is a CD-XA standard fact, not a
finding from this data.

**Redundant-copy authority is likewise a standard fact, not a finding.** The two parameter copies are
byte-equal in 98.2 % of 36,000 sampled groups, and the 1.8 % that differ also fail every other sanity check.
Using bytes 4…7 / 12…15 is the standard's rule. INFERRED.

---

## 8. CD-DA track 2

```c
#define CDDA_RATE            44100
#define CDDA_BYTES_PER_SEC   (44100*2*2)
#define CDDA_PREGAP_SECTORS  150      /* 2.000 s, physically present in the .bin */
#define TRACK2_TOTAL_SECTORS 13650    /* 3:02.00 incl. pregap */
```

Raw CD-DA: 44100 Hz, 16-bit signed LE, 2 channels interleaved, 2352 bytes/sector = 588 stereo frames, no
RIFF header. 32,104,800 bytes = 182.000 s including a 150-sector pregap; audible region 180.000 s.

**Content is verified digital silence:** a full byte scan found **0 non-zero bytes** in all 13,650 sectors.
A placeholder `.WAV` file sits in the ISO at exactly track 2's INDEX 01; its declared size is a count of
2048-byte logical sectors (a disc-space reservation), not playable audio.

> **CORRECTION — the engine really does drive CD-DA.** An earlier pass concluded the track was a decoy that
> a port could ignore entirely. In fact two entries in the music table carry `xai_file_index == -1` and are
> routed to a code path that issues `CdControl(CdlSetmode, 0x05)` then `CdControl(CdlPlay, &cdda_loc[id])`
> against a TOC table built at boot from `CdlGetTN` / `CdlGetTD` (with a +38-frame BCD offset and full carry
> fixup). Because the track is silence, a port may implement those two music ids as "no music" — but it must
> model the dispatch, not pretend it does not exist.

---

## 9. `SLES_015.34` — PS-EXE layout, key tables, and build/region detection

### 9.1 Executable header and memory map

```c
#define PSX_EXE_HEADER_SIZE 2048u

typedef struct {
    char     magic[8];      /* CONFIRMED: "PS-X EXE"                                */
    uint32_t pad0[2];       /* CONFIRMED: zero                                      */
    uint32_t pc0;           /* CONFIRMED: 0x80085F50 entry point                    */
    uint32_t gp0;           /* CONFIRMED: 0. The game sets $gp itself at startup —
                             * $gp-relative addressing IS used at runtime.          */
    uint32_t t_addr;        /* CONFIRMED: 0x80018000                                */
    uint32_t t_size;        /* CONFIRMED: 0x0009A800; image ends at 0x800B2800      */
    uint32_t d_addr, d_size;/* CONFIRMED: 0 — data is merged into text              */
    uint32_t b_addr, b_size;/* CONFIRMED: 0 — the game clears its own BSS           */
    uint32_t s_addr;        /* CONFIRMED: 0x801FFFF0 (SYSTEM.CNF overrides this)    */
    uint32_t s_size;        /* CONFIRMED: 0 = default                               */
    uint32_t saved[5];      /* CONFIRMED: all zero                                  */
    char     licence[0x7B4];/* CONFIRMED: NUL-padded ASCII region string. This is a
                             * MASTERING artefact, NOT the value the game acts on.  */
} psx_exe_header_t;         /* sizeof == 2048 */
```

Only two printable runs exist in the entire 2048-byte header (the magic and the licence string), confirming
that the saved-register area really is all-zero. File size is exactly `0x800 + t_size == 634,880`.

Memory map (all CONFIRMED by disassembling `crt0` at `0x80085F50` instruction by instruction):

| Region | Range | Notes |
| --- | --- | --- |
| Text + rodata | `0x80018000` … `0x800B2800` | data merged into text |
| rodata begins | ≈ `0x8009B520` | |
| PSY-Q library code | `0x800AED30` … `0x800B26D0` | ends where BSS clearing starts |
| BSS | `0x800B26D0` … `0x800EE718` | 245,832 bytes, zeroed by `crt0` |
| Heap | `0x800EE718` … `0x801FF600` | **1,118,952 bytes usable** |
| Stack top | `0x801FFFF8` | `(gp[0]-8) \| 0x80000000` |
| Stack reserve | `0x500` | |

> **CORRECTION — the heap ceiling.** Heap init stores `0x801FFB00` (RAM size minus the `0x500` stack
> reserve), but `_malloc` then recomputes its top as `that - 0x500 == 0x801FF600`, i.e. **the stack reserve
> is charged twice**. The real allocatable ceiling is `0x801FF600` (`0x1112E8` = 1,118,952 bytes), not the
> `0x1113E0` value `crt0` records in a global.

### 9.2 `SYSTEM.CNF`

68 bytes, four CRLF `KEY = VALUE` lines, no trailing NUL. Parse **case-insensitively and by key name**, not
by line order: the `BOOT` value is lowercase on disc while the ISO filename is uppercase.

```c
typedef struct {
    char     boot_raw[64];  /* CONFIRMED: "cdrom:\<boot>;1", lowercase              */
    char     boot_file[16]; /* derived: strip "cdrom:\" and ";1", uppercase          */
    char     serial[16];    /* derived: SLES_015.34 -> SLES-01534                    */
    uint32_t tcb;           /* CONFIRMED: 4                                          */
    uint32_t event;         /* CONFIRMED: 16                                         */
    uint32_t stack;         /* CONFIRMED: 0x801FFF00 — DISAGREES with the EXE
                             * header's s_addr (0x801FFFF0). SYSTEM.CNF wins on
                             * real hardware.                                        */
} q2psx_system_cnf_t;
```

### 9.3 Heap allocator

A single doubly-linked list of **allocated** blocks in address order. Free space is implicit; there is no
free list and no coalescing.

```c
#define Q2PSX_MALLOC_HDR_SIZE   28
#define Q2PSX_MALLOC_PROBE_SIZE 0x01000000u  /* silent-fail free-space probe */

typedef struct Q2MemBlock {
    struct Q2MemBlock *prev;    /* CONFIRMED: NULL only for the head sentinel        */
    struct Q2MemBlock *next;    /* CONFIRMED: NULL on the topmost block              */
    uint32_t           size;    /* CONFIRMED: REQUESTED size, unrounded              */
    char               tag[16]; /* CONFIRMED: debug owner tag, forced NUL at [15]    */
} Q2MemBlock;                   /* payload at (uint8_t*)hdr + 28 */

#define Q2_BLOCK_SPAN(sz)  (((sz) + 31u) & ~3u)   /* CONFIRMED occupied span */
```

Behaviour (all CONFIRMED from the disassembly): `_malloc(size, from_top)` — `from_top == 0` is first-fit,
`!= 0` is last-fit; the largest observed gap is tracked in a `$gp` global; a request of exactly `0x01000000`
**fails silently** and is the engine's own free-memory probe (`LargestFreeBlock` = probe, then read
`largest_gap - 28`). `Free` takes a **pointer-to-pointer** and always NULLs the caller's variable. The
debug-print routine is a 5-instruction **stub that produces no output**, so all allocator diagnostics are
dead.

A second, simpler bump arena ("Global Memory") exists with base / current / limit pointers plus success and
failure counters, reset wholesale and with no individual free.

### 9.4 Level table

Base vaddr `0x8009C6C8` (file `0x084EC8`), **stride 56**, 52 records (51 usable + a sentinel).

Stride 56 is proved **three independent ways**: an `addiu s1,s1,56` walk in the lookup routine, an
`addiu a0,a0,56` walk in the runtime-state clear, and a shift-add reciprocal in a third routine which,
executed numerically for i = 0…59, is an exact divide-by-56 (and produces garbage for strides 44/48/52/60).

```c
typedef struct {                /* 56 bytes */
    char      name[12];         /* CONFIRMED: lookup key. The scan ends at the first
                                 * record whose first WORD is zero.                  */
    char      dirname[10];      /* INFERRED: uppercase on-disc directory name. Width
                                 * 10 is deduced from the s16 at +0x16, not from a
                                 * located reader.                                   */
    int16_t   unit;             /* CONFIRMED: campaign unit; 0 == not in the campaign.
                                 * 16-bit width proved by two `lh` sites.            */
    void     *runtime_ptr;      /* CONFIRMED: a RUNTIME POINTER, not an ignorable
                                 * NULL callback — the resource loader writes here.
                                 * A port MUST model it as writable per-level state. */
    uint32_t  hook_b;           /* INFERRED: constant `jr ra; nop` address on all 52
                                 * records; no reader located.                       */
    uint8_t   kind;             /* CONFIRMED: u8 (NOT u16) — read via `lbu` at three
                                 * sites, all of which only test kind != 0. Observed
                                 * 0 (front-end/stub), 1, 2, 4. Nothing in the code
                                 * distinguishes 1 from 2 from 4.                    */
    uint8_t   pad0;             /* CONFIRMED: 0 on all 52 records                    */
    int8_t    music[14];        /* INFERRED: a byte list whose terminator equals
                                 * -(number of preceding entries) — that arithmetic
                                 * property is CONFIRMED on all 52 records. That the
                                 * entries are track selectors is NOT: no instruction
                                 * anywhere in the image loads offset 0x22 from a
                                 * level record.                                     */
    uint8_t   runtime[8];       /* CONFIRMED: cleared per-level at new-game start by
                                 * a stride-56 walk that stores zero to +0x30..+0x37 */
} Q2Level;
```

Six records name directories that do not exist on the disc; four directories exist that no record names.

### 9.5 Pickup / spawnable-entity table

Base vaddr `0x8009F5CC` (file `0x087DCC`), **stride 24**, 64 real records plus a terminator whose id byte is
`0xFF`. Stride and termination are proved from the two lookup routines (`lb` id, compare against −1,
`addiu 24`).

```c
typedef struct {                /* 24 bytes */
    int8_t    id;               /* CONFIRMED: matched against a u16 in the map spawn
                                 * record. Compared with `lb`, so 0xFF terminates.
                                 * Not ascending; one id appears twice.              */
    uint8_t   item_index;       /* CONFIRMED: copied into the spawned entity          */
    uint16_t  flags;            /* CONFIRMED bits: bit 8 suppresses a spawn attribute
                                 * (the spawner receives (!(flags&0x100))<<10); bit 1
                                 * selects one of two entity constants; bit 0 clears
                                 * a bit in the entity. Remaining bits UNKNOWN.      */
    char      model[12];        /* CONFIRMED: fixed width, passed to the spawner. Two
                                 * names fill all 12 bytes with no NUL.              */
    uint16_t  extra[4];         /* INFERRED: 0xFFFF-terminated list; empty in 52 of
                                 * 64 records, two entries in 12. A POINTER to this
                                 * field is stored into a sub-structure of the spawned
                                 * entity, so a consumer exists; meaning UNKNOWN.    */
} Q2Pickup;
```

### 9.6 Weapon tables — **consumed 1-based**

Eleven parallel arrays: name (`char[11][12]`), ammo-per-shot, owned-bit mask (`1<<i`), ammo type, a 3-byte
HUD glyph table, and an auto-switch preference list.

> **CORRECTION — the arrays are indexed 1-based.** The code never materialises the array bases; it
> materialises *base minus one element*. Proof: the auto-switch list's first value is 10, and indexing
> `weapon_bit - 4` by `10*4` reaches `weapon_bit[9]`. **The live weapon id in player state is 1…11, with 0
> meaning "no weapon."** The HUD glyph table is indexed directly by that id, so id 0 selects a blank glyph.
> A port that uses a 0-based enum will be off by one in ammo cost, ammo type, owned-bit and name lookup
> everywhere.

> **CORRECTION.** The name array is `char[11][12] == 132 bytes` with **no** zeroed terminator entry — only 4
> bytes of padding separate it from the next array. A `char[12][12]` declaration overruns.

> **CORRECTION.** Decoded 1-based, the auto-switch list omits the BFG, the grenade launcher and hand
> grenades — a sensible "never auto-switch to explosives" rule, not the arbitrary-looking omission an earlier
> 0-based reading produced.

There is a **second, differently ordered** weapon enumeration in the entity-class table (base `0x800A3A40`,
stride 48, 21 named records). Its field offsets are: shared stub pointer at `+0x00`, a second pointer at
`+0x04`, an optional class id at `+0x18` (−1 on most records, real values on three), the 12-byte class name
at `+0x1C`, a zero word at `+0x28`, and **the per-class function pointer at `+0x2C`** (an earlier pass placed
it at `+0x28`). **Do not conflate the two weapon orderings** — the railgun is index 3 in one table and 9 in
the other.

### 9.7 Sound-name tables

Five arrays of fixed 12-byte NUL-padded names, no terminator entries, 78 entries total (4 menu, 12
world/event, 11 item, 22 player, 22 weapon). All five bases are confirmed referenced by their stated consumer
functions. Names are resolved by string against `SNDVRAM.DAT`.

**Count caveat:** none of the arrays encodes its length, so counts are bounded by what follows in rodata.
Four are cleanly bounded by a format string, a jump table or a zero slot; the player array's boundary is a
judgement call because more 12-byte name slots follow contiguously.

### 9.8 Music / track selection

Table at vaddr `0x800A1DD8`, **22 records × 6 bytes**, ending exactly where the `.XAI` filename block begins.

```c
typedef struct {                /* 6 bytes */
    int8_t   xai_file_index;    /* CONFIRMED: 0..4, or -1 => the CD-DA path          */
    uint8_t  xa_channel;        /* CONFIRMED: 0..3                                   */
    uint16_t reserved_zero;     /* UNKNOWN: always 0                                 */
    uint16_t duration_decisec;  /* CONFIRMED: track length in 1/10 s. Cross-validated
                                 * against independently measured stream lengths:
                                 * 19 of 20 XA entries match to the decisecond; one
                                 * is 1.0 s short. The consumer multiplies by 5
                                 * (deciseconds -> 50 Hz PAL ticks) with a 30.0 s
                                 * fallback when the CD call fails.                  */
} Q2MusicEntry;

#define Q2_MUSIC_ENTRY_COUNT 22
#define Q2_MUSIC_FIRST_XA     2    /* ids 0 and 1 are the CD-DA entries */
/* id N >= 2  ->  file = (N - 2) / 4,  channel = (N - 2) % 4 */
```

Records 2…21 are exactly `(file 0..4) × (channel 0..3)` in ascending order, which is what makes the flat-id
mapping above exact.

**XA playback sequence (CONFIRMED, all three calls verified at their call sites):**

```c
#define CDL_PLAY        3
#define CDL_STOP        8
#define CDL_SETFILTER  13
#define CDL_SETMODE    14
#define CDL_READS      27
#define Q2_XA_CD_MODE  0x48   /* CdlModeRT | CdlModeSF; DOUBLE-SPEED BIT CLEAR -> 1x */

CdControl(CdlSetmode,   &Q2_XA_CD_MODE);
CdControl(CdlSetfilter, &(CdlFILTER){ .file = 1, .chan = entry.xa_channel });
CdControl(CdlReadS,     &xai_loc_table[entry.xai_file_index]);
```

The path is assembled at runtime from a levels-root literal + an audio-subdirectory literal + the filename +
`";1"`.

> **CORRECTION.** The table at `0x800DF18C` holds **`CdlLOC` values** (BCD minute / second / frame / track),
> not integer LBAs. The boot loop copies the 4-byte result of a file-search helper and hands it straight to
> `CdControl(CdlReadS, &loc)`. A parallel 16-entry `CdlLOC` table for CD-DA sits immediately below it.

### 9.9 Display, video mode and frame lock

> **CORRECTION — the framebuffer is 512 × 248, not 512 × 256, and it is runtime state, not a constant.**

Dimensions live in a BSS global pair: `uint16_t width` at `0x800B2DA0`, `uint16_t height` at `0x800B2DA2`.
All 44 references to that pair were enumerated: **exactly one writer** (storing 512 and 248, inside a
display-init function called once from `main`) and 42 readers, including the engine's `SetDefDrawEnv` call
and all four of its `SetDefDispEnv` calls. The 512 × 256 rectangles that an earlier pass quoted belong to a
*secondary* env-init function with a single unrelated caller — not the engine display path.

```c
#define Q2PSX_PAL_GLOBAL_FB_WIDTH   0x800B2DA0u  /* u16, = 512 */
#define Q2PSX_PAL_GLOBAL_FB_HEIGHT  0x800B2DA2u  /* u16, = 248 */
#define Q2PSX_PAL_GLOBAL_VIDEO_MODE 0x800A9E70u  /* u32, = 1 (PAL) */

typedef struct {
    uint16_t fb_width;          /* CONFIRMED: 512                                    */
    uint16_t fb_height;         /* CONFIRMED: 248 on PAL                             */
    int16_t  buf_x[2], buf_y[2];/* CONFIRMED: {0,512} / {0,0}. A runtime {s16 x,
                                 * s16 y} table with stride 4, indexed by buffer
                                 * number and cross-paired between draw and display. */
    uint32_t video_mode;        /* CONFIRMED: 1 == PAL                               */
} q2psx_display_state_t;
```

**Region is set explicitly in code.** `SetVideoMode(mode)` writes a libgpu global; it has **exactly one call
site in the entire executable**, four instructions before the framebuffer stores, with the literal argument
`1` (`MODE_PAL`). `SetDefDrawEnv` independently branches on `GetVideoMode()` using the canonical 257 (NTSC) /
289 (PAL) line thresholds. **This is a far better and cheaper build discriminator than a SHA-256 tier**, and
it is the value the game actually acts on — the header licence string is a mastering artefact.

VRAM is 1024 × 512; two 512 × 248 buffers side by side occupy rows 0…247, leaving rows 248…511 for textures
and CLUTs.

**The frame lock is statically provable.** At two sites the sequence is `DrawSync(0)` immediately followed by
`VSync(2)` in the delay slot — the canonical PSX swap-and-wait-two-fields lock.

```c
typedef struct {
    uint16_t field_hz;          /* CONFIRMED: 50 (established by SetVideoMode(1))    */
    uint16_t vsync_divisor;     /* CONFIRMED: 2 (the literal VSync argument)         */
    uint16_t native_logic_hz;   /* CONFIRMED: 25 on PAL. NTSC 30 is INFERRED — the
                                 * same divisor over 60 Hz — pending an NTSC EXE.    */
} q2psx_timing_t;
```

A separate slower path uses `VSync(3)` (16.67 Hz on PAL); its purpose is unknown.

> An earlier pass claimed all `VSync` call sites were library timeouts in a high address band. In fact eight
> of the 38 sites lie in game code far below that band, and two of them are the swap sites above.

### 9.10 Build / region detection for the port

Three tiers, cheapest first:

1. **`SYSTEM.CNF` `BOOT` filename** → serial. Case-insensitive, strip `cdrom:\` and `;1`.
2. **Boot-EXE size + SHA-256.** Size is a free pre-filter; the PAL EXE is 634,880 bytes.
3. **ISO9660 PVD** (LBA 16; in a raw Mode 2/2352 image the user area starts at `lba*2352 + 24`) — creation
   timestamp, GMT quarter-hour byte, and `volume_space_size`.

```c
typedef struct {
    char     creation[17];      /* "YYYYMMDDHHMMSShh" + NUL                          */
    int8_t   gmt_quarter_hours; /* CONFIRMED: +09:00 on the PAL disc. This is a Sony
                                 * Japan MASTERING artefact — never a region test.   */
    uint32_t volume_space_size; /* CONFIRMED: equals data track + audio track, i.e.
                                 * the WHOLE disc, not just the data track.          */
} q2psx_pvd_id_t;
```

PVD sanity fields worth checking: `type == 1`, std id `CD001`, and both system id and application id equal to
the console name — a cheap "is this even a PSX disc" gate. Modification / expiration / effective timestamps
are all-ASCII-zero on this disc and are useless as keys.

The build descriptor a port should carry per release: serial, boot filename, region, EXE size + hash + `pc0`
/ `t_addr` / `t_size`, PVD creation + volume size, data and audio track sector counts, **`video_mode_const`**,
**`fb_width` / `fb_height`**, **`vsync_divisor`**, the VRAM buffer origins, the movie filename suffix, and the
expected level-directory count (49).

> **NTSC values are unknown and must not be guessed.** Do not hardcode a 512 × 240 NTSC framebuffer: PAL
> turned out to be 248 rather than the widely assumed 256, so the folklore figure is *less* trustworthy now,
> not more. Locate the NTSC build's equivalent of the `SetVideoMode` + width/height store and read the
> literals.

### 9.11 Filenames are assembled at runtime

The executable contains **zero** occurrences of `MOVIES`, `STX`, `.STX`, `COMMON.DAT`, `SNDVRAM`, `ZONE`,
`.DAT` or `MAP.ALL`. It *does* contain the levels-root path literal (3 times), a `LEVELS` literal (twice),
the audio-subdirectory literal and the five `.XAI` filenames. So level and movie filenames are built at
runtime, and the movie player itself is not in the main executable — it must be an overlay.

---

## Open questions

Ordered by how much each blocks a working port. Items 1–6 stand between the project and a level you can walk
around in; the tail is cosmetic or archival.

### Blocking — cannot render or load a level

1. **`SNDVRAM.DAT` section A pixel compression codec is unknown.** Every texture and every 2D image on the
   disc is behind it. Nothing textured can be drawn until it is solved. Start from a stub map's smallest
   payload, where the record count is 1–2 and the name list bounds the data exactly.
2. **`CastList` face vertex-index base is unresolved.** Part-relative fails on 34 % of faces and is
   unsolvable for 257 of 965 models; whole-model absolute leaves most vertices of large models unreferenced.
   No model — player, monster, weapon or pickup — can be drawn correctly. Resolve from the renderer in the
   EXE; `CastList` block C (8 bytes per part) is the likely home of a per-part vertex base.
3. **World coordinate scale is unknown (×5 vs ×10 vs something else).** The authoring grid is visible
   (multiples of 640 are 86× enriched, 320 52×, 160 31×, 80 18×) but the multiplier relative to PC Quake II
   units is not established. Everything — player size, step height, speeds, weapon ranges — depends on it.
   Read the player bounding-box / step-height / max-speed constants out of the EXE and divide by the PC
   values (56 / 18 / 320). **Related sub-question:** dominant world-Y values arrive in ±1 *pairs* one unit
   *below* the grid multiple; a `floor(x*S)` exporter would explain both that and the asymmetric `Scene`
   bbox slop in one shot.
4. **`Events` operand stream is undecoded.** `size` / `sub` / `cls` framing is exact, but the body bytes are
   opaque. No doors, lifts, buttons, teleports, zone transitions or level progression without it. This is the
   gate between "static geometry viewer" and "game".
5. **Collision plane point encoding is only 95.6 % confirmed.** The `u16[3]` at `+0x00` reads as an unsigned
   offset from the owning node's `bboxMin`, which puts 46,968 of 49,148 planes inside their node and makes
   91 % of nodes convex-consistent — good, not good enough to trust player movement to. Resolve the residual
   4.4 % (a different base? a sign convention? a second plane class?).
6. **`LevelBin` / `CreAIBin` module ABI and the `Rel` fixup encoding.** The chunks are confirmed MIPS R3000
   and every fixup value is a valid in-`Bin` offset, but only 31 % are 4-aligned, so the array is not a plain
   word-address list. Level scripting and monster AI are both behind this. A port will most likely
   reimplement the behaviour rather than run the code, but decoding the modules is how you learn what the
   behaviour *is*.

### Blocking — degrades the level badly but does not prevent loading

7. **`SortData` encoding.** Bit-packed, no offset table, no fixed per-node record (4.0…88.6 bytes per scene
   node, a 22× spread). Almost certainly draw-order / BSP-ish data; without it, transparency and overdraw
   will be wrong. Requires the EXE's bit reader.
8. **`AreaConx` 9-byte link payload.** The portal graph's topology parses; the per-link payload does not. No
   fixed offset yields a 1.3.12 unit normal in more than 39 % of 3,494 links, and byte histograms suggest
   *unaligned* `int16_t` values that no single struct layout can express (links start at `record + 1 + 9*L`,
   so parity alternates). Byte `+3` is the best neighbour-index candidate. Blocks portal-based visibility.
9. **`SpaceLights` per-node partition.** A flat `uint16_t` index array with no length prefix and no
   discoverable partition (0.68…7.12 entries per scene node). Blocks correct dynamic/ambient lighting.
10. **`Population` `spawn.classId` target table.** 25 distinct values 0…37; 15 of 673 records exceed the
    map's `ModelNames` count, and resolving against `ModelNames` yields semantically wrong results. Until it
    is found, monsters and items cannot be mapped to their classes.
11. **`MapMod` `Poly.uvIdxFlags` bits 6–7.** Render flags on 11.7 % of all polygons. Almost certainly
    semi-transparency / double-sided / no-texture; guessing will look wrong on a tenth of the world.
12. **`Scene` node fields `flags08`, `unk0C`, `unk0D`, `unk0E`.** `unk0E` (range 0…197, 119 distinct values,
    non-zero on all but 3 of 17,035 nodes) is the highest-value single byte in the zone format. Find the
    52-byte-stride `Scene` reader in the EXE and see what it does with byte 14.

### Behavioural / audio-visual polish

13. **Which in-game situation selects which music id.** The id lives in a `$gp` global; the writer was not
    traced. The per-map id is probably in the level `.DAT` chunks. Without it, music is silent or arbitrary.
14. **Whether the engine loops XA tracks.** The duration field is converted to 50 Hz ticks and stored to two
    globals with a 30.0 s fallback — that looks exactly like a countdown to a restart or fade — but the code
    consuming those globals was not disassembled. The one entry that is 1.0 s *short* of its measured length
    hints the value is a deliberate restart point, not a pure length.
15. **MDEC output depth (24-bit vs 15-bit) for the movies.** Now harder to settle from the EXE, because the
    movie player is **not in the main executable** at all (see #16).
16. **Locate the movie player overlay.** The executable contains no `.STX` / `MOVIES` / `STX` string
    whatsoever, so both the player and its filename assembly live elsewhere. Finding it would also settle the
    movie filename suffix question and the MDEC depth in one pass.
17. **The `SNDVRAM` SPU RAM base / reverb work area,** and whether reverb is disabled at all — the worst-case
    map leaves only a **240-byte** margin against SPU RAM, which is suspiciously tight if a reverb buffer is
    also allocated.
18. **`VramImageRec.width` / `height`: dimensions or VRAM placement coordinates?** Both readings are legal.
    Settled the moment one payload is decompressed and its pixels counted — i.e. by solving #1.
19. **The secondary 512 × 256 display-env init function** with a single unrelated caller. It proves the game
    switches display configurations at least once (boot screen? loading screen? FMV?). Resolve before the
    port commits to a single display model.
20. **The `VSync(3)` path** (16.67 Hz on PAL), reached only through a function pointer. FMV, menu or
    load-screen tick?

### Low-impact unknowns and archival questions

21. `CastList` blocks A (8-entry directory confirmed, payloads undecoded), B (per-instance, articulated
    models only), C (per-part 8-byte elements — see #2), D; the animated-model frame layout; and the header's
    24-bit field at `+0x01` (range 261…333367).
22. `PrimaryRemap` value space — definitively *not* a scene-node index (max exceeds the scene node count in
    100 of 115 files); probably a polygon or surface id in a shared table.
23. `CollNode` fields `c` (0…65,077,433, non-monotonic) and `d` (0…75).
24. `Resources` `unk0` (−3000…6600) and `unk4` (40…180); `unk3` (64, occasionally 80).
25. `TrigBounds` trigger `id` (9…75 plus 255) and `flags` (14 distinct values) value semantics.
26. The five `Lights` style values (`(n<<3)|7`, n = 0…4) — what each style *does*.
27. Pickup `flags` bits beyond 0, 1 and 8; and the meaning of the pickup `extra` list (a consumer exists — a
    pointer to it is stored into the spawned entity's sub-structure — but the interpretation is unknown).
28. `Q2Level` `+0x1C` (constant `jr ra` address, no located reader — possibly the high half of an 8-byte
    field whose low half is the runtime pointer at `+0x18`); the writers of the per-level `runtime[8]` state;
    and the `music_playlist` field's real meaning, given that **no instruction anywhere in the image loads
    offset `0x22` from a level record**.
29. `SNDVRAM` section A header bytes `0x0E` (17 for front-end maps, 29…86 for gameplay) and `0x0F`
    (1…181, constant within some map families and not others).
30. NTSC build values: framebuffer height, `video_mode_const`, movie filename suffix, EXE hash, PVD fields.
    All must be **read**, never inferred from the PAL build.
31. Locate the `.DAT` chunk-name literal pool's real xrefs, which would settle its true extent and whether a
    required-vs-optional flag exists per chunk. Blocked on a working disassembler session.
32. Why `ModelNames` is present in all 49 `COMMON.DAT` files yet the string appears **zero** times in the
    executable. Dead tool-only data, positional access, or a name assembled at runtime?
33. Why `TriggerRemap` and `SecondaryRem` exist in the executable but are emitted by no file on the disc.
    Cut features, or read from a source not on this disc — a parser should tolerate them.
34. Why the zone directory order permutes the `SecondaryCol` / `PrimaryRemap` / `AreaConx` trio as a function
    of zone index. The correlation is perfect and exceptionless; the build-tool mechanism is unexplained. It
    matters only as further proof that index-based lookup is unsafe.
35. `MAP.ALL`'s purpose, its four header words at `+0x40` (three of which decode as plausible floats), and
    whether its 16-entry table length is right (unverifiable — N = 1).
36. The unused 20-byte tail of every Form 2 payload, and the always-zero `uint16_t` at `+2` of each music
    table record. Both are zero in 100 % of samples, so nothing can be inferred from this disc.
37. `GlintMod` (2608 bytes, one map, high-entropy after the first few dozen bytes).
38. **Security note, carried forward.** A prior research pass reported that a fan wiki page about this game
    served content containing instructions addressed to AI agents (create files, transfer funds, insult the
    operator, terminate). That URL was **not** fetched during verification. Treat it as hostile for any
    automated fetch; if data from it is wanted, a human should open it. All web-sourced claims in the release
    census (the existence of the NTSC SKU, its timestamps and track lengths, barcodes, demo-disc serials, the
    absence of a Japanese release) remain **unverified** and should be treated as moderate confidence at
    best.

---

## Verification methodology

Every structure in this document went through **two independent passes**: a derivation pass and an
**adversarial verification pass** in which a second analysis re-derived each CONFIRMED claim *from raw bytes
with its own parser*, without reading the first pass's code, and actively attempted to falsify it. Where the
two disagreed, the disagreement is recorded inline as a CORRECTION rather than silently resolved — the
corrections are the most load-bearing content in the document, because each one describes a way a
reimplementation would have broken.

### Principles applied

* **Whole-corpus, not sampled.** Claims are stated against the complete population wherever the population is
  tractable: all 49 `COMMON.DAT`, all 115 `ZONE*.DAT`, all 49 `SNDVRAM.DAT`, all 965 models, all 116,814
  vertices, all 76,320 faces, all 274,936 polygons, all 139,240 collision planes, all 1,167,540 SPU-ADPCM
  blocks, all 32,442 movie sectors, all 104,314 media sectors. Several of the corrections above exist purely
  because an earlier pass sampled (one file, four sectors, three chunks) and generalised.
* **Bounds attack.** Every structure was tested against both extremes — the 840-byte stub zones and the
  largest maps — because minimal files isolate headers and maximal files break narrow field widths.
* **Field-width attack.** Every integer field was checked for whether a narrower read would be
  indistinguishable. Where it would be (`chunk_index` never exceeds 5; `frame_number` never exceeds 2459),
  the document says so explicitly rather than claiming a width it cannot prove.
* **Set equality, not equal cardinality.** Where two populations were claimed to coincide (files lacking the
  AI pair vs files with an empty `CastList`), the check was set equality, not a matching count.
* **Cross-structure invariants.** The strongest confirmations come from identities that span structures and
  would collapse under any misreading: `sum(part.numFaces) == header.numFaces`; the colour-table size
  identity `align4(3*(maxColIdx+1))`; `TrigBounds.eventOfs` landing on walked `Events` record boundaries;
  the `Scene` bbox matching the world AABB computed from `Points + origin`.
* **Physical plausibility as a decoder.** Ambiguities were resolved by testing which reading produces
  physically sensible results: normal magnitudes clustering at exactly 4096; spawn coordinates landing inside
  the map's own light-coordinate envelope; stereo separation measured as a Nyquist-energy ratio; ADPCM
  decoded and checked for audio-like peak / RMS / zero-crossing statistics rather than noise.
* **Code beats data.** Where a meaning depended on runtime behaviour, it was read out of the MIPS
  disassembly at a named instruction address rather than inferred — the 1-based weapon arrays, the 96-sound
  clamp, the loop-detection test, the `VSync(2)` frame lock, `SetVideoMode(1)`, the 512/248 framebuffer, the
  allocator's double-charged stack reserve, and the XA `CdlSetmode` / `CdlSetfilter` / `CdlReadS` sequence
  are all *code* findings, not data findings.

### How to re-run the checks

Prerequisites: Python 3 (no third-party packages needed), the extracted disc tree, and the **raw Track 1
`.bin`** (required — the pre-extracted media files are lossy, see the warning in the Disc layout section).

1. **`.DAT` containers.** Walk every `COMMON.DAT` / `ZONE*.DAT`. Assert: `dir[0].offset % 16 == 0`;
   `entry_count == dir[0].offset / 16`; offsets strictly monotonic; final entry name is 12 zero bytes and its
   offset equals `os.path.getsize()`; every offset `% 4 == 0`; every chunk size `% 4 == 0`; no duplicate
   names; no gaps. Expect 164/164 clean.
2. **Per-chunk structure.** For each chunk, assert its size identity (`len % 32 == 12` for `Resources`,
   `len % 28 == 0` for `Lights`, `4 + 36*(nTrig+1) + 12*nPlanes` for `TrigBounds`, and so on) across all 49
   maps before parsing any record. A size identity that holds on 49/49 is what makes the field layout safe.
3. **Walk-to-end tests.** For variable-length structures (`Events`, model chains, `MapMod` records,
   `Population` lists), the acceptance criterion is that the walk consumes the chunk **exactly** — not
   approximately — and produces exactly the declared record count. Any residue means the layout is wrong.
4. **Fixed-point normals.** Compute `sqrt(nx²+ny²+nz²)` for every vertex normal, trigger plane normal and
   collision plane normal. Expect 4094…4096 with no outliers. This is the cheapest possible regression test
   for a field-offset error anywhere in the geometry path.
5. **Cross-references.** Verify that every `TrigBounds.eventOfs != 0xFFFF` lands on an `Events` record start;
   that every `Scene.mapmodOffset` sequence tiles `MapMod` with no overlap and ends at chunk end; that every
   `MapMod` vertex/colour/UV index is in range for its node; that `PrimaryRemap` length equals
   `align4(2 * PrimaryColl.numNodes)`.
6. **`AreaConx` regression.** Parse the offset table at `+0x02` and assert the first non-zero offset equals
   `2 + 2*numAreas` on 115/115. If a change ever makes this fail on ~100 files, the `+0x04` bug has returned.
7. **Sector-level media.** Read the raw Track 1 `.bin` at the ISO-derived LBAs. Assert mode == 2, subheader
   duplication, and that the BCD MSF at `0x0C` decodes to `LBA + 150`. Recompute the ECMA-130 EDC (reflected
   polynomial `0xD8018001`) over raw bytes `0x10…0x817` and compare against the word at `0x818` — expect
   28,342/28,342 Form 1 matches.
8. **`.STX` cadence.** Classify every sector; assert no video sector has `index % 8 == 7`; group video
   sectors by `frame_number` and assert per-frame agreement of `chunk_count` / `frame_size_bytes` /
   `bs_qscale`, contiguous `chunk_index` coverage, and that `chunk_count` follows 6,5,5,5 keyed to
   `(frame_number-1) % 4`.
9. **`.XAI` round-robin.** Assert `sector_index % 4 == channel_num` for every audio sector, and that no
   non-null sector appears in a channel's slots after that channel's EOF sector.
10. **EXE work.** Translate with `file_offset = 0x800 + (vaddr - 0x80018000)`. Prove table strides three
    ways where possible (a forward walk, a backward walk, and a reciprocal-divide executed numerically over
    the index range). Prove base addresses by scanning for `lui`/`addiu` materialisation pairs across the
    whole text segment rather than trusting a single site — that scan is what revealed the 1-based weapon
    arrays (the code materialises *base minus one element*, never the base).

### A note on adversarial value

Of the roughly 60 CONFIRMED claims re-tested, about 50 reproduced byte-for-byte. The remaining ~10 produced
corrections, and they were not evenly distributed: **every substantive error traced to a small sample** (one
file, four sectors, three chunks, one code site). The `AreaConx` offset error alone would have silently
corrupted 100 of 115 zone files. Future contributors should treat "verified on one map" as equivalent to
unverified.
