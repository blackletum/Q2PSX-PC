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

- [x] **13. Which in-game situation selects which music id. — SOLVED. It is the level's own playlist, and
      the id names a file and a channel.** Two tables, and neither had been read.
      The id at `gp+18524` (`0x800B2E5C`) indexes a **22-record, 6-byte table at `0x800A1DD8`**, with the
      stride spelled out at `0x80071760` as `(id*2 + id)*2`: `+0` is a signed file index into the five
      `QUAKE_x.XAI` names at `0x800A1E5C` — negative meaning silence, which is what ids 0 and 1 hold and
      which `0x80071778`'s `bltz` branches on — `+1` is the channel, and `+4` is the duration in **tenths of
      a second**. That last is checked rather than asserted: `q2psx-inspect music` demultiplexes and measures
      every stream on the disc and **19 of 20 agree exactly**, the exception being id 13 (QUAKE_C channel 3),
      whose table value is 175.1 s against a measured 176.1 — one second short, not a scale error, and worth
      leaving visible. Ids 2..21 are therefore the twenty (file, channel) pairs in file-major order.
      What WRITES the id is the cursor walk at `0x80071A68`, and the list it walks is in the **level table**:
      `p = cursor; cursor = p+1; v = *cursor`, where zero ends the list and a NEGATIVE byte is a relative
      jump back. A level record's `+0x22..+0x28` are seven track ids and `+0x29` is `0xF9` — minus seven,
      landing back on `+0x22` — so every level loops a seven-track playlist. The `s32` "always `0xF900 | n`"
      that leveltable.h recorded at `+0x28` was that seventh id, the loop byte and two zeros read as one
      word; and the "+0x22 varies without an obvious pattern" was the playlist's lead track. The seven are a
      lead and then six consecutive from lead + 3, wrapped into 2..17 — BASE0 is 14, 17, 2, 3, 4, 5, 6 —
      and ids 18..21, QUAKE_E's four channels, are in no level's list at all.
      Two records are not seven-and-loop and are what make the walk worth implementing rather than
      hard-coding: **QFRONT is one id then `-1`**, a single track looping for the title screen, and
      **MAGDEMO is four then `-4`**.
      `src/build/musictable.[ch]`, `q2_level_playlist_next`; the client plays its map's playlist and
      advances on end of stream.

