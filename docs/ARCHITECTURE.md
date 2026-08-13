# Architecture

## The shape of the problem

Quake II on PlayStation is not Quake II with a different renderer. Hammerhead
rebuilt the game for a console with 2 MB of RAM, 1 MB of VRAM, 512 KB of sound
RAM and no floating-point unit. The world is not a BSP tree streamed from a
`.bsp`; it is a set of pre-baked *zones* with their own geometry, collision,
lighting and visibility data. The entity system, AI, and level scripting are
Hammerhead's own.

So this project is not a port of id's GPL source with PSX data loaded into it.
It is a reimplementation of Hammerhead's engine, informed by the original
executable's structure and validated against the original data.

## Layering

Dependencies point strictly downward. Nothing in a lower layer knows a higher
layer exists.

```
        ┌──────────────────────────────────────────┐
        │  client      window, input, main loop    │
        ├──────────────────────────────────────────┤
        │  game        entities, AI, physics       │
        ├────────────────────┬─────────────────────┤
        │  render            │  audio              │
        ├────────────────────┴─────────────────────┤
        │  psx         GTE + GPU primitive model   │   ← fidelity core
        ├──────────────────────────────────────────┤
        │  formats     .DAT container, level schema│
        ├──────────────────────────────────────────┤
        │  build       release identification      │
        ├──────────────────────────────────────────┤
        │  disc        images, ISO9660, CD-XA      │
        ├──────────────────────────────────────────┤
        │  common      types, fixed point, hashing │
        └──────────────────────────────────────────┘
```

### `common`
Fixed-width types, little-endian unaligned readers, the fixed-point formats, and
SHA-256. Nothing here allocates policy.

The endian readers matter more than they look: every integer on the disc is
little-endian because the console was, and the host may not be. No code in this
project casts a pointer into disc data to a wider type.

### `disc`
Turns "a thing the user pointed at" into a flat file namespace plus a track list.
Handles `.cue`/`.bin`, `.iso`, and bare images; understands Mode 1, Mode 2 Form 1
and Mode 2 Form 2 sectors, which matters because ordinary files are Form 1 while
streamed audio and video are Form 2 with a different payload size.

The public surface is deliberately small — `disc_find`, `disc_read_file`,
`disc_read_raw_sector` — so that adding CHD support or a physical CD backend
later touches nothing above this layer.

### `build`
Identifies *which release* the disc is, keyed on the SHA-256 of the boot
executable rather than on the region. This is the design decision most likely to
be questioned, so: the game's text, level table and pickup tables live inside the
executable. A localised or revised release moves them. Keying data-table offsets
off "PAL" would break the first time a German disc appears; keying them off the
exact executable cannot.

An uncatalogued disc is *not* rejected. It reports as unknown and runs in generic
mode, because refusing to boot on a regional release nobody has dumped for us
would break the project's central promise.

### `formats`
Parsers for on-disc data. The `.DAT` container is a fixed-schema directory of
named chunks; `level.h` maps those names to typed slots.

The chunk schema was established by census over every level file on the disc, not
by reading one file and generalising — see `q2psx-inspect dats`. That distinction
caught a real subtlety: the last directory entry is a nameless sentinel whose
offset is end-of-data, and chunk *indices* are not stable across files even
though the *name set* is closed.

### `psx` — the fidelity core
The GTE and the GPU primitive model. This is where the PlayStation look is
decided, and it is deliberately a layer of its own rather than part of `render`.

The game emits GPU primitives into an ordering table exactly as the original did.
Backends consume that primitive stream. Keeping this indirection is what makes
faithful rendering possible at all: the backend sees what the hardware saw, so it
can reproduce the hardware's rules rather than approximate the result.

See [`FIDELITY.md`](FIDELITY.md) for what that buys and how it is verified.

### `render`, `audio`, `game`, `client`
Not yet implemented. `render` will consume `psx_ot`; `audio` needs SPU-ADPCM for
sound effects, CD-XA ADPCM for streamed music, and plain PCM for the CD audio
track; `game` is the reimplemented simulation; `client` is the SDL3 host.

## Why C11

The original is C. The data structures are C structures. Fixed-point arithmetic
with explicit overflow behaviour is easier to keep honest without operator
overloading quietly changing the type of an intermediate. The project builds with
clang, gcc and MSVC, and `-fwrapv` is on deliberately — the GTE relies on
two's-complement wrapping to reproduce hardware saturation.

## Testing strategy

The offline tool is the test harness. `q2psx-inspect verify` runs every level
file on the disc through the real typed loader — the same code the engine will
use — and fails if any file has an unknown chunk or is missing a mandatory one.
It currently passes on all 164 COMMON/ZONE files of the PAL build.

This is the pattern to keep: every format claim in `FORMATS.md` should have a
corresponding check in the tool, so that "we understand this format" is a
statement the build system can evaluate rather than an assertion in a document.

## Current status

| Layer | State |
|---|---|
| `common` | working |
| `disc` | working — cue/bin, iso, ISO9660, Form 1/2, SYSTEM.CNF |
| `build` | working — PAL build fingerprinted and catalogued |
| `formats` | container, level schema and vertex pool working; other chunk contents in progress |
| `psx` | GTE and OT implemented; needs conformance tests |
| `render` | not started |
| `audio` | not started |
| `game` | not started |

Validated against the PAL disc (`q2psx-inspect verify`):

```
COMMON.DAT : 49 resolved
ZONE*.DAT  : 115 resolved
vertices   : 461852 across all zones
failed     : 0
```

## What is genuinely known versus assumed

Worth stating plainly, because a reimplementation built on a confident guess is
worse than one built on an acknowledged gap.

**Established by evidence across the whole disc:**

- The `.DAT` container, including the nameless end sentinel.
- The complete chunk name set for COMMON.DAT (14, plus one map-specific extra)
  and ZONE*.DAT (12 mandatory, 2 optional), and the fact that chunk *order* is
  not stable while the *name set* is closed.
- The `Points` chunk: a grouped vertex pool, 12 bytes per point, exact size
  agreement on all 115 zones and coherent level-shaped bounding boxes.
- The disc's identity: serial, executable hash, volume timestamp.

**Not yet established:**

- What the three trailing `s16` values on each point mean. They behave like
  adjacency links with -1 as a terminator, but that is inference.
- The internal layout of every other chunk — `MapMod`, `Scene`, `SortData`,
  `PrimaryColl`, `CastList` and the rest.
- The coordinate scale relative to PC Quake II. The extents are consistent with
  a factor of 8, and nothing in the loader depends on it.

The reverse-engineering findings, with per-field confidence markers, live in
[`FORMATS.md`](FORMATS.md); the prioritised gaps are in
[`openquestions.md`](openquestions.md).
