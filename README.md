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

## Status

A level loads from a real disc and renders textured, models included, with the
simulation running on top of it. What is missing is not any one format — it is
the wiring between systems, and the parts of the original's behaviour that have
not been read out of the executable yet.

| Area | State |
|---|---|
| Disc access | cue/bin, iso, bare images; ISO9660; CD-XA Form 1 and 2 |
| Build identification | by executable hash, not by region |
| Level data | container, scene graph, geometry, collision, spawns, lights, triggers |
| Collision | the portal-walking hull trace, sliding, stepping and the entity sweep — transcribed from the executable, 47 of 47 maps walkable |
| Rendering | software rasteriser with the PSX's rules; world and models, textured |
| Surfaces | `Scene.flags08` fully decoded — object binding, hide flag, deferred path and the four draw variants; all four semi-transparency modes from the executable's own two tables; near-quad subdivision, which is what controls the affine texture warp |
| Draw order | `SortData` decoded — a self-describing bit stream, 178,801 node references across 715,260 bytes with none out of range |
| Lighting | the world's baked vertex lighting; the per-entity three-light gather through the GTE's own `NCT`; `SpaceLights` decoded — 37,285 index entries, none out of range; and the lens flares, whose four element tables match the executable's |
| Models | vertices, faces, texturing and animation — all 4,535 clips decode, and a creature's move now SELECTS its clip rather than being looked up as a position on one continuous timeline: a move's frame count times three is exactly some clip's length for 93 of the disc's 97 moves, and where lengths repeat the k-th move takes the k-th clip. That reproduces the correspondence already known from the other direction, and it is the difference between a body that falls over and one that stands back up halfway through dying. And they are now **backface-rejected**, which they never were: the model linker's own NCLIP pair, whose two comparisons turn out to be the world's exchanged — model quads are wound the opposite way round. With no depth buffer an emitted face paints over whatever the sort put behind it, so a model was drawing its own far side through its near side; a Soldier falls from 233 faces to 129, and a BASE1 frame with 22 creatures in it from 5,120 to 3,589 |
| Audio | sound bank and SPU-ADPCM decode, played through the device — the menu's five, the item table's eleven, the player's own footsteps, landing and four pain grunts, and every creature's from its own module's table, the slot index being the sound number (checked against the Soldier, whose numbering was read from its code). The Tank Commander asks for five `tnk_` names that appear in no sound bank on the disc: its voice was cut, not lost in the port; and the music, which is now the map's own — a level record carries a seven-track playlist and a track id names one of the twenty XA streams through the executable's own table, whose stated durations 19 of 20 streams match to the tenth of a second |
| Player movement | the whole of the player's frame read out of `0x8003A1C8` — one clamped-approach primitive doing all acceleration and deceleration, the wish/rotate/ease chain, the posted-impulse jump, the ground projection, fall damage, the rate-based look, swimming and ladders. No friction coefficient, no crouch button and no slope limit exist to find |
| Pad and view | the pad read at `0x80019154`: nine control styles, and the shared tail that turns four configurable pad masks into eleven derived bits — including the one that makes **jump and swim-up the same button**, a tap for one and a hold for the other. Full digital deflection is 127, not 128. And the view is not the aim: `0x80038260` composes three independently decaying kicks — firing over 30 ticks, damage over 150, landing over 90 — plus the strafe lean, and only then is it a camera |
| Environment volumes | crouching, swimming and the no-jump zones are authored per map, not by the pad: they are `UserFuncs` primitives a trigger volume calls. Resolved across the whole disc — 37 `INCROUCH`, 21 `UNDERWATER`, 13 `INWATER`, 52 `DONTJUMP`, and exactly **2** `INLOWCROUCH`, both on `SECURITY`. `DONTJUMP` is what sets the jump gate this project had recorded as never set |
| Level scripting | the trigger graph parses and executes, and a `CALL` now reaches its primitive: rotating brushes turn when a script asks. In the client, playing: walk LAB with the demo pad and a rotator that was at zero is standing turned. Firing every trigger volume on the disc — a player who has walked every map — 26 rotators built, 552 `CALL`s run, 17 rotation steps, 13 turned. What a rotation call's operands mean differs per primitive and is read in one place, beside the builder. A zone file carries an `Events` chunk too and **the engine never loads it**: the zone loader does not look the name up, and the image's only two references to the string are COMMON's loader and its teardown |
| Level flow | **the game plays through**: `LOADMAP` was decoded and nothing acted on it, so a session ended on the map it booted into. All 28 calls on the disc are reachable by a trigger volume and all 28 resolve — and 12 land past zone 0, so the destination zone is the named arrival point's rather than zero. One run now walks Strogg Outpost to Boss2, eleven levels and five units, with the MISSION screen at every boundary, the briefing on arrival, and the inventory carried across — by BOSS1 a weapon picked up two levels earlier is still in hand. The MISSION screen's own counters are the level's: secrets are `INSECRET` calls counted once each, kills come from the creature world, and the unit and the level's display name were sitting unread in each map's `Strings` chunk |
| Level scripting, part two | what a trigger volume ASKS FOR, measured rather than guessed — a histogram of every UserFuncs primitive a player walking the whole disc would run. It found five finished subsystems with no caller. **Doors and lifts**: `q2_movers_build`, `q2_movers_tick` and `q2_mover_trigger` were all dead, so 1,006 `MOVER_A` items stood still and a closed door was a wall — at one LAB frame the before and after differ by 94,068 pixels of 126,976, because before it the player is inside the door looking at its back face. **Scripted ambushes**: 58 of 89 resolvable `CREBATCH` calls name a group claiming no zone — `ShotgunRoom`, `BerserkHide` — and every one of them was standing in the room from level load. **The key gate**: `ONKEYDO` aborts the record it guards and nothing honoured it, so JAIL2 opened five doors without the key. **What the game says**: `STRING`, `SIMPLESOUND` and `HELPCOMPUTER` — *"Locate the Repair Facility and steal the Commander's head you find there."* — none of which had ever appeared |
| Simulation | inventory, combat, creature AI, save games |
| Multiplayer | the whole of QMULTI.C — a per-map LevelBin module, not engine code — and the client now RUNS it: `--dm` starts a match on an arena, the local player spawns at a `MultiSpawn` chosen the way the original chooses one, the clock and the frag limit run on the sim's own step, a death goes through the attribution rule to the scoring, and a finished match raises the engine's own game-state request. A one-minute match ends at 18010 dt with `TIME UP`, request 11 `load MPResults`, `DM SCORES`. The three cut modes are implemented and marked cut. What is absent is the split screen, the other three pads and the scoreboard screen. Driving it also found why no arena drew a HUD: the icon sheet is chosen by session — `qk_menu.lbm`, `qk2_menu.lbm` or `qkm_menu.lbm` — and the client asked every map for the single-player one, which no arena carries. A finished match now shows the SCOREBOARD it asks for: QMRESULT's own words — the six titles, the team line, `ALL PLAYERS PRESS` / `FIRE TO CONTINUE` — composed from the session, with VERSUS printing rounds won rather than frags. The words are the module's; the layout is this port's and is marked so. And the SPLIT SCREEN is live: the layout comes from the session's player count as the original chooses it — full screen, a two-way split on the HORIZONTAL SPLIT axis, or the quad — and every player is placed by the spawn selector against the ones already placed, so four of them spread across the arena. Each viewport draws from its own spawn with its own status bar. And each viewport is its own PLAYER: four sims, four spawns, four pads, four cameras following four eyes — measured on MATRIX5 as four positions on three floors after 400 frames. They SHARE a world: one sim with four players, the world half of a tick — the entity sweep, the effects, the glint, the clock — gated to run once a frame rather than once a player, and every player publishing their position to the same entity world. The combat block is split on the same line: `rules`, `rng`, `projectiles` and `targets` are the world's, while the inventory, weapon, refire gate, view kick and hurt-actor are a player's, swapped in and out around each player's tick. Four players, four inventories, one world — and they can hit each other: the target list holds every creature plus every other player, 3 others with four players and 0 with one, so a bolt that used to pass through everybody now lands — measured, not asserted: a staged pair trades fire and one ends at 68 health of 100, four hits at eight damage — and played through to the end: a kill, a frag to the right player, the frag limit reached, `GAME OVER`, and the scoreboard reading PLAYER 1 1 / PLAYER 2 0 over the arena. A kill also has a KILLER: an actor carries which player it is, the damage function records who last hurt it — the engine's own byte at entity+222 — and a death goes through the attribution rule to the scoring, where lava and the level's own hazards stay nobody's frag |
| Entities | one record with a think pointer, as the original has: spawn, tick, touch, draw |
| Items | every one of the 64 table records — what it looks like, how it behaves, what collecting it does. 1,013 of 1,013 placed items resolve |
| Weapons | all eleven read out of their own fire functions — damage, spread, kick, refire |
| Damage | armour, power armour, knockback, splash, 21 means of death — and what `T_Damage` (0x800627F8) itself does: the surprise bonus that doubles the first shot on a creature which has not noticed you, FL_NO_KNOCKBACK, FL_GODMODE, and the -9999 floor a corpse cannot be driven below. It does **not** post damage to a creature's module, which this project had recorded that it does; the subtraction is in T_Damage and the call that claim rested on passes a different entity |
| Death | a killed creature used to freeze in the pose the shot caught it in — drawn, but never ticked again. It now falls: the module's own death move is found by name (all seven carry one) and installed, and a corpse runs the frame driver without running the AI |
| Creatures | every spawn resolves to its class, model and health — 651 of 651 — and every module names its own animations: 83 of the disc's 97 moves carry a 20-byte `{char[16], u16 first, u16 last}` record, matched to a move by its frame range — and they are now **live in the client**: the map's own `CreAIBin` modules relocated, decoded and bound, the spawn records placed with the right variant and health, the AI running on its 10 Hz clock, and each one drawn into the world's ordering table and shootable. Sight, stepping and the ground probe all run against the zone's own `PrimaryColl`, so a creature sees what a player sees and walks the level rather than through it. And they **fight both ways** — a Soldier takes the player from 100 to below zero, and the player kills it back: `M_CheckAttack` (`0x8005D8C8`, the default every creature gets and no module overrides) decides the attack, and the Soldier's own figures — blaster 5/600, shotgun 2 with 12 pellets at 1000/500, machinegun 2 with 300/500 — are read out of its module and are id's exactly. Their PROJECTILES are wired too: five import slots resolved out of the loader at `0x8007DA00` — hitscan, rocket, rail, bolt and spread — three of them confirming against addresses this project had named for other reasons, and routed to the same fire hook a transcribed creature uses. The four vector-maths slots are deliberately excluded: they are 40 of the disc's 107 call steps and would otherwise fire three times an animation frame. And one of the six untranscribed creatures now SHOOTS: the Tank Commander's attack callback was picking the first of the four moves it installs, which is the animation it plays over a corpse — 22 fire calls, 22 sent, where there had been none. Four of the seven now have their attack callback read out of the module and written — Tank Commander, Gunner, Infantry, Arachner — each one function, the rest still on decoded actions. Five of the seven now have a hand-written callback — Soldier, Tank Commander, Gunner, Infantry, Arachner — plus the Berserk's melee, a coin between its two swings. The Insane has no attack and no melee, and its stand, walk and run are one address: a non-combatant, correctly served by the generic handlers alone |
| Menus | every page, its navigation and its settings — 21 of 21 checked against the executable — **and the front end**: the client boots into the title screen, START and OPTIONS over the QFRONT scene, with SINGLE PLAYER / MULTI PLAYER and PLAYER / SOUND / VIDEO OPTIONS / VIEW CREDITS below them. Those pages are not in the executable at all — they are a static record array inside QFRONT's own `LevelBin`, found by asking what points at each string — drawn with the console's own three faces, its gouraud selection bar and its line-drawn sliders |
| Memory card | all nine front-end screens and the release-gated state machine behind them — 31 of 31 items checked against the executable; the card I/O itself is a host interface, not an invention |
| Saved games | four slots reached through that front end, plus quick save. The container is an ordinary host file — a stated divergence, since a save file's container is invisible where the rendering's limits are the point — but what it holds is the original's state: the level clock every powerup deadline is measured against, the script's event flags, which trigger volumes the player is standing in, which items have been collected, the mover's carried cell and frame delta, and the weapon generator. Chunked, checksummed and fixed-width, so a save written by one build loads under another |
| View weapon | the model in the player's hands: its animation bank, four-state machine and view-space transform — 20 of 20 checks against the executable |
| Screen | display envs, double buffering, the sliced ordering table, all five viewport layouts, the frame lock, and the whole per-viewport draw — clip, clear, damage flash, world gate and the performance meter — 174 of 174 checks against the executable |
| Screen effects | the damage flash, raised from the player's own health and armour and drawn as the viewport's own tile; and the **water effect** — a framebuffer warp that displaces columns and rows of the drawn frame by a travelling cosine, under a blue tint that ramps as you go under. It is what writes the screen shake, which had sat in the view record with no writer at all: the inset is the margin the warp copies from |

