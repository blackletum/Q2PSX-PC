# Open Questions — Quake II PSX (`SLES-01534`) reverse engineering

Prioritised by how much each blocks the native port. **Tier 0** holds the items that have fallen, kept here
with their answers so nothing is silently dropped. Items in **Tier 1** stand between the project and a level
you can walk around in. The tail is cosmetic or archival.

**Where the blocking picture stands.** The world is now correctly textured. All four original Tier 1
blockers fell earlier — the texture codec, the model vertex-index base, the `Events` framing, and the world
scale down to one residue — and the binding that stood between decoded textures and *placed* ones has now
fallen with them: `MapMod.clut` (#1a), the tpage word (#1b) and the UV rotation (#11) were all read out of
the world renderer at `0x80068044` and then checked against the disc.

What made that possible is worth recording, because it changes what is attackable. The harness now carries
its own PS-X EXE loader and R3000A disassembler (`q2psx-inspect exe / disasm / xrefs / access / funcs`), so
questions about the original *code* no longer depend on an external disassembler session. Several items below
were parked on "needs a working disassembler session" and are now simply open work.

Animation followed, and is now complete. There are no per-part matrices to find (#2a): a part's transform is
a keyframed translation and quaternion in `CastList` block C. All 399 articulated models pose, and all 4,535
clips play, including the 3,241 variable-rate ones (#2c) — 2,036,080 keys decoded with none out of range.

Doors and lifts followed (#4a). The per-frame integrator turned out to be a seven-state machine that never
touches geometry — it accumulates a displacement in its runtime object and the zone draw adds it — which is
why an earlier pass, having proved every node in a zone shares one origin, wrongly concluded movers could not
displace anything.

Collision followed, and it turned out to be the case where inference from data had been most misleading. The
plane encoding was never the hard part; what mattered was everything the data could not show — that the
`extra[]` array is a **portal list**, that bit 15 of `firstPlane` marks a node solid, that every query runs in
16-bit wrapping arithmetic, that movement uses **`SecondaryCol`** rather than `PrimaryColl`, and that
`SecondaryCol` is `PrimaryColl` **eroded by the player's own 286-unit half-extent**, so the runtime moves a
point and never touches an entity's bounds. Three separate long-standing puzzles collapse into that last
fact. Details in #5.

Combat followed, and the reason it took three passes is worth recording. Two earlier passes swept the rodata
around the ammo arrays for an eleven-entry table of damage values and found nothing, so the port carried
figures inferred from the PC lineage and said so. The sweep was right and the conclusion was wrong in an
instructive way: **there is no table**. Each weapon has its own fire function, reached through a 12-entry
pointer array at `0x8009D704`, and damage, pellet count, spread, view kick and refire are immediate operands
inside it. Slot 0 of that array is `0x8004EB08` — `jr ra; nop` — which is the cleanest proof yet of the
1-based weapon indexing, because a do-nothing shot only makes sense as "no weapon".

Monster AI followed, and it is the largest single thing to fall so far. The whole engine-side framework —
target acquisition, the five movement verbs, the attack-state machine, eight-way chasing, stair stepping,
and the three-stage pursuit a creature runs when it loses you — is a transcription of PC Quake II 3.20's
monster code, and every constant lands on id's own number once the AI's scale is applied. Two things had
to be got right before any of it made sense, and neither is visible from the data: **the AI's world scale
is 12** where every other subsystem's is 10 (six unrelated constants agree on it), and **the vertical axis
is Y and points down**. Get either wrong and monsters cannot climb a step. `q2psx-inspect ai` checks 76
constants against the executable and all 76 agree. Details in #6 and FORMATS.md §14.

The creature modules came with it. There are only **seven** distinct ones on the disc, and each is decoded
by following its own code rather than searching its image — a route that turned up the entity's `pain` and
`die` hooks and the `skinnum` field along the way. All fifteen module instances decode with zero failures,
and the Soldier, which is most of the monsters in the game, is transcribed in full.

Everything downstream came with it: the damage function at `0x80057D54` and its twenty-one call sites, the
armour table at `0x8009C5EC` whose six values are PC Quake II's exactly, power armour, knockback and the
rocket jump's 3.2x self-multiplier, radius damage and its three read radii, the hitscan and rail paths, and
the projectile entity list at `0x800C91C0` whose per-frame sweep settles what a bolt's `+0x1A` and `+0x52`
mean. Full detail in FORMATS.md §13; `q2psx-inspect weapons` checks the data half against the disc, and
`tests/test_weapon.c` and `tests/test_combat.c` check the behaviour half.

Two negative results from that pass are load-bearing. The damage function **never kills** — there is no die
or pain callback, health simply goes negative and the entity's own think notices later — and for a creature
with an AI brain it does not even subtract health: it posts the amount to the module. So per-creature health,
pain and death behaviour sit behind #6 rather than behind anything in the executable, which raises #6's
priority again.

**Player movement followed, and it is the pass that most changed what the port does rather than what it
knows.** §9.12 had the constants and the vertical integrator and said so; the player's own frame —
`0x8003A1C8`, 926 instructions, plus `0x80039AA4`, `0x8003E110`, `0x80019154` and the three velocity
integrators — had never been read. What came out is not the shape a Quake port is expected to have, and the
port had invented all four of the pieces that were missing.

- **There is no acceleration/friction pair.** One clamped-approach primitive at `0x8006FE3C` is the entire
  movement model, and the same function is the view-height ease, the turn-rate ease and the liquid buoyancy.
  Nothing in the player's call chain multiplies velocity by a coefficient. The port had `vel -= vel/4`.
- **There is no crouch button.** `INCROUCH` and `INLOWCROUCH` are `UserFuncs` primitives (`0x8002E5B4`,
  `0x8002F214`) that a trigger volume runs, so where you can crouch is authored per map. The port had a key
  bound to it. This also explains the `0x600` step-height halving and the 1600 speed cap, which had been
  sitting in §9.12 with nothing to set them.
- **There is no slope limit.** `entity+0x9C` gates the ground flag, the ground projection and the jump, and
  nothing in the image writes it on an entity — the five stores that looked like it belong to the **camera**
  array at `0x800D5C30`, whose `+0x108` holds 4000 where an entity's holds health. It stays zero, so any
  upward-facing surface is ground.
- **Velocity and position are one tick apart**, because the three collision moves consume `entity+0xEC` and
  the integrator that writes `entity+0xEC` sits at the end of the same function.
- **There are two movers, not one.** `0x80045ADC` runs the lift/slide/drop sequence only for an entity that
  is already grounded and not submerged, and a plain slide (`0x80045CA4`) otherwise. The port ran the
  sequence unconditionally, and because its drop is a whole step height against a jump's ~100 units of
  upward delta per tick, **a jump in a real hull produced no motion at all** — while every collision-free
  test still passed, because there is no step sequence without a hull. It is also the only way an entity
  ever *becomes* grounded, since the sequence requires it to be grounded already.

Two rules the port was missing entirely, and both are load-bearing. The **ground projection** at
`0x800459E8` replaces `vel.y` with `-(vel.x*n.x + vel.z*n.z)/n.y` at the top of every mover call for an
entity that was grounded — zero on flat ground, which is why gravity does not accumulate while you stand
still, and the surface-following component on a slope. And nothing clears velocity on contact, so it is that
projection on the *following* tick that produces the velocity change **fall damage** measures at
`0x80039CB4`. Fall damage therefore arrives a tick after the impact; a port that zeroes velocity on landing —
the obvious thing to write — makes every landing painless.

**The movement frame was followed a second time, and the second pass is mostly corrections to the first.**
The first pass read `0x8003A1C8` and got the model right; what it got wrong was three things it had stated
confidently, plus one whole output it had not noticed.

- **The control-style comparison was inverted.** `0x8003A670` is `slti style, 6` branching to the *eased*
  arm when the test fails, so the six mouse and stick styles set the look rate outright and the three
  `STANDARD` ones ease it. Backwards, the analogue styles coast after release and the digital ones snap.
- **Full digital deflection is 127, not 128** — eighteen sites, no exceptions. The wish target is
  `(maxspeed * axis) >> 7`, so the port was running 0.8% fast on every style.
- **The view angles are not the aim angles.** `0x80038260` composes three independently decaying kicks on
  top of them — firing over 30 ticks, damage over 150, landing over 90 — and `0x8004F40C` is where that
  becomes the camera. The port had the fall kick as a field nothing read, the weapon kick as a field nothing
  read, and a roll it computed and threw away. All three now reach the screen.
- **`entity+0x10C` bit 12 is the fly cheat, not a movement-basis switch.** It is the *first* test in the
  vertical integrator, and it also blocks the jump, the view recentre and the water-exit jump. The port
  steered by the view basis while still falling.

Two things were added rather than corrected. `0x80019154` — the pad, nine control styles, and the shared tail
that turns four configurable pad masks into eleven derived bits — had never been read, so the port invented
its own mapping and left `look_scheme` with nothing to set it. The load-bearing find in there is that **jump
and swim-up are the same button**: bit 22 is its press edge and bit 21 is it held. And the **view recentre**
at `0x8003A780` is a chord, not the `AUTOCENTRE` setting: hold both look buttons for a second tick and the
pitch walks itself level.

**One of §9.12's negative results is closed, and it closed the way the multiplayer search did — by being
pointed at the wrong binary.** `entity+0x98 & 0x20000`, the gate in front of the jump, is set by the
`DONTJUMP` `UserFuncs` primitive, whose body is one OR into that word exactly like `INCROUCH`'s. Nothing in
the *executable* sets it, which is what the original search found and reported correctly — the setter is in
the level data. **52 volumes across 15 maps** call it. Resolving a volume's record to its primitive also
gives the port the whole environment dispatcher: 37 `INCROUCH` volumes, 21 `UNDERWATER`, 13 `INWATER`, and
exactly **2** `INLOWCROUCH` — both on `SECURITY`, which means the 1600 speed cap and the halved step height
are reachable in two places in the entire game. `q2psx-inspect pmove` runs the census and
`tests/test_pad.c` checks the behaviour half.

One correction to this document's own §9.12: the liquid `0x08` arm's two branches were the wrong way round.
`0x80046050` branches on `vel.y < 1024` **into** the `-= dt*24` push, not into the ease. The net behaviour is
still a slow sink because the push loses to gravity, but implemented inverted it oscillates instead of
settling. Full detail in FORMATS.md §9.12; `tests/test_sim.c` checks 82 behaviours including both directions
of that convergence.

What is left is correctness and completeness elsewhere: area connectivity (#8) is undecoded, and the
relocatable module ABI (#6) still hides the level scripting and every creature's own attack figures.

**Lighting followed, and the interesting part is what turned out not to exist.** #9 asked how `SpaceLights`
is partitioned and #26 asked what a light's five `type` values do; both had been attacked from the data and
both had the same shape of answer, which is that the missing information was in a structure nobody was
looking at. `SpaceLights` is partitioned by the **secondary collision node**, through a halfword at +30 of
the very field #23 had recorded as having no reader — it does not; the reader is two bytes further in. And
the `type` byte is not a style at all: it is three bit-fields, and bits 3–5 select one of four **lens flare**
element lists in the executable, whose ghosts are positioned along the line from the screen centre to the
light. The long-running search for a flare texture was looking for something that does not exist: every
flare on the disc is untextured Gouraud polygons and lines.

Two more things fell out with them. The world is lit **entirely at build time** — the per-corner RGB tables
in `MapMod` are the PlayStation's lightmap and nothing at runtime modulates them — so `Lights` exists only
to shade *models* and to draw flares, at most three lights per entity because the GTE's light matrix has
three rows. And #2d's two caller matrices are now read: they differ by exactly the view rotation, because
the light directions stay in world space while positions go to the camera. Full detail in FORMATS.md §17;
`q2psx-inspect lights` and `lit` check the disc half and `tests/test_light.c` the behaviour half.

Multiplayer followed, and it is the clearest case yet of a search failing because it was pointed at the wrong
binary. Two passes swept the executable for deathmatch scoring and found nothing, correctly: the executable has
the **hook** and the maps have the **rules**. The whole multiplayer game is a 5,608-byte `LevelBin` module —
`QMULTI.C`, which names itself in a debug string — that is byte-identical on all thirteen arenas, and exactly
those thirteen maps are the ones whose `StartPos` carries `MultiSpawn` points. Six modes, three of them
selectable; the frag/time/round limits, the farthest-point spawn rule, the round machinery and the two exit
paths (`MPResults` or replay the round) are all read out of it. Full detail in FORMATS.md §16; `q2psx-inspect
multi` checks the data half against the disc and `tests/test_multiplayer.c` checks the behaviour half. Reading
it needed `levdisasm`, which is `moddisasm` pointed at `LevelBin` — so #6's ABI is now attackable from the
*level* side as well as the creature side.

Full structural detail, evidence and confidence markers live in [`FORMATS.md`](./FORMATS.md).

Legend: `[ ]` open · `[~]` partially resolved · `[x]` resolved (move the item, keep the answer)

---

## Tier 0 — Resolved (kept here with their answers)

- [x] **0. The multiplayer runtime. — SOLVED, and it was never in the executable.**
      The whole deathmatch game is `QMULTI.C`, a **5,608-byte `LevelBin` module byte-identical on all thirteen
      arenas** (MATRIX1…9, THEVAT, TIMS, PODCITY, FRAGTOWE) and present on no other map — a partition that
      matches, exactly, the set of maps whose `StartPos` names `MultiSpawn0`…`MultiSpawn7`. The executable
      contributes one call: `0x800396AC` ends with `(*(0x800B2F58))->[4](killer, victim)`, and `0x800B2F58` is
      the map's own module. Session state lives in the shared block at `0x800B2FE4` beside the GAME VARIABLES
      the menu already owned — time limit `+874` in **minutes**, frag limit `+876`, round limit `+878`, mode
      `+880`, players `+882`, `frags[4]` `+1204`, `team_frags[4]` `+1212`, `kills[4][4]` `+1220` — and
      **nothing in the executable writes any of it**, which is why an EXE-only search kept failing;
      `QFRONT`'s own `LevelBin` does.
      Six modes exist and their numbering is fixed twice over, by QMULTI's branches and by `QMRESULT`'s six
      scoreboard titles: 0 DEATHMATCH, 1 TEAM DEATHMATCH, 2 CTF, 3 TAG, 4 TEAM TAG, 5 VERSUS. **Three are
      unreachable**: the selector at `0x8010459C` writes only 0, 1 and 5; QMULTI's init zeroes the team/flag
      enable and then branches on it, so its flag setup is dead code; and no map on the disc names a flag
      spawn. Exit is one enum — a round that ended replays (engine state 19), a match that ended goes to
      `MPResults` (state 11), after a 450-unit banner.
      Two original behaviours are kept rather than corrected: the kill matrix is incremented on the
      **diagonal** (`victim*8 + victim*2`) and is read by nothing on the disc, and a team kill is charged
      **twice**, personally and to the team. Implemented in `src/game/multiplayer.[ch]`, checked by
      `q2psx-inspect multi` and `tests/test_multiplayer.c`, full detail in FORMATS.md §16.
      Still open, and deliberately not guessed at: the flag entity itself, so CTF/TAG/TEAM TAG scoring; the
      per-player 34-byte configuration record's two colour fields beyond "one is chosen when the mode is 1";
      and `multipics.lbm` / `multipic2.lbm`, which are still undecoded (#39).
- [x] **1. `SNDVRAM.DAT` section A pixel compression codec. — SOLVED.**
      Byte-oriented **PackBits with a destination-bounded loop**: `c < 0x80` copies `c+1` literals,
      `c >= 0x80` emits `257 - c` copies of the next byte, looping while output remains. The target is
      `width * height` bytes, and **texture-page records ignore their stored dimensions and are forced to
      128 × 256** (`0x80068B74` / `0x80068B7C`). `width` is bytes-per-row; the VRAM rect is `width>>1`
      halfwords wide. All **553/553** records on the disc decode to exactly the expected length — zero
      overrun, zero underrun — and no neighbouring codec or output-target variant comes close (best wrong
      alternative 152/553). `0x80` means 129 repeats, **not** the Apple PackBits no-op; it occurs 0 times in
      2,965,034 control bytes, so this is unfalsifiable from data but unambiguous in code.
      Implemented in `src/formats/vram.[ch]`. Full detail in FORMATS.md §4.1.
- [x] **2. `CastList` face vertex-index base. — RESOLVED.**
      `q2p_part`'s byte at `+2`, previously documented as `flags`, is the part's **base index into a shared
      per-model scratch window**, and should be called `vertBase`. Parts write their transformed vertices
      into the window at `vertBase`; faces index the window, not the vertex array. Verified over all 1,723
      models / 553,160 indices: **0** out of range, **0** causality violations, **100.0000 %** storage
      coverage on 1,723/1,723 models, and +0.7117 mean normal agreement / 98.13 % sign agreement on
      articulated intra-part faces against +0.0062 at `vertBase+1`. Reversing part order produces exactly
      18,995 reads of never-written slots, so forward file order is required.
      Also settled in the same pass: the vertex normal component order is **`z, x, y`**, not `x, y, z`
      (+0.7526 vs −0.0073 mean dot) — lighting was noise before this.
      Implemented in `src/formats/model.[ch]`. See #2a for what remains.
- [x] **3. World coordinate scale. — S = 10, INFERRED but tightly bounded.**
      The exporter's output lattice is generated by `10 · 2^k`. Enrichment `P(v ≡ 0 mod S) · S` over
      1,385,556 axis values is at background (≈1.0) for S = 6, 7, 9, 11, 13, 18 and 1.38 for S = 12 — fully
      explained as `P(mod 4)·P(mod 3)`. **The viable set is exactly `{5, 10, 20}`**, so `216 = 18·12` and
      `9000 = 800·11.25` are provably coincidences, which in turn means the physics constants were retuned
      and must be read from the PSX EXE rather than derived from PC values.
      Quantisation is CONFIRMED as `(int32_t)(true * S)` **truncated toward zero**, not `floor()` — this
      answers old sub-question 3a. Constants in `src/formats/worldscale.h`. See #3a for the residue.
- [x] **4. `Events` operand stream. — FRAMING SOLVED.**
      The record body is a **nested TLV item list**; the byte at `+2` is the **item count** (it is the
      interpreter's loop bound at `0x800279F8`) and the byte at `+3` is a **mutable flags byte** the runtime
      writes back (latch at `0x800279A8`, opcodes `0x14`/`0x15` clearing and setting bit 7). The old
      `sub`/`cls` reading is refuted. Exact on **4,179/4,179 records / 6,646 items** across all 164
      containers. Opcode dispatch (table `0x800ABD48`), the 43-entry `UserFuncs` binding table at
      `0x8009B6F0`, and one argument length per primitive (38 primitives, 3,760 items, zero exceptions) are
      all CONFIRMED.
      **Load-bearing caveat: the chunk is SELF-MODIFYING.** A load-time pre-pass at `0x80026DC0`, with its
      own table at `0x800ABCF8`, rewrites the `int16_t` slots inside items — on disc they are zone-local
      `Scene` node indices (up to 332), after load they are runtime object indices into a **48-entry** array.
      A port that feeds disc values to the exec handlers overruns that array by ~26 KB.
      Implemented in `src/formats/events.[ch]`. See #4a for what remains.
- [x] **1a. `MapMod.clut` → VRAM CLUT binding. — SOLVED.**
      The field is never read as a halfword. The world renderer at `0x80068044` takes the **high byte** as an
      index into the CLUT-id table (`0x80068288`, table pointer at `0x800B2EDC`) and the **low two bits** as
      the semi-transparency selector, choosing primitive code `0x3E` over `0x3C` (`0x800682A8`). The previous
      refutation tested the whole `uint16_t` against the id table — the wrong quantity. The other six bits of
      the low byte are build-time residue the engine never reads, set on **251,872 of 274,936** polygons,
      which is precisely why the halfword reading looked unsalvageable.
      Checked against the disc by `q2psx-inspect cluts`: the index is in range on **49 of 49** maps (max
      16…85 against CLUT counts of 36…259). The port's existing assumption turns out to have been right;
      it is now evidence rather than a guess.
- [x] **1c. `MapMod.clut >> 8 == 0`. — SOLVED. It is "do not draw", and reading it as a palette blacked
      out doorways all over the game.**
      1a settled that the high byte indexes the map's `clut4[]` array and left it there. The value **0** is
      not a member of that array's usable range: the used set is exactly `{0} ∪ [16, count_clut4_a)` on every
      map, real surface starts at 16 because the first sixteen entries are the reserved all-`0x8000` blocks
      (#1's correction), and `0x8000` is **opaque black**. So the 11,255 polygons that bind index 0 paint out
      whatever they stand in front of. An earlier pass looked at the same numbers and concluded "index 0 is
      real and must not be treated as no palette, it samples genuine texture content" — true of the *tile*,
      false of the *palette*, and the port drew black planes across doorways for it.
      What settles it is crossing those polygons with the draw order, which needed #7 to be readable first.
      Every one of the 11,255 lives in a node where **every** polygon binds index 0 — 1,749 such nodes, zero
      mixed — and across all 115 zones, 8,968 streams and 178,801 node references, **not one stream names one
      of those nodes**, against 98.0 % of every other node being named. They are the build tool's sealing
      planes: flat quads across openings, degenerate on one axis in 1,606 of 1,749 cases. Nothing else marks
      them — `Scene.flags08` is the ordinary `0x0000` on 1,739 of them.
      `q2psx-inspect surfaces` prints both halves of that comparison and fails if either ever changes.
      Acted on in `q2_mapmod_rec_is_sealing` (`src/formats/scene.[ch]`) and gated in `world.c` only on the
      index-order walk, since a supplied stream has already made the choice. FORMATS.md §3.1.4.
- [x] **1b. Where the GPU tpage word is assembled. — SOLVED. Texture pages are 4bpp, CONFIRMED.**
      Nowhere per-polygon: `0x80077FE8` precomputes twenty words at `0x800DDD3C` via
      `GetTPage(0, 0, tbl_X[i], tbl_Y[i])`, and that literal `0` is the colour-mode argument. `MapMod.tpage`
      indexes the table. The earlier search failed because it looked for the bit arithmetic inline; it lives
      inside libgpu's `GetTPage` at `0x8008A1C8`, once.
- [x] **11. `MapMod` `Poly.uvIdxFlags` bits 6–7. — SOLVED. A UV rotation, not render flags.**
      Vertex `j` takes `uv[(3 - f - j) & 3]`. Read from `0x80068118…0x800681D8`, then confirmed against the
      disc by a test the disassembly cannot have arranged: over the 31,931 quads that carry a rotation, the
      engine's rule holds texel-scale anisotropy to a mean of **1.33** against **7.61** for both rivals.
- [x] **12a. Does the world backface-cull? — YES, and the port was not doing it.**
      Never posed as a question, which is how it survived: the three quad linkers were described from
      `SETWIBBLE`'s point of view, as differing in *when they subdivide*, and "full culling" in that
      description was read as frustum work. It is an **NCLIP pair**, byte-identical in all three
      (`0x800AF8A8`/`0x800AF8C8`, `0x800AFB08`/`0x800AFB28`, `0x800AFD6C`/`0x800AFD8C`), so culling is not a
      per-variant policy at all — it is what linking a quad means:
      `SXY0,SXY1,SXY2 = v0,v1,v3; NCLIP; MAC0 > 0` draws, otherwise `SXY0 = v2; NCLIP; MAC0 >= 0` drops.
      `v1`–`v3` is a diagonal of the perimeter quad, so the two tests are its two halves and either one
      facing the camera is enough — which is what keeps a quad folded through the near plane from vanishing.
      Omitting it is not a performance question: sealing brushes' outward faces land in front of the rooms
      they enclose. In `world.c`, counted as `quads_rejected_back`.
- [x] **2a. `CastList` per-part transform matrices. — SOLVED, and there are no matrices.**
      A part's transform is a keyframed **translation and quaternion**, packed two words per part per frame
      in **block C**, which is the animation bank. The pose selector at `0x8006B924` walks the clip chain
      subtracting durations from the entity's tick counter until a clip contains the current time, then
      unpacks each part's key: three signed fields for the translation (bits 0–10 ×2, 11–20 ×4, 21–31 ×2)
      and three unsigned **half** angles for the rotation, which `0x800699E8` turns into a quaternion with
      the textbook Euler products against the `{sin, cos}` table at `0x800A5430`.
      Searching block C for matrix elements found nothing because none are stored — it holds packed angles.
      Verified by `q2psx-inspect anims` over the whole disc: **1,723 of 1,723** models walk, 4,535 clips,
      123,704 frames, **zero** keys escaping their block, all **399** articulated models animated, and the
      decoded quaternions are unit to **0.27 %** (|q|² 4087…4098 against 4096) — which no wrong angle scaling
      survives. The `12 + 8*numParts` size law that an earlier pass measured without explanation now falls
      out of the layout and holds on **1,265 of 1,265** single-frame clips.
      Implemented in `src/formats/model.[ch]` as `q2_model_anim_get` / `q2_model_pose_at`.
- [x] **2c. The variable-rate clip path. — IMPLEMENTED.** Clips with `flags & 1` — 3,241 of 4,535, so the
      common case — put their four-byte entries **per part** rather than per frame, each naming a stream of
      4-bit frame durations and that part's own key list, with keys interpolated in between (`0x8006B4DC`,
      and the quaternion slerp at `0x80069C64`). All 4,535 clips now decode: **2,036,080 keys**, none
      escaping its block. Three details the disc had to teach: a zero duration nibble is legal and advances
      the key index without consuming a frame; the tick clamp is to the *start* of the last frame, not the
      clip end; and the last key of a model's last clip has no successor inside block C, which the original
      reads anyway. The port's computed inverse cosine matches the original's 4096-entry table on 4,094
      entries and is one unit out on the other two.
- [x] **39. The weapon stats table. — SOLVED, and there is no table.**
      Two passes swept the rodata around the ammo arrays for eleven damage values and found nothing. The
      sweep was right: every weapon has its **own fire function**, reached through a 12-entry pointer array
      at `0x8009D704`, and damage, pellets, spread, view kick and refire are immediates inside it. Slot 0 is
      `0x8004EB08` — `jr ra; nop` — a shot that does nothing, which only makes sense as "no weapon" and is
      the cleanest proof of the 1-based indexing.
      Read out with them: the damage function `0x80057D54` and its 21 means of death; the armour table at
      `0x8009C5EC`, whose six values are PC Quake II's exactly and whose rounding bias differs between
      single player (4095) and deathmatch (2048); power armour at `0x80057A9C`; knockback, including the
      3.2x self-multiplier that is the rocket jump and its −3072 single-player ceiling; radius damage at
      `0x80050810` with grenade 1000 / rocket 1300 / BFG 1300; the hitscan path `0x8004874C` and the rail
      `0x8004917C`; and the projectile entity list at `0x800C91C0`, whose per-frame sweep at `0x80047C6C`
      proves a bolt's `+0x1A` is a lifetime and `+0x52` its velocity.
      Two negatives worth keeping: the damage function **never kills** — no die or pain callback exists —
      and for a creature with an AI brain it does not even subtract health, it posts to the module.
      Implemented in `src/game/weapon.[ch]`, `combat.[ch]`, `projectile.[ch]` and
      `src/build/weapontables.[ch]`. `q2psx-inspect weapons` reads the data half back off the disc and
      compares it: **11 weapons, 3 armour classes, 22 sounds, 0 mismatches**. Full detail in FORMATS.md §13.
  - [ ] 39a. Residue: the BFG blast's flight speed. `0x8004BE04` works out its own direction and the port
        has not followed it; the 2400 at `0x8004BF20` goes to entity `+0xF4`, which the player spawn also
        writes (as 1), so it is not a speed. The port flies it at the rocket's and names the substitution
        `Q2_BFG_SPEED_UNREAD` so it is visible at every use.
  - [ ] 39b. Residue: the grenade launcher's fuse. Its spawner stores 380 into the entity at `+0x4C`
        alongside a second timer and which of the two is the fuse was not established. The hand grenade's
        1650 IS read — it is argument 3 at `0x8004EC3C`.
  - [ ] 39c. Residue: the chaingun's spin state. The bullet count per shot comes from the view model's
        runtime object at `+0x2C` (`0x8004CAE0`), and the view model is not reconstructed, so the port takes
        the count from its caller and defaults to one.
- [x] **Viewport handling and drawing. — SOLVED, and one silent inversion came out of it.**
      The screen's constants were already read; what was not read was the *drawing*. Now transcribed from
      `0x800780C0`, `0x80076A74`, `0x80076764`, `0x80077230` and `0x80076E88`, and implemented in
      `src/screen/screen.[ch]`:
      the per-frame build's clear is a **three-way** decision — `0x800B2D94` suppresses every clear,
      `gp+18712` arms one full-screen env at OT[1], and neither leaves the per-viewport clears standing —
      with the loop bound in both armed arms being the **live** viewport count, so a three-player split's
      fourth quadrant is left alone;
      a viewport publishes its state in globals rather than passing it (the clip extent at `0x800B2C20`,
      already shrunk by the shake, the slice base at `0x800B2D60`, the index at `0x800B2C1C`);
      **`view+144` bit 0 gates the world draw** and is set with bits 1-2 when a player's camera is installed;
      **`view+264 / 4` is an entity cut-off**, compared against a distance inside the model draw at
      `0x800698E0`, so entities stop at a quarter of the world's distance;
      the **damage flash** belongs to the viewport, not the overlay — a full-viewport semi-transparent `TILE`
      at the slice's frontmost bucket, with **four** colour modes and a countdown that runs once per drawn
      frame — and the port's copy has moved out of `src/game/hud.c` accordingly;
      the **overlay camera** publishes itself as viewport 0, never shakes, and does not recompute the entity
      cut-off; and the **performance meter** is nine two-pixel bars at OT[52], six of whose accumulators reset
      as they are drawn.
      **The inversion.** `ClearOTag` at `0x800837C0` builds a *forward* chain and `DrawOTag` is handed entry
      0, so a higher bucket draws later and is therefore nearer — which the background env at OT[1], the
      per-viewport env at slice bucket 1 and the flash at slice bucket 50 all independently confirm. The port
      was linking primitives by depth into *ascending* buckets and then walking forward, i.e. drawing far
      geometry over near. `psx_ot_add` now takes a depth and inverts it; `psx_ot_add_bucket` names a bucket.
      Also read out with them, and previously carried as guesses: the five-halfword table at `0x800B36D8`
      (`{32, 0, 32, 64, 96}`) is the **semi-transparency field of a tpage word**, indexed by a face's texture
      byte shifted right by five, so an unset selector is B+F rather than the half blend; and `InitGeom`
      leaves **ZSF3 = 341, ZSF4 = 256**, which no code overrides.
  - [ ] Residue: **what `view+308` points at.** Every layout stores its own argument there as a word and the
        callers pass function addresses (`0x800337D0` for one player, `0x80033D30` / `0x80034288` for the two
        splits). It is a per-view callback; what invokes it was not followed. The port expresses it as the
        `view` hook in `q2_screen_hooks`.
  - [x] Residue: **`view+156` is not a depth scale** — **ANSWERED, and it is angular.**
        The reader is `0x80038374`, inside `0x80038260`, which is the **view-angle composer**: `view+154`,
        `+156`/`+158` and `+160`…`+164` are three view KICKS, each with its own deadline and decay period —
        90 ticks for the landing kick, 150 for the damage kick, 30 for the firing kick — and each is scaled
        by `((deadline − now) << 12) / period` and added to the pitch, yaw and roll before `0x8004F40C`
        turns the result into the camera. So the "running three-halfword total" is an angle triple, not a
        wobble, and the previous reading had the *arithmetic* right and the *meaning* unlocated. The port
        applies all three now: FORMATS.md §9.12.11a, `q2_sim_view_angles`, `tests/test_pad.c`.
  - [ ] Residue: **`0x800D8D78`**, the table the viewport draw stamps twice through `0x800689F4(p, 1)` with
        the tick at `0x800B2DE4`. Eleven other materialisations across the world and model draw; looks like a
        per-viewport freshness stamp, unidentified, not reproduced.

- [~] **2d. What the two caller matrices hold when a model is drawn. — ANSWERED; one residue left.**
      The caller is the entity draw at `0x8006B924`, and it fills a draw context whose fields `0x800B1F90`
      then consumes at fixed offsets:

      | context | holds | becomes |
      |---|---|---|
      | `+0x60` | the entity's own 3×3 at entity `+0x2C0`, copied halfword by halfword | the **light** matrix, after the part's quaternion is composed into it (`0x800B21B8`…`0x800B223C`) |
      | `+0x74` | three light DIRECTIONS the gather at `0x8006AFE8` leaves there | the rows of that light matrix |
      | `+0x8C` | `view × entity`, built by `MulMatrix(0x800B28B8, entity+0x2C0)` at `0x8006BB94` | the **rotation** matrix, again composed with the part's (`0x800B2240`…) |

      So the two differ by exactly the **view rotation**, and that asymmetry is correct rather than an
      oversight: the light directions are in world space, so rotating them into the camera as well would
      make a model's shading swing as the player turned. The uniform scale rides in the same entity matrix
      (`0x8006B298` scales its rows by `(entity+0xFC · entity+0xFE) >> 11`), which is why a materialising
      item's parts stay attached as it grows.
      Implemented: `src/game/modeldraw.c` composes both the same way, and shades through `NCS`.
      **Residue:** posing an articulated model flat still does not restore agreement between its extents and
      the header's `ext2`/`ext3` (4/399 and 15/399 against 0/399 and 5/399 unposed), while static models are
      unaffected. Now that the matrices are known to carry no per-model transform beyond the scale, the
      remaining explanation is that `ext2`/`ext3` are authored bounds rather than measured ones.

- [x] **HUD. — SOLVED, and the answer is that there is no status bar.**
      The game shows no health, ammo or armour readout. That was established by enumeration, not by looking:
      every format string the executable hands to its text layer was resolved back through its `lui`/`addiu`
      pair — all 30 call sites of `0x80043518`, all 3 of `0x800434B8`, and every `sprintf` in the image — and
      none of them formats a player statistic. The only two readers of the font table at `0x8009D554` are the
      markup interpreter and the menu's glyph loop, so no separate digit drawer exists either.
      What the overlay *is*: a markup language (`0x80042328`) drawing 8x8 sprites out of `chars.lbm`, a
      four-slot notification ring retiring one line every 60 ticks, a centred line with a backdrop that fades
      by shrinking, a crosshair, and a damage flash whose colour says whether armour or flesh took the hit.
      The atlas lands at VRAM (0, 384) — slot 15 with a v origin of 128, read out of `0x8003FEA4` →
      `0x8006901C` → `0x800691A8` — and its palettes are in the executable's own bank at `0x800A2FEC`, not on
      the disc. Full detail in FORMATS.md §11; implemented in `src/build/hudtables.[ch]` and
      `src/game/hud.[ch]`, checked by `q2psx-inspect hud` and `tests/test_hud.c`.
      This also settles the menu's open item: the menu font is the same atlas plus `frontend.lbm`.

---

## Tier 1 — Blocking: cannot render or load a level

The residues of the resolved blockers keep their parents' numbers.

- [~] **2b. Cross-part index resolution for 21,217 faces (15.3 %), all inside the articulated
      models.** Last-writer-wins and a rival arithmetic rule both resolve 100 % of indices in range and
      differ on 18,283 articulated faces; geometry alone cannot separate them. Last-writer-wins wins on
      coverage (100.0000 % / 1,723 models vs 99.6131 % / 1,462) and is what the module implements, behind one
      function so it can be flipped. **It is now decidable**: the two rules transform those borrowed vertices
      with different parts' poses, and the poses are decoded, so posing a model both ways and comparing the
      seam quads against the stored vertex normals settles it.
- [~] **3a. The PSX-texel to PC-texel ratio — the last step to CONFIRMED for S = 10.**
      The texture measurement fixes 10 world units per *PSX* texel, which is one equation in two unknowns.
      S = 10 requires PSX 64×64 tiles to be 1:1 with PC Quake II 64×64 textures at scale 1.0; a 2:1
      downsample of 128×128 sources — standard PSX practice — gives S = 5. Nothing on this disc distinguishes
      them. Two supporting arguments were refuted: the 1280 secondary texture span is a per-surface artistic
      choice (**729 of 2,111 texture keys, 34.5 %, appear at two or more canonical world spans inside one
      map**), and the 286-unit cube "agreeing with Quake II's player to 2 %" is numerology (the eye sits 576
      above the feet on a 572-tall body, ratio 1.007, against PC's 46/56 = 0.82).
      *Attack:* now cheap, because #1 is solved — decompress one texture payload, count its pixels, compare
      against the PC Quake II `.wal` it derives from.
  - [ ] 3b. Residue: `|v| mod 10 == 1` (8.89 %) and `== 8` (10.43 %) are 3–4× the off-lattice background,
        and a single truncation can never *increase* a magnitude. Either the exporter dithered ±1 on top of
        the truncation, or more vertices are genuinely off-lattice than the residue-2…7 background implies.
        Does not affect the decode rule; it means the grid-snapping model is incomplete.
- [x] **4a. `Events` `fnB` motion integrators and the 92-byte runtime object. — SOLVED.**
      The per-frame handler every mover installs is `0x80025658`, reached from the sweep at `0x8002DC04`
      that walks 48 objects of 92 bytes at `0x800D6BB0`. It is a seven-state machine — at rest, opening,
      arrived, closing, blocked, delay, held open — driving `pos += speed * dt` along one axis, clamped to a
      signed target, with the delta applied down the `+0x30` chain, an obstruction veto at `0x80051EC0`, a
      16-tick blocked retry, and positional sounds at the node's bounding-box centre.
      **It never touches geometry.** The displacement accumulates in the object at `+0x12`, and the zone
      draw at `0x800678EC` adds it to the node's camera-space position as it draws. That is the piece the
      earlier pass was missing when it concluded from the shared node origin that movers cannot displace
      geometry: they displace it at draw time. Now wired into `q2_world_build_ot`, and visible —
      `bmodel <map> <zone> <group> out.ppm 1` opens a BASE1 lift and it moves 1,151 units up the Y axis.
      The full field map is in FORMATS.md §2.9.1. It corroborates the port's mover model, which was derived
      independently from the on-disc payloads: same states, same single axis, same absolute-valued speed.
  - [ ] 4b. Which physical chunk backs `gp+0x174` versus `gp+0x178`. Both are set from loaded chunk pointers
        (`0x8007AD54`, `0x8007C234`) and the pre-pass runs immediately after `gp+0x178` is set
        (`0x8007C278`). Given `COMMON.Events == ZONE0.Events` in 49/49 and only the `Scene`-index slots
        differing per zone, the natural reading is map-wide copy vs zone copy — but it was not established.
  - [ ] 4c. The `STRING` operand string namespace (165/363 resolve against the map's own `Strings`) and
        `MISEVENT` (**0/93** — entirely unlocated, not partially located). And record `flags` bits 3–5: the
        loader default `if ((f & 0x28) == 0) f |= 0x10` confirms `0x08`/`0x10`/`0x20` are alternatives with
        `0x10` as default, but the three categories are unidentified.
- [x] **5. Collision plane point encoding. — SOLVED, and the whole collision model with it.**
      The encoding was never the interesting part. It is an unsigned halfword offset from the owning node's
      `bboxMin`, read that way at four separate sites (`0x800441DC`, `0x800446C0`, `0x80044384`,
      `0x80043FB0`), and the "95.6 % / 99.85 % confirmed" figures were measuring how well a guessed reading
      matched a geometric expectation rather than reading the code.
      What the code says, in full, is in FORMATS.md §3.4. The load-bearing parts:
      **`firstPlane` bit 15 is a SOLID flag** (`bltz` at `0x80044190` and `0x800442BC`, every reader then
      masks `0x7FFF`); **`extra[]` is the PORTAL LIST**, `(planeInThisNode << 11) | neighbourNode` plus the
      matching plane index in the neighbour; **`d`'s low byte is the node's contents id**; and every query
      runs in **16-bit wrapping arithmetic** relative to the node's minimum corner, which a 32-bit port
      silently diverges from on a long move.
      **The movement hull is `SecondaryCol`, not `PrimaryColl`** — the mover at `0x80045144` loads the
      context the loader filled from it — and `SecondaryCol` is `PrimaryColl` **eroded by 286 on every
      axis**, i.e. the configuration-space hull of the player's own cube. That is measured, not asserted:
      over 5,275 axis probes across all 115 zones the free space differs by exactly 286 in 37 % and by
      286 ± 2 in 52 %. It explains the three things that never fitted — why Secondary has *fewer* nodes on 9
      of 115 zones, why the player path contains no bounds access, and whether 286 is the real hull (it is).
      Convexity is never assumed by the engine, so the 148 non-convex nodes are harmless.
      `q2psx-inspect coll` checks every invariant the transcribed code depends on across all 230 chunks:
      **0** out-of-range link nodes, planes or back-planes of 94,642 links; **0** nodes over the 32-plane
      limit and **0** hulls over the 2048-node limit that the 5/11-bit packing imposes — both holding exactly
      at their boundary; 94,620 reciprocal portal pairs; **0** traces leaving the hull. `q2psx-inspect walk`
      then drops a player into **47 of 47** maps: every spawn lands in a cell, every player grounds, none
      ever leaves the hull.
      Implemented in `src/formats/collision.[ch]` and `src/game/trace.[ch]`.
- [ ] **6. `LevelBin` / `CreAIBin` module ABI and the `Rel` fixup encoding. — HALF FALLEN.**
      Confirmed MIPS R3000; every fixup is a valid in-`Bin` offset, but only 31 % are 4-aligned, so it is not
      a plain word-address list. The fixup *encoding* is still open. What is no longer open is the **ABI**,
      and with it the whole engine side of monster AI.

      **The interface record is read.** `0x8007D990` builds a **304-byte, version 1** import table at the
      module's base and fills sixty function pointers into it. That table is the complete list of services a
      creature module can reach, and it names them: the five shared AI verbs at +0x54…+0x64, the vector
      helpers at +0xB8…+0xCC, `visible` at +0xE4, the melee attack `fire_hit` at +0xEC, the damage function
      at +0xF0, and — the one that unlocks the rest — a **class-method setter** at +0x118. Full table in
      FORMATS.md §14.11.

      **Both dispatch mechanisms are read.** A module writes its `stand`/`walk`/`run`/`dodge`/`attack`/
      `melee`/`sight` callbacks *directly into the entity* from its spawn function, and separately registers
      per-animation-frame handlers into a 256 × 32 class method table at `0x800D519C`. The Soldier's spawn
      (`BASE1` module+0x1604) writes exactly id's seven fields at exactly the offsets the engine reads, with
      `melee` explicitly zeroed, plus `mass = 100` — which is id's soldier, value for value, from a decode
      that was not tuned to produce it.

      **So the engine half of the AI is done and checked.** `ai_stand`, `ai_walk`, `ai_run`, `ai_charge`,
      `ai_move`, `ai_run_slide/missile/melee`, `ai_checkattack`, `FindTarget`, `FoundTarget`, `HuntTarget`,
      `visible`, `range`, `infront`, `M_MoveFrame`, `M_ChangeYaw`, `M_walkmove`, `M_MoveToGoal`,
      `SV_movestep`, `SV_StepDirection`, `SV_NewChaseDir`, `SV_CloseEnough` and the player trail are all
      transcribed in `src/game/ai.[ch]`, `aimove.[ch]`, `aiworld.[ch]` and `monster.[ch]`. `q2psx-inspect ai`
      reads 76 constants back out of the executable and compares them against the port: **76 of 76 agree.**
      The AI's world scale turns out to be **12**, not the 10 established elsewhere — six independent
      constants agree on it — and the vertical axis is **Y, pointing down**. FORMATS.md §14.

      **The modules themselves are now decoded too.** There are only **seven distinct creatures** on the
      whole disc — Soldier, Tankcomm, Gunner, Insane, Arachner, Infantry, Berserk — over fifteen module
      instances on thirteen maps. `src/game/creature.[ch]` follows each module's own code to its class
      bytes, animation speed scale, mass, callbacks, moves and frames; `q2psx-inspect creatures` runs it
      over the disc and **15 of 15 decode with zero failures**. Two entity fields fell out of it that
      nothing else had located: `+0xA0` and `+0xA4` are the entity's own **pain** and **die** hooks, and
      `+0x3A` is **skinnum**, which is what selects a Soldier's weapon.
      The **Soldier** — seven of the thirteen AI maps, so most of the monsters in the game — is fully
      transcribed in `src/game/cre_soldier.c`: all fourteen think indices, all eight callbacks, all
      eighteen moves. Its constants are id's (`random() < 0.8` as 26214/32767, and so on).

      **The other six creatures' think functions are decoded rather than transcribed.** Reading 37 MIPS
      functions one at a time was not going to happen, and writing them from the Soldier's pattern would
      have been invention. So the same trick that recovered the move tables was pushed one level down: a
      think function's *actions* are followed — the sound it plays, the claw it swings with its own aim,
      damage and kick, the frame it jumps to, the aiflags it sets. **51 of 51 think indices across the disc
      decode to an action, and all 7 creatures act.** The decode is validated against the Soldier, whose
      functions were read by hand first and which it reproduces independently; and the numbers it recovers
      are id's — the Berserk's two attacks come out at damage 15 and 5 with kick 400, which is
      `berserk_attack_spike` and `berserk_attack_club` exactly.
      What remains approximate is branch conditions: a step behind a branch is marked and handled by two
      stated rules rather than modelled. FORMATS.md §14.14.

      Also still open is where a specific creature's health subtraction, pain threshold and per-attack
      damage live. The damage function does not subtract a creature's health — it posts the
      amount to the module through `0x800627F8` and returns — so those numbers are in the modules and
      nowhere else in the image. Combat against creatures is complete on the player's side and hollow on
      theirs until *that* falls. What IS known from the executable, and is implemented, is which mod each
      creature attack carries: a contact hit is mod 7 (`0x800612F0`), a thrown grenade is the launcher's own
      spawner at speed 600 rather than 900 (`0x80061724`), and a creature rocket is `0x8004AF28` with the
      aim scaled by 3/2 (`0x80062164`).
      *Attack:* the modules disassemble now (`q2psx-inspect moddisasm`), each carries its animation names as
      plain strings, and its spawn function is export 0 — so the remaining work is per-creature transcription
      rather than another format problem.

      **A SECOND interface block exists, and it is not the same one.** Following the item spawner turned up
      `0x80079818`, which builds a **1268-byte block at the fixed address `0x800B2FE4`** — the size is its own
      first word — and fills **240 slots** with executable addresses and shared-global pointers. That is
      distinct from the 304-byte per-module table `0x8007D990` writes at a module's base: this one is global,
      lives in the engine's own data, and mixes services with state (the GAME VARIABLES settings the pause
      menu writes sit at `+886`…`+904` of it). It is what makes the old finding under #10 make sense — the
      Population globals have no reader *in the executable* because the readers are in the modules, and this
      is the door they come through. Slots 9, 11, 13 and 70 are the Population/item side; the full list is in
      FORMATS.md §15.5. What is still unread is which slot numbers the *modules* use for what, which is the
      same per-module transcription problem as the paragraph above.

---

## Tier 2 — Blocking: degrades the level badly, does not prevent loading

- [ ] **10b. One model part textures wrong, and it is the same part everywhere.** BASE1's `Soldier` renders
      correctly except for part 2 — 30 faces covering the head and one shoulder — which comes out as
      saturated purple noise. The evidence narrows it a long way without settling it:
      the same model renders identically in BASE1, BASE2 and WASTE1, so it is **not** a per-map palette
      problem; part 2 is the **only** part of the model that uses texture page 6 and CLUT index 46, and no
      other part shares either; every other part of the same model, and every weapon and item model tested,
      textures correctly under the same rule; and the map does upload seven pages, so page 6 exists.
      Saturated purple on an otherwise green-and-grey model is the classic signature of a palette that is
      right in form and wrong in identity. *Attack:* dump page 6 of a map's VRAM against several candidate
      palettes and see which yields colours consistent with the rest of the model — the diagnostic in
      `q2psx-inspect model` already reports the per-part page and texture ranges that localise it.

- [x] **7. `SortData` encoding. — SOLVED, and the world is not depth-sorted at all.**
      "No fixed per-node record" was the finding, not the obstacle: there are no records. It is a
      **self-describing variable-width opcode stream**, and the reader is the zone draw's own, inlined seven
      times between `0x80066B70` and `0x800676D8`. Bits are LSB-first inside little-endian 32-bit words
      against a mask table at `0x8009FBF0`; the start offset is a **byte** offset from the viewport record's
      `+28`, word-aligned down with the remainder consumed as bits.
      The header is seven fields, each holding its width **minus one** (widths 1…16), in the literal order of
      the `slti` bounds: `4,3,4,3,3,3,4` bits giving `w_base, w_op_short, w_op_long, w_f1, w_f3, w_f4, w_f2`,
      then `w_base` bits of `base`. Then opcodes: **0** ends, **1** is an entity draw record (four fields, and
      `f2` is the BIT length of a payload the entity consumes only if it was drawn — otherwise the stream
      skips it), **2** switches between a windowed mode that adds `base` and an absolute mode that does not,
      carrying the replacement opcode at the new mode's width, and **≥3** is a scene node, `op - 3`.
      **The load-bearing consequence.** The ordering-table bucket starts at 45 (`0x80066978`) or 43
      (`0x80066A3C`) and is decremented in exactly one place — after an entity record (`0x800675E0`). The node
      path never touches it: it draws a whole node into the current bucket and stops the stream once the
      bucket falls below 4. So **the world's draw order is authored, and the ordering table only carries it**;
      buckets exist to interleave entities with the world at the right depth. A port that computes a bucket
      per quad from the GTE's depth — which this one did — produces a *different* order, not a finer one.
      Implemented in `src/formats/sortdata.[ch]`, unit-tested in `tests/test_surface.c` against an
      independent encoder (word-straddling fields, multi-word skips, both modes, unaligned starts).
      Checked disc-wide by `q2psx-inspect surfaces`, which **tiles** each chunk end to end — decode to the end
      opcode, round up to the next byte, start again — over all 115 chunks and 715,260 bytes: **8,968 streams,
      87 overruns at chunk tails, 178,801 node references and ZERO out of range.** A desynchronised bit reader
      does not land on a valid header 8,968 times in a row, which is what makes tiling a test rather than an
      illustration.
  - [~] 7a. Residue: which stream a given viewport starts at — **narrowed, and no longer a functional gap.**
        The offset is an `int16_t` at `+28` of a 36-byte record indexed by the viewport's own `+146`
        (`0x80066AFC`), reached through the table pointer at `0x800C8E94`. That record is runtime state: the
        chunk pointer `0x800B2C84` has exactly **one** reader in the whole image (`0x80066B00`, the stream
        init) and two writers, both in the zone loader, and `+28` likewise has exactly one reader. Nothing on
        the disc carries the mapping. `Resources` was the obvious candidate and is ruled out — 32 bytes per
        record, not 36.
        What removes the practical consequence is that the streams are **self-delimiting and therefore
        enumerable**: `q2_sortdata_enumerate` / `q2_sortdata_stream_offset` tile a chunk and hand back a stable
        stream INDEX, which is a disc-derived handle where a raw byte offset is not. The renderer takes one;
        stream 0 is what a single-stream chunk holds. What remains unknown is only which index a given
        viewport picks, not how to reach any of them.
- [ ] **8. `AreaConx` 9-byte link payload.** No fixed offset yields a 1.3.12 unit normal in more than 39 % of
      3,494 links. Histograms suggest **unaligned** `int16_t` values that no single struct layout can express
      (links start at `record + 1 + 9*L`, so parity alternates). Byte `+3` is the best neighbour-index
      candidate. Blocks portal-based visibility.
- [x] **9. `SpaceLights` per-node partition. — SOLVED, and the partition was never in this chunk.**
      Two passes tried to split the array across the zone's **scene** nodes and got 0.68…7.12 entries per
      node with no rule that fit. The key is a **collision** node, and specifically a `SecondaryCol` one:

          lights of secondary node i = SpaceLights[ node[i].c_hi .. node[i+1].c_hi )

      where `c_hi` is the high halfword of the 36-byte node's field at `+28` — the field #23 recorded as
      unread because *"no instruction in the image loads offset +28"*. None does. The gather at
      `0x8006B0E4` loads offset **+30**, and its successor's at +66, exactly as the plane and link ranges are
      derived, which is what the totals sentinel was always there for. The entries are indices into
      `COMMON.DAT`'s `Lights` array (`0x800B2ED4`, stride 28, materialised at `0x8006B12C`).
      Checked disc-wide by `q2psx-inspect lights`: `PrimaryColl` carries the field on **0 of 115** zones —
      an independent confirmation that the engine's choice of hull is forced, not incidental — `SecondaryCol`
      is non-decreasing and starts at zero on **115 of 115**, and all **37,285** reachable entries name a
      light the map actually ships, **zero** out of range against light counts of 96…374. 13,805 nodes, mean
      2.70 lights each, max 45, 1,682 with none, and 15 zones (the front end, the intermissions and the FMV
      stubs) with an entirely zero partition. The 331 halfwords past the sentinel disc-wide are build
      residue the engine cannot reach.
      Implemented in `src/formats/spacelights.[ch]`; consumed by `src/game/lighting.c` and `flare.c`.
- [x] **10. `Population` `spawn.classId` target table. — SOLVED.**
      It is not an index into anything on the disc. The id is stored beside a **name** in a 48-byte-stride
      table at `0x800A3368`, and the engine reaches it from the other side: a `CreAI` module's 16-byte
      preamble starts with a 12-byte name, and the loader at `0x8007D990` looks that name up through
      `0x80057A18`. Each record also carries the class's `health` and a size-scaled offset.
      The health column is what proves it: the values are PC Quake II's own, creature for creature — three
      `Soldier` records at 30, 20 and 40 (shotgun, light and machinegun guards), `Infantry` 100, `Flyer` 50,
      `Gladiator` 400, `Jorg` 3000 — and nothing in the decode was tuned to produce them.
      `q2psx-inspect classes` checks it disc-wide: **651 of 651** spawn records resolve to a class, and
      **651 of 651** of those classes name a model the same map ships. Implemented in
      `src/build/classtable.[ch]`. Full table in FORMATS.md §9.8.
- [ ] **10b. One model part textures wrong, and it is the same part everywhere.** BASE1's `Soldier` renders
      correctly except for part 2 — 30 faces covering the head and one shoulder — which comes out as
      saturated purple noise. The evidence narrows it a long way without settling it:
      the same model renders identically in BASE1, BASE2 and WASTE1, so it is **not** a per-map palette
      problem; part 2 is the **only** part of the model that uses texture page 6 and CLUT index 46, and no
      other part shares either; every other part of the same model, and every weapon and item model tested,
      textures correctly under the same rule; and the map does upload seven pages, so page 6 exists.
      Saturated purple on an otherwise green-and-grey model is the classic signature of a palette that is
      right in form and wrong in identity. *Attack:* dump page 6 of a map's VRAM against several candidate
      palettes and see which yields colours consistent with the rest of the model — the diagnostic in
      `q2psx-inspect model` already reports the per-part page and texture ranges that localise it.

- [x] **7. `SortData` encoding. — SOLVED, and the world is not depth-sorted at all.**
      "No fixed per-node record" was the finding, not the obstacle: there are no records. It is a
      **self-describing variable-width opcode stream**, and the reader is the zone draw's own, inlined seven
      times between `0x80066B70` and `0x800676D8`. Bits are LSB-first inside little-endian 32-bit words
      against a mask table at `0x8009FBF0`; the start offset is a **byte** offset from the viewport record's
      `+28`, word-aligned down with the remainder consumed as bits.
      The header is seven fields, each holding its width **minus one** (widths 1…16), in the literal order of
      the `slti` bounds: `4,3,4,3,3,3,4` bits giving `w_base, w_op_short, w_op_long, w_f1, w_f3, w_f4, w_f2`,
      then `w_base` bits of `base`. Then opcodes: **0** ends, **1** is an entity draw record (four fields, and
      `f2` is the BIT length of a payload the entity consumes only if it was drawn — otherwise the stream
      skips it), **2** switches between a windowed mode that adds `base` and an absolute mode that does not,
      carrying the replacement opcode at the new mode's width, and **≥3** is a scene node, `op - 3`.
      **The load-bearing consequence.** The ordering-table bucket starts at 45 (`0x80066978`) or 43
      (`0x80066A3C`) and is decremented in exactly one place — after an entity record (`0x800675E0`). The node
      path never touches it: it draws a whole node into the current bucket and stops the stream once the
      bucket falls below 4. So **the world's draw order is authored, and the ordering table only carries it**;
      buckets exist to interleave entities with the world at the right depth. A port that computes a bucket
      per quad from the GTE's depth — which this one did — produces a *different* order, not a finer one.
      Implemented in `src/formats/sortdata.[ch]`, unit-tested in `tests/test_surface.c` against an
      independent encoder (word-straddling fields, multi-word skips, both modes, unaligned starts).
      Checked disc-wide by `q2psx-inspect surfaces`, which **tiles** each chunk end to end — decode to the end
      opcode, round up to the next byte, start again — over all 115 chunks and 715,260 bytes: **8,968 streams,
      87 overruns at chunk tails, 178,801 node references and ZERO out of range.** A desynchronised bit reader
      does not land on a valid header 8,968 times in a row, which is what makes tiling a test rather than an
      illustration.
  - [~] 7a. Residue: which stream a given viewport starts at — **narrowed, and no longer a functional gap.**
        The offset is an `int16_t` at `+28` of a 36-byte record indexed by the viewport's own `+146`
        (`0x80066AFC`), reached through the table pointer at `0x800C8E94`. That record is runtime state: the
        chunk pointer `0x800B2C84` has exactly **one** reader in the whole image (`0x80066B00`, the stream
        init) and two writers, both in the zone loader, and `+28` likewise has exactly one reader. Nothing on
        the disc carries the mapping. `Resources` was the obvious candidate and is ruled out — 32 bytes per
        record, not 36.
        What removes the practical consequence is that the streams are **self-delimiting and therefore
        enumerable**: `q2_sortdata_enumerate` / `q2_sortdata_stream_offset` tile a chunk and hand back a stable
        stream INDEX, which is a disc-derived handle where a raw byte offset is not. The renderer takes one;
        stream 0 is what a single-stream chunk holds. What remains unknown is only which index a given
        viewport picks, not how to reach any of them.
- [ ] **8. `AreaConx` 9-byte link payload.** No fixed offset yields a 1.3.12 unit normal in more than 39 % of
      3,494 links. Histograms suggest **unaligned** `int16_t` values that no single struct layout can express
      (links start at `record + 1 + 9*L`, so parity alternates). Byte `+3` is the best neighbour-index
      candidate. Blocks portal-based visibility.
- [x] **9. `SpaceLights` per-node partition. — SOLVED, and the partition was never in this chunk.**
      Two passes tried to split the array across the zone's **scene** nodes and got 0.68…7.12 entries per
      node with no rule that fit. The key is a **collision** node, and specifically a `SecondaryCol` one:

          lights of secondary node i = SpaceLights[ node[i].c_hi .. node[i+1].c_hi )

      where `c_hi` is the high halfword of the 36-byte node's field at `+28` — the field #23 recorded as
      unread because *"no instruction in the image loads offset +28"*. None does. The gather at
      `0x8006B0E4` loads offset **+30**, and its successor's at +66, exactly as the plane and link ranges are
      derived, which is what the totals sentinel was always there for. The entries are indices into
      `COMMON.DAT`'s `Lights` array (`0x800B2ED4`, stride 28, materialised at `0x8006B12C`).
      Checked disc-wide by `q2psx-inspect lights`: `PrimaryColl` carries the field on **0 of 115** zones —
      an independent confirmation that the engine's choice of hull is forced, not incidental — `SecondaryCol`
      is non-decreasing and starts at zero on **115 of 115**, and all **37,285** reachable entries name a
      light the map actually ships, **zero** out of range against light counts of 96…374. 13,805 nodes, mean
      2.70 lights each, max 45, 1,682 with none, and 15 zones (the front end, the intermissions and the FMV
      stubs) with an entirely zero partition. The 331 halfwords past the sentinel disc-wide are build
      residue the engine cannot reach.
      Implemented in `src/formats/spacelights.[ch]`; consumed by `src/game/lighting.c` and `flare.c`.
- [x] **10c. The `Population` place record's `id` and the item table. — SOLVED, and every item on the disc
      with it.**
      The id is not an index into anything on the disc either. It is the key into a **24-byte table at
      `0x8009F5CC`**, 64 records, scanned with `lb` and terminated by `0xFF`, and the reader is the item
      spawner at `0x800599DC` — which the level modules reach through import slot 11 of the block at
      `0x800B2FE4` (see #6). Each record names the item's MODEL, its BEHAVIOUR (a nine-bit flag word) and its
      EFFECT (an index into a 55-entry jump table at `0x800AC30C`, taken as `effect - 2`).
      What makes the flag decode CONFIRMED rather than plausible is the same class of argument the class
      table's health column carries: bits 4/5/6 are one dynamic-light channel each, and decoded that way the
      twelve key records spell out the colours their own names claim — Redkey R, Greenkey G, Bluekey B,
      Yellowkey R+G, Pkeypurp R+B, Whitekey R+G+B. Nothing in the decode was tuned to produce that.
      All 43 live effect handlers are disassembled and transcribed with the instruction each amount came from
      (`src/game/item.c`), including two asymmetries that are the original's and not the port's: **combat
      armour has no "already full" test** while body and jacket do, and INFINITE AMMO makes a weapon pickup
      grant its ammo type's *capacity* rather than a fixed amount.
      Twelve dispatch slots point at the failure exit, eight of them named by real records — the Xatrix/Rogue
      weapons, their ammo, the power screen and the stimpack. **No map on the disc places any of the eight**,
      which is the independent check that they are leftovers rather than a decode failure.
      `q2psx-inspect items` reads the table and the dispatch off the disc, diffs both against the port's
      transcription, then walks every place record: **1,013 of 1,013** resolve, **1,013 of 1,013** name a
      model the same map ships. Implemented in `src/build/itemtable.[ch]`, `src/game/entity.[ch]`,
      `src/game/item.[ch]` and `src/game/entitydraw.[ch]`; full decode in FORMATS.md §15.
      Three residues, all recorded rather than papered over:
  - [ ] 10d. The place record's halfword at `+0x0C`: the low twelve bits are the entity's yaw (`andi 0xFFF`
        at `0x80058938`), but **bits 12…15 are unread**. 627 of 1,013 records set at least one and all four
        occur. The angle itself is zero in 905 of 1,013 and never lands on a quarter turn, which is what a
        spinning pickup would look like — but it means the field carries almost no information and the upper
        nibble probably carries most of it.
  - [ ] 10e. The item table's `clips[4]` is a `0xFFFF`-terminated list whose pointer is stored into the
        entity's model state; twelve records carry two entries each. Which of the model's own clips those
        indices select is not checked against a real CastList yet, so the port stores them and plays the
        first.
  - [ ] 10f. The entity's `+0xFC` scale is multiplied by a second halfword at `+0xFE` before the renderer
        scales the matrix by `(a * b) >> 11` (`0x8006B298`). Nothing was found that writes `+0xFE`, so the
        port folds the pair into one 1.0.12 scale. If `+0xFE` is ever anything but a constant, every item's
        size is wrong by that factor.
- [x] **11. `MapMod` `Poly.uvIdxFlags` bits 6–7. — RESOLVED as a UV rotation; kept in Tier 0 above.**
- [x] **12. `Scene` node `flags08`. — SOLVED. It is four fields, not one bitfield.**
      The zone draw and the node emitter read the halfword at node `+8` four different ways, and the low ten
      bits are the piece that mattered most:

      | bits | meaning | read at |
      |---|---|---|
      | 0-9 | runtime object slot **+ 1**; 0 = none | `0x80067724`, `0x800665D4` |
      | 10-13 | the SETWIBBLE field; only 10-11 are read | `0x80066740` |
      | 14 | deferred, depth-sorted draw path | `0x80066524` |
      | 15 | do not draw | `0x8006771C` |

      **Bits 0-9 are the node-to-mover binding**, and their emptiness on disc is exactly why movers looked
      impossible from the data side: the loader clears them and a map's script fills them in. The value scales
      by 92 and indexes the runtime object array whose first element is `0x800D6BB0`. The object supplies more
      than the port was applying — the zone draw at `0x800678B4` calls `RotMatrix` on three `s16` Euler angles
      at `obj+0x0C` and then adds **two** independent `s16` triples, `+0x12` and `+0x18`. Rotating brush
      geometry (`ROTHATCH`, `SIMROT`, `ROTBUTTON`) is therefore drawn, not absent, and `mover.h`'s "there is
      no rotation anywhere in the engine" is true only of the linear `MOVER_A/B/C` integrator.

      **Bits 10-11 are a warp control.** They select one of four quad linkers — `0x800AF7CC`, `0x800AFC9C`,
      `0x800AFA2C`, or none — and what separates them is *when a near quad is subdivided*. Subdivision is
      `0x800B007C`: a 5×5 vertex grid (21 freshly projected, the four corners passed in as pointers) written
      into 16 `POLY_GT4` packets at 52-byte stride, i.e. one quad becomes a 4×4 mesh. Since UVs interpolate
      affinely, splitting a near quad sixteen ways is precisely what suppresses the texture warp — so
      "SETWIBBLE" names the wobble it controls. Variant 1 additionally rejects a quad whose vertex 1 or vertex
      3 projected to depth zero; variant 2 gates subdivision on `clut & 0x3C`; variant 3 links nothing, which
      is how a script hides a surface group. The decision is also budget-aware (`0x800698A0`): freely while
      the packet pool is healthy, only within a quarter of the threshold while it is half gone, never once it
      is nearly exhausted.
      Implemented in `src/formats/surface.[ch]` and wired into `src/game/world.c`; `tests/test_surface.c`
      pins the decode and `q2psx-inspect surfaces` checks it against all 17,035 nodes. Three predictions the
      reading makes are all confirmed: bit 15 is **never** set on disc, draw variant 3 **never** occurs, and
      the object field is **always** zero — the states a runtime-only field must not have been authored with.
      All 17,035 nodes fall inside the eight known values.
- [x] **4d. Rotating brush geometry. — SOLVED.** `mover.h` said "there is no rotation anywhere in the engine";
      that is true of the `MOVER_A/B/C` integrator and false of the engine. A node bound to a runtime object
      picks up a full rotate-about-pivot transform in the zone draw (`0x800678B4`–`0x8006793C`):

          node position = origin - camera - (R . p) + p + d

      with `R = RotMatrix(obj[0x0C..0x10])` (libgte, `0x80089E38`), `p = obj[0x18]` the **pivot**, and
      `d = obj[0x12]` the linear displacement. `R . p` is the 1.3.12 matrix-vector routine at `0x8006FB18`
      (three `mult`s summed, then `>> 12`). The `- R.p + p` shape is what identifies `obj+0x18` as a pivot
      rather than the "second independent displacement" an earlier pass called it, and `ROTHATCH`'s
      constructor confirms it from the data side: it fills `obj+0x18`/`+0x1A` with the item's pivot at
      `+10`/`+12` **minus the node's own origin** (`0x8002B7B4`–`0x8002B808`).
      The integrator is `0x8002F1A8`, reached from the 48-object sweep at `0x8002DC04`:

          if (!(obj[0x50] & 0x01000000)) return;
          obj[0x20] += (s16)obj[0x3A] * dt;
          obj[0x0C + 2*axis] = (obj[0x20] >> 8) & 0xFFF;
          obj[0x50] &= ~0x01000000;

      **It is one step per request, not a free spin** — the handler clears its own enable bit and `SIMROT`'s
      exec (`0x8002DEC8`) is what sets it, so continuous rotation is a script firing every tick. A port that
      implements a constant angular velocity spins doors that should have nudged. `axis = (obj[0x50] >> 14) & 3`,
      and since only `obj[0x0C + 2*axis]` is ever written, exactly one Euler angle is non-zero — which makes
      the matrix's composition order unobservable on this disc.
      Two operands were missing from the `SIMROT` table and are now recorded: **`item+4` is the angular speed**
      (`0x8002867C`) and **`item+20 & 3` is the axis** (`0x80028664`).
      Implemented in `src/game/rotator.[ch]`, applied in `src/game/world.c`, pinned by `tests/test_rotator.c`
      (49 checks). `q2psx-inspect surfaces` builds every `SIMROT`/`SIMROT2` on the disc: **11 rotators across
      49 maps, axis X 2 / Y 7 / Z 2 with the invalid axis 3 never occurring, speeds −3200…8000, and 0 node
      indices outside their map's zones.** Eleven is a small sample and is stated as such — it is consistent
      with the operand offsets rather than decisive about them.
      **All four rotation primitives are now built, and there are THREE integrators, not one.** Reading one
      family's arithmetic onto another turns a hatch 256 times too fast or a SIMROT eight times too slow:

      | primitive | handler | motion |
      |---|---|---|
      | `SIMROT`, `SIMROT2` | `0x8002F1A8` | `accum += speed*dt`; `angle = (accum >> 8) & 0xFFF`; one step per request |
      | `ROTHATCH` | `0x8002B460` | `angle += (speed*dt)/8` rounded **toward zero**, sweeping until it passes `obj+0x44` |
      | `ROTBUTTON` | `0x8002BFD8` / `0x8002C078` | a **snap**: the angle *is* 2048 when pressed and 0 when the hold expires |

      `ROTHATCH` runs inside a seven-state machine whose jump table is at `0x800ABDA0` — the same shape as the
      linear mover's. Its axis is a **byte at `item+8`**, not a halfword at `+20` as `SIMROT`'s is, and its
      speed's sign is chosen by which half of the circle the target lies in (`0x8002B70C`) so it always turns
      the short way. `ROTBUTTON` has neither an axis nor a target operand: its exec hard-wires `obj+0x0E`, the
      Y slot, and the literal 2048.
      Census over the disc: **33 rotators across 49 maps, axis X 4 / Y 25 / Z 4 with the invalid axis 3 never
      occurring, 0 node indices out of range**; 18 carry no speed, which is exactly the `ROTBUTTON` population.
  - [ ] 12a. Residue: `flags08` bits 12-13. SETWIBBLE writes a four-bit field (`0x8002E7CC` masks with
        `0xFFFFC3FF`) but the renderer masks to two, so values 4…15 alias onto 0…3. Bit 12 is nevertheless
        authored on disc — `0x1000` on one node and `0x1400` on three — and **no reader was located** for
        either bit anywhere in the image.
  - [ ] 12b. Residue: `unk0C`, `unk0D`, and `unk0E`'s meaning. `unk0E` now has a consumer: on the bit-14
        deferred path only, its low 7 bits index a 64-byte-stride table at `0x800D8D78` and the record is
        handed to `0x80047744` (`0x80066800`). What that table holds is still open. `unk0C` (0…4, non-zero on
        6 nodes) and `unk0D` (0…3, non-zero on 5) remain unread. Byte `+0x0F`, documented as "always 0", is
        scratch: the zone draw writes a per-frame counter into it at `0x80067A34`.

---

## Tier 3 — Behavioural / audio-visual polish

- [x] **12a. The effect system. — SOLVED, and it is four systems rather than one.**
      Recorded here because the shape of the answer is what made it tractable, and because two of the four
      were previously mentioned in passing as if they were the same thing.

      **The four.** *Particle groups* (`0x80030284`, drawn and integrated by `0x800304A8`): a pool of
      288-byte records, each holding up to fifteen screen-aligned quads that share one origin, one velocity,
      one acceleration and a pair of colour ramps. *Transient beams* (`0x80064E64`, drawn by `0x80064F10`):
      a 32-slot list refilled from empty every frame. *Debris* (`0x80064558` → `0x80064398`, think at
      `0x80064124`): real entities with physics. *Trails* (`0x80064C00` → `0x80064780`): a 96-vertex tube
      behind any entity carrying flag `0x04000000`.

      **A group is a burst, not a particle.** The fifteen quads cannot outlive each other, cannot be culled
      separately and all read the same ramp entry, so a burst fades as one object — which is why the
      original's explosions pulse rather than dissolve. Quad 0 is absolute; quads 1..14 carry an offset and a
      velocity *relative to it* (`0x800303C8` does the subtraction at spawn), so the burst translates with
      whatever made it while it spreads. The acceleration lives in what would otherwise be a fifteenth offset
      slot, which is why fifteen and not sixteen.

      **The ramp header is not a count, and the difference is visible.** Nineteen 132-byte records at
      `0x8009BA60`, ending exactly where a nineteen-entry pointer table begins at `0x8009C42C`. The leading
      `u16` reads 32 on sixteen of them — the entry count — but 64 on records 3, 4 and 13, which are still
      132 bytes apart. It is the semi-transparency field of a draw-mode word (`0x80030828` ORs it with
      `[0x800DDD5A]`): sixteen ramps add, three **subtract**. Ramp 3 is cyan and is the blood spray's second
      ramp; cyan subtracted from a lit wall is red, so a reader that took the header as a count would turn
      blood into a cyan flash and still pass every bounds check.

      **The ramp is indexed by age.** `colour[32 - life]`, so a 15-tick group only reaches the dim tail
      (entries 17..31) and changing a lifetime changes an effect's starting *colour*. The bright head of
      every ramp is unreachable for all but the 25-tick spark.

      **The beam hull folds rather than wrapping.** `0x800634E4` emits three axes 120° apart **and their
      negations, in that order**, so the six ring points come out at 0/120/240/180/300/60° and the six quads
      of the face list fold back through the beam's own axis. "Correcting" that ordering into a clean
      hexagonal prism produces geometry that looks nothing like the original at the same polygon cost.

      **Blood colour is a creature property.** `0x80059648` tests three flag bits — `0x10` red, `0x20` green,
      `0x40` blue — in that order and takes the first set. The chain has **no final else**, so a creature
      with none of them reaches the spawn with an uninitialised register: the same defect class as the
      `T_Damage` fifth argument, and handled the same way (the port picks a defined value and says so).

      All of it is checked by `q2psx-inspect effects` against the disc and by `tests/test_effect` without
      one. Full write-up in FORMATS.md §18.

      **The glint mesh is the `GlintMod` level chunk**, which was already in the schema as "OPTIONAL,
      present on BIGGUN only" and had never been connected to anything. `0x8007AB44` looks the name up and
      `0x800651BC` splits the chunk at a literal 864 bytes: 216 faces of four u8 indices, then vertices of
      four s16. The fourth halfword is the BAND COORDINATE, not padding — `0x80064938` reads `+6` off each
      source vertex. Two things confirm the split independently and neither was tuned for: on BIGGUN the
      chunk gives 218 vertices and the highest index anywhere is exactly 217, and the first vertex's band
      value is 1024, which is precisely the constant the renderer offsets its band centre by.

      **A particle quad is the 16x16 corner of `chars.lbm`** — the HUD atlas. `0x8001AD14` selects
      `[0x800DDD5A]` for the 8-pixel face and `0x80030830` hands the particle draw the same global, so
      uploading the overlay's font uploads the particles.

      **Debris moves in PrimaryColl at radius 2048**, not the player's hull: `0x80046DA0` installs
      `0x800C8E90` and the entity's `+0xA0`, where the player's path (`0x80046DDC`) installs SecondaryCol
      and `+0xA2`. Tracing shards against the eroded hull leaves them 286 units off the floor.

      **AND IT IS NOT A WEAPON TRAIL.** "A mesh dragged behind an entity with a band running along it" reads
      as a trail, and that is what it was recorded as until the decoded mesh was drawn: BIGGUN's 216 quads
      over 218 vertices are a SPHERE and the band sweeps its surface. Which means nothing in this engine
      draws a weapon trail at all — the blaster bolt is its own oriented hull and leaves nothing behind it.

      **What raises the glint flag is a LEVEL SCRIPT, not the engine.** Bit `0x04000000` of entity `+0x10C`
      gates the draw, and nothing in the executable sets it: all thirty-nine writes to `+0x10C` raise at
      most `0x8000`, and the only site in the image materialising `0x04000000` is the test itself. Read from
      the executable alone the glint is dead code. BIGGUN's `LevelBin` — a *relocatable MIPS module*, not
      bytecode — carries it at chunk `+0x1998` (`lw +0x10C; lui 0x0400; or; sw`) and tests it at `+0x1B00`,
      alongside `sb 6, 695(a0)` (the band count) and `sh 4, 702(a0)` (the phase). That 4 is the check that
      the band formula reads the right way round: `(width/4) * (4 - phase)` sweeps forward as the phase
      counts down, and the reversed subtraction looks identical standing still. The port therefore draws no
      glint by default — it cannot run relocated level modules — and puts the reconstruction behind a
      toggle rather than inventing an entity to wear one.

      **The particle CLUT is built-in palette 75.** `[0x800E3FC2]` is not a standalone global, which is why
      the address sweep found no writer: `0x80030DB8` materialises `0x800E3F2C` — the CLUT-id table the boot
      palette loop fills, indexed by record id — and reads `+150`. On this disc id 75 resolves to `0x3F0A`.

      **The glint has TWO draw paths** and only one had been reconstructed at first. `0x80064CE4` branches
      on the band count: zero uses the entity's own matrix, colour and phase at width 8192; non-zero walks
      that many 12-byte records at `0x800D565C`, each with its own angles, phase and colour, at width
      **4096**. The halved width is not cosmetic — by the shading formula the two uses of `width` do not
      cancel, so a multi-path band is narrower AND twice as bright. Both paths are now implemented. The
      phase is a plain byte decrement that UNDERFLOWS (`addiu 255`, no clamp, no reload), so refreshing it
      is the script's job and wrapping it here would be inventing a reload the console does not perform.

      **Debris gravity is READ, not modelled.** `0x80046464`: skip if `ent+0x10C & 0x2000`; otherwise
      `vel.y += [0x800AE924] * frame_delta` — the same global the player uses — clamped at **8192** on the
      way down only, which is what lets the burst's -1536 launch bias survive its first tick.

      **And the script can be READ rather than run.** That looked like a hard stop — `LevelBin` is a
      relocatable MIPS module and this project deliberately has no interpreter — but the question a port
      needs answered is not what the script computes, only whether it raises a glint and with what. So
      `q2_fx_glint_scan` looks for the raise triple and recovers the two immediates beside it. They are
      neither adjacent to their stores nor near each other (the phase's `addiu` is five instructions before
      its `sh`, and the sites are 0x168 apart), so each source is found by walking back for a load of the
      store's own register. On this disc that reads **6 bands, phase 4** out of BIGGUN and nothing out of
      the other forty-eight maps. The glint is therefore ON automatically wherever a script turns it on,
      with the script's own numbers — no interpreter and no special case.

      **AND THE GAME DOES HAVE A WEAPON TRAIL — the BFG's.** Two earlier passes here said it did not, on
      the grounds that every *particle* spawn site is an impact or a death. That is true and it is not the
      whole story: the trail is a BEAM. `0x800CABC0` is twelve 28-byte records walked every frame by
      `0x80048CA8`, which ages each timer by the frame delta and re-submits the live ones into the transient
      pool — the only effect in the game that outlives its frame without being an entity. `0x80049CF0`
      fills it from inside the BFG's own tick (`0x8004BD04`), behind a visibility test, with a timer of 45,
      a radius of 64 and beam style **3**. So the ball beams everything it can see and each beam lingers
      after it has passed. The dedup at `0x80049D30` is load-bearing: without it a ball in view of one
      creature fills all twelve slots in twelve frames and then stops drawing, because nothing frees a slot
      early. This also corrects the claim that beam styles 3 and 4 are unreachable — they are unreachable
      *from the laser dispatcher*, and reached from here.

      *Still open:* only WHICH ENTITY wears the glint. The script's search loop takes the record whose
      `+0xD2` is 46 and `+0xDA` is 20, over 48 entries of 768 bytes; the rule is recorded and the lookup
      becomes exact once the port's entity records carry those two fields. Nothing else is unread.

- [ ] **13. Which in-game situation selects which music id.** The id lives in a `$gp` global whose writer was
      not traced; the per-map id is probably in the level `.DAT` chunks. Music is silent or arbitrary
      without it.
- [ ] **14. Does the engine loop XA tracks?** The duration field is converted to 50 Hz ticks and stored to
      two globals with a 30.0 s fallback — that looks like a countdown to a restart or fade — but the
      consuming code was not disassembled. One entry is **1.0 s short** of its measured stream length, hinting
      the value is a deliberate restart point rather than a length.
- [ ] **15. MDEC output depth for the movies (24-bit vs 15-bit).** Blocked on #16.
- [ ] **16. Locate the movie player overlay.** The executable contains **no** `.STX` / `MOVIES` / `STX`
      string at all, so both the player and its filename assembly live elsewhere. Solving this also settles
      the movie filename suffix and #15 in one pass.
- [ ] **17. SPU RAM base / reverb work area,** and whether reverb is disabled — the worst-case map leaves a
      **240-byte** margin against SPU RAM, suspiciously tight if a reverb buffer is also allocated.
- [x] **18. ~~`VramImageRec.width` / `height`: dimensions, or VRAM placement coordinates?~~ — RESOLVED by
      #1.** `width` is BYTES PER ROW of the decoded buffer, `height` is rows, and the VRAM rect is
      `width>>1` halfwords wide. Confirmed independently by `(width>>1) * height * 2 == decoded size` in
      553/553 records and by the allocator at `0x80068BAC` taking `mflo(mult(w,h))` with no shift.
- [x] **19. The secondary 512 × 256 display-env init function. — SOLVED, and it is dead.**
      It is not a second display configuration the game switches to: it is the first half of the boot display
      bring-up at `0x8006DFB8`, and **everything it writes is overwritten before it can reach the hardware**.
      The two `SetDefDispEnv` calls at `0x8006E03C` / `0x8006E054` set 512 × 256 rectangles, and four
      instructions later `0x8006E0C8` calls `0x80077540`, which rewrites both display envs to 512 × 248 at
      (0,0). The draw envs it fills — including the giveaway per-buffer debug colours (255,8,32) and
      (8,255,32), which would make a torn frame obvious — are rewritten every frame by `0x80076A74`. The
      single unrelated caller is the host-filesystem boot path at `0x8006E150`, which is also where the
      `c:\PsxData\Q2Data\` literal lives. **The game never displays 512 × 256**, and the port is safe to
      commit to one display model. Checked by `q2psx-inspect screen`, which asserts both the rectangle and
      the overwriting call.
- [x] **20. The `VSync(3)` path. — SOLVED. It is a CD settling delay, not a frame rate.**
      The site is `0x80069188`, inside the drive bring-up at `0x80069090`: poll `CdlNop` until the status
      clears, retry `CdlGetTN` until it succeeds, `CdlSetmode(0x80)` — double speed — then `VSync(3)` and on
      to `0x80071548`. Three fields is 60 ms on PAL, which is a mode-change settle, and nothing about it is
      periodic. The earlier note that it was "reached only through a function pointer" was a search artefact;
      the call is direct.
- [x] **42. What writes the screen shake. — SOLVED, and it is a whole screen effect: the water.**
      The shake at `view+780` had been transcribed from the per-viewport draw that *reads* it and had no
      writer, so the port carried a field nothing could set. It has exactly one writer in the image,
      `0x80062F90`, inside `0x80062DF0` — and that function is the **underwater screen effect**, called per
      view from the game update (`0x80038164`, `0x800384C0`) and gated on bit `0x100` of the owning entity's
      flag word, i.e. `UNDERWATER`.

      **What named it.** The pool of packets it draws out of is described in the executable's own allocator
      table as `"Water Moves"` (`0x800ACF7C`), with `"Used lots frame %d"` right after it as the overflow
      message. So the identification is the executable's own word for it, not an inference from what an
      underwater screen ought to look like.

      **What it does.** An amplitude at `view+776` ramps to 4096 at 24 per `dt` unit while submerged and back
      down at the same rate. From it come two more amplitudes (`amp*4095/1024` and `amp*4095/2048`), the shake
      (each of those `>> 12`, giving 0..3 and 0..1), and a pair of phase accumulators advancing at 5 and 7 per
      `dt`. Two run-length-encoded passes over the viewport then emit `DR_MOVE` packets — **VRAM-to-VRAM
      copies of the frame that has just been drawn** — displacing columns vertically and rows horizontally by
      `(cos(phase) * amplitude) >> 24` plus the shake. A semi-transparent tile of `(amp*15/4096, 0,
      amp*55/4096)` goes in last, so it is drawn first and the strips displace an already-tinted picture.

      **And it explains the shake.** The offsets are never negative, because the shake added to them is
      exactly the largest the cosine term can reach. The draw env insets the viewport by that shake and
      shrinks it by the same amount, so the frame is drawn with a margin around it and every copy reads from
      inside the viewport. The inset is not a shake at all — it is the wobble's margin. Nothing in the port
      could have worked that out from the read side alone, which is why the field sat there unexplained.

      This is also the first thing to need what `src/screen`'s header had flagged as unmodelled: the
      framebuffer read back as a source. `PSX_PRIM_MOVE` in `src/psx/gpu.h` is that seam. Full detail in
      FORMATS.md §12.6.4; `q2psx-inspect screen` checks 45 further constants against the disc (**174 of 174**)
      and `tests/test_screen.c` checks the behaviour, including the property the whole thing rests on — that
      no strip copy reaches outside its own viewport.

      *Residue, and it is small:* these tiles carry no texture page, so the hardware's blend mode for them is
      whatever the last polygon drawn before them left in the register and is not statically determined. The
      port picks `B + F` for both this and the damage flash, on the grounds that both fade to `(0,0,0)` and
      only a mode where zero is a no-op makes that fade continuous — and that the world's own selector table
      makes an unset selector additive anyway. Reasoned, not transcribed, and marked as such in `screen.c`.
- [ ] **41. Which of the view weapon's two angle triples is the aim and which the kick.** The transform at
      `0x8004F40C` sums the triple at player+230 with the one `0x80038260` returns, negates x, and hands the
      result to `RotMatrix`. Nothing at the call site distinguishes them — both are three signed angles added
      to the same matrix — so `src/game/viewweapon.c` takes them as two inputs and adds them the way the
      original does. If the attribution turns out to be the other way round, nothing in the port changes;
      this is recorded so the ambiguity is not mistaken for a decision.
- [ ] **39. The screen fade, if there is one.** `gp+16660` (`0x800B2714`) is set to **255** and `gp+16676`
      (`0x800B2724`) to **−16** at the top of every session (`0x8001834C` / `0x80018354`) — a fade level and a
      per-frame decrement if ever there was one — and **neither is read anywhere in the image**. Not
      gp-relative, not through a materialised base, and neither address appears as a data word. Either the
      consumer is in an overlay (the movie player, #16) or a relocatable module (#6), or the fade was cut and
      the writes are vestigial. The port implements no fade on this evidence. *Attack:* look for the readers
      once a relocated module can be disassembled — the same capability #6 and #10 needed.
      **Re-checked when the water effect turned up (#42), because a ramped full-screen tint is exactly what
      this item was describing and it would have been satisfying to close two things at once. It is not the
      same thing:** the water ramps `view+776` per VIEW by 24 per `dt` unit toward 4096, not a `$gp` global
      per SESSION by −16 from 255. Both fade addresses still have one writer and no reader. #39 stands.
- [~] **40. What SCREEN POSITION moves. — INERT ON THIS BUILD; the port honours it anyway.**
      The menu page writes `0x800B3368` / `0x800B336A` (defaults 0 and 24), and the sweep finds **no reader**.
      The obvious consumer would be the display env's screen rectangle, which `SetDefDispEnv` at
      `0x8008AD5C` explicitly *zeroes* — the game never fills it. So either the offsets are applied somewhere
      not in this image, or the page is inert on this build, and nothing here can distinguish those.
      That is the finding, and the port does not pretend otherwise about the console. It does, however,
      apply the offset **at presentation** — after the ordering table, where it shifts the finished image the
      way a television's own position control would and cannot perturb clipping or the viewport rectangles
      the reconstruction depends on. A control that exists and does nothing is a bug from the player's side;
      the default y of 24 is treated as the neutral point because that is what the reset routine writes.
      *Still open:* whether an overlay or a relocatable module reads them. Same capability as #6.
- [x] **20a. The menu system. — SOLVED, and it is data, not script.** Every menu is an array of 24-byte item
      records in the executable's data segment, walked by an engine at `0x80019B88`…`0x8001BA14`: the record
      is `{label, s16 x, s16 y, action, *toggle, *slider, on-release}`, the loader is `0x8001A474`, and a
      page is one or two of those arrays with the *last* one deciding what is navigable — which is how a
      page of pure text is expressed (a second call passing the empty table at `0x8009B30C`). All 17 page
      ids, the four GAME VARIABLES tables selected by cheat level, the two VIDEO variants selected by
      `0x800AEBCC`, the skip-on-`'g'` rule, the wrap, the fire-on-release flag, the toggle's
      LEFT-means-ON, the slider's two-units-per-held-frame over `0..127`, and the four colour codes
      `b/d/g/u` are all in FORMATS.md §10. Implemented in `src/menu/`, and
      `q2psx-inspect menu <disc>` reads the tables back off the disc and compares them: **21 pages,
      0 mismatches**.
      Still open: the MISSION screen belongs to the HUD system rather than the menu (#39).
- [x] **20d. The memory-card front end. — SOLVED for everything that is on this side of the hardware.**
      The screens are not pages: they carry no page id, are never handed to `0x8001A384`, and are installed
      directly by nine functions in `0x8001D2xx`…`0x8002040x`. But they are ordinary 24-byte item records, so
      the existing checker applies to them unchanged — **9 screens, 31 items, 0 mismatches** against the
      disc. SELECT SLOT, NOT FORMATTED (+NO/YES), FORMATTING, SAVE FILE, OVERWRITE? (+NO/YES), SAVE?
      (+YES/NO), SAVING, LOAD MESSAGE and NO CONTROLLER; full table in FORMATS.md §10.10.
      **Every action pointer in them is `0x8001FD80` — `jr ra; nop`.** That is the design and not a gap: the
      front end hangs nothing off the item callbacks and reads the cursor instead. The one exception is
      SAVE?, whose YES at `0x80020428` enters page **39**, parks `0x8001EFDC` as the per-frame handler and
      sets the mode flag at `0x800B32A2` — which is what starts the machine.
      The machine polls a state through `0x800B3234` and dispatches a **19-entry jump table at
      `0x800AB734`**; fourteen entries are the common exit, and the five live arms are 3 (hand the chosen row
      to `0x800B324C`), 5 (row 1 → 6, row 0 → 1), 13 (compose the cheat-level name), 14/16 (accept: apply the
      variables and leave) and 19 (row 1 → 20). All five gate on the **release** of CROSS, tested inline
      against `0x800B3290`/`0x800B3298` rather than through the record's flag bit.
      Implemented in `src/menu/memcard.[ch]`. The card I/O itself is three function pointers into `libmcrd`
      and is exposed as a host interface rather than invented.
      *Also corrected in the same pass:* the selection bar's suppression test at `0x8001A7FC` compares
      against `0x800AE634`, which is the **NUL terminating the previous string** — so the comparand is `""`,
      not the word at `0x800AE638`. An earlier reading of this pass had it as the label `PAUSED`; it is
      "a row with no text draws no bar", which is exactly what SAVE FILE's four empty slots need.
  - [~] 20e. Residue: which of the nineteen states shows which of the nine screens. Only two are settled by
        the arms themselves — state 3 hands over a row and SAVE FILE is the only row list, state 13 composes
        text and LOAD MESSAGE is the only composed screen. *Attack:* xref each of the nine table installers
        and follow its callers back to the state that reaches it.
        **The ENTRY POINT is now known**, from retail capture: the front end's MULTIPLAYER page carries
        `LOAD SETTINGS` and `SAVE SETTINGS` as its last two items, and those are what open the card screens.
        The in-game `SAVE?` prompt at `0x8009B3B4` is a second way in. So the machine is reachable from the
        menus rather than only from a level-completion flow, which is what an earlier note guessed at.
- [x] **20b. The menu FONT. — SOLVED, and the menu now draws with it.**
      The face is in two files already on the disc, and the answer is one function: the text drawer at
      `0x8001ACDC` branches on the drawable's `size` field at `+0x46` for 8, 16 and 32. Size 8 is
      **`chars.lbm`** — the same atlas the HUD draws from, through the same glyph table at `0x8009D554` —
      and sizes 16 and 32 are **`frontend.lbm`**, registered into VRAM slot 13 at `0x8003FE74` and landing
      at (896, 256), a whole 4bpp texture page.
      The 16- and 32-pixel faces are **not** table-indexed: `0x8001B494` computes a cell, letters in rows of
      15 (size 16, cells 16 x 11, v origin 100) or 7 (size 32, cells 32 x 20, v origin 0), digits on row 3
      and punctuation on row 2 through a jump table at `0x800AB564`, with four size-32 overrides so the big
      face carries `2`, `3`, `4` and `?` past the letters. Colours are the executable's built-in palettes —
      68/70/71/72 by size and by the drawable's `+0x48` highlight flag — modulated by a flat 128, or 32
      under the `g` code.
      **What made the placement checkable is the wider finding underneath it:** the standalone-image slot
      tables at `0x800A3274`/`0x800A329C` are twenty entries wide and cover standalone images exactly as
      they cover texture pages, which FORMATS.md had recorded as "not established". `0x8006901C(name, slot,
      v_offset)` → `0x800691A8` builds RECT{slotX, slotY + v_offset, width>>1, height}, and every UI image
      the game owns is registered through it in one function at `0x8003FE20`.
      Implemented in `src/menu/menufont.[ch]`, `src/menu/menudraw.[ch]` and `q2_vram_upload_named`; the
      menu now emits POLY_FT4 glyphs, a POLY_G4 selection bar and a LINE_F2 slider frame into the ordering
      table at the buckets the original names, and the client links it into the overlay slice before
      composition rather than blitting over the finished frame. Full detail in FORMATS.md §10.7–10.9.
  - [x] 20c. **`qk_menu.lbm` is the STATUS BAR's sheet, and the bar is now reconstructed.**
        The sheet registered immediately after the font — slot 14, (960, 256), and chosen by session mode at
        `0x8003FEAC` between `qk_menu.lbm`, `qk2_menu.lbm` and `qkm_menu.lbm` — decodes to a grid of
        **32 x 24 item and weapon icons followed by a set of large digits**. The digits matter, because the
        HUD reconstruction's headline result is that this game has no status bar (§11.1), and that result
        was reached by enumerating format strings — which large *bitmap* numerals would not appear in.
        What is established: `0x80033320` draws one cell as a POLY_GT4 from a four-byte {u, v, w, h} record;
        the icon rects are a five-byte table at `0x8009C478` ({u, v, w, h, id}, w = 32 and h = 24 on every
        entry but the leading 1 x 1 blank); a second table sits at `0x8009C658`; and the two callers,
        `0x80035B38` and `0x80035EA0`, sit inside a composite that runs five sub-draws in a row
        (`0x800352C0`, `0x80035554`, `0x80035B38`, `0x800359C0`, `0x80035EA0`).
        **UPDATE — it is the STATUS BAR, and §11.1 is retracted.** Retail screenshots settle it: the console
        draws a persistent bottom-of-screen bar with health, ammo and armour in large numerals beside their
        icons, and a weapon strip along the bottom right. The five sub-draws match its five parts. The
        cautious note this item used to end with — "this does not overturn §11.1" — was wrong, and wrong in
        the instructive direction: §11.1's method enumerated format strings and font-table readers, and a bar
        built from pre-rendered numeral sprites uses neither, so the method could not have found it and its
        negative result was an artefact of the instrument.
        **The bar's DATA is now read and checked**, in `src/build/icontable.[ch]`:
        the rect table at `0x8009C478` is **fifty-seven** records, not the 96 an earlier guess took it to be
        — the run of valid rectangles ends at `0x8009C595` and what follows is not rectangles at all. Its
        geometry is a 32 x 24 grid, eight per row, seven rows, **and the rightmost cell of every row is 31
        wide rather than 32**: `u` is 224 there and 224 + 32 wraps to zero in the u8 a primitive carries, so
        the narrow column is deliberate and a grid check that does not know it rejects one cell per row.
        Also read: the weapon-to-ammo-icon map at `0x800ABE9C` and its companion at `0x800ABEA8`, twelve
        bytes each on the same 1-based weapon id space as the weapon tables, with id 0 selecting the 1 x 1
        blank; and the split-screen size reduction at `0x800353C8` — 32 x 24 in single player, **24 x 18**
        for two and **16 x 12** for three or more. `q2psx-inspect hud` checks all of it: 57 records, 56 on
        the grid over 7 rows, 1 blank, 0 bad.
        **SOLVED — and the composite is not a screen.** `0x800337D0` is the **per-viewport draw hook**, the
        function the one-player layout stores in the view record at `+308` (`0x80033D30` and `0x80034288`
        are the two splits). The sweep never named its caller because there is no caller to name: the
        screen calls it through that pointer, once per viewport. So the bar is drawn by the same call that
        draws a viewport's world, which is why it is anchored per viewport and why its cells shrink in
        split screen.
        Its anchor is **view+304 / view+306** — the two halfwords screen.h carried as `pad_a`/`pad_b`,
        unknown. Thirteen 10-byte field records at `0x800337EC` give every cell a literal offset from it,
        and sorted by x they read **three digits then an icon, three times**, digits 24 apart:
        A at −71/−47/−23 with its icon at +0, B at +64/+88/+112 with +135, C at +179/+203/+227 with +250.
        A thirteenth field at +330 is unidentified.
        The **numerals are at `0x8009C598`** — ten four-byte {u, v, w, h}, all 24 x 24 at v = 168, u = 24 *
        digit. That 24 is the same stride the field table uses, and the two were read independently.
        Retail capture gives the left-to-right order health, ammo, armour; the three groups are identical in
        the code, so that one fact is from the screenshot and is labelled so at the enum.
        Implemented in `src/game/statusbar.[ch]`, wired into the client's per-viewport draw and fed the
        sim's real health, armour and current-weapon ammo. Full detail in FORMATS.md §11.1.1.
  - [ ] 20f. Residue: **which rect is which item**, and **the quad layout's table.** Each record's fifth
        byte is an id whose value space is unidentified, so there is no name-to-rect vocabulary — health's
        and armour's icons are caller inputs rather than named constants. And the three draw hooks build
        three DIFFERENT field tables: the one-player one at `0x800337D0` (17 fields in two rows), the
        two-player one at `0x80033D30` (9 fields, two counters, digits 20 apart rather than 24), and the quad
        one at `0x80034288`.
        **All three are now read — SOLVED.** None of them is a table in the data segment, which is why a
        search for arrays of ten-byte records never found them: all three are built **inline on the stack**
        and copied into the view record. Evaluating the quad builder symbolically gives **sixteen** fields,
        and the shape says what it is — four counters of three digits and an icon, in **two rows**. In
        four-player split the bar is not per viewport: it carries every player's counters at once, two along
        the top and two along the bottom. The lower row's `dy` is `40 - screen_height`, taken from the live
        framebuffer height at `0x800B2DA2` rather than from a constant, so it follows the display mode. Digit
        pitch is 20, the two-player table's.
        Running the same extraction on `0x800337D0` re-derives the one-player table byte for byte, so that
        transcription is now confirmed from two directions rather than one. The four trailing bytes of every
        record are the same in all three layouts — a source rect of (255, 255) sized 1 x 1, the blank — and
        are initial values the draw overwrites, not layout.
        `src/game/statusbar.[ch]`, `q2_sbar_fields_4p`; `tests/test_statusbar.c`.
- [ ] 46. **The deathmatch scoreboard.** Capture shows a `DM SCORES` screen: a title on a **red** bar rather
      than the usual blue, one row per player carrying that player's own bar colour (RETRODAN red, PLAYER 1
      blue) with a name and a frag count, a `READY` marker on the left of a row that has pressed fire, a
      backdrop image, and a centred `ALL PLAYERS PRESS / FIRE TO CONTINUE` in the 16-pixel face. Every part
      of it is machinery this project already has — the bar, the two faces, the per-player colour — so what
      is missing is the screen's own table and flow, not anything that draws it.
      It also shows the title bar is **not always blue**, which §10.8's four-colour table already allows
      (red is index 1) but which nothing had confirmed for a title.
      **What this pass added.** The scoreboard is a **level**, the same way the front end is: level-table
      record 12 is `MPResults` -> `LEVELS/QMRESULT/`, on the disc, whose `ModelNames` are `Q2LOGO`, `q2title`
      and the four coloured player models `Male2`, `Male2aqua`, `Male2purple`, `Male2red`. That is a direct
      confirmation of §10.8's per-player colour table from a second, independent place — the four colours
      exist as four separate model assets — and it explains the backdrop the capture shows. The rows
      themselves are menu items over that scene, so the missing piece is the same run-time item construction
      as #44 rather than a table to transcribe.

---

## Tier 4 — Low-impact unknowns

- [~] 21. `CastList` blocks A, B, C and D — **structure now known, semantics not.** The load-time rescale at
      `0x8006C214` reveals all three of B, C and D:
      **B** (`+0x30`, never relocated) is 8 `u16` chain heads — which is what "exactly 16 zero bytes on
      821/965 models" always was, eight empty chains — with nodes `{u16 countAndFlags; u16 next; entry[count
      & 0x7F]}` and entries of two `u16`, both multiplied by **10** at load.
      **C** is a chain of records whose `s16` at `+0` is multiplied by **10** and whose word at `+4` is the
      byte delta to the next.
      **D** is 20-byte records ending at a zero word, with three `u16` at `+12`, `+14`, `+16` multiplied by
      **5**.
      The vertex array is NOT rescaled, so model vertices are already at world scale. Still open: what the
      values mean, block A's payloads, the animated-model frame layout, and the header's 24-bit field at
      `+0x01` (261…333367). **Block C is not a vertex-base candidate** (#2a).
- [ ] 22. `PrimaryRemap` value space. Definitively **not** a scene-node index — the max exceeds the scene
      node count in 100 of 115 files. Probably a polygon or surface id in a shared table.
- [~] 23. `CollNode` fields `c` and `d`. **`d` is SOLVED**: its low byte is the node's **contents id**,
      read with `lbu +32` at `0x80044DB8` — where a trace records it into its contact list whenever it
      changes along the path — and at `0x8004510C`. The other three bytes are zero on all 22,773 nodes.
      **`c` (+28) is HALF SOLVED, and the negative result that stood here was the clue.** It said no
      instruction in the image loads offset +28 of a collision node, which is true and stays true: the
      gather at `0x8006B0E4` loads offset **+30**. The field is two halfwords, and the high one — bytes
      +30..+31 — is the node's first index into `SpaceLights` (#9). Reading `c` as one 32-bit quantity is
      what made it look like noise; its "max 65,077,433" is `0x03E10039`, i.e. light index 993 beside a low
      half of 57.
      The **low** halfword at +28 is still unread, and no instruction loads it either. It is non-zero on
      8,968 of 9,083 primary records and 13,625 of 13,920 secondary ones, reaching 18,544 and 20,603. The
      obvious next test — that it is a second partition of the same shape into another chunk — is
      **REFUTED**: it is non-decreasing on only 30 and 15 of 115 zones respectively, where the light index
      is monotonic on all 115. Whatever it is, it is not a running offset.
- [ ] 24. `Resources` `unk0` (−3000…6600, 49 distinct) and `unk4` (40…180, 17 distinct); `unk3` (64, but 80
      in two records).
- [~] 25. `TrigBounds` trigger `id` (9…75 plus 255, where 255 is "none") and `flags` (14 distinct values up
      to 10240). **Three flag bits are now known**, read out of the contents test at `0x80050CE0` and the
      player integrator that consumes it: `0x0200` makes an entity **sink slowly** (vertical velocity eased
      toward +1024, decelerating by `dt*24` above it), `0x2000` makes it **buoyant** (eased toward −3072,
      plus a −9216 impulse when it is already on the ground — the water boost), and `0x1000` gates the
      entity's own `0x800` flag on `|vel.y| < 1024`. The mask the player passes is `0x3360`, so `0x0100`,
      `0x0040` and `0x0020` are also volume classes it cares about and their effects are not yet traced.
      The same test doubles as the entity-overlap query, which is how a trigger volume blocks movement.
- [x] 26. The five `Lights` style values (`(n<<3)|7` for n = 0…4). — **SOLVED. They are LENS FLARES, and the
      byte is three fields, not one.** The flare pass at `0x80075708` splits `type` as: bits 0–2 (always 7,
      never read), bits 3–5 the **flare style** 0…4, bits 6–7 a **size shift** giving a screen reach of
      `64 << k`. Style 0 draws nothing — the per-viewport pass at `0x80075AA4` masks the word at `light+16`
      with `0x3800` and skips the light outright — so a style-0 light lights the world and is never itself
      seen. Styles 1…4 index four NUL-terminated element lists at `0x800A1FDC`, `0x800A2014`, `0x800A2024`
      and `0x800A203C`, whose 8-byte records are `{kind, size, position, colour}` with the position a 1.12
      factor **along the line from the screen centre to the light** — which is what makes this a lens flare
      rather than a corona. The four lists are nested: style 2 is style 3's first element, style 3 is style
      4's first two, style 4 is style 1 less its last. Disc-wide: 6,183 lights at style 0, 660 at 1, 183 at
      2, 2 at 3, 786 at 4; every light on the disc has size shift 0.
      Both element kinds are untextured and additive — a Gouraud fan bright at the centre and black at the
      rim, plus eight Gouraud lines for the starburst — so the long search for a flare texture was looking
      for something that does not exist. Full detail in FORMATS.md §17.3; implemented in
      `src/game/flare.[ch]` and checked element-for-element against the executable by
      `q2psx-inspect lights`.
- [ ] 27. Pickup `flags` bits beyond 0, 1 and 8; and the pickup `extra` list's meaning (a consumer exists — a
      pointer to it is stored into the spawned entity's sub-structure — but the interpretation is unknown).
- [ ] 28. `Q2Level` `+0x1C` (constant `jr ra` address, no located reader — possibly the high half of an
      8-byte field whose low half is the runtime pointer at `+0x18`); the writers of the per-level
      `runtime[8]` state; and the `music_playlist` field's real meaning, given that **no instruction anywhere
      in the image loads offset `0x22` from a level record**.
- [x] 29. **~~`SNDVRAM` section A header bytes `0x0E` and `0x0F`, and the split.~~ — FULLY RESOLVED.**
      They are 4bpp CLUT counts (the fix-up loop at `0x800762B4` adds them, shifts left by 4 and sets the STP
      bit over exactly that many halfwords), and the **split is world versus models**: the world renderer
      indexes the CLUT array from zero, while the model emitter at `0x8006A3FC` adds `count_a` first, so a
      `CastList` face's `texture` byte addresses section B. Checked disc-wide — no world polygon indexes past
      its map's `count_a`, and none of the 138,290 model faces has `texture >= count_b` (max 180 against a
      largest `count_b` of 181). The engine using only the sum to size the upload is why the split looked
      vestigial from the data side.

---

## Tier 5 — Archival / other-build / process

- [ ] 30. **NTSC build values:** framebuffer height, `video_mode_const`, movie filename suffix, EXE hash,
      PVD fields. All must be **read**, never guessed — PAL turned out to be 512 × **248**, not the widely
      assumed 256, so the folklore 512 × 240 NTSC figure is *less* trustworthy now, not more.
- [~] 31. Real xrefs to the `.DAT` chunk-name literal pool at `0x800AD414` — **found, and the pool is not
      indexed.** The zone loader at `0x8007B3F8` names each chunk by materialising its 12-byte literal
      directly (`0x8007BA78` for `MapMod`, `0x8007BB74` for `Points`, and so on), copying it to the stack and
      calling the directory search at `0x8006DBC0`. There is no per-chunk flag word and no table walk, so
      required-vs-optional is expressed only by what the loader does with a failed lookup. Still open: that
      per-chunk failure handling, chunk by chunk. The same pass mapped each chunk to the global its pointer
      lands in — `Scene` `0x800B2C3C`, `MapMod` `0x800B2C6C`, `Points` `0x800B2CA0`, `SortData` `0x800B2C84`,
      `SpaceLights` `0x800B2ED0`, `AreaConx` `0x800B2D1C`, `PrimaryColl` `0x800B2E0C`, `MapNames`
      `0x800B2C9C` — which is what makes each chunk's consumer findable with `xrefs`.
- [ ] 32. Why `ModelNames` is present in all 49 `COMMON.DAT` files yet the string appears **zero** times in
      the 634,880-byte executable. Dead tool-only data, positional access, or a runtime-assembled name?
- [ ] 33. Why `TriggerRemap` and `SecondaryRem` exist in the executable but are emitted by no file on the
      disc. Cut features, or read from a source not on this disc — a parser should tolerate them appearing.
- [ ] 34. Why the zone directory order permutes the `SecondaryCol` / `PrimaryRemap` / `AreaConx` trio as a
      function of zone index. Perfect and exceptionless correlation; the build-tool mechanism is unexplained.
      Matters only as further proof that index-based chunk lookup is unsafe.
- [ ] 35. `MAP.ALL`'s purpose; its four header words at `+0x40` (three decode as plausible floats near
      −1.93…−1.97); and whether the 16-entry table length is right (unverifiable — N = 1). Almost certainly
      an editor leftover: the filename appears **zero** times in the EXE and the file's last 44 bytes are
      MSVC uninitialised-heap fill.
- [ ] 36. The unused 20-byte tail of every Form 2 payload, and the always-zero `uint16_t` at `+2` of each
      music table record. Both are zero in 100 % of samples — nothing can be inferred from this disc.
- [ ] 37. `GlintMod` (2608 bytes, one map only, high-entropy after the first few dozen bytes).
- [ ] 39. The HUD's residuals, none of them blocking, now that the overlay itself is done.
      **~~The MISSION / level-completion screen.~~ — RECONSTRUCTED, and it draws.**
      `0x80021ADC` builds it with `sprintf` into a scratch buffer at `0x800C6EE8` and hands each row to the
      text printf at `0x80043518`, into the **overlay camera's** context at `0x800D6870` rather than into any
      viewport. Its rows are three centred body lines from the runtime buffers at `0x8009B5E8`, `0x8009B608`
      and `0x8009B634` — starting at y = 36 and stepping **+10 then +8** — followed by a label column at
      `box_x + 20` reading `Location`, `Secrets`, `Kills` and, after a further gap, `Totals:`, with a value
      column to their right. The text box is `gp+352`…`gp+358`, and each body line is centred by the helper
      at `0x80022550` against `left` and `left + width`, measured with the HUD's own off-by-one measurer.
      **The format strings are markup, and that is the confirmation.** Every one begins `@%03X%2X` or
      `@%3X%2X` — the `@XXXYY` pen escape with its three hex digits of x and two of y, formatted in at run
      time. Finding the game assembling its own escapes with `sprintf` is independent evidence that the
      markup reading in §11.4 is right, since the formats only make sense if the interpreter reads them that
      way. Three colours carry the whole visual grammar: `BEF0E6` for labels and the title, `DCF082` for
      values, `C8F000` for totals. The title is `"Mission  %d  -  Complete"` (`0x800AB9DC`).
      **A bound that falls out of the formats.** Label rows use `@%03X`, value rows use `@%3X`. The escape
      reads exactly three characters as hex, so the unpadded form only produces three hex digits when
      x >= 0x100 — below that `sprintf` pads with a space, the space is eaten as part of the field, and the
      pen lands elsewhere. Using the plain form for every value row and the padded form for every label row
      is only consistent if **the value column sits at x >= 256**. That is a proven interval, not taste, and
      the port's constant is named for it.
      Implemented in `src/game/mission.[ch]`. The counters are inputs rather than something the module
      invents, because kills and secrets are simulation state the sim does not yet tally — a caller that has
      counted nothing gets a screen saying zero rather than a screen that lies.
  - [ ] 39a. Residue: the y step between the label rows and the value column's exact x. The rows are emitted
        with individually incremented values rather than from a table, so pinning each needs another pass
        over `0x80021CC0`…`0x80021ECC`. Both are marked `_UNREAD` at every use.
      **~~The split-screen overlay.~~ — RESOLVED, and there is no split-screen overlay.**
      `multipics.lbm` and `multipic2.lbm` are not overlay art. Decoded, they are **deathmatch map preview
      thumbnails** — ten in the first and two in the second, in a 2 x 5 grid of roughly 128 x 51 texels — and
      they live in `QFRONT` alone, not in any playable map. They belong to the front end's multiplayer map
      select, which is a screen this port does not have, and nothing in the in-game overlay draws them.
      The same pass decoded their two neighbours, which are the same kind of thing: `control.lbm` and
      `mouse.lbm` are **controller diagrams with callout lines** — a DualShock over a standard pad, and the
      same pad over a mouse — for the CONTROLLER page to label. Also `QFRONT`-only.
      What made all four readable is that they are **8bpp**, which nothing had established. The geometry
      forces it: the upload rect is `width >> 1` halfwords, a texture page is 64 halfwords, and a primitive's
      `u` is eight bits, so an image 128 halfwords across is 512 texels at 4bpp and cannot fit a page, while
      at 8bpp it is 256 and fits exactly. Decoding confirms it — at 8bpp `multipics.lbm` is ten recognisable
      screenshots and `control.lbm` is a photograph of a controller; at 4bpp both are noise. Depth is
      therefore **per image**, not global: the two font atlases and the icon sheet are 4bpp and the front
      end's photographic art is 8bpp. The 256-entry CLUT the section carries per standalone image cannot tell
      them apart — it is fully populated on all of them, `chars.lbm` included.
      Recorded as `q2_vram_ui_images` in `src/formats/vram.[ch]`, together with the complete slot map from
      `0x8003FE20` (thirty images, and slot 4 is reused by four of them, which is why name resolution matters
      here for the same reason it does for texture pages).
      So what remains of split screen in the overlay is what was already known and already implemented: the
      notification layer narrows to 2 / 1 / 1 lines by player count. There is nothing else to find.
      **Three orphan words.** The icon vocabulary carries `was`, `die` and `door`, and no located string uses
      any of them — the shape of a frag-message template (`&P was &O`) that was cut.

---

## UI conformance against retail capture

Twelve screenshots of the retail PAL game were compared against the reconstruction. This is the first time
any of it has been checked against the running console rather than against the executable alone, and it
moved three things.

**Confirmed by the capture, having been derived only from the code:**

| what | evidence |
| --- | --- |
| the three faces and their letterforms | every screen's text is the reconstruction's `chars.lbm` / `frontend.lbm` faces |
| the full-width gradient selection bar, white text on it | OPTIONS, PLAYER, SOUND, START, SAVE?, CREDITS |
| the slider: white bar when selected, blue when not | SOUND — MUSIC selected and white, SOUND FX blue |
| the toggle: `LABEL ON OFF`, live value bright, other dim | PLAYER — `CROSSHAIR ON OFF` |
| greying takes a row out of the navigation | START's `MULTI PLAYER`, CONTROLS' `SWAP Y OFF` |
| the PLAYER page's items and rows | CROSSHAIR / AUTOCENTRE / CONTROLLER / RESET TO DEFAULTS, exactly the transcribed y values |
| the memory-card SAVE? screen | question, YES on the bar, NO below — the transcribed table, row for row |
| the MISSION screen as a six-row table | `Location` / `Secrets` / `Kills` header on ONE row, columns at +0 / +176 / +256 |
| the 56-pixel centred value fields | `4/4`, `20/20`, `99/100` land where `0x8002260C`'s odd/even rule puts them |
| `control.lbm` is a controller diagram with callouts | the CONTROLS page draws exactly the decoded image |

**Corrected by the capture:**

1. **There IS a status bar** — health, ammo and armour in large numerals beside icons, plus a weapon strip.
   §11.1 is retracted; see #20c. This is the single biggest error the project had.
2. **The page title gets a selection bar too**, not just the cursor row. It follows from the rule already
   read (`+0x48` set, text non-empty) but had been implemented as a special case that excluded the title.
   CREDITS and every OPTIONS-family screen show it.
3. **The `%3X` reading was backwards.** An earlier pass here inferred from the unpadded format that the
   MISSION value columns had to sit at x >= 0x100. They do not — the capture shows the Location column near
   x = 104 — so Sony's `sprintf` is not space-padding and the inference was unsound. The port emits padded
   digits, which reaches the same pen position without depending on the libc.

**What the capture made visible, and what became of it.** Five items were opened here. Three are now closed
and one is half closed; the two that remain both reduce to the same thing — the front end builds its menu
item records at run time instead of transcribing a table, so there is nothing to read out.

- [x] 41. **The UI panel frame — SOLVED.** It is `frontend.lbm` after all, in the strip at rows 223-255 that
      the menu font's tables never reach. `0x8003F0E4` lays down two black semi-transparent tiles (a quarter
      of the scene shows through, because it draws the same tile twice at 50%) and `0x8003E8D0` builds the
      border out of eight `POLY_FT4` cut from one corner, one horizontal edge and one vertical edge -
      mirrored by writing the screen corners backwards against ascending UVs, not by flipping the UVs. It
      overhangs its rectangle by 10 and 6. FORMATS §11.10; `src/menu/panel.[ch]`; `tests/test_panel.c`.

- [~] 42. **The button prompts — the art and the table SOLVED, the per-screen placement not.** The reason a
      string sweep never found `BACK` is that there is no such string: each prompt is one 76 x 16
      pre-rendered cell in `frontend.lbm`, glyph and word together, drawn as a single quad. The table is
      three 18-byte records at `0x8009B4D8` giving uv, size, x and a target/current y pair; the three sit at
      the quarter marks of the 512-pixel screen, and they **slide** three pixels a frame towards the target.
      `q2_menu_open` parks all three below the screen on every page open, so a screen that wants a prompt has
      to ask again. FORMATS §11.11; `src/menu/prompt.[ch]`.
      *Still open:* which screen asks for which prompt at which y — every caller but the page open reaches
      the setter through the front end's function-pointer table at `0x80079ECC`, which is #44.

- [x] 43. **The briefing screen — SOLVED.** `0x800215A0`. It is a sibling of the MISSION screen only in that
      it hands one markup string to the same printf: MISSION positions every run with an explicit `@XXXYY`
      pen escape because it is a table, and the briefing sets margins once and writes flowing text because it
      is prose. Box (96, 32, 336, 100) from `gp+300..306`, inside the §11.10 panel. Its colours are written
      into the text context rather than escaped into the string, and its `#000000` at the end clears the wrap
      flag so the next screen is not still wrapping at 406.
      The three fields come from the map's own `Strings` chunk, which this pass also decoded — see the note
      under #45. FORMATS §11.12; `src/game/briefing.[ch]`; `q2psx-inspect text <map>`.

- [ ] 44. **The front end's own menus.** A separate page set from the in-game pause menu — the in-game
      OPTIONS at `0x8009AB14` has three items and no credits entry. Capture shows at least:
      the title screen (START / OPTIONS); START -> SINGLE PLAYER / MULTI PLAYER; OPTIONS -> PLAYER / SOUND /
      VIDEO OPTIONS / **VIEW CREDITS**; MULTIPLAYER -> DEATHMATCH / TEAM DEATHMATCH / VERSUS /
      **LOAD SETTINGS / SAVE SETTINGS** (the memory-card entry, #20e); the DEATHMATCH setup with a
      `2 3 4 PLAYERS` choice row, a map name, a **preview thumbnail from `multipics.lbm`**, FRAG LIMIT,
      TIME LIMIT, GAME VARIABLES and PROCEED; a versus player-setup screen with a **blue START bar for
      player 1 and a RED one for player 2**; and CREDITS.
      Three things this confirms rather than adds: `multipics.lbm` really is the deathmatch map previews
      (#39's retraction), the bar colour really is per player with 0 = blue and 1 = red (§10.8's table), and
      the choice widget really does render its live value bright with the alternatives dim.
      The front end reuses the same font, bar, slider, toggle and choice, so **only its tables are missing**
      — everything that draws them is already reconstructed.
      **What this pass added.** The front end is page **46**, entered at `0x80079364`, and it is special-cased
      inside `q2_menu_open` itself (`0x8001A40C`). Its own screens are a **level**: the level table's record 0
      is `QFront` -> `LEVELS/QFRONT/`, which is on the disc and whose `ModelNames` are `Q2LOGO`, `q2title`,
      `q2logowire`, `joypadwire`, `Quaddamage` and the four coloured player models `Male2`, `Male2aqua`,
      `Male2purple`, `Male2red`. So the title screen is a rendered scene with menu text over it, not a page of
      art — which is why no static page table for it exists in the executable. Page 46 installs exactly **two**
      item records (`0x800A3314` and `0x800A3344`, filled at run time), matching the capture's START / OPTIONS.
      A dev-time path `LEVELS\TITLE\` at `0x800AD090` is a leftover: no such directory ships.
      *Still open:* the deeper pages' item records, which are built at run time by front-end code rather than
      transcribed from a table.

- [x] 45. **Word wrap in practice — SOLVED, and it was the wrong screen.** The wrap the capture shows is the
      **briefing's**, not the MISSION screen's: `#06A196` at `0x800AE740` sets margins 106 and 406, which is
      the only place in the game that turns the flag on, and `#000000` at `0x800AE758` clears it again. The
      MISSION screen genuinely does not set margins — it is a table and positions every field itself. The
      port's `#` escape was already correct; what was missing was a caller.
      Along the way the **`Strings` chunk** turned out to be the text database the wrap operates on. It had
      been catalogued (FORMATS §2.8) but never decoded: it is a `{char key[12]; u32 offset}` dictionary read
      by the name lookup at `0x800701B4`, 49/49 decode, 360 entries, 34 maps with real text. `MapTitle` is
      the briefing's `Location:`, `Unit<N>Miss1` its `Mission Objective:` and `Unit<N>Curr<S>` its `Current
      Orders:` — and the step is **hexadecimal**, because SECURITY runs `Unit2Curr1`..`Unit2CurrA`.
      `src/formats/leveltext.[ch]`; `tests/test_leveltext.c`; `q2psx-inspect text`.

---

## In-game conformance against retail capture

The UI section above compared twelve menu screenshots. This is the first comparison against a frame of the
game being **played** — the BASE0 spawn, looking down the opening canyon, blaster in hand — and it settles
two things that the executable alone could not, and opens one.

**The picture is 4:3.** The capture is pillarboxed inside a 16:9 frame with the game filling a 4:3
rectangle, so the 512 × 248 buffer is presented at 1.333:1 and a framebuffer pixel is about 0.646 as wide as
it is tall. Nothing in the executable says this — it is what the display does with what the GPU emits — and
the port had been showing the buffer one pixel per window pixel, which is a **1.5× horizontal stretch**.
`Q2_SCREEN_FIT_FULL_4_3` is now the default and the client letterboxes or pillarboxes into any window;
`Q2_SCREEN_FIT_TELEVISION` keeps the stricter hardware reading (all five horizontal modes span the same
active line, PAL fills the 4:3 raster with 256 of them, so a pixel is exactly 2:3 and the 248 drawn lines
come out at 1.376:1). The two differ by 3%.

Two independent pieces of the game's own art agree with a pixel narrower than it is tall, which is worth
recording because it is evidence internal to the disc: the `qk_menu.lbm` menu faces and the 8 × 8 `chars.lbm`
HUD face are both drawn texel-for-pixel into square rectangles and are both authored about 1.35× wider than
tall — a normal letterform only once the display has narrowed the pixels.

**The world's field of view is right.** With `proj` 160 over the 512 × 248 viewport the reconstruction puts
the canyon's sky wedge at 0.37–0.53 of the picture width against the capture's 0.33–0.47 — the same 0.16
width, offset by a slightly different yaw. So the one-player layout's 116.0° × 75.6° is confirmed against the
running game, and with it the reading that `view+262` is `SetGeomScreen`'s argument.

That also means **the console's own picture is anamorphic**, by exactly 1.5. The GTE has one projection
distance and it reaches SX and SY alike, so the frustum is symmetric in framebuffer pixels while the display
is 1.333:1. There is no term anywhere that undoes it: the only matrix scale on the world's transform chain is
the uniform `(768, 768, 768)` at `0x800AEB30`, applied per column by `0x80055AF8` — an object scale of
exactly 3 — and the view weapon's own chain (`RotMatrix` at `0x8004F464`, `MulMatrix` at `0x8004F474`) has no
scale call at all. The squeeze is the game's, not the reconstruction's, and must not be "corrected".

- [ ] 46. **The view weapon sits too far left, and it is not the projection.** Placed through
      `q2_vw_place` at the BASE0 spawn, the blaster's drawn geometry spans x 278…376 of the 512-wide
      viewport — 0.54…0.73 across. The capture has it running from about 0.79 to off the right edge, with
      the forearm entering from the bottom-right corner rather than from the bottom centre. Vertical
      placement and apparent size are close (the port's y 143…258 against a capture top edge at ≈0.62 of
      the picture height); it is the horizontal offset that is wrong, by roughly a quarter of the screen.

      What has been ruled out. The **projection** is shared with the world, which the sky-wedge measurement
      above confirms is right, and the world and the weapon go through the same `SetGeomScreen` /
      `SetGeomOffset` pair — the eleven `SetGeomScreen` call sites are all viewport or bring-up code, none
      of them in the weapon's `0x8004Fxxx` range, and the only inline writes to `OFX`/`OFY` are at
      `0x80065C54` and `0x80065E0C`, in the world renderer's own displaced-centre path and its restore.
      A **scale** is ruled out by the matrix chain above. The `286 - viewOffset` eye base is confirmed at
      `0x8004F608` and the drop with a crouch measures 290 as it should.

      What is left is the translation itself: `q2_vw_place` puts the grip 140 right, 160 down and 44 forward
      of the eye in view space at rest, and something about that triple — which key the idle clip rests on,
      how `cur_t` interpolates into it, or whether a per-part offset is being dropped — is short by about
      the same quarter screen. `0x8004F5E0`'s operands are the thing to read next.

---

## Putting the creatures in the client

Every piece of the creature chain existed and nothing joined them up. The modules relocate, decode and bind;
Population's records spawn; the AI runs — and the only caller was the inspector, which decodes a module to
report on it and draws a creature standing still at its spawn point. A level in the client was its geometry,
its items, and nothing that moves. `src/game/creworld.[ch]` is the join, and making it work turned up three
things worth recording and left two open.

**Three numbers name a creature and they are not the same number.** A Population spawn record carries a
**class id** 0..37; the executable's class table turns that into a **name** and a health, and names are not
unique — ids 18, 19 and 20 are all `Soldier` with 30, 20 and 40 health; and a module serves one or more
**class bytes** 64..94, the Soldier's being 87, 89 and 88 *in that order*. The module is found by matching its
header name against the class table's name, which is the direction the engine's own lookup runs. The variant
is taken from the id's ordinal among the entries sharing the name — three ids, three class bytes, in the order
both tables state them. That last correspondence is inferred from the two tables agreeing in length and order,
not read out of code, and it is the one inference in the module.

**`q2_creature_spawn` overwrites `class_id` with the class BYTE**, because that byte is what the runtime bind
is keyed on. After spawning there is nothing left on the monster that indexes the class table, so anything a
caller wants out of that table — the model name above all — has to be taken while it is still there.

**A creature's line of sight runs through `PrimaryColl`, not `SecondaryCol`.** `SecondaryCol` is `PrimaryColl`
eroded by the *player's* 286-unit half-extent, and a Population spawn point sits in exactly the part the
erosion cuts away: look a creature up in it and it lands outside every cell, so every trace from it fails and
every creature on every map looks straight through the player. In `PrimaryColl` they are inside a cell and
sight works — on BASE1, twenty creatures, sixteen of which acquire the player with no walls in the way and two
across the real geometry.

**Population is per MAP and a session is in one ZONE**, and a spawn record carries no zone field, so the test
has to be geometric: a creature inside no cell of this zone's hull belongs to another one. It is not a small
correction — **twelve of BASE1's twenty**, seventeen of BASE2's twenty-eight and seven of COMMAND's eight
stand in another zone's rooms, and without the test they think, are drawn and are shootable through the void.
Single-zone BASE0 loses none of its ten, which is what says the test is measuring the right thing.

- [ ] 47. **Which CastList clip a creature's move plays.** A module's moves are numbered in one global frame
      timeline — the Soldier's run 0..474, and `q2psx-inspect creatures` now prints every move's range —
      while its model carries a list of clips, 31 of them for the Soldier. Those clips are **not** that
      timeline laid end to end: they total 434 frames against the module's 474, and there are 31 of them to
      the module's 18 moves.
      What they are is the **moves themselves**. Every one of the Soldier's clip lengths is exactly three
      ticks per frame times some move's length, and clips 1..4 are the four consecutive moves 50-54, 55-61,
      62-79 and 80-96 *in order*. So the tick rate inside a clip is 3 — not `Q2_MODEL_TICKS_PER_FRAME`'s 10,
      which is the view weapon's — and `q2psx-inspect model <map> <name>` now prints every clip's length so
      the arithmetic can be checked on any creature.
      *Open:* the index. Clip order is not move order (clip 0 is the 36-frame move 272-307) and it is not
      frame order either. The port matches a move to a clip **by length**, first match wins, which is right
      whenever the length is unique and can pick the wrong animation of the right duration when it is not.
      What is missing is the engine's own selector, which will be wherever the creature draw turns an
      entity's frame into a pose.

- [x] 48. **A creature's movement has no hull to run against — SOLVED, and it was never about the hull.**
      The symptom was that a creature acquires the player, animates, and moves zero units for as long as you
      care to watch. The diagnosis blamed the erosion: a box move wants a hull eroded by the MOVER's own
      extent and the disc ships exactly one erosion, the player's.
      That was wrong, and the thing that settled it was counting rather than reasoning. Instrumenting
      `q2_ai_world_bind` says **1052 of 1052 traces** on BASE1 took the "could not place the start" arm, and
      **84 of 84** ground probes reported a drop — under creatures standing on a floor. Numbers that
      absolute are not geometry.
      `q2_coll_move` **returns false when the move was STOPPED**, not when it could not begin
      (collision.h §0x80044C44); `out_pos` is filled in either way and `out_node` is -1 only when the walk
      never found a cell at all. A stopped move is the normal and useful answer for a walker's step trace,
      which exists precisely to be stopped by the floor. The binding read that false as
      `startsolid`/`allsolid`, and `SV_movestep` bails on `allsolid` before it ever looks at the fraction —
      so every creature in the game was told it was buried in the floor it was standing on. `check_bottom`
      had the same inversion: a probe that is stopped has FOUND ground, and it was reporting failure.
      With the contract read correctly the same BASE1 run goes to **1 unplaced trace of 30, no ground-probe
      failures, and 6,522 units of creature movement**; BASE2 and COMMAND move too. And the hull question
      answers itself in passing: `PrimaryColl` is right for all three questions — with `SecondaryCol` it is
      214 of 214 unplaced and 412 of 432 sight lines blocked, because that is the hull a creature genuinely
      is outside.
      `src/game/aiworld.[ch]`; the counters are kept, and the client prints them next to every capture.

---

## ⚠ Security note (carried forward, do not drop)

- [ ] 38. A prior research pass reported that a fan wiki page about this game served content containing
      **instructions addressed to AI agents** (create files, transfer funds, insult the operator, terminate
      operations). That URL was **not** fetched during verification. **Treat it as hostile for any automated
      fetch**; if data from it is wanted, a human should open it in a browser.
      Consequently, all web-sourced claims in the release census — the existence of the NTSC SKU, its
      timestamps and track lengths, barcodes, magazine demo-disc serials, and the absence of a Japanese
      release — remain **unverified** and should be treated as moderate confidence at best.
