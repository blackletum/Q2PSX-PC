# Changelog

What has changed, per release, written for someone deciding whether to download
this rather than for someone reviewing the diff.

Entries land under `## [Unreleased]` as work happens. Cutting a release moves
that section into a dated one of its own and empties the queue, and the same
text becomes the body of the GitHub release — so the notes are written by
whoever did the work, not reconstructed afterwards from commit subjects.

Headings are fixed; `scripts/changelog.py categories` prints the list, and
`scripts/changelog.py check` rejects anything else. Empty headings are dropped
when the notes are rendered, so leaving one at `_Nothing yet._` costs nothing.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
the version numbers follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the major version is 0, a minor bump is where behaviour is allowed to
change; see [`docs/RELEASING.md`](docs/RELEASING.md).

## [Unreleased]

### Reconstruction
- Creatures drop what they carried. A creature whose spawn record carries the drop flag now leaves an item when it dies: `monster_death_use` queues it in the console's four-slot queue (`0x80020D60`), the frame drains it through the 30-row pick table at `0x800AB79C` (`0x80020680`), and the item is thrown from the body's own origin with the console's toss and lands before it becomes a pickup (`0x8002085C`, `0x80020C48`). A scripted firefight on SECURITY asks for six drops and all six land. Nothing had ever called any of it, so every creature on the disc died empty-handed.
- Gibs. A body taken past its gib threshold comes apart as it does on the console: a blood spray laid over its posed mesh (`0x8005B320`), then a ring of tumbling chunks thrown along the killing hit's push (`0x8005A3D4`, `0x8005A0AC`), each trailing blood until it lands (`0x80059DE0`). Before, a Soldier hit by a rocket played an ordinary death.
- Damage shows on the body. An energy hit crackles across a creature's posed mesh, the other damage timers throw sparks off it, and a player under quad damage wears the blue shell — the five hooks of `0x8005B880`, drawn through the third particle spawner (`0x8002FDFC`, absolute world points) and a posed world-vertex lookup (`0x8006CC44`) the port did not have. A corpse carrying one of the two lingering effects dissolves and is freed rather than sparking on (`0x8005B2A8`).
- Each creature sees from its own eye and turns at its own rate: a walker from its model's height — a Soldier at -252, a Tank Commander at -508 — a flyer at -250 and a swimmer at -100, turning 228 or 114 a tick where every creature had turned 200 (`0x80062448`..`0x80062584`). A prone Insane is a flyer for its whole life.
- Event scripts run once when they say once. A one-shot item retires itself before it dispatches (`0x80027468`) — BASE2's end-of-unit call drops from twenty runs to one on an in-out-in walk — a record's one-shot is its DISABLED bit, so an ENABLE genuinely re-arms it (`0x80027950`); opcode `0x09` WAIT is live (`0x800276C4`); an accepted zone gate abandons the rest of its record (`0x80027784`); and a trigger volume must have its enable bit set and its id match the player's cell (`0x80027E64`), a gate checked against every event volume on the disc before it was switched on.
- Crossing a zone boundary inside a map keeps the level's script state. The console carries every record's and item's latch across a same-map zone change and replays the spent ones with seven settling passes (`0x8002936C`, `0x800296D4`..`0x8002974C`), so a door a script opened arrives open and a one-shot batch, message or secret does not happen twice. The port had been re-arming the whole script at every seam.
- The status bar is the console's. The ammo counter reads the pool the weapon fires — the rocket launcher showed 200 where it holds 50, and the BFG showed 0 — the selected weapon is drawn in the centre slot the port never had (`0x80037CAC`), the previous and next weapons sit either side at their true left edges, faded away from the centre and drawn as the console draws them, an additive icon over a subtractive shadow; health goes negative, to -99; every field draws at the console's 1.5x brightness; split screen gets its own numeral size; and a dead one-player bar strips back to health and the gun (`0x80033C68`).

### Client
- _Nothing yet._