| HUD | the overlay — notifications, centre line, crosshair, damage flash — its markup language and its own font; the MISSION screen, whose format strings turn out to be markup the game assembles at run time; and the **status bar**, which this project once proved did not exist. It does: health, ammo and armour in 24x24 numerals beside their icons, drawn by the per-viewport hook at `0x800337D0` and anchored to the viewport's own `view+304`. The earlier negative result came of enumerating format strings, and the bar draws sprites |
| Effects | all five machines, nothing modelled: the fifteen-quad particle groups and their nineteen colour ramps, the one-frame beam pool and its folded hexagonal hull, the debris burst with the hull it slides in and the mover's own gravity and terminal velocity, and the `GlintMod` glint's two draw paths — which the port turns on by READING the level script that raises them, no interpreter needed. Spawned by the simulation, drawn into the same ordering table as the world. The weapon trail is the BFG's: a persistent green beam held on every target the ball can see, refreshed each tick and lingering 45 units after it passes |

Checked against the PAL disc: 164 level files, 461,852 vertices, 274,936 quads,
139,240 collision planes, 94,642 collision portals, 1,723 models, 2,036,080
animation keys, 2,475 sounds, zero failures. The remaining gaps are tracked in
[`docs/openquestions.md`](docs/openquestions.md).

## Building

