# Q2PSX-PC

A **native PC recreation of Quake II for the PlayStation 1** (Hammerhead's 1999 port).

This is *not* an emulator. There is no MIPS interpreter, no recompiler, and no PSX
hardware simulation in the hot path. The original game's logic is reimplemented as
portable C running natively on the host CPU, and the original PSX GPU/GTE *semantics*
are reproduced exactly so the game still looks like it did on a PlayStation.

You point it at your own Quake II PSX disc — an image file or a real CD in the drive —
and it runs.

```
q2psx --disc "D:\games\Quake II (Europe).cue"
q2psx --disc E:            # physical CD drive
q2psx                      # auto-detect: scans configured search paths and optical drives
```

## Why not just emulate it?

Emulation gives you the original binary running at the original 320x240/30fps with the
original bugs, input latency and load times. A native port gives you the game itself:
arbitrary resolution and framerate, modern input, instant loads, moddability — while
still being able to *choose* to look exactly like the PS1 did.

## Faithfulness

Visual fidelity is a hard requirement, not an afterthought. The PS1 look is not a
post-process filter here; it is a consequence of the pipeline being built the same way
the original was:

- **Exact fixed-point GTE.** Vertex transform runs through a cycle-faithful
  reimplementation of the PlayStation's Geometry Transformation Engine, including its
  1.3.12 / 1.19.12 fixed-point formats, its saturation flags, and its Newton-Raphson
  reciprocal table. Screen coordinates land on integer pixels with no subpixel
  precision — so the characteristic **vertex wobble** is inherent, not simulated.
- **Affine texture mapping.** UVs interpolate linearly in screen space with no
  perspective divide, producing the authentic **texture swim/warp** on large polygons.
- **Ordering-table sorting, no depth buffer.** Primitives are bucketed by Z into an OT
  and drawn back-to-front, reproducing the original's polygon sort artifacts exactly.
- **15-bit RGB555 output** with the PSX's ordered dither matrix, its four
  semi-transparency blend modes, texture pages, CLUTs, texture windows, and the mask bit.
- **Near-plane behaviour** matching the original's clipping (or lack of it).

Every one of these is individually toggleable, so you can also run it perspective-correct
at 4K if you want. The default is faithful.

See [`docs/FIDELITY.md`](docs/FIDELITY.md) for the full rendering conformance spec.

## Disc support

| Source | Status |
|---|---|
| `.cue` + `.bin` (Mode2/2352, multi-track) | supported |
| `.iso` (Mode1/2048) | supported |
| `.img` / `.ccd` / `.sub` | supported |
| `.mds` / `.mdf` | planned |
| `.chd` | planned |
| Physical CD drive (Windows SPTI / Linux SG_IO) | planned |

Regions and revisions are detected per *build*, not per region, because localised
releases move the executable's data tables. See [`docs/FORMATS.md`](docs/FORMATS.md).

## Building

Requires CMake 3.20+ and a C11 compiler.

```bash
cmake -B build -G Ninja
cmake --build build
```

SDL3 is optional. Without it you still get the core libraries and the offline tools;
with it you get the playable client.

## Tools

`q2psx-inspect` is the reverse-engineering harness — it opens a disc image and dumps
the filesystem, build fingerprint, and asset structure without needing a game window.

```bash
build/tools/q2psx-inspect disc  "Quake II (Europe).cue"
build/tools/q2psx-inspect ident "Quake II (Europe).cue"
build/tools/q2psx-inspect dat   Q2DATA/LEVELS/BASE0/COMMON.DAT
```

## Legal

This repository contains **no game assets and no id Software or Hammerhead code**. It is
a clean reimplementation for interoperability and preservation, in the tradition of
ScummVM. You must supply your own legally-obtained Quake II PSX disc. Quake II is a
trademark of id Software LLC.

## Layout

```
src/common/     shared types, fixed-point math, memory, logging
src/disc/       CD image + physical drive access, ISO9660, CD-XA
src/build/      per-build (region/revision) identification and data tables
src/formats/    on-disc asset parsers (.DAT container, zones, sound banks)
src/psx/        exact GTE and GPU primitive model — the fidelity core
src/render/     rasterizer backends that consume PSX primitives
src/audio/      SPU-ADPCM, XA-ADPCM, CD-DA
src/game/       reimplemented game logic
src/platform/   host layer (SDL3)
tools/          offline inspection and extraction utilities
docs/           format specs and architecture
```