### Rendering and audio
- Split screen: a particle group's per-viewport skip tested the wrong bit, so nothing marked as one player's own was ever hidden from the others (`0x80030614`).

### Tools
- `q2psx-inspect ai` checks 149 constants against the executable, up from 130, all passing: the death-drop chain, the go-routines' eye heights and turn rates, and the start wrappers.
- `q2psx-inspect hud` no longer always exits 1 — it read a count after freeing the struct that held it — and now checks the weapon strip's table and the per-weapon ammo pools against the executable.
- `q2psx-inspect creatures` prints a census of every placed creature's eye height and turn rate, and `events` shows the zone-gate aborts.

### Fixes
- Armour did nothing but the weakest thing it could. The projection that hands the player to the damage function wrote a literal 0 into the armour class, and 0 is jacket — so combat and body armour both absorbed 0.30 of an ordinary hit instead of 0.60 and 0.80, and neither absorbed anything at all from an energy weapon, where jacket's column is genuinely zero. Because that projection is rebuilt on *every* damage attempt there was no window in which the field could hold anything else, and no save or pickup could get a real class past it. The power shield was worse off still: the two bits the damage path tests live in the inventory's flag word, which the projection never copied, so `q2_combat_power_armour_absorb` returned at its first guard, spent no cells and saved nothing. A player wearing body armour now takes 19 of a 100-point hit rather than 69.
- Quad damage multiplied nothing. Every fire function on the disc picks one of two immediates by comparing the level clock against the player's own expiry word, and the sim called them with that comparison hardcoded to false. The powerup was picked up, drawn on the HUD, counted down and played its firing sound for thirty seconds while the shotgun kept doing 6 a pellet.
- The difficulty never reached the damage function. `skill` sat at the default 1 for the whole run because only the clock was refreshed in the combat rules, so easy was exactly as dangerous as medium: the rule that halves what a monster does to you at skill 0 could not fire. It now comes from the same global the creature AI already reads, so the two cannot disagree.
- Nothing ever told the sim it was in a deathmatch. `q2_sim.multiplayer` had one writer — the save loader — and a match started from the menu left it false, so every rule that hangs off it ran in its single-player form inside a deathmatch: the railgun did 100 instead of 150, armour used the rounding bias that favours the wearer, items gave single-player amounts and did not respawn, and the rocket-jump ceiling was applied where the console does not apply it.
- The player carried two different gib thresholds. The damage path used -100 and the death chain used -40, which is the one the disc writes at `0x800397FC`; there is now one copy. The corpse health floor was likewise spelled out twice under two names, so the two clamps could drift apart while both looked cited.
- Warnings-as-errors had never passed on any compiler, so CI had been red since 20 August and nothing it said could be trusted. Every one is fixed rather than switched off: 33 on GCC and Clang, and another 20 behind them on MSVC, which stops at the first and so had never reported the rest. Six were real — two undefined behaviour (`gte_sxy` read through a `psx_xy` in the glint and bolt draws), a computation left dead by an earlier fix, an always-true bound on a `u8`, a POSIX function reached through a platform `#ifdef` that bought nothing, and a nested struct zeroed with too few braces. Thirteen were formats that really could truncate a path or a menu label and now say what they cut to. Two MSVC warnings are turned off, both with a reason: C4100 is the unreferenced parameter this project already ignores on GCC and Clang, and C4127 fires on every `CHECK(SOME_TRANSCRIBED_CONSTANT == 36, ...)` in the test suites, which is what those suites are for.
- A solid collision node did not hold nothing. `q2_coll_point_in_node` carried its own copy of the solid-bit mask instead of asking `q2_collision_node_is_solid`, and on one MSVC build the copy did not fire while the accessor did — so a node marked impassable let a point sit inside it. There is now one place that decides what solid means. Found only because MSVC had never been able to build the project in CI, so its tests had never run there.
- The Environment Suit was god mode. The port refused every hit while it ran; the console's only test of it sits inside the acid arm of the damage function (`0x80058230`), so it stops acid and nothing else.
- Splash damage went through walls. Radius damage now sweeps from the blast to each candidate and clips against the entity boxes before it hurts anything (`0x80050A24`, `0x80050A3C`), and the blast's own owner takes half (`0x80050A0C`).
- The GAME VARIABLES page changed nothing. The menu computed the cheat word and dropped it, so ONE SHOT KILL, NO FALL DAMAGE and the rest remembered a choice and acted on none (`0x8001C698`). ONE SHOT KILL also now fires on a hit that armour absorbs whole, where the console falls straight through (`0x80058390`).
- On Easy, creature shots landed at full strength. The client handed every shot to the damage function with no attacker, so the rule that a monster does half to you at skill 0, rounded up (`0x800582C8`), could only ever fire for a claw.
- The player cried out at every world death, and in deathmatch a creature's kill could still cost the victim a frag. The death voice fires only on a raw -1 in the killer byte, which only acid and lava write (`0x80039728`, `0x800396DC`); the frag gate is that raw byte, signed, below 4 (`0x80039774`); and a deathmatch hazard that kills with no attacker is the victim's own suicide (`0x80057DC8`).
- Armour's rounding bias follows the difficulty, not deathmatch (`0x80057C10`), and the inventory's own damage helper no longer absorbs a flat third of every hit.
- Soldiers were silent when hurt, and a Tank Commander's idle played nothing. Their sound slots now fall back to the names the map's bank carries, as the modules do when they load (Soldier `+0xE50`, Tank Commander `+0x808`); a scripted firefight on SECURITY goes from eight creature sounds missing from the bank to none. The Tank Commander's machine gun also fires with no enemy, as the console's does.
- The weapon strip's left icon was drawn on top of the armour icon. The strip table holds left edges, and the port had subtracted a width (`0x80033448`).
- Saves are version 6. This round changed what a saved event flag means, so a version-5 save's spent script records are migrated on load and stay spent.