Requires CMake 3.20+ and a C11 compiler.

```bash
cmake -B build -G Ninja
cmake --build build
```

SDL3 is optional. Without it you still get the core libraries and the offline tools;
with it you get the playable client.

## Running the client without a player

Everything the frame does — the tick, the viewport build, the world draw, the
ordering table, the rasteriser, the HUD — happens before a single SDL call, so the
client can run with no window at all, on a fixed 1/30 s step, driving the pad from a
script and writing the console's own framebuffer out:

```bash
build-client/bin/q2psx --disc "Quake II (Europe).cue" --headless --demo \
                       --frames 120 --shot run.ppm --shot-every 10
```

`--watch` adds a camera that stands in front of the nearest live creature and looks
at it — the inspector's `mob` framing, but of a creature that has thought, turned and
is playing whatever move its AI put it in. It exists because a face count says nothing
about what you can see: with an ordering table and no depth buffer, a monster behind a
wall is emitted and then painted over.

That is not a convenience. `q2psx-inspect` composes its own frames, so it cannot
catch anything that goes wrong *between* the client's systems — a table loaded after
the thing that reads it, a model never bound, a screen never fed. The first run of
this found two: the overlay was initialised from a flag that had not been set yet, so
the client had never drawn a notification or a crosshair; and a session booted into
the free-fly debug camera, so none of the player's frame ran until you pressed a key
nothing told you about.