- [x] **13a. (was 13) What, other than a stream ending, moves the playlist cursor. — SOLVED: nothing does.**
      The cursor at `gp+1536` has exactly **two** writers and both are now read.
      `0x80071B34` is the entry point: it clears `0x800B2710`, sets the music-enabled flag at `gp+1532`, and
      parks the cursor at `list - 1` — one before the first id, which is what the walk's pre-increment
      wants. It has a **single caller**, `0x800796C8`, and that caller passes `*(gp+18832) + 34`.
      `gp+18832` is the current level record (written at `0x8007C584`, at the head of the name lookup that
      resolves a level), and 34 is `0x22`. So the only thing that starts a playlist is a level load, and the
      list it starts is the level record's own — which corroborates the `+0x22` reading from the CONSUMER
      side, independently of the data's shape.
      The other writer is the walk itself at `0x80071A68`, and it runs when a stream ends. A zero byte ends
      the list and calls `0x80071B6C`, which stops the CD; a negative byte jumps back and loops.
      So there is no scripted music change: a level's seven tracks cycle for as long as the level lasts, and
      nothing in a boss room or a set piece can jump the cursor. That is a negative result about the game's
      design, not about the search.
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
      item records (`0x800A3314` and `0x800A3344`, filled at run time).
      A dev-time path `LEVELS\TITLE\` at `0x800AD090` is a leftover: no such directory ships.

      **Correction: those two records are not START and OPTIONS.** This entry read them as matching the
      capture, and the bytes say otherwise. `0x800A3314` holds `{ char *text; s16 x; s16 y; }` = `0x800AECC8`,
      256, 124 — and `0x800AECC8` is the string **`LOADING`**, centred. `0x800A3344` is all zeros, and
      `0x8001A474`'s own `lw v0, 0(s3); beq v0, zero` skips a record whose text pointer is null, so the
      second call installs nothing at all at that moment. What `0x80079364` sets up is therefore the front
      end's **loading screen**, shown while `LEVELS/QFRONT/` streams in — which fits: it is the first thing
      `q2_menu_open`'s special case does, before the level that the title screen is drawn over exists.
      **And the front end's own code is not in the executable at all — it is `QFRONT`'s `LevelBin`.**
      That is why every sweep for START and OPTIONS failed, and why the item records are "filled at run
      time": the thing filling them is a relocatable module, exactly as `QMULTI.C` is for deathmatch. This
      entry already had the piece that gives it away and did not follow it — record 0 of the level table is
      `QFront` -> `LEVELS/QFRONT/` — and QFRONT ships a **118,216-byte `LevelBin`**, by far the largest
      chunk in it, against 13,008 bytes of `LevelRel`. Its `Strings` chunk is a red herring: three
      placeholders, `sTRING`, `Another String` and a lorem-ipsum wrap test.
      The module's text pool sits at its very front, before the code, and it is the whole front end —
      `q2psx-inspect modstrings QFRONT` lists 349 runs and the first eighty are the menus:

          START / OPTIONS · SINGLE PLAYER / MULTI PLAYER · NEW GAME / LOAD GAME
          EASY / MEDIUM / HARD · PLAYER OPTIONS / SOUND OPTIONS / VIDEO OPTIONS / VIEW CREDITS
          CROSSHAIR · AUTOCENTRE · CONTROLLER · RESET TO DEFAULTS
          "      MUSIC" / "   SOUND FX" · STEREO · HORIZONTAL SPLIT · SCREEN POSITION
          DEATHMATCH / TEAM DEATHMATCH / VERSUS · LOAD SETTINGS / SAVE SETTINGS
          "2 3 4 PLAYERS" · "TIME LIMIT   10" · "FRAG LIMIT   10" · GAME VARIABLES
          GRAVITY · FALLING DAMAGE · WEAPON STAY · ONE SHOT KILL · GAME SPEED · BLAST FORCE
          INFINITE AMMO · ALL WEAPONS · RULES, and the five modes' rule paragraphs in full

      Two things fall out of the list that the capture could not show. The **leading spaces are the layout** —
      `"      MUSIC"`, `"   SOUND FX"`, `"    GRAVITY"`, `" GAME SPEED"` are padded to right-align against
      their sliders, which is why the in-game SOUND page's transcription needed no such padding and the
      front end's does. And there are **five** deathmatch rule paragraphs, not three: alongside the three
      selectable modes the capture shows, the pool carries full rules for a flag-capture mode and a
      last-one-standing mode, matching the six modes `QMULTI.C` implements of which three are selectable
      (#0). BRONZE cheats have an unlock file, and there is a `DEMO OF GAME` / `STARTING` pair for the
      attract loop.
      `q2psx-inspect modstrings <map> [crea]` is the reader; BASE0's `LevelBin` yields five runs against
      QFRONT's 349, which is the contrast that says the scan is finding a pool rather than manufacturing one.
      **And the item records are not built at run time either — they are a static array in the module,**
      which `modxrefs` finds by asking what points at a string. Every one of the eight checked has exactly
      one word reference, and it is the record's own first field. The layout is the executable's own, 24
      bytes: `{ char *text; s16 x; s16 y; void (*action)(void); ... }`, so the engine's `0x8001A474` takes a
      module's record and the executable's without knowing the difference.

          module+0x0EC3C  START            256, 151  -> module+0xCDC4
          module+0x0EC54  OPTIONS          256, 177  -> module+0xCCA4
          module+0x0EC84  SINGLE PLAYER    256, 111  -> module+0xCD40
          module+0x0EC9C  MULTI PLAYER     256, 137  -> module+0xCF68
          module+0x0ED44  PLAYER OPTIONS   256,  85  -> module+0xCADC
          module+0x0ED5C  SOUND OPTIONS    256, 111  -> module+0xCB74
          module+0x0ED74  VIDEO OPTIONS    256, 137  -> module+0xCC0C
          module+0x0ED8C  VIEW CREDITS     256, 163  -> module+0x35C8

      Two things that are layout rather than data. Every row is **centred at x = 256** and the pitch is
      **26 pixels** — 85, 111, 137, 163 on the OPTIONS page, 111 and 137 on START's, 151 and 177 on the
      title's. And the title page's two rows sit *lower* than any sub-page's first row, which is the space
      the `Q2LOGO` model occupies above them: the title screen is a rendered scene with two lines of menu
      over it, as this entry said, and now the two lines have coordinates.
      **Three more pages came out of the same walk**, and every string in them has exactly one word
      reference, so the arrays are contiguous 24-byte records like the first three:

          module+0x0EF9C  NEW GAME        256, 111  -> module+0xD0AC
          module+0x0EFB4  LOAD GAME       256, 137  -> module+0xD400
          module+0x0EFE4  EASY            256,  98  -> module+0xD380
          module+0x0EFFC  MEDIUM          256, 124  -> module+0xD3A8
          module+0x0F014  HARD            256, 150  -> module+0xD3D4
          module+0x0F104  DEATHMATCH      256,  80  -> module+0x4AD8
          module+0x0F11C  TEAM DEATHMATCH 256, 102  -> module+0x4AD8
          module+0x0F134  VERSUS          256, 124  -> module+0x4AD8
          module+0x0F14C  LOAD SETTINGS   256, 146  -> module+0xD148
          module+0x0F164  SAVE SETTINGS   256, 168  -> module+0xD1FC

      Two things in there are not guessable from the capture. **The five-row page is tightened to a
      22-pixel pitch** — 80, 102, 124, 146, 168 — where every two-, three- and four-row page in the front
      end is 26. And **the three deathmatch modes share one action**, `module+0x4AD8`, so the mode is
      decided by which row is on rather than by three handlers, which is what `QMULTI.C` wants: it
      implements six modes of which three are selectable (#0).
      The flow they describe is START -> SINGLE PLAYER -> NEW GAME -> a difficulty, and only the difficulty
      begins the game — the port follows it, and hands the chosen skill to `q2_cre_set_skill` before the
      level loads so the creatures that load already have it.
      **The deathmatch SETUP page is the last one the capture shows, and its rows are not widgets** — this
      entry guessed they were and that was wrong. `module+0x0F914` is six of exactly the same bare 24-byte
      records, with bytes +8 onward all zero: no widget field, no setting index, no bound variable.

          module+0x0F914  "2 3 4 PLAYERS"    256,  64   no action
          module+0x0F92C  (filled at run time) 256, 81  no action
          module+0x0F944  "TIME LIMIT   10"  256, 155   no action
          module+0x0F95C  "FRAG LIMIT   10"  256, 172   no action
          module+0x0F974  "GAME VARIABLES"   256, 189  -> module+0x49F8
          module+0x0F98C  "PROCEED"          256, 206  -> module+0x59CC

      **The values are in the TEXT.** The pool holds `"TIME LIMIT   10"` and `"FRAG LIMIT   10"` with the
      number padded into the string, and the module rewrites it in place — the module image is RAM. That is
      the same device as the leading spaces on `"      MUSIC"` and `"    GRAVITY"`, which right-align against
      their sliders: this front end lays out with padding rather than with fields, which is why none of its
      records needs a widget.
      The row at y = 81 carries a short string filled at run time — the chosen map's name — and the
      74-pixel gap below it is where the capture's `multipics.lbm` preview goes.
      **The padding and the x coordinate are the same fact.** Following the second word reference that
      `2 3 4 PLAYERS` and `GAME VARIABLES` each carry reaches the VERSUS setup at `module+0x0F9BC` and the
      GAME VARIABLES arrays at `module+0x0F194`, and the variables rows are the first in this whole front
      end that are **not centred at x = 256**:

          module+0x0F194  "    GRAVITY"        x 168, y  97
          module+0x0F1AC  "FALLING DAMAGE"     x 256, y 124
          module+0x0F1C4  "RESET TO DEFAULTS"  x 256, y 151

      The rows at 168 are exactly the ones whose strings are PADDED — `"    GRAVITY"`, `" GAME SPEED"`, and
      on the SOUND page `"      MUSIC"` and `"   SOUND FX"` — and the rows at 256 are exactly the ones that
      are not. So the padding is not decoration: a padded label is left-anchored at 168 so its text ends
      where the slider begins, and an unpadded one is centred at 256 because it has no slider to meet.
      That is one rule covering both observations, and it is checkable on every row in the pool.
      There are also **several variables arrays**, not one — `"    GRAVITY"` appears at `+0x0F194` (y 97)
      and again at `+0x0F1F4` (y 76) with different neighbours — which is the same shape as the in-game
      VARIABLES page's four cheat-level variants at `0x8009A6C4` (§10). Eight toggles spread across them.
      VERSUS at `module+0x0F9BC` is four rows: the player count at y 63, the run-time map name at 82,
      a third string at 153 and GAME VARIABLES at 172.
      *Still open:* which variables array goes with which cheat level, and what each `action` does beyond
      the page it opens.

      *Also still open: the SCENE the title is drawn over,* and the reason is worth stating so nobody
      invents it. QFRONT's world is two nodes and eight vertices — there is no room in it for a title
      screen. The picture is its `ModelNames`: `Q2LOGO`, `q2title`, `q2logowire`, `joypadwire`,
      `Quaddamage` and the four coloured player models. Every one of them is authored **centred on its own
      origin** — `Q2LOGO`'s posed bounds are `[-725 -916 -80] .. [727 1081 81]` — and QFRONT's single
      `StartPos` puts the eye at the world origin, so drawing them where they sit would put the camera
      inside the logo. Their placement is the module's, and until it is read there is no honest position to
      draw them at. The port therefore shows the menu over an empty scene rather than a guessed one.

      **The thread to pull is the module's engine vtable**, and it is worth writing down because every
      `LevelBin` reaches the engine the same way — `QMULTI.C` included. A module holds the block at its own
      `+0x8`, the loader writes it, and the installer that fills it is the long run of stores from
      `0x80079818`. So a slot is named by grepping that function for its offset:

          +0x1C   0x8003B250      +0x20   0x8007F328
          +0x170  0x80077D0C      +0x174  0x800781F0

      QFRONT's `init` (export 0, module+0x30F4) calls `+0x170` with 0 and then `+0x174` with
      **(0, 160, 4000)** before it does anything else. 160 is the world's own projection distance and 4000
      a far plane, so that pair is the front end setting up **its own viewport** — which is an independent
      corroboration of `proj = 160` from a code path that has nothing to do with `SetGeomScreen`'s eleven
      call sites or the sky-wedge measurement against the retail capture.
      Following `init` past that produced three more things, and one of them is behaviour rather than data.

      **The front end dispatches on a mode the engine hands it.** `init` reads `engine+0x4A8` and branches:
      1 goes to `module+0x35C8`, 2 to `module+0xCF68`, anything else to `module+0xCEE0`. Those first two are
      the actions on the VIEW CREDITS and MULTI PLAYER item records, so the word is a **re-entry mode** —
      how the front end comes back up on the right screen after a game or a deathmatch ends rather than
      always at the title. `module+0xCEE0` is the title page's own builder.

      **The title screen starts an attract demo after thirty seconds.** `module+0xCEE0` parks 9000 in
      `module+0x12DC0` and installs `module+0xC6AC` as the page's per-frame hook, and that hook is a
      countdown: any input resets it to 9000, otherwise it subtracts the frame delta and calls
      `0x80101B08` when it reaches zero. 9000 of the console's 1/300 s units is **30 s**, and it is what
      the `DEMO OF GAME` / `STARTING` pair in the string pool is for.

      **The models are shown and hidden, not placed per page.** Every builder starts with
      `module+0x3414(list, mode)`, which calls `engine+0x1E4(mode)` and then walks a table at
      `module+0x12B20` — built at `init` from `module+0x1163C` — setting bit `0x80` in three fields (`+0x118`,
      `+0x17C`, `+0x1E0`) of each object it names. So the scene's models are placed once and each page turns
      the right ones on, which means the logo's position is in that object table rather than in per-page
      code. Two more engine slots come with it: `+0x200` installs an item record array (the title's call is
      `(module+0xEC3C, 32)`, against the executable's own `0x8001A474(record, 16)`) and `+0x290` is the
      per-frame page hook.
      **`module+0x1163C` is four 8-byte records**, terminated by a zero word:

          +0x1163C  data 0x80110D94   a -1   b 0
          +0x11644  data 0x80110F94   a -1   b 3
          +0x1164C  data 0x80111194   a -2   b 1
          +0x11654  data 0x80111394   a -2   b 2

      The `data` blocks are 0x200 bytes apart, so each object owns 512 bytes. `b` is 0, 3, 1, 2 — an index
      the builder passes on as its own argument — and `a` is -1 for the first pair and -2 for the second,
      which is what makes them two pairs rather than four singles.
      The builder at `module+0x22C4` walks the list, and for each entry hands `engine+0x184` a four-halfword
      record built on the stack as `{ 0, 255 - i, 256, 1 }` together with the object's data pointer. The
      descending `255 - i` is a draw priority and 256 is a unit scale in the port's usual 1.8.8.
      *What is left for the scene:* the 512-byte object blocks themselves, and `engine+0x184`, which
      consumes them. **Two dead ends recorded so they are not walked twice.**
      `engine+0x184` cannot be named the way the other slots were: the installer's store is
      `sw a1, 388(v1)` at `0x80079E3C`, so that slot is filled from an installer ARGUMENT rather than a
      constant, and grepping the installer for the offset gives the parameter, not the callee. It has to be
      reached from the installer's own caller.
      And the object blocks are not coordinates. `0x80110D94` is 512 bytes of dense data with no run of
      small signed values anywhere in it — nothing that reads as a position triple in any of the port's
      fixed-point formats. Whatever a position is here, it is not stored plainly in the block, so the block
      has to be identified before it can be read.

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

      **`0x8004F5E0`'s operands have now been read, and they say the port is right.** Three things are ruled
      out, and they were the three this entry pointed at.
      *The translation is the disc's.* `0x8004F47C…0x8004F5DC` interpolates the key's `+6` triple as
      `base + (key - base) * (total - left) / total` and hands it to `0x8006FD38`, which is `(M · v) >> 12`
      with the shift at `0x8006FE08`. The blaster's raise keys read `t 140 257 44`, `140 157 44`,
      `140 157 104`, `140 157 44` — `t.x` is **140 on every key**, so there is no key the machine can rest
      on that puts the grip further right.
      *The interpolation is not it either*, since every key agrees on x.
      *And the rotation ORDER is not it.* `RotMatrix` at `0x80089E38` reads a packed `{sin, cos}` table at
      `0x800A5430` and writes `m[1][2] = -sin(x)` (`0x80089F0C`) and `m[0][2] = sin(y)·cos(x)`
      (`0x80089F20`). Those two elements are the signature of **Ry·Rx** with Z outermost — `Rx·Ry` would give
      `m[0][2] = sin(y)` and `m[1][2] = -sin(x)·cos(y)` — which is exactly what `q2_rotation_euler`
      implements. A wrong order would have mattered a great deal here, because the blaster's clip rotation is
      `(2248, 1280, 1748)`, about 198°, 112° and 154°.
      So `q2_vw_place` agrees with the executable on every operand that can be checked against it, and the
      quarter screen is not in it. What has NOT been checked is the port's own invention: `q2_vw_build_ot`
      cancels the camera by drawing the model with `camera^T · clip`, which is not a transcription of
      anything — the console composes `MulMatrix(RotMatrix(view), entity)` and lets the world draw apply the
      camera. That cancellation is exact only when the two matrices are built the same way, and they are
      not: `q2_vw_place` drops `ang[2]` where the camera's own `q2_rotation_view` includes roll. That is a
      real divergence whenever the strafe lean is active, though it cannot explain a still frame at zero
      roll — so the measurement itself may also want re-taking against a fresh capture.

---

- [x] 49. **"The player cannot damage a creature" — RETRACTED the same day it was
      written. They can; the test could not see one.**
      The measurements in the original entry were all correct and the conclusion drawn
      from them was not: 195 attack ticks, bolts in flight, 22 targets registered,
      matching hit radii, actors synced both ways, and creature health unchanged. What
      none of them measured was whether a bolt had anywhere to go.
      Two faults in the test, and the second only showed up once the first was fixed.
      The aim was being written AFTER `q2_sim_advance`, and the shot is taken inside
      it, so an aim applied at the end of a frame governed the frame after the one
      that fired — the run was firing wherever the demo happened to face. Correcting
      that turned every bolt into a floor impact instead, which is the second fault:
      on BASE1 the nearest creature is a storey below, so a correctly aimed shot goes
      into the floor between them. **That is geometry, not combat.**
      `--watch` now stands the PLAYER in front of the creature as well as the camera —
      700 units along its own facing, at head height — and with a clear line the
      player kills: eight creatures become seven, 200 total health becomes 180, in
      under 250 frames.
      The lesson is worth more than the item: every step of the chain was instrumented
      and every number was right, and the conclusion was still wrong, because the one
      thing not instrumented was whether the experiment was capable of a positive
      result. A test that cannot succeed reports the same numbers as a broken feature.

## The port's real gap is missing callers, and here is the list

Four separate finds this session were the same shape — a finished piece with no
caller: the overlay initialised from a flag that had not been set, the creature
action hooks nothing ever assigned, the free-fly camera a session booted into, and
the death screen. That is a pattern rather than four coincidences, so it was worth
measuring instead of noticing.

Sweeping every `q2_*` and `psx_*` function declared in a header and counting calls
across all of `src/` and `tools/` gives **89 that are never mentioned anywhere but
their own definition**. Most are honest accessors. Some are whole subsystems.

**The first version of this sweep said 100, and it was wrong in a way worth
recording**, because it is the same mistake as trusting a measurement without
asking what it can and cannot see. It counted `name(` — call syntax — so every
function installed as a FUNCTION POINTER looked dead. Eight are: the item thinks
`q2_item_think` and `q2_item_shrink_think`, and the AI verbs `q2_ai_stand`,
`q2_ai_walk`, `q2_ai_run`, `q2_ai_move`, `q2_ai_charge`. Those are wired and always
were, and the table below no longer claims otherwise. Counting bare identifiers as
well separates the two.

| what | evidence |
| --- | --- |
| the **multiplayer runtime** | eleven functions — `q2_mp_session_init`, `q2_mp_frame`, `q2_mp_player_killed`, `q2_mp_find_winner`, `q2_mp_banner`, `q2_mp_score_title`, `q2_mp_team_name`, `q2_mp_may_respawn`, `q2_mp_attribute_kill`, `q2_mp_hud_image`, `q2_mp_take_request` — the whole `QMULTI.C` reconstruction, with nothing to drive it |
| the **rotating brushes** | `q2_rotators_build` had one caller and it was an inspector command, so ROTHATCH, SIMROT, SIMROT2 and ROTBUTTON were never even constructed in the game |
| the **AI breadcrumb trail** | `q2_trail_init` and `q2_trail_add`, so the ring at `gp+17892` was always empty and a creature that lost you had nowhere to follow you to |
| **`q2_monster_damage`** | the module-owned health path, which is how a creature with an AI brain is meant to take damage |
| the **per-frame lighting** | `q2_light_world_begin_frame`, `q2_light_glow_fade`, `q2_light_env_apply` — and the client passed a NULL light world with `coll_node = -1`, so nothing that is not the world was lit at all |
| the **view weapon's own outputs** | `q2_vw_take_refire`, `q2_vw_take_event`, `q2_vw_wants_fire` — so running a gun dry never switched off it, and an animation event never reached the frame it belongs to |
| **`q2_weapon_autoselect`** | what a pickup is meant to consult |

**The lights are wired now**, and they were two separate omissions rather than one.
The client passed `ectx.lights = NULL`, and it also passed `ectx.coll_node = -1` —
"no node" — so even a light world would have handed every entity the fallback. Both
come from what the sim already tracks: `Lights` out of COMMON.DAT, `SpaceLights`
opened against the same `SecondaryCol` the sim uses (that being what partitions it,
FORMATS §17), and the player's own cell.
Items pick it up through the entity draw; creatures did not, because the creature
loop calls `q2_model_build_ot` directly, so it gathers its own three — three being
all the GTE's light matrix has rows for.
The result is strongly coloured and that is the map, not a fault: `q2psx-inspect
lit BASE1` reports the accepted lights in zone 0 as `rgb 2988 1992 937`,
`1447 964 457` and `912 415 0`, so a Soldier standing in that room comes out
orange. Checked rather than assumed, because the change is large enough to look
like a bug.

Three more are now wired. The view weapon's refire signal drives the auto-switch off an
empty gun, and its event is drained on the frame the clip raises it rather than the
frame the trigger was pressed — and the verdict handed back to the machine is now
`last_shot.dry` rather than an unconditional "it fired", since `fired` is also
false during an ordinary refire wait and would have reported every shot as denied.
The client builds and ticks the rotator set — BASE0 has two
rotators, BASE1, COMMAND and POWER1 one each, JAIL2 none — and drops a breadcrumb
every ten frames. *Still open for the rotators:* nothing triggers a step. A
rotator moves when SIMROT's exec calls `q2_rotator_trigger`, and the event runtime
has no primitive-dispatch hook to reach it from, so `q2_rotators_tick` reports zero
moved. Built and ticking is not turning, and the log says which.

— **the hook now exists** (`q2_event_rt.on_call`) and `q2_rotator_trigger` is called through
`q2_rotators_call`; disc-wide, 46 steps turn 47 rotators. The client still reports zero, for a
different reason: the demo pad had not walked into a volume that fires one. It does on LAB —
`rot 1 steps 0 moved 1 turned` — and `rot moved` was the wrong statistic anyway, since a SNAP
takes its whole rotation at trigger time and never tick-moves. See question 50 below.

## Nothing had ever opened the death screen

Page 41 has been transcribed since the menu was reconstructed — `RESTART LEVEL`,
the resupply line with its own greying rule at `0x8001D774`, `QUIT GAME` — and no
caller ever opened it. So the player's health simply ran negative and the game
carried on: measured before the fix, a Soldier took the player to **-353** and the
run continued as if nothing had happened. It is the same shape as the overlay that
was never initialised, the creature hooks nothing ever set, and the free-fly camera
a session booted into — a finished piece with no caller.

It is raised from the client rather than the sim, because the sim has no menu and
the page IS the death sequence here: the world freezes behind it, which is what
every other page already does.

That immediately exposed a second thing. A scripted run could not answer the page —
`client_menu_pad` reads the keyboard — so the death screen ended every headless
run: the world frozen, the demo's pad going to a simulation that was no longer
ticking, every later frame identical. The demo now answers a page with CROSS on a
slow cycle, which takes the row a page opens on, and a run dies, restarts and
carries on.

## The creature modules name their own animations

Chasing the sound table turned up a third table beside it, and it is the more
useful of the two. Every creature module carries **three** blocks of text: its own
name at about `+0x168` (`Soldier`, `Tank`, `Arachner`, `Gunner`, `Insane` — note
that the module calls it `Tank` where the class table says `Tankcomm`), the
12-byte sound-name table at about `+0x1C4`, and a **move-name table** further in.

A move-name record is twenty bytes:

        +0   char name[16]     NUL-padded, and NOT terminated when it fills the
                               field — `Attak 1 Loop` is exactly twelve
                               characters, the same rule the level table and the
                               `Strings` dictionary use
        +16  u16 first_frame
        +18  u16 last_frame

The two frames are the move's own range, which is what ties a name to a move
**without depending on the table's order** — and that matters, because the order
is not the decoder's: the decoder finds moves through whichever callback reached
them first. Matching by range instead, `q2psx-inspect creatures` names **83 of the
disc's 97 moves**, including the Soldier's, which a first look at the string pool
had wrongly suggested carried none.

So a creature's animation set is self-describing: `Start Walk` 34-49, `Attak 1
Pre` 65-70, `Attak 1 Loop` 71-76, `Attak 1 End` 115-135, `Stop Walk` 222-253. That
is what a port needs to say which move is which without inferring it from a
callback slot, and it is on the disc.

## Two things the creature chain was still missing

**A correction to this section's own first version.** The sound hook was wired with
`cre_pain1` and `cre_die1` for indices 0 and 1, and that was an invention twice over:
the bank has no such names, so it silently played nothing, and the index-to-name
mapping had never been read. It is gone.
What replaces it is the module's own table, which turned out to be on the disc all
along. A creature module carries **two parallel tables**: eleven resolved handles at
`module+0x32A0` on a 4-byte stride, zero on disc because the engine fills them at
load, and eleven 12-byte NAME fields at `module+0x1D0`, which are not:

        0  sol_idle1     4  sol_pain3     8  wep_machgf1b
        1  sol_sght1     5  sol_deth1     9  wep_shotgf1b
        2  sol_pain1     6  sol_deth2    10  msc_udeath
        3  sol_pain2     7  sol_deth3

They share one index, and `cre_soldier.c` already recorded three handle addresses
against its enum — which is what makes this checkable rather than merely plausible.
All three agree: `SOL_SND_COCK` at `+0x32C4` is index 9, `wep_shotgf1b`, a shotgun
guard's own fire sound. **And the two the enum had guessed were wrong**: it numbered
pain and death 3 and 4, following on from the three it knew, where the table says 2
and 5 — 3 and 4 are `sol_pain2` and `sol_pain3`.
Every one of the eleven is in the map's bank, so the port now plays them. Another
creature's table has not been read, so those stay silent rather than borrowing the
Soldier's.

**Nothing had ever set the action hooks.** `crebind.h` defines a sound hook, a
fire hook and a melee hook, and the only definitions of the setters were in
`cre_soldier.c` — no caller anywhere in the tree. So every claw, every shot and
every sound a creature made went to a null pointer: they chased the player and
could not touch them. The melee and sound hooks are now wired in the client. The
FIRE hook deliberately is not: a module's melee carries real decoded figures (the
Arachner's is `aim 1020,-48,0 dmg 20+r%5 kick 100`) but its shot reaches an
indirect call the action decoder reports as `call(+D8)?`, so what damage a
creature's gun does is not read yet (#6), and an invented number would make every
creature in the game lethal on a guess.

**Where a creature's shot damage actually lives — a redirect for #6.** It is not in
the module. `soldier_fire` at `module+0x1120` picks one of three 8-entry tables by
skin (`module+0x3240`, `+0x3260`, `+0x3280`) and the entries are **small integers**
— table 0 reads 39, 40, 83, 86, 89, 92, 95, 98 — which are muzzle-flash indices,
not damage. It then calls engine slots `+0xC8` and `+0xD8` with vectors.

      **A retraction, one step old.** This entry briefly said the figures were
      "reachable from `0x80078288`", the callee of `+0xC8`. They are not:
      `0x80078288` indexes the CAMERA array at `0x800D5C30` with a 784-byte
      stride, reads its `+262`/`+266`/`+268`, and ends by loading the GTE's
      `TRX`/`TRY`/`TRZ`. It is a view setup. The call being traced was placing
      the muzzle in view space, not firing, and the inference from "the fire
      function calls this" to "the damage is in this" skipped the step of
      establishing WHICH of the calls is the shot. `+0xD8` resolving to
      `0x800B2C2C` — a data address, not a function — is the other half of the
      same warning: the base register in that fragment was never shown to be
      the engine block.
      So the shot figures remain unlocated, and the honest statement is narrower
      than the one it replaces: they are not in the flash tables, which hold
      muzzle-flash indices, and the module's own image does not carry them as
      immediates. Where they are is open.

      **What the second attempt did establish is general, and worth more than the
      one slot it was after.** The right indirection was never the LevelBin engine
      vtable — a CreAI module reaches the engine through its OWN 71-pointer import
      table at `module+0x14`, and the loader that fills it is at **`0x8007DA00`**,
      writing every slot individually as `sw v0, N(s0)` with the address
      materialised two instructions above. So any slot can be named by grepping
      that one function for its offset, and the census's `call(+XX)?` reports stop
      being opaque numbers. The method checks out on the slot already known:
      `+0x28` resolves to `0x8006FC1C`, the SVECTOR rotate, which is
      `Q2_IMP_LOCAL2WLD`.
      Applied to the Soldier's fire think, whose actions decode as
      `call(+D8)? call(+2C)? call(+28)? call(+C8)? call(+D8)? call(+C0)x3`:
      `+0xC0` is `0x8005C460`, a per-component scaled vector add
      (`out = a + (s*d) >> 12`), so **the three trailing calls are muzzle
      arithmetic, not three shots** — which is the specific wrong reading the
      retraction above was heading towards. `+0xC4` is `0x8005C634`, `+0xEC` is
      `0x80061118` — the `fire_hit` the melee already uses — and the two that
      were unread are now read: `+0xC8` (`0x8005F934`) is **`vectoangles`**,
      ratan2 through `0x8008A358` with the horizontal length through
      `0x8008A7E8`, returning 3072 and 1024 for straight down and up; and
      `+0xD8` (`0x8005BB58`) is **angles to vectors**, three angles masked to
      `0xFFF` indexing the packed `{sin, cos}` table at `0x800A5430`.

      **The shot was found, and it was one indirection further out.** `soldier_fire`
      does not reach it through `s3`, the register the decoder follows, but through a
      fresh `lui` — `lw v1, 128(v0)` with `v0 = module base` — which is import
      `+0x80`. The loader at `0x8007DA00` names the whole family, and it is
      contiguous, exactly as id's is:

          +0x80  0x80062000  monster_fire_blaster
          +0x84  0x80061DFC  monster_fire_bullet
          +0x88  0x80061ED0  monster_fire_shotgun

      And the Soldier's three arms carry their figures as immediates:

          skin < 2   blaster    damage 5, speed 600
          skin < 4   shotgun    damage 2, kick 1, spread 1000/500, 12 pellets
          otherwise  bullet     damage 2, kick 4, spread 300/500

      **Every one of those is id's own number** — `monster_fire_blaster(…, 5, 600, …)`,
      `DEFAULT_SHOTGUN_HSPREAD` 1000 / `VSPREAD` 500 / `COUNT` 12,
      `DEFAULT_BULLET_HSPREAD` 300 / `VSPREAD` 500 — which is the check that says the
      read is right rather than merely self-consistent. The port's fire hook now
      carries them, and a creature whose table it does not know is dropped rather
      than handed a Soldier's gun.
      *Measured, not assumed:* on BASE1 over 1800 frames with four creatures hunting,
      the hook is invoked **zero** times. **The root cause is found and it is a
      one-line guard in the port.**
      The frame driver dispatches a frame's think through the class method table, and
      the Soldier's methods are installed, so that path was never the problem. What
      stops it is one step earlier: `q2_ai_checkattack` ends with
      `if (!m->checkattack) return false;`, and **the original does not test it** —
      `0x8005E320` is `lw v0, 260(s1)` and `0x8005E328` is `jalr v0` with nothing
      between them. `entity+0x104` is never NULL on the console, so the engine
      installs a default at spawn that a module may override.
      **No creature module on the disc installs one.** The Soldier's callbacks are
      stand, walk, run, dodge, attack, sight, pain and die, and the other six are the
      same. So a guard that reads as ordinary defensive coding disables every attack
      in the game — which is why creatures chase and never fire, and why the melee
      hook never fired either.
      **The default is `0x8005D8C8`, and it is installed at `0x80061B18`.** Found by
      scanning the whole text segment for the instruction rather than guessing at
      neighbourhoods: `sw rt, 0x104(rs)` appears **ten** times in the image, six of
      them with `rs = sp` (stack frames), one in the module import loader where
      `+0x104` is an import slot rather than an entity field, and exactly one with an
      entity base in the monster spawn — `0x80061B18`, guarded by
      `bne a0, zero` two instructions above, so a caller can suppress it.
      `0x8005D8C8` is `M_CheckAttack`: it reads the enemy through `entity+0xBC`,
      tests its health at `+0x108`, builds the two eye points from `+0x00..0x08` and
      `+0x4C`, and traces between them with contents mask `0x0200001B` before the
      range and random decision.
      **Transcribed, and the creatures now attack.** On BASE1 the same run that
      measured zero hook calls now reports **135 shots and the player at -353 hp**;
      health falls from 100 as soon as a Soldier has line of sight. Every constant in
      it is the original's and every one is also id's, which is the check that the
      read is right rather than merely self-consistent: the four chances are 1638,
      819, 410 and 82 out of 4096 — 0.4, 0.2, 0.1 and 0.02 — skill 0 halves them and
      skill 2 or more doubles them, and the flyer's sliding roll is 9830 of 32768,
      which is 0.3.
      **The blind-fire branch is dead code on this build, and the port returning false
      there is what the console does rather than a narrowing** — which corrects the
      caveat first written here. Its first gate is bit 17 of `entity+0x138`
      (`0x8005D9EC`: `srl 17; andi 1; beq`), and scanning the whole text segment for
      writes to that word finds eight outside stack frames: four are `M_CheckAttack`'s
      own attack-state stores, which mask with `0xFFE3FFFF` and so PRESERVE bit 17;
      one is `ai_checkattack`'s; one writes bits 21 and up; and the two `sh` sites
      reach only the low halfword. **No instruction in the image ever sets bit 17.**
      The blindfire flag is always clear, the branch cannot be entered, and
      `blind_target` — which the port does write, in three places — is read by
      nothing that can run.
      Its shape, for the branch that remains:

        - the first gate is the ENEMY's health, reached through `entity+0xBC` then
          that object's `+0x24` then `+0x108`, and `blez` leaves immediately;
        - it builds two eye points, self and enemy, each as the position triple at
          `+0x00..0x08` with the view height at `+0x4C` added to the middle one;
        - it calls `0x8005BD3C` — a trace — **twice, with different masks**:
          `0x0200001B` for the first and a bare `0x02000000` for the second, the
          second starting from `self+0x5C` rather than the eye, which is the
          blind-fire target `blind_target` at `+0x5C` (§9.12);
        - between them are five or six further gates on the enemy's flags, on
          `self+0x138` bit 17, on `self+0x1C` against 201, and on `self+0x110` and
          `self+0x124` against the global at `0x800E46DC`.

      The guard in `q2_ai_checkattack` stays until all of that is transcribed rather
      than some of it. A checkattack that returns true too readily is worse than one
      that never does: it would put invented aggression on every creature in the game
      and look like a working feature while doing it.

      The earlier statement that follows was written before this and is kept because
      the reasoning it records is still what eliminated the call route:
      **there is no fire call in the Soldier's fire think's IMPORT LIST.** Every one
      of its six distinct imports is aiming arithmetic. That eliminates the whole
      call route rather than narrowing it, and leaves one candidate standing: the
      think loads `s7` from the flash table at `0x80101194` and the tables hold
      INDICES, so the shot is most likely POSTED — a field written for an engine
      pass to act on — which would put the damage in an engine table indexed by
      those flash numbers. That is a hypothesis and is labelled one: neither the
      store nor the table has been found. What is established is that looking for
      a call was the wrong search, and the decoder's own step kinds say what to
      look for instead, since it already classifies stores to `entity+0x138`,
      `+0xD8` and `+0xDC` as their own actions.
      The port leaves the fire hook unset rather than guessing, and a Soldier
      chases and swings but does not shoot.

**Eight of the disc's levels ship an EMPTY `CreAIBin` — four bytes — and place
creatures anyway.** JAIL2, JAIL3 and JAIL4 have Infantry; SECURITY, WASTE2,
BIGGUN, BOSS1 and BOSS2 likewise. Their spawn records name classes the class
table resolves perfectly well, and the port placed nothing at all on them because
the module was missing. The census had the answer and it was read as a
convenience: fifteen module instances across the disc, **seven distinct**,
deduplicated by name. A module of a given name is the same wherever it appears —
the same argument that settled `QMULTI.C`, byte-identical on all thirteen arenas
— so a map with none borrows from one that has them.
JAIL2 goes from 0 creatures to 5, BOSS1 to 2, BIGGUN to 3; BASE1, which ships its
own, is untouched at 22 of 22, which is what says the borrow only fires where it
is needed. The assumption is written into `creworld.c` rather than buried.

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

- [x] 47. **Which CastList clip a creature's move plays. — SOLVED: none of them, and none is the point.** A module's moves are numbered in one global frame
      timeline — the Soldier's run 0..474, and `q2psx-inspect creatures` now prints every move's range —
      while its model carries a list of clips, 31 of them for the Soldier. Those clips are **not** that
      timeline laid end to end: they total 434 frames against the module's 474, and there are 31 of them to
      the module's 18 moves.
      What they are is the **moves themselves**. Every one of the Soldier's clip lengths is exactly three
      ticks per frame times some move's length, and clips 1..4 are the four consecutive moves 50-54, 55-61,
      62-79 and 80-96 *in order*. So the tick rate inside a clip is 3 — not `Q2_MODEL_TICKS_PER_FRAME`'s 10,
      which is the view weapon's — and `q2psx-inspect model <map> <name>` now prints every clip's length so
      the arithmetic can be checked on any creature.
      **The index does not exist, and that is the answer.** `0x8006B924` is the selector, and it does not
      index anything: it holds the animation position in a halfword at `entity+0x100` and the current clip
      at `model+0x34`, and *while* the position is past the clip's length it advances the pointer by that
      clip's own `next` byte delta — through `0x80070188`, which is the two-instruction `*p += d` — and
      subtracts that clip's `frames`. A model's clips are therefore **one continuous timeline** and the
      animation position is an offset into it; a clip boundary is wherever the subtractions fall.
      So the question "which clip does a move play" was malformed. A move's frames are positions on the same
      timeline and the walk lands in the right clip on its own, which is also why the measured coincidence
      held: clips 1..4 being the four consecutive moves 50-54, 55-61, 62-79 and 80-96 *in order* is what a
      shared timeline looks like, not a lucky run of matching lengths.
      The three-ticks-per-frame scale survives unchanged, and it is still measured rather than read.
      `q2_model_anim_at` implements the walk and the client's creatures are drawn through it; the
      match-by-length heuristic it replaces is gone.

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

## Rotating brushes turn, and the script that turns them was the one we were already running

The rotator set built last round never moved: `rot moved 0` on every map. Not a bug in the rotation — every
kind returns early unless a step is pending (`0x8002F1B8`), and the step is consumed after one step
(`0x8002F204`). One request, one step, and **nothing was requesting**. `q2_rotator_trigger` had no caller.

The request comes from a script `CALL`, and `Q2_EVOP_CALL` fell through the event runtime's `default:` case,
counted with FX and WAIT as "recognised but not implemented". It is now reported to the owner rather than
interpreted: `q2_event_rt.on_call` hands over the item and its UserFuncs index, because *which index is
SIMROT* is a per-map question that `events_rt.[ch]` has no map to answer. What the operands mean lives in
`q2_rotators_call`, beside the builder that reads the same offsets — they differ per primitive (SIMROT names
four objects at +12..+18, ROTHATCH one at +18, ROTBUTTON one at +10) and two copies of that table would rot
apart and turn the wrong geometry.

**In the client, playing:** LAB, demo pad, 900 frames — `rot 1 steps 0 moved 1 turned, 4 calls`. The player
walks into a volume, the volume fires a record, the record calls a rotation primitive, and a rotator that was
standing at zero is standing turned. Across the disc, firing every trigger volume once — a player who has
walked every map — **26 rotators built, 552 CALL items run, 17 rotation steps, 13 rotators turned.**

- [x] 50. **What fires a zone's Events records? — NOTHING, and that is the console's own behaviour.**
      RETRACTED, same day: this was written up as "the single largest piece of level behaviour still missing"
      on the strength of a zone's Events chunk carrying 2959 CALL items, 805 movers and 619 zone gates that
      the port never ran. **The engine never loads that chunk.** The zone loader (0x8007B3F8) looks its
      chunks up by name — PrimaryColl, SecondaryCol, PrimaryRemap, Scene, Points, MapMod, MapNames, SortData,
      SpaceLights, AreaConx, CastList, CreAIBin, CreAIRel — and `Events` is not among them. The image holds
      exactly one copy of the string "Events" (0x800AD480) and exactly two references to it: COMMON's loader
      at 0x8007AC30, whose match stores the chunk pointer into the events global 0x800AE774 (0x8007AD54), and
      the teardown that clears the same global (0x8007C250). Every reader of that global — the load-time
      pre-pass at 0x80026DC0, the execution dispatch at 0x80027950 — therefore reads COMMON's script and only
      COMMON's. A zone's Events chunk is build output the retail engine ignores; 21 of 74 are byte-identical
      to their map's COMMON one and 53 differ, and it makes no difference either way. **The port was already
      running the right script.**

**Two counting tests were run before that and both decided nothing** — recorded so neither is repeated. All
834 trigger offsets start a record in COMMON's script *and* in a zone's (an offset is just a number, and
record starts are dense); and none runs past the end of either chunk. The commit before this one cited the
first of those as evidence the sim fires the right chunk. It was not evidence. The disassembly is.

The disc-wide figures in the previous commit — 112 rotators, 46 steps, 47 turned — were measured on the zone
chunk and so describe data no console ever executes. The live figures are the 26/17/13 above.

`render` grew an optional rotation-tick argument so a rotator can be looked at rather than counted: it builds
the map's rotators from COMMON's script, drives them, frames the one that turns furthest and renders it. A negative count builds
and frames without turning, which is the "before" of a pair taken from one camera — zero could not serve,
because zero also means "no rotators" and frames the whole zone instead. Two things that pass a count and
fail an eye: a **SNAP rotator turns exactly 2048 of 4096 about its own centre**, and a symmetric brush at 180°
is byte-for-byte the frame it started as; and **one call buys one step**, so a single tap turns an ACCUM
rotator by one speed's worth and stops. The render re-triggers each tick, which is a script holding the
rotation on.

---

## The damage function does not post to the AI, and a corpse now falls over

`q2_actor.ai_owned` carried this: *"a creature with a module posts rather than subtracts: 0x800584B4 hands the
amount to 0x800627F8 and jumps past the health store"*. It does not. **0x800627F8 is T_Damage**, with id's own
argument order and id's own content, and it subtracts health itself at 0x80062958 into `(entity+0x24)+0x108`.
The call at 0x800584B4 passes a DIFFERENT entity — the one at `entity+0x2EC` — and the caller has already
stored its own target's health at 0x800583F8. There is no posting anywhere. The flag was never set true, so
deleting it changed no behaviour; what it had been hiding was three things T_Damage really does, now
transcribed:

- **the surprise bonus** (0x800628C8-0x80062910): `svflags & SVF_MONSTER`, attacker has a client block,
  `!targ->enemy`, `targ->health > 0` → damage doubles. Four conditions, id's exactly. The first shot on a
  creature that has not noticed you is worth two.
- **FL_NO_KNOCKBACK** (0x800, 0x8006291C) zeroes the impulse and nothing else; **FL_GODMODE** (0x10,
  0x8006292C) zeroes the damage.
- **the corpse floor** (0x800629B4): health stops at -9999 however hard a body is hit.

T_Damage ends by calling the entity's own `die` at `entity+0xA4` (0x80062A9C) or its `pain` at `entity+0xA0`
(0x80062AF4). Neither had a caller here: `pain` and `die` are decoded into every creature's callback table and
nothing ever dispatched them. What can be reconstructed from a module's DATA rather than its code is the
animation, and that is now wired — every one of the seven modules names a death move (`Death1`, `Death 4`,
`St Death`, `Death 2`, `Death`, `Death3`, `Death2`), matched by frame range, so a kill installs it.

**`q2_monster_set_tick` skipped anything with `dead` set**, which is why this mattered. The body was still
drawn — the draw loop only checks `in_use` — so a killed Soldier stood in whatever pose the shot caught it in,
mid-stride, for the rest of the level. A corpse now runs the frame driver and not the AI: it animates, it does
not think, and it does not walk.

- [x] 51. **ANSWERED the same day: a move SELECTS a clip; the frame indexes into it.**
      The port walked the whole clip chain treating an AI frame as a position on one continuous timeline.
      That is what `0x8006B924` does when a position OVERRUNS its clip — but the engine holds a *current*
      clip pointer at `model+0x34` and only walks when it has to, so the clip is chosen elsewhere and the
      position is relative to it. What chooses it is not in the module's data, and the correspondence that
      is: **a move's frame count times three is exactly some clip's length, for 93 of the disc's 97 moves.**
      Per module: Tankcomm 16/16, Insane 18/18, Gunner 13/13, Infantry 11/11, Berserk 10/10, Soldier 16/18
      (lengths 12 and 33 have no clip), Arachner 9/11. Where several clips share a length the k-th move of
      that length takes the k-th clip, which reproduces the one correspondence already known from the other
      direction: the Soldier's consecutive moves 50-54, 55-61, 62-79 and 80-96 resolve to clips 1, 2, 3 and 4.
      Its death move 308-342 — 35 frames, 105 ticks — resolves to clip 11, which is 105 frames long and is
      the death animation, standing to fallen. The timeline walk put frame 308 at tick 924, inside clip 12,
      which is why a body stood up halfway through dying. The four moves with no matching clip fall back to
      the old walk rather than to nothing. `q2_model_anim_by_length` is the selector.

      The original evidence for `Q2_CRE_TICKS_PER_FRAME 3` stands and is now better supported: it was
      measured on four short consecutive moves, and 93 of 97 across seven modules agree with it.

- [ ] 51a. **What does the engine use to select the clip?** Matching lengths is a reconstruction, not a
      read: it recovers the right answer 93 times out of 97 and the four misses are silent. Something in a
      module's code sets `model+0x34` when it installs a move, and that store has not been found.

  *The finding as originally written, kept because the render that produced it is the reason the drift was
  visible at all:*

- [ ] ~~51. **The AI frame → model clip mapping drifts across a long move.**~~ `Q2_CRE_TICKS_PER_FRAME` is 3 and
      it lands the START of the Soldier's `Death1` correctly: posed at AI frames 310, 314, 318 and 322 the
      model is a body collapsing to the floor, progressively. But the move runs 308-342, and at 330 and 336
      the same creature is standing upright again — the timeline has walked into the next clip well before
      the move ends. Either the move's frame range and the model's clip lengths do not correspond one to one
      for this creature, or the tick scale is not a constant 3 across the whole timeline. The measurement that
      established 3 used the first four moves, which are consecutive and short; nothing checked a move 35
      frames long. `q2psx-inspect mob <disc> <map> <zone> <n> <out.ppm> <ai-frame>` is what took those poses.

## The multiplayer runtime had no caller anywhere in the game

`multiplayer.[ch]` reconstructs the whole of QMULTI.C — the scoring, the frag and time limits, the VERSUS
round rules, the spawn selector, the banner countdown, the attribution rule and the two game-state requests —
and the test suite was the only thing that had ever run any of it. Nine of its entry points had no caller
outside the module: `q2_mp_session_init`, `q2_mp_player_killed`, `q2_mp_frame`, `q2_mp_take_request`,
`q2_mp_banner`, `q2_mp_may_respawn`, `q2_mp_attribute_kill`, `q2_mp_find_winner`, `q2_mp_hud_image`. The
rules were right and nothing ever asked them anything.

The client now runs a match. `--dm` boots an arena, `--dm-mode`, `--dm-players`, `--dm-frags` and
`--dm-minutes` set it up, and what runs is the reconstruction rather than a re-statement of it:

- **the spawn**: the local player starts at a `MultiSpawn`, picked by `q2_mp_select_spawn` — farthest from
  everybody already placed, ties broken by an RNG. All five arenas tried resolve their points: MATRIX1 4,
  MATRIX5 8, THEVAT 5, PODCITY 8, FRAGTOWE 4.
- **the clock**: `q2_mp_frame` on the sim's own 1/300 s step. A one-minute match ends at 18010 dt — the
  limit is `level_time > minutes * 18000` and 18000 units is sixty seconds — with the banner `TIME UP`,
  then request 11, `load MPResults`, winner 8, `DRAWN MATCH`, scoreboard title `DM SCORES`, HUD set
  `qk2_menu.lbm` for two players.
- **the death**: a killed player goes to `q2_mp_attribute_kill` with the means of death and then to
  `q2_mp_player_killed`, before the death screen opens.
- **the cut modes**: asking for CTF prints that the mode is cut and the front end cannot select it, rather
  than pretending it is a shipped feature.

Taking the request also STOPS the session, which is not a detail: on the console the request changes the game
state and the level hook stops running. The first version left it ticking and the runtime re-asked on every
frame — sixty-odd identical requests for one match that ended once.

- [x] 52. **ANSWERED: the client was asking for the single-player icon sheet on maps that carry only the
      multiplayer ones, and the failure was silent.** `q2_menu_icons_name` picks `qk_menu.lbm` in single
      player, `qk2_menu.lbm` for a two-player match and `qkm_menu.lbm` for three or four — which is the same
      thing `q2_mp_hud_image` says, and neither had a caller that knew whether a match was running. The
      client passed `menu.multiplayer, 1` — the MENU's flag, which is about which menu pages to show, not
      whether a game is in progress — so every arena asked for `qk_menu.lbm`, and no arena carries it.

      Silent because `q2_menu_font_upload` returns Q2_OK when ANY of its images lands, and the two text
      atlases always do. `icons_resident` was false, `tpage_icons` addressed an empty texture page, and the
      status bar dutifully emitted its quads into it. The draw is now gated on `icons_resident` rather than
      on the upload having half-succeeded, and a map that lacks the sheet says which one it lacks.

      Surveyed across the thirteen arenas: **none of them carries `qk_menu.lbm`**, which is right — an arena
      is never played in single player — and all thirteen carry `qkm_menu.lbm`. Twelve carry `qk2_menu.lbm`;
      **FRAGTOWE does not**, so a two-player match there has no status bar and a three- or four-player one
      does. That is a fact about the disc, not about this port. No single-player map regressed: eight checked
      by hand and BASE0's bar is pixel-identical before and after.

  *As first written, before it was chased:*

- [ ] ~~52. **No HUD is drawn on any arena map.**~~ Found by looking at a deathmatch instead of reasoning about
      one. On BASE0 the overlay draws health, armour and ammo; on MATRIX5 it draws nothing, and it is the MAP
      and not the session — the same map without `--dm` is equally blank. The gate is not the cause: at frame
      60 on both maps `hud_ready 1, font 1, menu 0, mission 0, mcard 0`, so `q2_hud_build_ot` runs and emits
      nothing visible. The font upload reports no failure either. The suspicion is VRAM: the HUD atlas is
      uploaded into the map's own texture memory and an arena's pages may land on top of it, which would make
      this a load-order fault rather than a HUD fault. Not chased yet.

## The scoreboard the runtime was asking for

`Q2_MP_REQ_RESULTS` is engine state 11, "load MPResults", and until now the client recorded the request and
did nothing with it. QMRESULT is a level directory of its own — an 840-byte zone, a 99 KB COMMON and a 29,988
byte LevelBin — and that module carries every word the screen shows: the six titles in mode order, the four
colour names, `"%s TEAM SCORED %d"`, and the prompt `ALL PLAYERS PRESS` / `FIRE TO CONTINUE`.

`q2_mp_scoreboard` composes those lines from the session. Deathmatch gives the title, a line per player, and
the prompt; a team mode inserts the team lines after the players, one for each team with a score; VERSUS
prints ROUNDS WON rather than frags, because that is what its `team_frags` array holds. Nothing in the text
is invented — the strings are the module's, read out of it.

**The layout is not reconstructed and is marked as such.** Where QMRESULT puts each line goes through engine
text calls whose offsets have not been read, so the port stacks them centred. Showing the right words in the
right order is what it claims; the original's pixels are not.

Two mistakes on the way, both worth keeping because both are easy to repeat:

- **A line's position is the CONTEXT's home, not the pen's x and y.** The pen carries state across a string;
  `ctx->home_x`/`home_y` say where a string starts. Setting the pen put all five lines on one row, each
  beginning where the last one ended — and it looked like a layout bug rather than an API mistake.
- **`q2_hud_measure` returns CHARACTERS, not pixels.** It is the original's measurer at 0x800702A0,
  off-by-one and all, and the glyph advance is a constant 8. Centring on its raw value put the block half a
  screen right.

## The split screen, and what is behind each viewport

The screen module has had every layout decoded for a long time — `ONE`, `TWO_H`, `TWO_V`, `QUAD`, with each
viewport's origin, size, GTE projection, geometry offset, far plane and status-bar anchor — and the client
installed `ONE` and left the rest to an F5 debug cycle whose own comment said the extra viewports show the
same camera because there is one player. The layout is not a debug toy: it is chosen by the session's PLAYER
COUNT at 0x800B3356, through the jump table at 0x800AC90C. One or none is full screen, two is a split whose
axis is the HORIZONTAL SPLIT setting, three is the quad layout with the view count forced to three
(0x8003FAE4), and four is the quad. `--dm-players` now installs it.

Each viewport also looks from its own place. Every player is put through `q2_mp_select_spawn` in turn, each
placed against the ones already placed — which is what that selector is *for*: it takes the point farthest
from everybody standing somewhere, so four players spread across an arena instead of piling onto whichever
point comes first. On MATRIX5 with four players they land on MultiSpawn 5, 3, 6 and 1 of the eight.

**What each viewport is NOT is a second player.** There is one `q2_sim`. Viewport 0 is the simulated player;
1 to 3 stand at their spawn and do not move, and their status bars read player 0's health and ammo because
the status bar is fed from the one inventory that exists. The screen work, the layout selection, the spawn
spread and the per-viewport HUD set are real; the other three participants are not, and the code says so
where it does it rather than in a note here.

- [x] 53. **DONE: four players in one world.**

      The tick was two things wearing one name — the player's frame (movement, view, weapon, the volumes they
      stand in) and the world's (the entity sweep, the effects, the glint, the clock). With one player they
      are indistinguishable. With four they must not be: running the entity sweep four times would age every
      item respawn four times as fast and step the effects four times a frame.

      So the world half is gated on `cur_player == 0` and `q2_sim_advance_player` runs an extra player's frame
      against the world that `q2_sim_advance` has already advanced this frame. The entity world has taken a
      player INDEX since it was written and nothing had ever passed anything but 0; every player publishes
      their position through it now, so they are all visible to the same item and trigger logic.

      One bug caught by reading the numbers rather than the render: the extra players were seeded by copying
      player 0's state and overwriting the position. A `q2_player` carries its collision NODE, and a node is
      where you are — so a player placed elsewhere with someone else's node fell out of the world. Two of four
      ended a capture at y 64847. Spawning each through `q2_sim_spawn` with `cur_player` set fixes it: four
      players at y -1024, 604, 0 and 604, all on the floor.

      **Single player is byte-identical** to before the whole refactor, all 26 tests pass, and the four-way
      split renders four viewpoints of one MATRIX5.

      **The combat block is split too.** `rules`, `rng`, `projectiles`, `targets` and `target_count` are the
      world's — one list of bolts in flight, one set of things that can be hurt — and the inventory, weapon,
      refire gate, view kick, chaingun spin, hurt-actor and last-shot record are a player's. Four players now
      have four of each: `100 hp` apiece on MATRIX5.

      They are SWAPPED in and out of `sim->combat` around a player's tick rather than addressed through an
      index, because `sim->combat.inv` appears eighty-six times across the game, the client and the tests, and
      every one of those sites means "the player whose frame is running" — which is precisely what the swap
      makes true. `cur_player` selects which, exactly as it does for `player[]`. Four checks in `test_sim`
      hold it down: a player's health survives another's tick, a player's weapon survives another's tick,
      hurting one leaves the other alone, and three extra players do not advance the world clock.

      Single player is byte-identical, all 26 tests pass, and COMMAND still reports 22 fire calls, 22 sent.

  *As first written:*

- [ ] ~~53. **Four simulated players — the client half is done, and `sim.c` now holds four players instead of
      one.**~~ The scaffolding is in: `q2_sim.player` is `q2_player[Q2_SIM_MAX_PLAYERS]` with a `cur_player`
      index, and every one of the 52 references in `sim.c`, 15 in `simcombat.c` and the rest across the tests
      and tools goes through it. `cur_player` stays 0 and nothing sets it yet, so this is a change of shape
      and not of behaviour — deliberately, because the way to verify a refactor of the file that owns the tick
      is to prove it changed nothing.

      **Verified byte-identical**: BASE0 in the client renders the same frame pixel for pixel before and
      after; COMMAND still reports 22 fire calls, 22 sent; the four-player deathmatch still places four
      players at four MultiSpawns; all 26 tests pass.

      One thing found on the way and worth recording, because it nearly went in silently: `q2_save` has a
      `player` member of its own, a single `q2_player` and not an array. A regex that turned `s->player` into
      `s->player[0]` hit it in `save.c` and in the inspector's `save` command, where it compiled in one place
      and not the other. The save format is untouched and the round-trip test — 156 checks over health,
      armour, ammo, tier, keys, weapons, the held weapon, the refire gate, the view kick, the chaingun spin
      and the weapon RNG — still passes.

      What remains is the tick: `q2_sim_tick` runs the player half and the world half together, so four
      players in one sim means splitting it and calling the world half once. The sections are already
      separable — `q2_fx_tick`, `q2_fx_timed_tick`, `q2_fx_glint_advance` and
      `q2_item_mega_health_tick` are world; everything from `update_view_offset` to `update_pain` is a
      player's; `update_triggers` and `q2_sim_combat_tick` are per-player and want the world's script and
      projectile list, which is exactly what one shared sim gives them.

  *As first written:*

- [ ] ~~53. **Four simulated players — done in the client, still owed in `sim.c`.**~~ `q2_sim` is now an array
      indexed by player, and players 1..3 each get their own instance: spawned at their own MultiSpawn,
      advanced every frame on their own `q2_pad_state`, with each viewport following its own player's eye and
      view angles rather than a camera parked at a spawn point. Measured on MATRIX5 with four players after
      400 frames — positions `[-14926 -1024 12546]`, `[9412 42 3321]`, `[-24811 -2556 4128]`,
      `[-3204 611 17148]`, having moved 9029, 4239, 1923 and 6223 units from where they started, on three
      different floors. Single player is byte-identical to before the refactor.

      **What is still wrong is the world.** Each instance owns a copy of the map's items and its own script
      runtime, because the player lives inside `q2_sim`, and only player 0's is read or drawn. The duplicates
      are invisible and self-consistent, so this is a cost rather than a bug — but it means four players do
      not yet share a world, and therefore cannot pick up the same item or shoot each other. Fixing it is a
      change to `sim.c`: the world half and the player half want separating, and every rate, timer and event
      in that file has to end up on the right side of the line. That is the remaining piece, and it is the
      one that turns four people walking about into a match.

  *As first written:*

- [ ] ~~53. **Four simulated players.** The remaining piece is `q2_sim` being an array rather than a member:~~
      four players means four movement states, four inventories, four view weapons and four sets of pad
      input, and every consumer in the client that says `c->sim` today means `c->sim[p]`. Nothing found so far
      says the sim cannot be instanced — `q2_sim_init` already takes the zone as an argument, so several can
      share one world — but it is a refactor of the client rather than a reconstruction of the original, and
      it is the largest single piece of multiplayer left.

## Naming the import slots, and why six creatures never fired

A decoded creature reaches the engine through its module's import table, and `cre_actions.c` ran every kind of
decoded step except one: `Q2_CRE_OP_CALL` fell through to `default: break`. 107 call steps across the disc did
nothing.

The import loader at `0x8007DA00` writes all 71 slots individually — `sw v0, N(s0)` with the address
materialised two instructions above — so any slot can be named by reading that one function. The method was
already checked against `+0x28` (`0x8006FC1C`, the SVECTOR rotate) and `+0xEC` (`0x80061118`, `fire_hit`); it
reproduces both. The rest of the slots the census reports:

| slot | address | what it is |
| --- | --- | --- |
| +0x84 | 0x80061DFC | a hitscan — goes through `0x80044C44`, the swept move |
| +0x98 | 0x8006210C | the rocket — `0x80062164` calls `0x8004AF28`, which combat.h already records as being called *from that address* |
| +0x8C | 0x8006217C | the rail — `0x800621A4` calls `0x8004917C`, named in combat.h |
| +0x80 | 0x80062000 | a bolt with a visual — calls `0x8004E920`, an effect constructor (effect.h) |
| +0xFC | 0x80062240 | three calls to one spawner (`0x800619E0`) — the shape of a spread |
| +0x1C | 0x8005C8C8 | unnamed |
| +0x2C | 0x8006C6C8 | unnamed |
| +0x38 | 0x8006CC44 | unnamed |
| +0x94 | 0x800614D4 | unnamed |
| +0xA0 | 0x80031094 | unnamed |
| +0xB4 | 0x8005EF84 | unnamed |
| +0x114 | 0x8005CBBC | unnamed |
| +0x12C | 0x80040800 | unnamed |

Three of those five confirm themselves against addresses this project had identified for other reasons
entirely, which is the strongest form the evidence takes here.

The five are now routed to the same fire hook a transcribed creature uses. **The four already named as vector
arithmetic (+0xC0, +0xC4, +0xC8, +0xD8) are deliberately not:** they are the muzzle maths every fire think
does first, and they are 40 of the 107 call steps. Treating them as shots would have every creature fire three
times an animation frame — which is exactly the mistake the Soldier's transcription notes warn about.

**What has NOT been observed is one of them firing in play.** Tankcomm's thinks 8, 10 and 13 carry +0x80,
+0x98 and +0x84, and every one is marked gated; a 400-frame `--watch` capture on COMMAND produces its sounds
and no shots, and the same capture produces the same ten shots with the routing removed, so those ten are
somebody else's. The creature is not reaching its attack thinks in a run that short. The routing is covered by
five checks in `test_creature` instead — hook called with an enemy, not called without one, not called at a
dead enemy, carrying the slot it came from, and not called for muzzle arithmetic.

## Every decoded creature has been doing nothing, and the counters found it in one run

`cre_actions.c` decodes what each of a module's think functions DOES and executes it. Six of the seven
modules on the disc run entirely on that. It had never executed anything.

`creworld.c` called `q2_creature_bind_thinks(&m->bind, ...)` and then `q2_creature_bind(&m->bind, ...)`, and
`q2_creature_bind` opens with `memset(b, 0, sizeof(*b))`. The think table was installed and wiped two lines
later, so `q2_cre_run_think` found `b->think` NULL and returned — every time, for every creature, on every map.

It was invisible because it looked exactly like the state it was meant to be an improvement on: a Tank
Commander that walks, chases and does nothing is what "no hand transcription yet" looks like. The file's own
header said as much, and had been stale since the trampolines were added.

Found by counting rather than reading. Adding `q2_cre_action_stats` — thinks run, thinks unbound, calls seen,
calls unclassified, fire calls, and where each fire call stopped — and running one 400-frame capture on
COMMAND printed `31 thinks (31 unbound)`, which is not a subtle number. Moving one line below the bind:

| | before | after |
| --- | --- | --- |
| thinks run / unbound | 31 / 31 | 31 / 0 |
| CALL steps reached | 0 | 31 |
| creature sounds | 3 | 34 |

WASTE3's Gunner goes from 0 decoded thinks to 22 and from silence to 16 sounds. BASE0 reports 0 decoded
thinks, which is correct: the Soldier is hand-transcribed and does not use this path.

The 31 calls COMMAND reaches are all import `+0x12C` (`0x80040800`), which is not one of the five projectile
spawners. It takes a player index or 4 for "all", reads the active player count at `0x800B2C2C`, walks the
player array at `0x800D5C30` and tests each player's position against a supplied point and a radius on the
stack — a per-player proximity call. Named that far and no further.

So the fire routing committed an hour ago is still not observed in play: what a Tank Commander reaches in a
400-frame capture is its sounds and this proximity call, not its attack thinks.

## A creature was being sent to a melee it does not have

Chasing "why does a decoded creature never fire", with counters rather than reading. `q2_ai_decision_stats`
records how far an attack got — checkattack reached, enemy invisible, decision run, decision yes, attack
callback run, attack callback missing — and `q2_cre_action_stats` gained a per-slot tally of which moves the
generic implementation could find. One capture on COMMAND said it:

    attacks  190 checkattack (77 blind, 60 decided, 53 yes), 10 attack calls
    moves    attack set 0 / missing 0, melee 0 / missing 43

Fifty-three attacks granted, and the generic attack handler never ran once — while the melee handler ran 43
times and found no move to play. The Tank Commander was going to melee, standing there, and going again.

**Its module has no melee callback.** The census lists stand, idle, walk, run, attack, sight, pain and die.
But `q2_creature_spawn` installed the IMPLEMENTATION's callbacks unconditionally, and the generic
implementation supplies a handler for every slot because it does not know which creature it is being used
for — so `m->melee` was non-NULL for a creature that has no melee. `M_CheckAttack` then does exactly what the
original does: `m->attack_state = m->melee ? Q2_AS_MELEE : Q2_AS_MISSILE`. The transcription was right; it was
being lied to about the creature.

A callback the module does not have now stays NULL: `c->callback[slot]` is the module's own address for that
slot and is zero when it has none, so the module decides and the implementation only supplies. After it, on
the same map: `attack set 1 / missing 0, melee 0 / 0` — no phantom melee, and the attack move installs.

The Soldier is unaffected, as it should be: its module has every callback, and BASE0 reports the same 19
shots before and after.

- [x] 54. **ANSWERED: the generic attack handler always takes the FIRST move its callback installs, and for
      the Tank Commander that is the one attack of four that does not shoot.**

      A think index only ever runs because an animation FRAME names it, so the question was never about the
      AI — it was about which frames the move being played carries. `q2psx-inspect creatures` now prints that
      for all 97 moves on the disc: `(via) range -> think bytes`, the callback that installed each move and
      the distinct think bytes its frames call.

      The Tank Commander's attack callback installs FOUR moves:

      | range | thinks its frames call |
      | --- | --- |
      | 77-114 | 0, 6, 11, 9 |
      | 168-196 | 0, **13** |
      | 115-135 | 0, 6 |
      | 55-70 | 0, **8** |

      Its fire thinks are 8, 10 and 13. `generic_attack` calls `set_via(m, 6)`, which returns the first move
      for that slot — 77-114, whose thinks are the `+0x12C` proximity call and two sounds. Every attack it
      makes is the one that does not shoot, which is exactly the 31 unclassified calls and no fire calls the
      counters reported. `cre_generic.c` documents "the first move a callback installs is the one taken" as a
      caveat about randomised animation; for this creature it silently disables firing altogether.

      The same census answers it for the others, and they do not all lose: **the Arachner's first attack move
      (94-109) carries thinks 3 and 4 and both call `+0x8C`, the rail**, and the **Infantry's first (199-206)
      carries think 11, which melees and calls `+0xFC`**. Those two should fire on the current code. The
      Gunner loses the same way the Tank Commander does — its first attack move (108-128) carries thinks 0
      and 1, while `+0x84` is on think 2. Berserk and Insane have no attack move at all, which is right: they
      are melee creatures.

- [~] 55. **READ, for the Tank Commander. It is a range-and-chance table, and the move the port plays is the
      one for a DEAD enemy.**

      `tankcomm_attack` is at `module+0x11BC`, and it decodes cleanly:

          v1 = self->enemy                     ; entity+0xBC
          v0 = v1->[0x24]->[0x108]             ; the enemy's health
          if (v0 < 0) {                        ; bgez v0, +0x54
              self->currentmove = M_77_114;    ; module+0x1DE4
              self->aiflags &= ~0x200;
              return;
          }
          v    = enemy->origin - self->origin  ; three subtractions onto the stack
          dist = import[+0xB8](v)              ; the vector's length
          r    = import[+0x14]()               ; 0..32767

          if (dist < 1501)       move = (r < 13106) ? M_168_196 : M_55_70;   ; 40%
          else if (dist < 3001)  move = (r < 16384) ? M_168_196 : M_55_70;   ; 50%
          else if (r < 10813)    move = M_168_196;                           ; 33%
          else if (r < 21626)  { move = M_115_135; self->[0xA8] = g + 50; }  ; 66%
          else                   move = M_55_70;

      **The first branch is the whole bug.** `M_77_114` — the move the port plays every time, whose thinks are
      a proximity call and two sounds — is what the creature does when its enemy's health has gone NEGATIVE.
      It is the after-the-kill animation. `q2_creature_move_via` returns the first move for a slot in decode
      order, and for this creature the first is the one that only ever runs over a corpse.

      The three live-enemy moves are the ones that shoot: `M_168_196` at close range or on a roll (nineteen
      consecutive frames of think 13 — the sustained burst), `M_55_70` otherwise (three separate think 8s),
      and `M_115_135` on the middle roll at long range, which also sets a timer at `entity+0xA8`.

      The 1501 and 3001 are the range bands in the module's own units, and 13106, 16384, 10813 and 21626 are
      0.4, 0.5, 0.33 and 0.66 of 32768 — the same shape as `M_CheckAttack`'s odds and as id's `tank_attack`.

      What is left is transcription: this is one of the six modules, read but not yet written, and the other
      five have their own tables at their own `cb at` addresses. The census prints those addresses, the thinks
      each move calls and where they sit, so each is now a reading job rather than a search.

  *As first written:*

- [ ] ~~55. **Which of a callback's several moves the module picks.**~~ The module's attack function branches —
      on range, on a roll, or on state — and the decoder records only WHICH moves a callback installs, not
      the branch that chooses between them. Four attack moves for the Tank Commander, two for the Gunner,
      two for the Infantry. Until that branch is read, a generic creature will always play one of them, and
      for two of the four ranged creatures that one is silent. This is the concrete form of "transcribe the
      remaining six modules" and it now has a per-creature list rather than a shrug.

- [x] 56. **ANSWERED AND FIXED: the decoder was not modelling the branch DELAY SLOT.**
      MIPS executes the delay slot whichever way a branch goes, and the compiler puts a `lui` in it: when the
      branch is taken the `lui` runs and its matching `addiu` is at the TARGET. A linear register walk pairs
      that `addiu` with whatever the FALL-THROUGH path last left in the register, which yields a plausible
      in-image address that fails validation and is dropped without a word.

      The Arachner's run callback (`0x80101010`) is exactly that shape:

          lw   v0, 220(a0)        ; aiflags
          andi v0, v0, 1          ; STAND_GROUND
          beq  v0, zero, +0x18
          lui  v0, 0x8010         ; <- delay slot
          lui  v0, 0x8010
          addiu v0, v0, 5944      ; 0x80101738, the stand-ground move
          jr   ra
          sw   v0, 216(a0)
          addiu v0, v0, 6152      ; TARGET: 0x80100000 + 6152 = 0x80101808

      The linear walk computed `0x80101738 + 6152`, not `0x80100000 + 6152`, so the real run move at
      `0x80101808` was never recorded. The tracker now keeps each register's last `lui` beside its tracked
      value and offers BOTH candidates for a materialising `addiu`; every candidate is validated structurally
      before being accepted, so the wrong one costs nothing.

      Disc-wide: **97 moves become 101, and moves attributed to the run callback go from 5 to 8.**

      What that does in play, POWER1, the same 500-frame capture:

      | | before | after |
      | --- | --- | --- |
      | checkattack reached | 0 | 101 |
      | attack callback ran | 0 | 3 |
      | run move found / missing | 0 / 2 | 5 / 0 |
      | decoded thinks run | 28 | 33 |

      The Arachner went from standing still for the whole capture — no run animation, so `M_MoveFrame` had
      nothing to advance and it never moved a unit — to running, chasing and attacking. The Soldier is
      unaffected: BASE0 reports the same 19 shots.

  *As first written:*

- [ ] ~~56. **Some callbacks have no move attributed to them at all.**~~ The Arachner never moves on POWER1:
      `run 0 / missing 2` — `set_via(m, 4)` is called and finds nothing, so there is no animation to play and
      `q2_M_MoveFrame` advances nothing. Its module has a run callback (or `m->run` would now be NULL) but no
      move records `via == 4`, which means it installs one indirectly — through another move's endfunc, which
      the decoder marks `via == -1`. Sixteen of the disc's 97 moves are `via == -1`. The Gunner shows the same
      shape on WASTE3, where it never reaches a single `checkattack` in 700 frames.

  *As first written:*

- [ ] ~~54. **A decoded creature has still not been seen firing.**~~ Two real obstacles are gone — the think
      table was being wiped, and the phantom melee was absorbing every attack — and the fire routing is
      covered by unit tests, but no capture has yet produced a `fire_sent`. What the counters now say is where
      to look: on COMMAND the Tank Commander's attack move installs and the thinks that run carry import
      `+0x12C`, not the fire slots, so the question has narrowed to which of its animation frames carry think
      8, 10 and 13 and whether the attack move reaches them. On WASTE3 the Gunner never gets as far as one
      checkattack in 700 frames, and its module has no run move via slot 4 either — a separate thread.

## Where a fire think sits in its move, and why the animation never gets there

The census now prints each move's think bytes **in frame order, run-length encoded**, because which thinks a
move calls is only half the question. `0*12 3 4` and `3 4 0*12` are the same set and a very different
creature: an animation that is cut short reaches the second and never the first.

The Arachner's attack move, 94-109, is `0*5 3 0*5 4 0*4`. Its fire thinks are 3 and 4 — both call `+0x8C`,
the rail — and they sit at frame **6** and frame **12** of sixteen. The move has to survive six AI ticks
before anything is fired.

A per-index tally of which thinks actually run says it does not. On POWER1, 500 frames, five live creatures:

    moves      attack set 3 / missing 0, run 5 / 0
    think hit  1:12  6:9  7:12
    decoded    33 thinks, 36 calls (36 unclassified), 0 fire calls

Thinks 3 and 4 never run. Nor does 0 — which is the first thing the tally settled: **think byte 0 means "no
think"**, and every move on the disc is mostly zeros, so an absent 0 in the tally is correct rather than
alarming. The 1, 6 and 7 belong to the other creatures in that zone.

So the attack move is installed three times and cut short before its sixth frame each time.

- [x] 57. **RETRACTED: nothing cuts it short. The premise was wrong.**

      A per-tick trace — one creature, one line, showing its move, frame and attack state — settles it in
      thirty lines. On COMMAND, creature 0:

          t9    move 30   frame 31   as 4      <- the decision
          t10   move 77   frame 32   as 1      <- the attack move installs
          t11   move 77   frame 77   as 1      <- snaps to its first frame
          t12   move 77   frame 78   as 1
          ...   ...
          t29   move 77   frame 95   as 1      <- still playing, nineteen ticks in

      The Tank Commander's attack move plays through cleanly. It is never interrupted, by pain, by run, by
      stand or by anything else. The question this entry was opened to ask does not exist.

      What was actually happening is what question 54 already established and I then talked myself past: the
      move that plays, 77-114, carries thinks `0*6 6 0*3 6 0*7 11 0*6 9 0*11 6` — a proximity call and two
      sounds, and **no fire think at all**. Its fire thinks live in the other three moves the same callback
      installs, and it never plays those:

      | move | thinks in frame order | verbs |
      | --- | --- | --- |
      | 77-114 *(the one that plays)* | `0*6 6 0*3 6 0*7 11 0*6 9 0*11 6` | `5*38` |
      | 168-196 | `0*5 13*19 0*5` | `4*5 0*19 4*5` |
      | 115-135 | `0*15 6 0*5` | `4*21` |
      | 55-70 | `0*9 8 0*2 8 0*2 8` | `4*16` |

      The 168-196 move is worth looking at: nineteen consecutive frames of think 13 with no verb running under
      them. That is a sustained burst — the creature holds still and fires for nineteen ticks.

      The "cut short before its sixth frame" claim came from the Arachner on POWER1, where the counters said
      the attack move installed three times and thinks 3 and 4 never ran. The trace shows why that was a bad
      inference: creature 0 there spends the whole capture in its run loop with an enemy and `as 0`, so those
      three attacks belonged to other creatures whose moves I never checked. Counters over a capture cannot
      attribute an event to an actor; a trace can.

      **Question 55 is therefore the whole of the remaining gap** — which of the four moves an attack callback
      installs is the one the module picks. Nothing else is in the way.

  *As first written:*

- [ ] ~~57. **What cuts the attack animation short — and it is NOT the AI verb, which was my guess.**~~

      The census now prints the `ai` byte per frame beside the think byte, run-length encoded, and the answer
      is that the port already has this right. Across the disc the verbs land exactly where they should:

      | move installed by | verb its frames carry |
      | --- | --- |
      | stand | 1 |
      | walk | 2 |
      | run | 3 |
      | attack, melee | 4 |
      | pain, die | 5 |

      and `q2_ai_verbs[]` is already one-based to match — `NULL, ai_stand, ai_walk, ai_run, ai_charge,
      ai_move` in the executable's own slot order at `0x800D561C`. So the Arachner's attack move runs
      `ai_charge` sixteen frames running (`ai: 4*16`), which is correct and does not hand the creature back to
      the chase. **The suspicion recorded when this question was opened is wrong and is withdrawn.**

      One move on the disc is worth noting on the way past: an attack whose verbs read `4*5 0*19 4*5`. Verb 0
      is the slot the original stores zero in, so nineteen of its frames deliberately run no verb at all —
      a hold in the middle of an attack.

      What remains: the attack move installs three times in a 500-frame capture and never advances past its
      sixth frame. The counters now cover every callback that can install a move, and the shape is:

          attack set 3, melee 0, run 5, pain 0, die 0, stand 14

      **Pain and die are ruled out** — the obvious guess, that the player shooting the creature in a `--watch`
      capture keeps interrupting it, is wrong: neither fires once. **Stand is installed fourteen times**, more
      than run and attack together, so the creature is oscillating back to standing rather than being
      interrupted by damage.

      Two more things checked and correct on the way past, so neither is looked at again: the medic branch of
      the dead-enemy test (`enemy->health > 0` really does mean "stop caring" for a medic, and it is guarded
      by `Q2_AI_MEDIC` exactly as id guards it), and the sight client the AI treats as the player, which is
      set up with `in_use`, both halves of the INUSE flag, `client`, and health 100 — so creatures are not
      dropping their enemy because the proxy looks dead.

      Also measured: of 101 `checkattack` calls in that capture, 97 reach the decision and only 3 say yes.
      That is consistent with `M_CheckAttack`'s own odds rather than a fault — the chances are 0.4, 0.2, 0.1
      and 0.02 by range band.

      What is left is a per-tick trace of one creature: which move is installed, which frame it is on, and
      what replaced it. The counters say what happens across a capture; they cannot say what happens between
      two consecutive ticks, and that is now the only question.

  *As first written:*

- [ ] ~~57. **What cuts the attack animation short.**~~ The generic run handler installs the run move whenever
      `m->run` is called, and the counters show run installed 5 times against attack's 3 in the same capture.
      In the original a monster's attack move plays out because its frames carry an AI verb — the `ai` byte of
      `{u8 ai; s8 dist; u8 think}` — that keeps the AI in the attack rather than returning it to the chase.
      The suspicion is that the port's frame driver maps that verb in a way that lets the chase reinstall the
      run move on the next tick. That is the last thing between a decoded creature and a shot: the fire is
      routed, the import is named, the think is decoded, the move is installed, and it is being interrupted
      six frames too early.

## A decoded creature fires

`22 fire calls: 22 sent`, `think hit 8:22`, on COMMAND over 400 frames. The first shot from a creature this
project never transcribed — and it took one function.

The chain that had to hold, end to end: the AI grants an attack (`M_CheckAttack`, transcribed), the attack
callback picks a move, the move's frames name a think, the think decodes to a step, the step names an import,
the import is one of the five projectile spawners, and the hook fires. Every link was already in place. The
attack callback was picking the wrong move.

`cre_tankcomm.c` writes ONE callback. Everything else — stand, idle, search, walk, run, pain, die and all
thirty-two think indices — stays on the generic handlers and the decoded actions. That is now possible
because `q2_creature_bind` falls back to the generic implementation's method for any think index a partial
transcription leaves NULL, which turns "transcribe a creature" from an all-or-nothing job into a per-function
one.

The transcription is the table read last round, with two departures stated in the file: the distance is
compared as a square against 1501² and 3001² so no root is taken, which orders identically; and the timer at
`entity+0xA8` that the long-range branch sets is not written, because which global it reads is not
established. The move it accompanies is installed either way.

Before and after on the same map and capture length:

| | before | after |
| --- | --- | --- |
| think hits | 1:12 6:9 7:12 | **8:22** |
| CALL steps reached | 31 | 88 |
| fire calls | 0 | **22** |
| fire sent | 0 | **22** |

Five modules remain, and each is now a reading job at a printed address rather than a search: `q2psx-inspect
creatures` gives every callback's address, every move's record address and installing callback, and the think
bytes each move's frames call in frame order.

## The Gunner, and one more import named

Second transcription, same shape as the first: one callback, everything else on the generic handlers and the
decoded actions. `gunner_attack` is at `module+0x1814` and is shorter than the Tank Commander's —

    v0 = import[+0xB4](self, self->enemy)
    if (v0 == 0)                  move = M_137_143;
    else if (import[+0x14]() & 1) move = M_108_128;
    else                          move = M_137_143;

**`import[+0xB4]` is `q2_range`**, which this port already carries — the eleventh import slot named, and this
one identified by more than shape. `0x8005EF84` subtracts the two origins, takes `q2_vector_length_sq`
(`0x8005C59C`, already named in `ai.h`), and compares against `0x003F803F` or `0x000FE00F` depending on
whether the entity's class byte at `+0x23` is 68. Those are 2040² - 1 and 1020² - 1: the long-melee and
ordinary melee bands, with the same off-by-one `q2_range` already reproduces in `monster.c`. So the branch is
`range == Q2_RANGE_MELEE`, read rather than guessed.

Unlike the Tank Commander, the Gunner was not silenced by the generic handler: **both** its attack moves are
firing animations, so taking the first happened to agree with the original. The transcription makes that
agreement deliberate and adds the melee-range branch the generic handler had no way to know about.

On WASTE3 the Gunner now attacks — `2 checkattack, 1 yes, 1 attack call` — and **think 13 runs 33 times**,
which is `M_137_143`'s first frame. It still fires nothing, and the census says why:

- [x] 58. **ANSWERED AND FIXED: a move's endfunc could only be run if it was also a callback or a think.**

      `resolve_endfunc` maps an endfunc address to one of the thirteen callbacks or thirty-two methods the
      implementation carries. A standalone installer is neither, so it resolved to NULL and the chain it
      exists to make simply did not happen.

      The Gunner's is three instructions at `module+0x11D8`:

          lui   v0, 0x8010
          addiu v0, v0, 7292        ; 0x80101C7C — move 144-151
          jr    ra
          sw    v0, 216(a0)         ; delay slot: self->currentmove = it

      That move is eight frames of think 2, and think 2 is the one carrying `call(+0x84)`, the hitscan. The
      Gunner's entire ranged attack was on the far side of a function the port could not name.

      The decoder now records what such an endfunc installs — one store to `entity+0xD8`, a materialised
      address that validates as a move record, and nothing else, so a function that does more is left alone —
      and the bind installs a shared endfunc that performs it. **43 of the disc's 101 moves have a decoded
      endfunc target**, where none did.

      A second delay slot caught this one out on the way: the store is in `jr ra`'s slot, and the first
      version tracked that word without checking it, so every endfunc on the disc reported as installing
      nothing. That is twice in two days the delay slot has hidden something — worth remembering as the first
      thing to check when a linear walk comes back empty.

      WASTE3, 700 frames: `think hit 1:22 2:1 6:15 7:15 13:1`, `1 fire calls: 1 sent`. The Gunner reaches
      think 2 and fires. The Tank Commander is unaffected — COMMAND still reports 22 of 22.

  *As first written:*

- [ ] ~~58. **The Gunner's fire think is 2, and think 2 lives in a move nothing reaches.**~~ Its move
      `144-151` is eight frames of think 2, and think 2 is the one carrying `call(+0x84)` — the hitscan. That
      move's `via` is **-1**: no callback installs it, so it is reached only through another move's endfunc.
      Presumably `M_137_143` or `M_108_128` ends into it. The endfunc chain is resolved at bind time
      (`resolve_endfunc`), so either the chain is not firing or the move it names is not the one the census
      thinks. Sixteen of the disc's 101 moves are `via == -1` and this is the first one shown to matter.

## The Infantry and the Arachner, and one attack that declines

Two more attack callbacks read and written. Four of the seven modules now have one; the Berserk and the
Insane have no attack callback at all, which is correct — they are melee creatures — so the ranged set is
complete except for the Soldier, which was transcribed long ago.

**Infantry**, `module+0x1848`, the shortest of the four:

    v0   = q2_range(self, self->enemy);
    move = (v0 != 0) ? M_184_198 : M_199_206;

One range test, no roll. Both moves shoot — 199-206 carries think 11, a melee plus `call(+0xFC)`; 184-198
carries think 8, `call(+0x84)` — so the generic handler's choice was not silent, merely always the
close-quarters one.

**Arachner**, `module+0x12CC`, the only one so far that can DECLINE:

    v    = self->origin - enemy->origin;
    dist = length(v);
    if (dist < 1053) return;                       ; installs nothing
    self->[0x5C..] = enemy->origin;
    self->[0x60]  += enemy->[0x4C];
    move = (|v.x| + |v.z| < |v.y|) ? M_94_109 : M_130_132;

Two things the generic handler could not have expressed. It **installs nothing** below 1053 units, so a close
Arachner carries on with whatever it was playing — `set_via` always installs something. And the choice is
**vertical**: `|dx| + |dz| < |dy|` asks whether the target is further away up-or-down than along the floor,
which for a creature that walks walls is the question that decides the attack. Not a range band, not a roll.
94-109 is the rail (thinks 3 and 4, both `call(+0x8C)`); 130-132 is a three-frame gesture with no think at
all.

The `self->[0x5C]` block the original fills with the enemy's origin, and the accumulation into `self->[0x60]`,
are not written: what reads them is not established and neither affects which move is installed.

**Neither was observed firing in a capture.** POWER1 reports `1 fire calls: 1 sent`, but the think hits there
are 2, 6, 7 and 13 — the Gunner's, since that zone carries one. POWER2's Infantry does not reach a single
attack in 500 frames. That is the same limit as ever: these creatures engage rarely in a scripted demo run,
and a capture long enough to catch one costs more than the harness has. The transcriptions are read from the
disassembly and tested for regressions; they are not yet confirmed in play, and this says so.

## Two address spaces, and the check that tells them apart

`moddisasm` was reading the wrong creature, and would have gone on doing it silently.

`q2_ai_module_load` relocates a COMMON.DAT's whole `CreAIBin` as ONE blob. That is right for a LevelBin and
wrong for CreAIBin: the chunk is a LIST of modules — a 12-byte name, a next-offset, a body — and `creatures`
relocates each one separately to its own base. **The two address spaces agree only on a map carrying exactly
one module.**

The trap is that a wrong address disassembles perfectly well. Feeding the Berserk's melee address from
`creatures` into `moddisasm WASTE4` produced clean, plausible MIPS: a health test, a move install, an aiflag
clear — the Tank Commander's attack, in a different module entirely.

**The check that catches it, and that vouches for the four transcriptions already written: do the moves the
code installs appear in that creature's own move list?** Every one of the Tank Commander's, Gunner's,
Infantry's and Arachner's did, which is why those reads were sound. The Berserk's did not, which is what
exposed this.

`moddisasm` now takes a creature name and relocates that module alone, so the addresses match what
`creatures` prints and the question stops arising.

## The Berserk, and the ranged set closed

With the right module in view the Berserk's melee at `module+0x11D8` is the simplest of the five:

    r    = import[+0x14]();            ; the random
    move = (r & 1) ? M_84_95 : M_76_83;

A coin. No range test, no health test. Both moves are swings — `76-83` carries thinks 3 and 4, `84-95` thinks
3 and 6 — so the generic handler taking the first was half right, and this adds the other half: the creature
alternates its two swings instead of always throwing the same one.

That is **five of the seven modules** with a hand-written callback. The Soldier was transcribed long ago. The
Insane has no attack, no melee, and its stand, walk and run callbacks are all the same address — a
non-combatant, correctly modelled by the generic handlers alone.

WASTE4 reports `24 fire calls: 24 sent` and COMMAND holds at `22 of 22`. All 26 tests pass.

## Rotator coverage is complete, and the number that looked like a gap is not one

"26 rotators" has been quoted back at this project as though it were a fraction of something larger. It is
not, and `zonescript` now says so in the output rather than leaving it to be inferred:

    rotation CALLs  : 95  in COMMON's scripts, disc-wide
      too short     : 0   (the item cannot hold the operands)
      no object     : 69  (first object slot is -1)
      usable        : 26
    rotators built  : 26  (one per object slot each call names)

Ninety-five CALL items across the disc name a rotation primitive. **Sixty-nine of them have -1 in their first
object slot**, and the original's constructor stops at the first negative one — `0x80028628`, recorded in
`rotator.h` since the builder was written. Those calls install nothing on the console either. Not one item is
too short to hold its operands.

So the ratio that matters is **26 of 26 usable calls build a rotator**, and the port skips nothing the
hardware does not. A disc-wide count of what a system *could* act on is only a denominator if the data
underneath it is live; here two thirds of it is not.

## Players can hit each other

`combat.targets` held creatures and nothing else, so in a deathmatch every shot passed straight through
everybody. It now holds every creature plus every OTHER player, rebuilt per player rather than per frame —
a player's own actor is skipped, because registering it would let a bolt hit its own muzzle.

The pointers are stable: the live player's hurt-actor is `sim->combat.self` and a parked one's is
`sim->pcombat[i].self`, both inside the sim.

Measured, which is the point of building it this way:

| players | targets | of which |
| --- | --- | --- |
| 1 | 0 | — |
| 2 | 1 | 1 other player |
| 4 | 3 | 3 other players |
| 2 on COMMAND | 26 | 26 creatures, 0 players (that map has no MultiSpawn, so only one spawns) |

**A parked player takes damage on their actor, and their inventory is a separate field.** Only the live
player's pair is synchronised, so a hit landed while parked has to be copied back or it is lost when their
frame runs. That sync exposed a real bug immediately: `q2_sim_player_reset_combat` initialised an extra
player's inventory but left their ACTOR zeroed, so the first sync wrote health 0 over a full 100 and three of
four players ended a capture dead without anything having shot them. The actor is now built from the
inventory at reset, so the pair starts in step.

Single player is byte-identical, all 26 tests pass.

## A kill has a killer

`q2_mp_attribute_kill` has taken a killer's id since it was written, and nothing could supply one. The engine
keeps it as a signed byte at `entity+222`; the port's `q2_actor` had no equivalent, so a deathmatch kill had
a victim and nobody to credit it to.

An actor now carries `owner` — which player it is, or -1 for anything that is not one — and the damage
function writes `target->last_attacker = attacker ? attacker->owner : -1`. Each player's actor is stamped
with its own index at spawn, and any player whose health crosses zero goes through
`q2_mp_attribute_kill` and then `q2_mp_player_killed`, which is the engine's own hook at `0x800396AC`
reproduced: `(*module)->[4](killer, victim)`.

Checked in `test_combat` rather than asserted:

- a shooter with `owner = 2` leaves `last_attacker = 2` on its victim, and attribution gives player 2 the frag;
- world damage leaves -1 and stays nobody's frag;
- **lava is nobody's frag even when a player last touched you** — the environmental means of death are
  excluded by the rule `q2_mp_attribute_kill` already carried, and this is the first caller able to reach it.

Not observed in a capture: the demo players are scattered across an arena firing blindly and never hit each
other in 1200 frames. The scoring path is wired and unit-tested; it has not scored a frag in play, and this
says so rather than implying otherwise.

Single player is byte-identical, COMMAND still reports 22 fire calls of 22 sent, all 26 tests pass.

## Staging an encounter, and what it has not yet shown

Every unverified claim in the last few rounds has the same shape: "wired and unit-tested, not observed in
play". The reason is always the harness — a scripted demo wanders, it does not arrange a fight. `--watch`
already fixed that for one player against a creature by standing the player in front of one. `--dm-stage`
does the equivalent for players: it puts the others a few hundred units from player 0, aims everyone at
everyone by POSITION, and holds their fire button.

Two real faults fell out of building it, both of which would have silently defeated any attempt to test
player-versus-player damage:

- **Aiming by reversing a yaw is not aiming at someone.** The first version set the other player's yaw to
  player 0's plus 2048, which points them back down player 0's line of sight and only coincides with pointing
  AT them when player 0 happens to be looking at the right spot. 900 frames of that produced nothing.
  `q2_vectoyaw` on the difference of the two positions is the fix.
- **The extra players had no weapon.** `q2_sim_player_reset_combat` called `q2_inventory_init` and set
  `weapon_id = 0`, which is "no weapon" — so three of four players spawned holding nothing and could not have
  fired a shot between them. They now start with what player 0 has, which is what a deathmatch start does.

- [~] 59. **A staged encounter produces shots but no damage, and the counters have narrowed it to one
      comparison.** Three real faults fixed on the way, then an instrument that should have come first.

      Fixed, each of which alone would have prevented a hit:

      - **the target pointer aliased.** `client_targets_for` chose `&combat.self` for whichever player was
        live when the list was BUILT — but `q2_sim_advance_player` swaps afterwards, so that pointer named a
        different player by the time the shot traced. Player 1 was firing 301 shots at its own actor. Every
        other player is parked during `who`'s tick, so `&pcombat[i].self` is always right.
      - **the hurt-actor's position was stale.** `q2_actor_from_player` only runs inside
        `q2_sim_hurt_player`, so an actor's origin was wherever that player last got SHOT. With one player
        nothing ever traced at them and it never showed. It is now updated every tick.
      - **the extra players had no weapon**, and aiming was by reversed yaw rather than at a position — both
        recorded above.

      Then the counters, which say where a shot stops considering a target: `190 tested, 0 skipped, 0 dead,
      146 behind, 44 beyond world, 0 off axis, 0 hit`.

      **Nothing is off-axis**, so aim is not the problem — the ray is either pointing away from the target
      (146) or the world stopped it first (44). Adding 2048 to the computed yaw made it 1409 of 1409 behind,
      which confirms the convention already in use is the right one and that the 44 are the genuine
      in-front cases.

      **The units are consistent** and that line of enquiry is closed: `along = (dot * 4096) / len2` is a
      1.12 fraction of the ray, `world_fraction` is clamped to the same 0..4096, and `q2_combat_ray_dist_sq`
      says why — "nothing is normalised because the direction's LENGTH is the weapon's range". So a shot
      counted `beyond world` really was stopped by geometry.

      Placing the staged player IN FRONT of player 0 rather than at a blind diagonal — the same reasoning
      `--watch` uses to frame a creature — moved 9 shots from `beyond world` into `off axis`, which is the
      first evidence of a shot reaching a target at all.

      **And then three further changes produced byte-identical counters: 2126 / 1988 / 129 / 9 every time.**
      That is the finding. Numbers that do not move when the thing they supposedly measure is changed are not
      measuring it: the scans are player 0's shots, not player 1's. Player 0 fires from the demo pad
      constantly, is aimed at the top of each frame by the staging, and then turns away during its own tick —
      which is exactly the 1988 `behind`. The staging holds the EXTRA players' aim and never held player 0's.

- [ ] 59a. **Hold player 0's aim while staging, and count the extra players' scans separately.** One shared
      counter cannot say whose shot it was, which is the same mistake as reading a whole-capture creature
      count and attributing it to one creature. The counters want a per-player dimension before the next
      attempt, not another fix.

      One real bug fell out of looking: `pcfg.style = c->sim[pi].player[0].look_scheme` still read the OLD
      per-player sim array, which has been an uninitialised struct since the players moved into `sim[0]`.

  *As first written:*

- [ ] ~~59. **A staged encounter still produces no damage.**~~ Two players 339 units apart, facing each other by
      position, both holding fire, both with the level's weapon, each registered in the other's target list
      (`has 1 targets (0 creatures, 1 other players)`) — and both end 900 frames at 100 health. Everything
      upstream is measured: the target list is right, the actors have `radius` 286, the attribution rule is
      unit-tested and the kill hook is wired. What has NOT been measured is whether the shot is taken at all
      for a player whose frame runs through `q2_sim_advance_player`, and that is the next thing to count
      rather than reason about — the fire path reads `combat.last_shot`, which is part of the swapped half.

---

## ⚠ Security note (carried forward, do not drop)

- [ ] 38. A prior research pass reported that a fan wiki page about this game served content containing
      **instructions addressed to AI agents** (create files, transfer funds, insult the operator, terminate
      operations). That URL was **not** fetched during verification. **Treat it as hostile for any automated
      fetch**; if data from it is wanted, a human should open it in a browser.
      Consequently, all web-sourced claims in the release census — the existence of the NTSC SKU, its
      timestamps and track lengths, barcodes, magazine demo-disc serials, and the absence of a Japanese
      release — remain **unverified** and should be treated as moderate confidence at best.