### Build and packaging
- The repository is licensed: GPL-2.0, in `LICENSE`, and it ships in the release archives as that licence requires.
- Five megabytes of rendered frames — a title screen, a HUD test, two model renders, two level renders and an accidental screenshot of a terminal window — were tracked at the repository root while the README said the repository contains no game assets. They are gone, `.gitignore` covers them, and `scripts/check_paths.py` now fails the build on any tracked file that is an image, a sound or a film — by extension *or* by magic number, because the screenshot was a PNG named `C`.

### Documentation
- `docs/openquestions.md` #135 records this round: the frontier measured (461 of the 821 game functions were cited nowhere in the port), what was reconstructed, and what review caught that no unit test could.

## [0.1.0] - 2026-08-28

### Reconstruction
- Every on-disc format is read: the `.DAT` container, zones, scene graph, geometry, collision hulls, spawns, lights, triggers, level scripts, sound banks, models and animation. Checked across the PAL disc's 164 level files — 461,852 vertices, 274,936 quads, 139,240 collision planes, 94,642 collision portals, 1,723 models, 2,036,080 animation keys and 2,475 sounds — with zero failures.
- `docs/openquestions.md` closes with no open questions: 157 resolved, 18 partial with the remaining residue stated, and 4 marked terminal because this disc cannot answer them.
- All seven creature modules are transcribed from their own MIPS, 57 of 57 callbacks, together with the framework they run on — `T_Damage`, `M_ReactToDamage`, and the 71-slot import table named end to end.
- The player's whole frame, the pad read with its nine control styles, and the view's three independently decaying kicks are read out of the executable rather than approximated.
- The `.STX` film format is read *and* written. All 5,301 frames of the three films decode, and the encoder returns every one of `TAKE1BP.STX`'s 7,712 sectors byte-identical, EDC and Reed-Solomon included.