## Tools

`q2psx-inspect` is the reverse-engineering harness — it opens a disc image and dumps
the filesystem, build fingerprint, and asset structure without needing a game window.
Every format claim in [`docs/FORMATS.md`](docs/FORMATS.md) has a corresponding check
here, so "we understand this format" is something the build can evaluate rather than
an assertion in a document.

```bash
build/bin/q2psx-inspect ident  "Quake II (Europe).cue"
build/bin/q2psx-inspect verify "Quake II (Europe).cue"
build/bin/q2psx-inspect coll   "Quake II (Europe).cue"             # every hull, checked
build/bin/q2psx-inspect walk   "Quake II (Europe).cue" BASE1 0 150 # drop a player in
build/bin/q2psx-inspect pmove  "Quake II (Europe).cue"             # styles, jump, view, volumes
build/bin/q2psx-inspect pmove  "Quake II (Europe).cue" SECURITY 0  # and one map, frame by frame
build/bin/q2psx-inspect audio  "Quake II (Europe).cue"
build/bin/q2psx-inspect render "Quake II (Europe).cue" BASE0 0 out.ppm 0 1024
build/bin/q2psx-inspect model  "Quake II (Europe).cue" BASE1 Soldier 0 0 out.ppm

build/bin/q2psx-inspect hud    "Quake II (Europe).cue" BASE0 hud.ppm
build/bin/q2psx-inspect weapons "Quake II (Europe).cue"            # every weapon, checked
build/bin/q2psx-inspect effects "Quake II (Europe).cue"            # every ramp, beam and laser, checked
build/bin/q2psx-inspect lights  "Quake II (Europe).cue"            # every light, its cell and its flare
build/bin/q2psx-inspect lit     "Quake II (Europe).cue" BASE1      # what lights the player where they start
build/bin/q2psx-inspect items   "Quake II (Europe).cue"            # every item, checked
build/bin/q2psx-inspect menu   "Quake II (Europe).cue"             # every page, checked
build/bin/q2psx-inspect menu   "Quake II (Europe).cue" 26 pause.ppm BASE1  # real font
build/bin/q2psx-inspect text   "Quake II (Europe).cue"             # every map's text, checked
build/bin/q2psx-inspect text   "Quake II (Europe).cue" SECURITY    # and the briefing it would show
build/bin/q2psx-inspect screen "Quake II (Europe).cue"             # every constant, checked
build/bin/q2psx-inspect viewweapon "Quake II (Europe).cue"         # the weapon in hand, checked
build/bin/q2psx-inspect viewweapon "Quake II (Europe).cue" 10 rail.ppm COMMAND 0
build/bin/q2psx-inspect screen "Quake II (Europe).cue" split.ppm quad BASE1 0
build/bin/q2psx-inspect screen "Quake II (Europe).cue" wet.ppm one+water BASE1 0   # underwater
build/bin/q2psx-inspect screen "Quake II (Europe).cue" hit.ppm one+flash BASE1 0   # damage flash
build/bin/q2psx-inspect surfaces "Quake II (Europe).cue"           # flags, blends, draw order
```