### Client
- `q2psx --version` reports the build and the commit it came from, the way `q2psx-inspect` already did.
- The campaign plays through: eleven levels across five units, Strogg Outpost to Final Showdown, with the mission screen at every unit boundary, the briefing on arrival, and the inventory carried across.
- The boot chain runs ahead of the menu — four logo screens and the intro film — and the campaign ends on the outro, both played to the frame the original stops them at rather than to the end of the file.
- The front end: title screen, single- and multiplayer pages, player, sound and video options, the credits, and all nine memory-card screens.
- Saved games in four slots plus quick save, holding the level clock, the script's flags, trigger residency, collected items, which doors are open and where in their travel, which windows are broken, and who is dead.
- Multiplayer with `--dm`: up to four players in split screen, each with its own spawn, pad, camera, viewport and inventory, sharing one world, played through to the frag limit and the scoreboard.
- Doors and lifts move, rotating brushes turn, scripted ambushes fire, key gates hold, hazards hurt and glass breaks.

### Rendering and audio
- A software rasteriser built to the PlayStation's rules rather than filtered to look like them: exact fixed-point GTE with its saturation flags and reciprocal table, affine texture mapping, ordering-table sort with no depth buffer, 15-bit RGB555 with the ordered dither, all four semi-transparency modes, texture pages, CLUTs and the mask bit. Every one is individually toggleable, so the same build runs perspective-correct at 4K.
- Models draw textured, animated, lit through the GTE's own three-light gather, and backface-rejected against the model linker's own NCLIP pair.
- Lens flares, the water warp, the damage flash, and the status bar with the icon and caption for what you just picked up.
- Audio: SPU-ADPCM sound bank playback for the menu, items, player and every creature; and each map's own seven-track XA playlist.

### Tools
- `q2psx-inspect` identifies a disc, verifies every level file against the documented schema, and checks every format claim in the docs against the disc — so "we understand this format" is something the build evaluates rather than something a document asserts. It also renders levels, models, HUDs and menus to a PPM with no window, and carries a PS-X EXE loader and an R3000A disassembler for the questions only the executable can answer.
- `stx2avi` demuxes a film into the raw video and audio streams ffmpeg can mux, driving the same decoder the game uses rather than a second one.

### Fixes
- The repository could not be cloned on Windows at all. A tracked file was called `nul.ppm`, and `NUL` is a DOS device name, so Git refuses to create it: the *checkout* failed, not the build. Renamed to `parity-frame.ppm`, content unchanged, and `scripts/check_paths.py` now fails CI on any tracked path Windows cannot create.
- Nothing had ever built on Linux. The link failed on undefined `sin` and `sqrt` because nothing asked for libm, which glibc keeps in a library of its own where Windows and macOS fold it into the C runtime.
- `disc.c` did not compile under GCC either. `fseeko`, `ftello`, `off_t` and `strtok_r` are POSIX rather than C11, and this project compiles as strict C11, under which glibc hides them — and an implicitly declared `strtok_r` would have truncated its returned pointer to 32 bits on a 64-bit host. A CD image can exceed what C11's `long`-based `fseek` reaches, so the POSIX interfaces are declared rather than given up.

### Build and packaging
- The version lives in one file, `VERSION`, which CMake reads to seed `project()` and the generated `version.h`. A binary, a tag and an archive cannot disagree about what was built.
- A manual release workflow builds Windows, Linux and macOS, tests each, and only then tags and publishes — so a tag never points at a commit that does not compile. `scripts/` holds the version, changelog, release-note and packaging tools it runs, each usable by hand.
- CI builds and tests on GCC, Clang, MSVC and Apple Clang with warnings as errors, and checks that the version each binary reports is the one in `VERSION`.
- A fetched SDL3 never shipped in the Linux or macOS archives, so those would have contained a client that could not start. Shared libraries now build beside the executables and the client carries an `$ORIGIN` rpath.

### Documentation
- `docs/RELEASING.md` describes the versioning scheme and how to cut a release.