`render` needs no window — it writes a PPM. That is how the geometry pipeline was
brought up before the client existed, and it remains the quickest way to check a
change end to end.

It also reads the *code*. The remaining unknowns are questions about the original
executable, so the tool carries a PS-X EXE loader and an R3000A disassembler and
answers them from the disc, with no external disassembler in the loop:

```bash
build/bin/q2psx-inspect exe    "Quake II (Europe).cue"              # map + landmarks
build/bin/q2psx-inspect disasm "Quake II (Europe).cue" 0x80076378   # to the return
build/bin/q2psx-inspect xrefs  "Quake II (Europe).cue" 0x80068A58   # calls, constants, tables
build/bin/q2psx-inspect modstrings "Quake II (Europe).cue" QFRONT   # a level module's own text
```

`exe` re-checks nine addresses that `docs/FORMATS.md` makes claims about, so a
documentation drift against the real executable fails the command.

For the few functions where control flow matters more than field access,
`tools/ghidra/decompile.py` is an optional headless script that decompiles
addresses out of the same segment. Ghidra is not a dependency of anything here;
`q2psx-inspect exe <disc> text.bin` writes the segment with its header stripped
so a raw import at `0x80018000` lines up with every address in the docs.

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
src/menu/       the menu pages and engine, read out of the executable
src/screen/     display environments, viewports, the ordering table's shape
src/audio/      SPU-ADPCM, XA-ADPCM, CD-DA
src/game/       reimplemented game logic
src/platform/   host layer (SDL3)
tools/          offline inspection and extraction utilities
docs/           format specs and architecture
```
