# Functional parity follow-up

Updated 28 October 2026 against the current checkout and the user's PAL
SLES-01534 disc. This pass checks integration and continuation behavior. It
does not claim whole-game equivalence to a hardware capture.

## Corrected behavior

| Gap | Result |
| --- | --- |
| Creature/player ambient used the same guessed value | Creature bodies now initialize and recover toward native RGB 64; gameplay player bodies use RGB 32. Refresh preserves energy-hit colour, while a fresh player body resets it. |
| Extra players always returned to RIGHT STICK | Each player now owns the controller settings edited through their pause menu. Style selection survives later input frames, respawns and map loads. |
| Inversion, vibration and controller sliders borrowed player one's values | Menu bindings, displayed values, input decoding and motor output use the pausing/playing owner's configuration. Video, sound, crosshair and autocentre remain shared settings. |
| Player one's host mouse suppressed every player's autocentre | Continuous mouse look clears only player one's recenter state. Other players retain the shared retail pad-autocentre setting when their movement runs. |
| Old settings cards could retain another session's new settings | Appended controller fields preserve all existing indices. Missing fields load their defaults; truncated settings payloads are refused before mutation. Saved games and multiplayer settings cards include the new profiles. |
| Returning from multiplayer could retain the previous pauser | The front end selects player one's controller profile on entry. |
| PROCEED skipped retail's player setup | Each player now has an independently controlled NAME / COLOR or TEAM / CONTROLLER panel and a READY state. All active players must be ready, and Team Deathmatch requires both teams. Disconnect/reconnect preserves the panel and cursor. |
| Match startup discarded chosen identities | Names, teams and personal colours survive startup and return to the menu. Bodies, end banners and results use those choices; settings cards save them with the controller profiles. |
| `--movie` returned to QFRONT after completion | Standalone playback exits after the film's authored stop and fade. Campaign and boot films retain their successors. The last film render target remains available for `--shot`. |
| Credits used a guessed HUD-font scroll and looped forever | Read the actual 177-row tables, retain blank spacing, use the menu font and fixed title, clip the roll, and return to Options on Back or completion. Rendering no longer advances time. |
| The campaign outro returned straight to the title | The outro enters credits, then the authored WELL DONE question. Easy/Medium/Hard immediately grant Bronze/Silver/Gold, including when Back skips credits or No declines a save. |
| Unlock tiers never reached persistent settings or saved games | MULTI UNLOCK files share the settings slots and load through MULTIPLAYER → LOAD SETTINGS. Ordinary settings and game saves also retain the tier. Older files remain readable and imply tier NONE. |
| Gold's ALL WEAPONS flag had no grant consumer | Multiplayer start, resume and respawn grant weapons present in the current CastList, fill each player's ammo capacity, retain the held weapon and refresh the carousel. Ordinary frames do not refill ammo. |
| COMMAND's white-key return spawned a fresh zone-4 population | The native key predicate suppresses startup selection. Saves made after collecting the key preserve their already resident creatures and pickups. |
| SECURITY's boss arrival and escape depended on unimplemented frame callbacks | Two completed Tank Commander deaths summon Boss1 and start the four wall timers. The boss's completed death opens the hatch. Pending events and timers survive cold saves. |
| BOSS2 never opened the train after the final fight | The rider's completed death selects Zone4, enables the train door and updates mission text once, including after leaving the arena during its death animation. Jorg's first stage alone cannot open it. |
| POWER2's reactor and laser frame callback never ran | Both pumps start coolant drainage, timed glass destruction and the exit explosion. Early departure completes the reactor; saves preserve an active sequence. The tagged guard shuts off RockLaser, and the two zone-4 banks alternate on their authored clock. |
| POWER1's native explosions and shaft damage never ran | The entrance repeats its two explosions; the shaft follows its sixteen-phase cycle, player-height gate and strict damage box. Cold saves retain its deadline and phase. |
| Acid/lava damage omitted its burn cries | Shared damage admission now queues the victim's sound, with acid protection/throttling and lava's first/fifth/ninth cadence. Retail's `pla_burn2` fallback restores sound in all nineteen PAL banks that carry a burn sample. |
| Zero player damage entered knockback, while negative damage was rejected | Zero skips the impulse block but retains downstream feedback and cheats. Signed damage now follows retail's armour arithmetic, health stores and creature reactions. |
| The event opcode census omitted COMMON scripts | The inspect tool counts both COMMON and ZONE items and lists every FX damage record, so caller claims include both script sources. |
| Every damaging hit assigned an invented weapon flinch | Damage and its feedback now preserve the stored view amplitudes, as retail does. The native reader, pain deadline and separate impact shock remain active. |
| BOSS1's zone-0 laser callback was skipped | The five red entrance lasers follow their authored event states, deal damage once per logic tick and shut down at `DoorBlown`. Cold saves and zone returns retain them; the zone-1 sweep and projection paths stay confined to their branch. |
| BASE3/MAGDEMO's laser wall and shutdown callback were absent | Both zone-3 beams render and damage actors; the contact volume reads the resident zone's operands. The tagged Gunner's completed death disables the wall permanently, including after saves and zone/map returns. |
| Ordinary LASERBEAMs rendered without hurting actors | The shared walk now applies each kind's authored damage and MOD to its first actor, drains creature damage immediately, and respects script shutdown across saves and zone returns. |
| Shared effects ran after the native mission callback | Teleports, laser walls and ordinary lasers now precede that callback, matching retail's random-number and shutdown order. |
| LAB's shared lifts reset during zone changes | Lift A and Lift C retain their position, motion, waiting timer and collision across both seams, cold saves and map returns. A new game clears their history. |
| Loading a mover's saved pose left collision at its starting position | Saved-game apply now rebuilds mover collision before movement and prevents the whole restored offset from becoming a false one-tick push. |
| Five more shared lifts reset at zone seams | SECURITY's two lifts, BOSS1's entrance lift and BOSS2's paired stems now preserve motion and collision through zone changes, cold saves and map returns. |
| SECURITY and BOSS2 omitted their post-boss earthquakes | Both callbacks now apply the native physical impulses, rumble and random explosion sound with their original timing and saved cadence. |
| SECURITY's laser course stayed fully enabled and lacked its moving beams | Its 24 bank/ring/grid gates now follow the native clock and alternating sides; both moving kind-1 beams render and hurt actors on their authored paths. |
| SECURITY's exit lift never latched, and orb arcs vanished on their expiry frame | The lift now holds its upper landing and collision pose through saves and revisits. The orb submits all five arcs after updating every trajectory, preserving expiry damage and native random-number ordering. |
| LAB sweep sparks were absent and BOSS1 sparks used the wrong render area | Both callbacks now use their cached emitter areas. LAB's final signed sweep phase survives saves, and scripted actor use follows both sweeps rather than preceding them or running during a render-only refresh. |
| Restart retained native hazard clocks, while zone gates rearmed actor-use attempts | Full loads now reset module state even within the same map. Ordinary gates retain LAB/BOSS1 use latches and module clocks, subject to each zone callback's explicit resets. Saved games retain their recorded state. |
| Sound effects bypassed native Gaussian filtering and envelopes | Voices now run the SPU predictor, Gaussian filter, pitch accumulator and ADSR at 44,100 Hz, with release tails and immediate logical-slot reuse. Twenty-eight published hardware capture cases match after documented phase alignment. The host output conversion and unmeasured SPU transitions remain open in the audio audit. |
| Music and movies used the wrong CD gain scale | The CD matrix now uses 128 as unity and the mixer applies the native CD-input and main-volume gains. All 512 stereo/volume register combinations are checked against executed PAL SDK code. Independent final-output amplitude and clipping measurements remain open. |
| The mixer downsampled native voices and omitted CD interpolation | Mixing now remains at 44,100 Hz, with native voice consumption and the CD's seven-phase, 25-tap XA interpolation. Movie scheduling/fades use the same output clock. The software reference, full suite and disc sweep pass; independent XA waveform and device-output measurements remain open. |
| RESTART LEVEL reloaded the zone the player died in, with the blaster, and RESUPPLY did the same | Both now restore the copy of save memory taken when the level was last fully loaded. That brings back the arrival zone, position and facing, the carried inventory and every zone's script, population, tallies and mission words. RESUPPLY tops up the restored record as `RestartLevel(1)` does. Settings, cheat level, skill, continues and the random generators carry on (2 November section). |

The controller ownership follows the per-player 34-byte records used by
`0x8001C81C`, the pauser selection at `0x8001C9B4`, and the vibration reader at
`0x8003179C`. Reset at `0x8001BDA8` iterates all four records. Existing
`Q2_SET_*` indices are unchanged; the three additional seven-field profiles
fit the current saved-game and settings-card payloads without a format break.

The host has one keyboard/mouse owner. Extra gamepads expose their own analog
styles and inversion; their unavailable USE MOUSE row is disabled and grey.
They can change their controller profile by pausing and opening PLAYER CONTROL.
These changes do not add multiple host mice or assign physical devices differently.
Before starting a match, PROCEED now opens the shared player-setup screen. The
keyboard and host pointer control player one's panel; each gamepad controls its
assigned panel. The name editor uses directions to select/cycle characters, or
the pointer and wheel on the host. Back leaves a subpanel or cancels READY.

### Player-setup reconstruction

The previous sweep missed this entire reachable screen because its multiplayer
scenarios launched arenas directly. QFRONT `+0x59CC` opens module page 11;
`+0x5D94` dispatches seven per-player states and `+0x75C8` constructs their rows.
`+0x6654` supplies the eight-character alphabet (A–Z, 0–9 and space), while
`+0x6B94` selects four personal colours or two teams. `+0x6F34` allows a ready
player to back out. The all-ready path calls `+0x1F60` to start the match.

QFRONT's two-player builder calls engine `+0x168`, bound at `0x80079C94` to
the **stacked** viewport builder `0x80077900`, with projection 210 and far
distance 4000. Player two's rows move to local x=384 at `+0x9BB8`. Three and
four players use quadrants with projection 160. `+0x9C24` reveals only each
viewport's chosen body. Text, selection bars and models are rendered from the
actual disc assets. These are disassembly-derived layouts; paired hardware
captures have not yet established pixel equivalence.

Settings-card schema 2 appends 24 halfwords for four names, teams and colours.
Its bounded payload capacity is 96 halfwords. Schema-1 cards remain readable
and restore default identities. Truncated records and invalid identity bytes
are rejected before changing live settings. Older executables cannot read the
new schema; settings indices are unchanged. The completion work below adds
reward metadata to container version 2, independently of the opaque schema.

## Rechecked reports

### Credits reconstruction

QFRONT `+0x35C8` installs DEVELOPED BY from `+0xFAF4`; `+0x36A0` streams
the remaining rows from the table at `+0xFB24`, advancing before reading.
The previous string-pool scan missed the opening line, repeated text and
74 blank rows. The generic menu scanner's rejection of empty labels and
single-row tables was mistakenly interpreted as evidence that no tables existed.
The reader now validates raw or relocated records and retains their order.

The native callback uses a sixteen-object ring, inserts every sixteen ticks,
and moves one pixel per tick. The port advances this on the disc's logical
video clock, independent of host rendering. It uses the 16-pixel menu face,
a fixed 32-pixel CREDITS title, the native y=60..203 clip, and QFRONT's subpage
logo state. The normal Back/completion callback is `+0xCCA4`, which opens
Options. The last nonempty row clears at tick 2965; completion follows at 2966.
Esc, Backspace, right-click and controller Back use that return. Confirm and
movement do not dismiss the page.

### Campaign completion and unlock files

The separate campaign continuation is now wired:
the outro enters dispatcher state 17; `0x80018800` calls `0x8007CCE0(1)`,
setting engine `+0x4A8`. QFRONT `+0x3244` then opens credits with a different
return callback, `+0x4454`. Both Back and natural completion reach that callback.
It assigns engine `+0x378` to difficulty + 1 before asking whether to save an
unlock file: Bronze for Easy, Silver for Medium, Gold for Hard. This is a direct
assignment, including after an earlier higher award. No returns to the title
with the award still active; the question has no Back action. Its four static
rows at `+0xF044` and YES/NO at `+0xF0BC` use the 16-pixel face and subpage logo.

YES (`+0xD2C4`) sets bit 31 while clearing the game-save type bits. The native
backend names that file `Q2_MULTI UNLOCK` (`0x80080968` / `0x800AD7D4`), alongside
ordinary `Q2_MULTI DATA`. `0x80080CF0` saves/restores Game Config's engine
`+0x2D0..+0x38B`, which includes the tier. The directory filter at `0x800821DC`
groups both MULTI kinds for LOAD SETTINGS; it does not automatically load the
award at startup. Successful save acknowledgement returns to MULTIPLAYER
(`+0xA244`); cancel returns to the question. Host I/O uses the existing four
settings positions and overwrite/report screens, separately from game slots.

`Q2CF` version 2 adds checksummed tier/type metadata after its opaque payload.
Version 1 remains readable. Invalid tiers, types and unlock-without-tier files
are refused before applying any state. Saved games use an optional `UNLK`
chunk; older games without it restore tier NONE. No installation saves or
settings were migrated or modified during verification.

Reaching Gold exposed a second missing consumer: `Q2_CHEAT_ALL_WEAPONS` was
set but never granted anything. Native `0x8003B040` filters ids 1..11 by the
map's view-model names, ORs their owned bits, fills all six ammo pools to the
current base/bandolier/pack limits, retains the current gun, and writes the
carousel pair. Its callers are variable installation (`0x8001C7BC`, all present
players) and respawn (`0x8003DE38`). The client now performs those event-time
grants, while its ordinary per-frame scalar/settings refresh remains free of
inventory grants. Removing the toggle leaves weapons already granted intact.

The previously recorded three `raster_quads` failures no longer reproduce in
the starting checkout: all 67 baseline CTests passed.

The recorded chaingun cold-reload RNG failure was in its fixture. It advanced
weapon animation without advancing audio, so one-shots never ended. The first
client filled the 24 voices and stopped drawing pitch RNG; the freshly loaded
client could allocate voices and still drew it. The fixture now advances the
production headless mixer every frame and checks that neither timeline drops
voices. All 334 disc-backed save checks pass, including weapon state, ammunition,
projectiles, shot serials and RNG continuation. No gameplay-RNG or voice-allocation
workaround was introduced. Transient sound tails remain outside saved games.

### Native population progression

Reviewing the remaining selector sites exposed three reachable gaps that an
unconditional static name scan could not establish. The census inspected 83
selector calls in 21 campaign/demo LevelBin modules. COMMAND branches on the
white key; SECURITY and BOSS2 call the selector from gameplay callbacks after
specific tagged deaths. The client now implements those decisions and keeps
the existing static scanner as a fallback, with its limitations explicit.

Kill completion follows retail's corpse-detachment/departure calls to
`RegisterKill`, not the first zero-health frame. Stable creature identities
and zone history distinguish a completed casualty from a dormant spawn or
Jorg's living second stage. The optional NMIS v2 chunk adds four SECURITY wall
timers; v1 and saves without NMIS remain readable. Existing pending-event
serialization also preserves an explosion queued just before saving.

The focused selector fixture passes 259 checks. It verifies the real wall and
hatch geometry changes after restoring their pending events, both final-boss
stages, cold saves, map/zone returns and departure while the rider is dying.
COMMAND's white-key fixture changes from four enemies and three pickups to
zero, with the same deterministic camera. SECURITY captures show the arena
before and after Boss1 appears. Disassembly, census, captures and logs are in
`.agents/tmp/functional-parity/selectors/`; detailed retail addresses are in
[ENTITY-AUDIT.md](ENTITY-AUDIT.md).

### Reactor callback follow-up

Tracing actual init registrations found POWER2's missing `+0x11A0` callback.
The core now uses its original red beam faces and spark emitter. Both pumps
start the objective popup and coolant drain; six timed glass breaks stop the
core and emit the native beam/explosion/debris effects; the exit opens at
10000 ticks. Leaving the reactor early completes it for the next visit.
The tagged zone-3 guard's completed death disables RockLaser. Zone 4 alternates
its two laser banks with the original 47-tick dark windows.

NMIS v3 adds the start clock, alarm accumulator and consumed-glass mask while
retaining v1/v2 compatibility. Cold saves preserve the active sequence and
pending event queue. The focused fixture passes 205 checks with real disc
data, disk saves and deterministic engine captures; its unpatched baseline
failed eleven behaviour checks. Evidence, the callback registration census and
before/after images are in `.agents/tmp/functional-parity/callbacks/`.

The image comparison also found that native beams were being queued with a
collision-cell index instead of the drawing-area byte the port expects. The
core and shared LAB/BOSS1/SECURITY helper now resolve that byte, and the fixture
requires actual emitted faces. The corrected core capture differs visibly
from its baseline; a non-empty beam queue alone had missed the defect.

The census also disproved the suggested COMMAND boss-counter gap: its module
installs no frame callback and never consumes the zero-initialized counter.

### POWER1 callback follow-up

POWER1's `+0x5F0` callback and `+0x414` shaft helper are now implemented. The
entrance repeats two explosions after each strict 900-tick deadline. The shaft
advances every 100 ticks through seven blasts and nine silent phases, limiting
itself to the first two blasts when the player is below the shaft. Its strict
damage box uses entity-origin coordinates and sends 30 points through the
ordinary lava damage path. Zone changes reset the deadline while retaining
the phase; map changes clear both; cold saves preserve the exact active state.

NMIS v4 adds these two words and reads versions 1 through 3. The focused fixture
passes 180 checks, with disk save/restore, all cycle phases, damage boundaries,
replay/render exclusion, and an actual headless frame call. Six behaviour
assertions failed on the unpatched baseline. The engine captures show the
restored blast above the shaft's central pedestal. Evidence is under
`.agents/tmp/functional-parity/power1/`.

The entrance coordinates fall outside the retail collision hull. Retail's
unchecked area lookup consequently reads the node-count byte in the chunk
header. The port reproduces that value safely and retains the original spawn,
sound and shock rather than relocating the effects. `MakeBlitzes` has no named
event on the disc, so its kill-counter call is inert. BIGGUN's secondary
callback is its already implemented lightning draw, confirmed against its
eight accumulator updates and by 117 passing disc-backed client checks.

### Shared acid and lava sound path

The missing POWER1 damage cue exposed a shared integration gap. Acid helper
`0x8003DEE4` sounds on each hit admitted by its protection and 400-tick gate.
Lava helper `0x8003DF0C` increments the process-wide word at `0x800AE898` before
testing its low two bits. The executable seeds that word to **3**: calls 1, 5,
9, and so on sound, rather than waiting for the fourth call. Its only located
read/write sites are that helper. The port retains the call count across all
loads and in-session save restoration, with a fresh client starting at zero
calls. Saved games do not rewind it; a paired retail card test remains unmeasured.

Lava requests sound before general invulnerability; acid's suit/invulnerability
tests precede sound. Armour absorption and zero incoming player damage still
reach this stage. The request belongs to the hurt player, and all players share
the lava cadence. A full event queue still consumes the admitted hit. BIGGUN
uses this common hook in place of its old special-case cue, and the client now
drains events after native frame callbacks so POWER1's late cue is not discarded.

The real-disc fixture found a second defect: `Sound_Burn` at `0x8003B7EC` prefers
`pla_burn1` but explicitly falls back to `pla_burn2` at `0x8003B894`. The client
had only named the first. Across all 49 PAL banks, none carries burn1, nineteen
carry burn2, and thirty carry neither. Tests exercise both choices and missing
pairs; no additional replacement is introduced. The production mixer now emits
the selected sample with the existing pitch variation of 33 or 34.

### Signed damage and complete script census

The outer damage routine tests the original amount for zero at `0x80057EC0`
before knockback. Zero player damage still reaches protection, feedback and
the ONE SHOT KILL rewrite, but must leave pending impulse state untouched.
Negative damage enters the native nonzero paths. Both armour helpers reject
zero rather than nonpositive damage, and a negative saved amount restores
inventory. Power armour compares against the signed halfword of twice the
cells; ordinary armour sign-extends its shifted low halfword before the cap.
The full signed remainder passes between stages. The client now also delivers
the creature pain callback for a negative direct-damage reaction, while ordinary
healing outside damage remains free of that callback.

`tests/fixtures/generate_combat_reference.py` runs the actual PAL instructions
at `0x80057A9C` and `0x80057BE4` in a bounded integer interpreter with branch
and load delays. Its sole external hook records `SetWinFade`; it is not a
general PlayStation emulator. The input segment's SHA-256 is pinned to
`2dbdb53efd709a4cb5930ce91a39b55e8edad162c5fb773a7c6fa164a960fda8`.
The generated header contains 130 numeric vectors, with no retail code. They
cover signed damage, positive controls, zero, both skill biases, all armour
classes, energy damage, fade ordering and halfword inventory boundaries.

The caller search also exposed an incomplete inspect-tool census: it counted
ZONE scripts alone. Including COMMON yields 164 Events chunks, 4,179 records
and 6,646 items. All nine FX damage copies are positive: BIGGUN has five copies
of lava damage 1, and WASTE3 has four copies of damage 65. The previously
reported seven copies omitted the two COMMON records. No normal-play negative
damage source has been established; extreme inventory vectors describe defined
arithmetic, not an observed gameplay encounter. These signed-damage edge cases
have no demonstrated ordinary-play pixel change; the weapon correction below
has its own independent capture.

### Damage preserves stored weapon offsets

Following the guessed amplitude writer found another visible discrepancy.
The outer damage routine `0x80057D54` and PlayerThink's feedback block
`0x8003AE10..0x8003AFA8` preserve `client+0x9C/+0x9E`. Spawn initializes the
client with a 224-byte clear at `0x8003B2BC`. The port nevertheless assigned
`damage * 8`, capped to 455, with roll selected from the impact side. That
created an extra weapon flinch. The current client already gives the world
camera plain aim; these offsets feed the gun through `0x8004F404`.

The assignment has been removed. The measured offset reader and serialized
fields remain, including native scaling of explicitly seeded values by the
210-tick pain deadline over a 150-tick period. Impact shock is independent:
the damage routine calls `0x80040800` for its MOD-dependent GameWindow effect.

The reference generator now also executes native damage, the feedback block
with its established register bindings, and view-offset composition. Its 132
cases cover all 22 MOD values, signed/zero damage, both impact sides, armour,
skill, zero amplitudes and seeded amplitudes. The real fade and impact-shock
routines execute; explicit hooks cover memset, axis-vector normalization and
burn/pain audio dispatch. These bounded cases establish the changed paths,
not arbitrary module execution or a whole-console playthrough.

The new simulation checks failed 206 assertions with the guessed assignment
and pass after its removal. A disc-backed fixture waits for the actual pain
deadline, then captures the render target after a 20-point melee hit. Before
the fix its composed offsets were pitch -224 and roll -112; afterward both
are zero, with health still 80 and the damage flash retained. The comparison
changes 10,321 pixels, all in the weapon rectangle (303,140)..(494,247).
Captures are `damage-view-before.png` and `damage-view-after.png` under
`.agents/tmp/functional-parity/damage-audit/`.

### BOSS1 entrance lasers

The callback census exposed a missing branch inside a registered handler.
BOSS1 `+0xD3C` tests the resident zone name and calls `+0x1708` for zone 0.
That branch emits five kind-4 lasers when their named records become disabled,
unless `DoorBlown` is disabled. It bypasses the random sweep velocities,
phase clocks and projection-use callback that belong to zone 1. The client
previously ran only the latter path.

The new disc-backed regression reads the beam order and endpoints directly
from LevelBin and exercises all 32 combinations, damage, the render-only path,
disabled rendering, authored event execution, cold disk saves and zone returns.
It passes 815 checks. A control run with the new dispatch branch removed fails
112 assertions and draws zero beam faces; the corrected capture draws 260.
The matching headless images and disassembly are in
`.agents/tmp/functional-parity/boss1-entrance/`. JAIL5's revisited
`ExplodingBox` / `QuadInCrate` branch remains inert because this disc has no
matching population group. See [the entity audit](ENTITY-AUDIT.md) for offsets
and gate semantics.

### BASE3 / MAGDEMO laser wall

Reviewing the next frame callback found three connected omissions: the shared
LASERWALL renderer/actor sweep, its resident-zone contact-damage binding, and
the native completed-kill shutdown. The wall now emits its two thin red beams,
applies 512 beam damage and 200 contact damage, and stays off after the tagged
Gunner finishes dying. Leaving during its death animation also completes the
kill. Saves retain the state before, during and after death.

The headless fixture uses the actual Scene bounds and scripts on both maps.
Its matching before/after frames change 4,477 pixels and draw 84 beam faces
after the fix. The contact test initially failed six assertions, exposing the
COMMON `-1` object index that had suppressed damage even in resident zone 3.
Details, offsets and MAGDEMO's older import-ABI qualification are recorded in
[the entity audit](ENTITY-AUDIT.md#base3--magdemo-laser-wall).

### Ordinary map lasers and frame order

The shared `DrawLaserBeams` walk at `0x8002EE38` calls the damaging routine
`0x80048DC8`. Passing `ends=0` suppresses endpoint particles, not the actor
sweep at `0x80048F8C` or damage call at `0x80048FB4`. The port had implemented
only the visual half. Crossing a red JAIL2 laser left health at 600; the same
fixed-step capture now shows 88 after one 512-damage hit.

The disc fixture covers all 77 resident beam instances in POWER1, POWER2,
JAIL2, JAIL3, COMMAND and SECURITY. These are 71 authored live beams, six of
which appear in more than one zone; the remaining authored JAIL2 item is
never registered. It checks independent damage/MOD constants, render-only
frames, hidden rendering, exhausted visual pools, disabled records, immediate
creature pain/health persistence, COMMAND's real `LasersOff` switch, a cold
disk save and zone returns. Entering a map without beam records clears the
previous set.

`QuakeLoop` at `0x80038FA4..0x80038FD4` runs teleports, laser walls, ordinary
lasers, then the registered mission callback. The client now uses that order,
including the teleport walk's unconditional 45 random draws. POWER2's zone-4
clock verifies that the bank still draws on its shutdown frame and that the
newly enabled six-beam `Laser1` bank appears on the next frame.

### LAB lift continuity

The next callback review found LAB's `+0x3D0` lift recorder and `+0xD8` entry
restorer had no native implementation. Lift A spans zones 0/3 and Lift C spans
2/4, with different Scene indices on each side. The generic event replay left
both at the bottom on entry. A disc-backed failing control moved each 3600
units upward, crossed the seam, and observed offset zero and idle state.

Both lifts now retain their motion and collision data in unit state. The
entry hook rebinds by name after event replay; neither unloaded zones nor
failed loads erase a previous snapshot. NMIS version 5 adds two bounded
60-byte records to the saved-game payload, retaining readers for versions
1–4. Earlier files cannot recover unloaded lift history they never recorded.
The fixture covers both travel directions, open/wait timers, closing motion,
collision traces and subsequent steps, cold saves with and without a resident
lift, map returns and new-game reset. Native offsets and geometry are recorded
in [the entity audit](ENTITY-AUDIT.md#labs-shared-lift-state).

An older-save case also exposed a shared MOVR restore gap: visible displacement
was applied while collision stayed at its initial pose. Four failing assertions
established the stale bounds and false next-frame push. Saved-game apply now
rebuilds collision immediately, with zero previous motion when an old file did
not record it. V5 LAB records retain their exact previous step.

### SECURITY, BOSS1 and BOSS2 lift continuity

The broader module review found five more lifts with the same omitted
save-memory callbacks: SECURITY's `Lift A`/`Lift B`, BOSS1's `Lift A`, and
BOSS2's `LiftStemA`/`LiftStemB`. All span zones 0 and 1. The failing disc
control moved them partway upward and observed every one reset to idle at
offset zero after crossing the seam. Their state now survives zone and map
departures, saved games and return trips, with collision restored immediately.
BOSS2's paired parts resume together. Separate entries prevent the shared
`Lift A` names from overwriting one another.

NMIS version 6 appends five 60-byte records, keeping versions 1–5 readable.
The combined regression saves all seven native lifts while absent, reopens
the client and checks each map's state. The focused fixture also covers open
wait timers, closing motion, both directions of every seam, collision traces
and next-frame movement, failed loads and new-game reset. Native addresses,
Scene indices and timings are in
[the entity audit](ENTITY-AUDIT.md#security-and-boss-map-lift-continuity).

### SECURITY and BOSS2 earthquakes

The native callback review found two omitted earthquake sequences. The
failing disc control completed both maps' actual boss gates but produced no
player impulses or rumble. SECURITY now shakes from the two Tank Commander
deaths through the fourth wall explosion. BOSS2 begins after the rider's
defeat and its strict 300-tick delay, then follows the native shaking/quiet
cycle. The callbacks restore physical movement, Explosion rumble and the
random positional explosion sound, including the original signed arithmetic.

Three saved clocks preserve pulse cadence through cold loads and zone changes.
NMIS version 7 keeps all earlier readers and leaves an unrecorded legacy BOSS2
clock inactive. Full loads, including Restart/Resupply in the same map, reset
module clocks and SECURITY's pending wall timers, while retaining unit
completion flags. Details and
native addresses are in
[the entity audit](ENTITY-AUDIT.md#security-and-boss2-earthquakes).

### SECURITY laser course

SECURITY's zone-0 callback at `+0xE90` was absent: the failing control found
all 24 patterned lasers enabled and neither moving beam. The restored callback
cycles the six bank positions out and back, the four ring positions and the
two grid positions, alternating sides on the world counter. Its two moving
beams follow a triangular sweep, including their initial vertical section.
Both use the native kind-1 damage and first-hit actor rules.

The static beam walk still precedes the callback, so a changed gate affects
the next frame. Rendering alone does not apply damage or advance either
clock. Existing saved level time, world counter and event flags preserve the
course through cold loads and zone returns without a new save field. Native
addresses, numeric periods and endpoint details are in
[the entity audit](ENTITY-AUDIT.md#security-laser-course).

### SECURITY orb and exit lift

The missing zone-2 callback now holds `HexLift` at its upper landing once
player 0 crosses the native strict height threshold. Its unit-save flag
survives zone changes, map returns and cold saves. Restoring that flag also
restores the raised collision box, while retaining BASE0's different conveyor
collision behavior.

The orb no longer loses its five arcs on their expiry frame. Signed timer
residuals survive saves, and all five trajectories update before any of their
damage calls can consume effect RNG. Both vertical beams and the five arcs
use the fixed orb-origin cell. Rendering alone does not advance trajectories
or deal damage. The existing NMIS layout accommodates both corrections.

PAL's module requests `LiftRing1`, but its zone names contain only `ring`.
The exact lookup is preserved without inventing an asset alias. Native
addresses, the missing-name behavior and compatibility limits are documented
in [the entity audit](ENTITY-AUDIT.md#security-orb-and-exit-lift).

### LAB and BOSS1 endpoint sparks

The port looked up each moving beam endpoint to choose a spark draw area.
LAB's endpoints lie outside PrimaryColl, suppressing both bursts; BOSS1's
endpoints returned area 12 although the native callback submits area 9.
Both now use the areas recorded by their zone-entry callbacks. The original
fifteen-particle velocities, lifetimes, hit rules and dynamic lights remain.

The review also corrected LAB's signed final phase decrement and the order
of both modules' actor-use helpers. Saves retain expiry phases down to -59;
the final beam still damages, later inactive frames stay quiet, and the
normal mission call can rearm the sweep. Actor use follows both sweeps and
is not consumed by rendering alone. Native addresses and the captured failing
controls are in [the entity audit](ENTITY-AUDIT.md#lab-and-boss1-sweep-endpoint-effects).

### Native module lifetimes

The full native loader rereads COMMON and runs LevelBin export 0 even when
Restart or Resupply keeps the current map. Ordinary zone gates retain that
module and run only the indexed zone-entry callback. The port had used the
map name to decide when to reset hazard clocks, but cleared LAB/BOSS1's
one-time actor-use latches at every zone load. Both boundaries are corrected.

The regression exercises six modules, cold saves, failed loads and both real
menu requests. Older hazard fixtures now set the same carry flags as ordinary
zone gates, instead of using full reloads as a substitute. The failing control
left both LAB sweep beams visible after Restart; the corrected frame has none.
Native call paths and module writes are recorded in
[the entity audit](ENTITY-AUDIT.md#module-state-across-gates-and-full-reloads).

## Validation

- The native-lifecycle fixture passes 185 focused checks after its original
  17-failure control. All 5,139 population checks with capture/save output,
  689 save-codec checks, 334 client-save checks and 68 CTests pass. The disc
  sweep passes 215 cases (213 executed, the same two crossing skips).
  Matching LAB Restart frames differ in 32,691 pixels: the stale sweep beams
  and their light are cleared. Native disassembly, before/after PNGs and logs
  are retained in `.agents/tmp/functional-parity/native-lifecycle/`.
  The staged executable and SDL3 match Release, `--version` succeeds, and all
  21 zones across the six affected maps complete 300 headless frames from
  `.install/` without warnings or errors. `stage-verification.json` records
  the hashes and run results.

- The LAB/BOSS1 sweep fixture passes 144 focused checks and the save codec
  passes 689. All 4,652 population checks, 334 client save checks and 68 CTests
  pass. The 215-case disc sweep passes (213 executed, the same two crossing
  skips). Seven-step headless control/corrected captures differ in 859 LAB
  pixels and 10 BOSS1 pixels; the first-frame control reproduces the original
  baseline exactly. Evidence is in `.agents/tmp/functional-parity/native-sweeps/`.
  The staged executable and SDL3 match Release, `--version` succeeds, and all
  five LAB plus both BOSS1 zones complete 300 headless frames from `.install/`
  without warnings or errors. `stage-verification.json` retains the hashes
  and results.

- The SECURITY orb/lift fixture passes 206 focused checks; its failing control
  exposed the missing lift hold and the two-beam expiry frame. The save codec
  passes 687 checks, including the held flag, signed timer bounds and existing
  NMIS compatibility. All 4,530 population checks, 334 client save checks and
  68 CTests pass. The 215-case disc sweep passes (213 executed, two documented
  crossing skips). Matching headless expiry captures differ in 534 pixels:
  the corrected frame keeps all five arcs. Evidence is retained in
  `.agents/tmp/functional-parity/security-orb/`. The staged executable and SDL3
  match Release; `--version` and 300-frame runs in all three SECURITY zones
  pass from `.install/` without warnings or errors. Their results and hashes
  are recorded in `stage-verification.json`.

- The SECURITY laser-course fixture passes 650 focused checks. All 4,350
  population checks, 764 static-laser effect checks and 68 CTests pass. The
  215-case disc sweep passes (213 executed, two documented crossing skips).
  Evidence is in `.agents/tmp/functional-parity/security-lasers/`. Matching
  headless captures at level time 30 and world counter 1 show the changed
  gates and moving beams. The staged executable and SDL3 match Release;
  `--version` and 300-frame runs in all three SECURITY zones pass from
  `.install/` without warnings or errors. `stage-verification.json` retains
  their hashes and results.

- The earthquake fixture passes 184 focused disc checks; the save codec
  passes 685, including NMIS v1–6 compatibility and malformed v7 records.
  All 3,717 population checks, 334 client save checks and 68 CTests pass.
  The 215-case sweep passes (213 executed, the two documented crossing skips).
  Evidence is in `.agents/tmp/functional-parity/earthquakes/`. Matching
  fixed-step SECURITY captures change the player position from
  `(-1849,-11040,-8974)` to `(-2560,-11040,-8642)` with no input; BOSS2 also
  displaces the player. The staged executable and SDL3 match Release, and
  `--version` plus 300-frame smoke runs in all six SECURITY/BOSS2 zones pass
  from `.install/` without warnings or errors. `stage-verification.json`
  records hashes, version and results.

- The SECURITY/BOSS lift fixture passes 771 focused disc checks. The save
  codec passes 661, including v5 compatibility and malformed v6 records.
  The complete population fixture passes 3,557 checks, the client save
  fixture passes 334, and all 68 CTests pass. Baseline failures, native
  disassembly and final logs are in `.agents/tmp/functional-parity/native-lifts/`.
  Matching SECURITY captures differ in 4,908 pixels, confined to the lift at
  the centre of the shaft view. The 215-case disc sweep passes (213 executed;
  PODCITY/THEVAT crossings remain explicit skips). The staged executable and
  SDL3 match the Release build, `--version` starts, and all 13 zones across
  LAB, SECURITY, BOSS1 and BOSS2 complete 300 headless frames from `.install/`
  without warnings or errors. `native-lifts/stage-verification.json` records
  version, hashes and smoke results.

- The LAB lift fixture passes 223 focused disc checks, and the expanded save
  suite passes 609 checks including NMIS v4 compatibility and malformed v5
  records. The full population run passes 2,846 checks and the real-client
  save fixture passes 334. All 68 CTests and the 215-case headless disc sweep
  pass (213 executed, two explicit crossing skips). Matching before/after
  engine captures, disassembly and the failing
  baseline are retained in `.agents/tmp/functional-parity/lab-lifts/`.
  The staged executable and SDL3 match the Release build; `--version` starts
  successfully and all five LAB zones complete 300 headless frames from
  `.install/` without warnings or errors. `lab-lifts/stage-verification.json`
  records version, hashes and results. The matching lift images differ in
  47,251 pixels.

- The ordinary-laser fixture passes 764 checks across six maps. All 2,655
  population checks and 117 generator/evacuation checks pass after the frame
  order change, along with all 68 CTests and the 215-case headless sweep
  (213 executed; PODCITY/THEVAT crossings are explicit skips). Native
  disassembly, the failing baseline, final logs and
  matching JAIL2 before/after captures are under
  `.agents/tmp/functional-parity/static-lasers/`.
  The staged client starts and matches the Release build, including SDL3.
  JAIL2/1, POWER2/4, SECURITY/0, BOSS1/1, LAB/2 and BASE3/3 each complete 300
  fixed-step frames from `.install/` without warnings or errors. Version,
  hashes and smoke results are recorded in `static-lasers/stage-verification.json`.

- The laser-wall follow-up passes 677 focused disc checks, all 2,655 population
  checks and 1,104 effect checks. All 68 CTests and all nine BASE3 headless
  scenarios pass after the Release rebuild. Evidence, exact disassembly and
  before/after/completed engine captures are under
  `.agents/tmp/functional-parity/laserwall/`.
  The staged executable and SDL3 match the Release output; `--version` starts
  successfully. BASE3/3, MAGDEMO/3, BOSS1/0 and LAB/2 each complete 300 fixed-step
  headless frames from `.install/` without warnings/errors. Hashes and results
  are recorded in `laserwall/stage-verification.json`.

- The BOSS1 entrance follow-up passes 815 focused checks and all 2,027
  population checks. All 68 CTests, six BOSS1 headless scenarios and nine LAB
  scenarios pass after the complete Release rebuild. These cover map startup,
  event scripts, gates, crossings and save/load; they are not full playthroughs.
  Matching doorway captures differ in 2,720 pixels within (216,82)..(329,172).
  Evidence is retained under `.agents/tmp/functional-parity/boss1-entrance/`.
  The staged executable and SDL3 hashes match the Release output. BOSS1 zones
  0 and 1, LAB zone 2 and BASE1 zone 0 each complete 300 headless frames from
  `.install/` without warnings/errors; `stage-verification.json` records them.
- The weapon-offset follow-up passes 1,120 simulation checks and all nine
  focused disc-client checks. `view-baseline.log` and `view-before.log` retain
  the failing controls; `sim.log` and `view-after.log` record the corrections.
  After the complete Release rebuild, all 68 CTests and all 215 headless sweep
  cases pass again (213 executed, two explicit no-gate skips).
- The signed-damage follow-up passes 695 combat checks, including the 130 native
  vectors; its initial regression run failed 145 checks. All 45 synthetic
  creature-client checks and 10,536 disc-audio checks pass. All 68 CTests and
  the complete 215-case headless sweep pass again (213 executed, two explicit
  no-gate skips). Evidence, the native segment used to regenerate the numeric
  fixtures, disassembly and the complete script census are retained under
  `.agents/tmp/functional-parity/damage-audit/`.
- The burn-sound follow-up passes 724 simulation checks, 10,536 disc-audio
  checks, 1,240 population checks and 117 BIGGUN client checks. All 68 CTests
  and the full 215-case headless sweep pass again. The focused POWER1 fixture
  also checks mixer output, victim ownership, persistent cadence and the late
  native callback. Evidence and the actual mixed WAV are in
  `.agents/tmp/functional-parity/burn-sounds/`. The current staged executable
  and SDL3 hashes match the Release output. POWER1/1, BIGGUN/1, BOSS1/0 and
  BASE1/0 each completed 300 headless frames from `.install/` with zero
  warnings/errors; `stage-verification.json` records version and hashes.
- All 68 configured CTests pass in a complete Release run after the POWER1
  follow-up. The credits test adds 747 checks for table bounds, malformed pointers,
  blank/repeated rows, ring wrap, host-rate-independent timing, completion and
  native/adaptive draw-area clipping. The prior CRT legacy-card correction and
  full-capacity settings-container round trip also pass. The save unit suite
  now passes 592 checks, including legacy NMIS v1/v2/v3, wall/reactor/shaft clocks, malformed native
  state, legacy containers and malformed rewards.
- After the POWER1 follow-up, the full 215-case headless disc sweep passed:
  213 cases executed and two gate cases were explicitly skipped because
  PODCITY and THEVAT have no inter-zone gate. It covers loading, rendering,
  scripts, combat, saves, campaign transitions and cinematic starts.
- Both credits scenarios and the new 5,000-frame campaign completion pass.
  Standalone intro and outro playback finish without entering credits or
  awarding a tier. A separate 300-frame BASE1 smoke
  run from `.install/` has no warnings or errors. The staged executable and
  SDL3 hashes match the Release build; `--version` reports
  `v0.2.0-20-ga590ce9-dirty`.
- The added standalone-movie completion regression passes. The PAL intro shows
  all 1,280 intended pictures and exits after 1,539 fixed-step frames, before the
  5,000-frame diagnostic cap, with no errors or warnings.
- All 18 split-screen scenarios pass: thirteen arenas and full two/four-player
  Deathmatch, Team Deathmatch and Versus lifecycle checks, including READY and
  return to the multiplayer menu.
- The disc-backed controller capture fixture passes 84 checks. Its images come
  from the engine framebuffer with fixed headless stepping; they show each
  player's own style, inversion, vibration and slider values.
- The disc-backed save fixture passes 334 checks. Menu and split-client tests
  additionally cover profile isolation, actual inverted input, motor ownership,
  current/legacy settings round trips and malformed-card refusal.
- The full disc-backed population fixture passes 1240 checks without capture
  output; the focused selector run passes 259 with on-disk save round trips
  and engine captures. Both run headlessly, and save tests use project-local
  TEMP/TMP/TMPDIR. Current full-suite logs are under `power1/`; the prior
  selector pass's captures and logs remain under `selectors/`.
  The selector pass staged BASE1 zone 0, SECURITY zone 1 and BOSS2 zone 1; each
  complete 300 headless frames from `.install/` without warnings or errors.
  The reactor pass additionally checks POWER2 zones 0/3/4, LAB zone 2 and
  BASE1 zone 0 for 300 headless frames each from the refreshed `.install/`.
  The POWER1 pass checks its zones 0/1/2, POWER2 zone 0, BIGGUN zone 1 and
  BASE1 zone 0 for 300 headless frames each, again without warnings or errors.
  `--version` starts successfully; executable and SDL3 hashes match Release.
- The player-setup fixture passes 1,220 checks. It enters through PROCEED with SDL-local virtual pads,
  edits all four names and teams, changes independent controller styles, checks
  rapid Start/Back aliases, waits for every READY, starts Team Deathmatch and
  reaches its results screen. It also starts two-player Deathmatch and
  three-player Versus. Two-player setup keeps its retail stacked layout even
  when the ensuing match uses the user's vertical split preference.
- Menu checks cover held entry input, name alphabet/length, colour/team wrap,
  disconnected-player restoration, the two-team gate, readiness and mouse hit
  areas. Synthetic controller events and coordinate-level pointer tests do not
  read or control the user's input devices.
- The credits client fixture passes 274 checks against the real PAL disc,
  compares raw and relocated row decoding, checks the exact completion tick,
  rejects Confirm, and handles held/tapped controller Back. Repeated draws and
  window-aspect changes leave the timeline unchanged. Engine captures show the
  opening rows, later groups, a wide display and the returned Options page.
- The unlock client fixture passes 332 checks: actual relocated page records,
  all three awards, natural/Back completion, No, cancel/retry, overwrite
  refusal, failed I/O, fresh-client settings loads, and ordinary settings
  retaining the tier. It starts a four-player match from the restored Gold
  page and checks grants, all ammo capacities, respawn and non-refilling
  frames. BASE1 verifies that missing weapon models are not granted.

Reproduction from the repository root:

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure --parallel 4
python tests/check_headless_disc.py --disc .install/disc/game.cue --output .agents/tmp/functional-parity/disc-sweep --jobs 4
python tests/check_split_support.py --client .install/q2psx.exe --disc .install/disc/game.cue --output .agents/tmp/functional-parity/multiplayer
```

The save/capture fixtures require existing output directories:

```powershell
.\build-msvc\bin\Release\test_client_save.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/save-verified
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/controllers --profiles
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/players --players
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/credits --credits
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/unlocks --unlocks
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/reinsert --reinsert
.\build-msvc\bin\Release\test_client_population.exe --disc .install/disc/game.cue
.\build-msvc\bin\Release\test_client_population.exe --selectors .install/disc/game.cue .agents/tmp/functional-parity/selectors/after
.\build-msvc\bin\Release\test_client_population.exe --callbacks .install/disc/game.cue .agents/tmp/functional-parity/callbacks
.\build-msvc\bin\Release\test_client_population.exe --power1 .install/disc/game.cue .agents/tmp/functional-parity/power1
.\build-msvc\bin\Release\test_client_population.exe --boss1 .install/disc/game.cue .agents/tmp/functional-parity/boss1-entrance/after
.\build-msvc\bin\Release\test_client_population.exe --nativelifts .install/disc/game.cue .agents/tmp/functional-parity/native-lifts
.\build-msvc\bin\Release\test_client_population.exe --quakes .install/disc/game.cue .agents/tmp/functional-parity/earthquakes
.\build-msvc\bin\Release\test_client_population.exe --security-lasers .install/disc/game.cue .agents/tmp/functional-parity/security-lasers
.\build-msvc\bin\Release\test_client_population.exe --security-orb .install/disc/game.cue .agents/tmp/functional-parity/security-orb
.\build-msvc\bin\Release\test_client_population.exe --native-sweeps .install/disc/game.cue .agents/tmp/functional-parity/native-sweeps
.\build-msvc\bin\Release\test_client_effects.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/damage-audit/view-after --hurt
```

Evidence is retained under `.agents/tmp/functional-parity/`. No disc data or
external implementation code is added to the repository.

## Actor ambient lighting

Native allocation at PAL `0x8006C098` seeds current ambient (`+0x2AC`) and
target ambient (`+0x2B0`) to RGB 64. Gameplay player spawn at `0x8003B250`
explicitly replaces both with RGB 32 at `0x8003B444..0x8003B470`. The port had
retained a guessed 48 for both actor types; its initially zeroed player record
could also start with zero ambient until the presentation pass raised it.

The independent fixture executes both verified retail executables: 50 player
spawn prefixes, 1,024 complete fade calls, and export-2 initialization followed
by shared spawning/export 0 for all 49 distinct creature module images (24 PAL,
25 US, representing 166 module occurrences per disc). Those creature spawns
retain RGB 64 without writing either colour. Geometry, model lookup and sound
services use explicit recorded hooks and neutral records; the native link and
monster-start functions execute. These are bounded initialization/fade tests,
not full-machine or paired retail-frame evidence.

The actor refresh now distinguishes first player conversion from a later
refresh. Four-player tests retain hit colour through refresh, compare 32
recovery ticks against native results, and reset each respawn to RGB 32.
Items, viewmodels and front-end showcase entities keep their own spawn paths.
No saved-game format changed.

The control failed 346 simulation checks and both disc-client ambient checks.
After correction all 73 CTests pass; the simulation passes 1,977 checks, the
full PAL creature client passes 560, and the focused US client passes 59.
Matched engine captures change 7,536 creature pixels and 4,234 player-body
pixels, confined to the respective bodies. These are before/after port
captures, not retail comparisons. Native traces, controls, logs and images
are in `.agents/tmp/functional-parity/actor-ambient/`.

The staged Release executable and SDL3 match the current build, and PAL/US
BASE1 each pass a 90-frame headless smoke from `.install` with no warnings or
errors. No new unrelated runtime issue was found. Automatic approval review
rejected cleanup with "blocked by policy": 58 converted raw PPMs, seven test
saves and one superseded partial disassembly remain (22,202,850 bytes).
`cleanup.json` records the checked paths; each PPM has a verified lossless PNG.

## Player-body animation progress, 28 October

Body pose sampling no longer chooses moves, advances time, consumes RNG or
completes deaths. The client advances each owner's body on a positive game
tick, including bodies that no viewport draws. RUN uses horizontal wish input;
JUMP uses the successful jump impulse. ATTACK cannot override either, and a
new nonzero damage byte requests PAIN. Death and respawn update the same state.

`q2_player_anim_step` implements the complete native `0x8003CE14`, including
advance-before-install, strict end boundaries, jump holds and contact release,
terminal/loop rest positions, pain/death locks and repeated attack restarts.
Pain consumes the shared RNG before the installation lock; death draws only
on the first request. Native corpse think tests the previous animation flags
before advancing, so a newly completed death settles on the following tick.

Independent fixtures execute 4,080 complete animation calls on each PAL/US
executable, covering boundaries and sequential histories. They also execute
48 bounded request-selection cases per revision. The latter supply jump and
weapon returns explicitly; they do not execute those two producer functions
or a whole player think. Loader and player-binding execution establishes that
block D's three positions are multiplied by five, then Stand/Run's rest is
rewritten to start. Earlier claims in `model.h` and reconstruction note 51f
denying that loader conversion were incorrect and are now corrected.

Regressions cover all four owners, successful jump versus airborne movement,
real railgun damage and pain consumption, offscreen death/respawn, non-ticking
frames, and repeated pose sampling at 30/60/90/120 Hz display deltas. The real
Male2 model fixture checks all ten clips at start/middle/end. Its moving-and-
firing scene retains RUN where the old renderer selected ATTACK. The controlled
before/after framebuffers differ in 5,694 pixels, confined to the body. The
restored old chooser/pose fails the priority and read-only sampling checks.
Evidence is under `.agents/tmp/functional-parity/player-animation/`.

Validation: all 73 configured CTests pass, including 618 split-client and
1,979 simulation checks. The actual-model fixture passes 326 checks on each
disc; all 18 PAL split-support scenarios pass, including match completion,
Versus rounds and return to the multiplayer menu. The staged Release client
and SDL3 match the build hashes, `--version` starts successfully, and PAL/US
each complete 90 BASE1 frames plus 600 four-player MATRIX5 combat frames with
zero errors or warnings. `stage.json` records commands, hashes and reports.
Twenty-one redundant raw PPMs (7,987,515 bytes) were removed only after lossless
PNG pixel verification and path/link checks; `cleanup.json` records them.

The old pain-animation regression was also found to be absent from its test
program's main function. Its replacement and the new animation regressions
are now explicitly invoked. This coverage defect and the stale format claims
were separate findings from the rendering fix.

**Remaining animation/weapon boundary:** the client still uses shot-serial
changes for its ATTACK request. Native `0x8004EE0C` returns the last substep's
fire result at `0x800B2B7E`: the state machine writes zero or the idle-fire
result (`0x8004FC40`), and continuous-fire arms can overwrite it. Dry result 2
also requests ATTACK; a later zero substep can erase an earlier shot. The
view-weapon driver still runs on display updates. These producer/tick ordering
differences require native driver sequences and client regressions before
the overall player-animation item can close. The current fixtures do not
claim to validate that boundary or whole-game RNG ordering.

Two additional implementation gaps were found while reconciling source:

- `client_advance_view_weapon` supplied zero to aim-assist easing. The native
  target search and its firing direction are restored by the auto-aim follow-up.
- The projectile world-hit path passes a NULL surface normal to grenade
  impact. Its bounce arm requires a normal, so geometry contact detonates
  instead of reflecting the grenade. This geometry defect is corrected by
  the grenade follow-up below; additional projectile details remain open.

Other source comments identify candidates whose shipped-game effect still
needs qualification: secret deduplication and the secret chime's source
position; reconstructed creature-hit knockback points and vertical impulse;
the shared AI noise stand-in and path-corner scan order; BFG beam
area retention and actor effect-area refresh; first-matching-unit briefing
selection; and the fifth mission camera's out-of-table fallback. A comment
labelled "DEVIATION" is not sufficient evidence of a reachable retail defect.
These candidates remain explicit below until their callers and state histories
are compared. Legacy host saves that predate secret-history storage cannot
recover that history; this is a host-format compatibility limit.

## Grenade geometry follow-up, 28 October

Launcher and released hand grenades now run the shared native toss behavior.
Runtime entity boxes precede PrimaryColl, supply their contact normals and
withdraw the whole displacement before the hull query. Geometry reflects the
remaining displacement, applies the SDK's quantized square root and vector
normalization with restitution 2304/4096, and rests below the native speed
threshold. Moving tosses use three shared RNG draws for tumble; resting ones
skip movement and tumble. Blur vertices use the pre-tumble attitude.

A rebound no longer applies blast damage or spawns a fireball. It queues
`wep_grenlb1b` at the resulting position. Shootable geometry receives the
native three-point contact damage. The fuse is checked after movement and
contact, including a rebound on the expiry tick. Damageable actor contact
still detonates; geometry contact suppresses that tick's actor sweep. The
creature-projectile/player-hit branch also now creates the explosion model
and sound that were missing there.

The independent fixture executes 2,592 complete toss calls on each PAL/US
executable, with explicit collision-service outputs. Native SDK instructions
and tables execute; the fixture models their GTE SQR/GPF operations. This is
CPU arithmetic and control-flow evidence, not a hardware GTE capture or a
complete native collision-world comparison. All 45,302 weapon checks pass.
The simulation suite passes 2,027 checks, including both grenade types,
entity contacts, rest, actor priority, glass damage and expiry ordering.

The real-disc fixture passes 1,483 checks on each supported disc. MATRIX5
captures show the old first-contact explosion versus the surviving rebound;
BASE1 cold saves preserve a moving rebound and resting grenade, including
their flags, velocities, angles, fuse and subsequent shared-RNG history.
Evidence is under `.agents/tmp/functional-parity/grenade-bounce/`.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the build hashes, `--version`
starts successfully, and PAL/US each complete 90 BASE1 frames plus 600
four-player MATRIX5 combat frames with zero errors or warnings. Commands,
reports and hashes are recorded in `grenade-bounce/stage.json`.

The owner-exclusion and actor-contact gaps found after the bounce fix are
corrected in the follow-up below. Simultaneous sound ordering still needs
native sequence comparisons. Gib toss uses inferred
Euclidean root and normalization instead of the SDK arithmetic measured here.
Extreme SDK inputs outside the signed-positive table domain retain a defined
host fallback and are not qualified by these cases.

## Projectile actor contacts and save continuation, 28 October

The independent fixture executes 768 bounded grenade owner-gate cases, 3,072
complete actor queries and five named integration probes on each verified
PAL/US executable. Both revisions agree. Grenade2 and released Grenade3 now
permanently clear their owner-ignore mask after the pre-move position leaves
the owner's world bounds expanded by 40 in X/Z; the native containment check
ignores Y. The query excludes an ignored owner before scanning, so it no longer
hides another valid target by selecting the owner and discarding the result.

The complete native query does not filter damage immunity. Its integer root,
strict endpoint gate and successive clipping differ from the older query;
the latter disagrees with 860 of the 3,072 fixtures when comparing selected
actors and clipped points. All five live projectile kinds now use the measured
query and retain its clipped point for damage and effects. Damage rejection is not equivalent to
removing a collision body: a body can stop a projectile without taking damage.

`q2_combat_trace_actors` now matches all 3,072 native selected actors and clipped
points, including the native fractional accumulator's overflow. The combat
suite passes 3,767 checks. The owner helper matches all 768 cases and the weapon
suite passes 47,612 checks. A missing `s3` initialization in the launcher fixture
was found when those cases were first checked against the implementation. It
was corrected from the thinker's prologue, then both original executables were
rerun. These are CPU execution comparisons with modeled GTE
operations, not hardware captures. Evidence is retained under
`.agents/tmp/functional-parity/projectile-contact/`.

The simulation now includes the single player in projectile contact/fuse
candidates, including a creature grenade's timed explosion. A grenade touching
its owner applies only half self-splash, without a direct hit. The host capture
god mode still refuses damage while retaining the collision body. Player damage
uses the entity origin and the measured contact without the former extra
feet-origin conversion; splash visibility likewise resolves the canonical
player origin. The simulation suite passes 2,127 checks, covering all five
projectile kinds, immune blockers, all four player owners and a creature owner.

The real-disc return/save fixture passes 1,531 checks per revision. Before the
fix, the grenade passed through its owner with health 100 and no explosion.
The fixed run contacts at a relative Z of 287 and leaves health 46. Engine
before/after frames are retained. Cold saves preserve both armed and cleared
owner masks, including a cleared grenade that re-enters the expanded box, and
match eight subsequent ticks of positions, velocities, attitudes, cells, fuses,
health and RNG. The existing flags payload carries the state without a format
change; older host saves cannot recover a cleared mask they never recorded.

That continuation also exposed a separate restore defect: ZPOP rebuilt a
projectile's cell using SecondaryColl, then moved it through PrimaryColl.
In BASE1 it restored cell 235 instead of 161 and missed a later wall contact.
Restore now uses the projectile hull; the complete continuation matches.
The previous rebound/rest/cold-save fixture still passes 1,483 checks per disc.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the current build; `--version` starts
successfully. PAL/US each complete 90 BASE1 frames and 600 four-player MATRIX5
combat frames with zero errors or warnings. `projectile-contact/stage.json`
records commands, hashes and reports. The matched before/after engine frames
change 110,689 pixels around the returning grenade's explosion; they are port
captures, not matched retail framebuffers.
Twenty-three redundant raw PPMs (8,737,113 bytes) were removed after lossless
PNG pixel verification and path/link checks; `cleanup.json` records the files.

**Still open:** hitscan and AI consumers of the older cylinder helpers need
their own migration and integrated qualification. The bolt trail and impact
sequence identified here is corrected by the follow-up below. Other projectile world/contact ordering,
simultaneous sounds, gib SDK arithmetic and extreme math domains remain in
the inventory. These contact and save fixes do not establish whole-frame parity.

## Bolt flight and impact sequencing, 28 October

The old comment claimed the trail used the committed endpoint, but the call
still passed the old position. The light also ran before movement, and expired
or actor-contact bolts emitted a normal flight trail. Actor hits additionally
emitted the world spark burst before blood. These extra draws changed later
particles and weapon randomness.

`generate_bolt_reference.py` now executes the complete PAL/US bolt sweep at
`0x80047C6C` and its blood spawner. Both revisions agree on 192 cases: lifetime
boundaries, clear flight, actor contacts, type-2 entity contacts shielding an
actor, hull impacts, flag combinations, negative/odd displacements and signed
halfword overflow. The native actor query runs unchanged. Entity contacts,
cached hull results, output allocation, damage and sound are explicit hooks;
zero views skip body packet submission. These are control-flow and effect/RNG
fixtures, not hardware captures or cached-hull equivalence tests.

The bolt path now follows those decisions. Expiry emits nothing. Type-2 solid
contacts precede actor damage, so a door shields the actor behind it. Actor
contacts emit the native blood pair, including immune collision bodies, without
an extra spark pair. The native type-2 impact branch retains the previous bolt
position for its sparks. Clear flight commits before lights and trail; hull
impacts retain that frame's flight effects before their spark pair. Impact-bit
gating and the otherwise-unused larger-light bit are covered independently.
The movement delta now uses the native signed-halfword truncation, and the
trail avoids signed-left-shift overflow while preserving native rounding.

Trail and impact draws now share the bolt's saved combat RNG stream. The first
test draft compared the separate global stream and did not seed the combat
stream; that fixture setup was corrected before accepting results. The wider
port still has separate combat, effect and global gameplay streams. Unifying
their native call order and save continuation is a remaining parity gap, not
something these isolated sweep comparisons establish.

`test_sim` passes 5,903 checks, including every fixture's position, liveness,
actor damage, lights, particle counts, anchors, ramps, velocity/offset hashes
and RNG result. The disc-backed BASE1 test passes 1,587 checks on both PAL and
US, captures the real trail, and compares 32 uninterrupted/cold-loaded steps
through impact and retirement, including cell and both saved RNG states.
The old BASE1 build fails the trail-anchor and light-position checks by 480
world units. Evidence is under `.agents/tmp/functional-parity/bolt-sequence/`.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the build, and `--version` starts.
PAL/US each complete 90 BASE1 frames and 600 four-player MATRIX5 combat frames
with no warnings or errors. Commands, hashes and reports are in `stage.json`.
The matched BASE1 port captures change 384 pixels along the six-particle
trail; they are not paired retail framebuffers.

Twenty-one raw captures were converted to pixel-identical PNGs. The cleanup
manifest also identifies an obsolete root-level `disasm` file whose SHA-256
matches the PAL executable segment, a disposable smoke helper and two
superseded status-build logs. Automatic approval review rejected the removal
command as "blocked by policy"; all 25 files remain, with `removed: false`
recorded in `cleanup.json`. No alternate deletion was attempted.

The cached native hull marcher, posed-model contact refinement, body packet
submission on the final hull-impact frame, non-type-2 entity contacts, noise
and simultaneous sound ordering still need integrated qualification. The
numeric fixture intentionally exposes its hooks instead of claiming those
paths are covered.

## Weapon driver, hitscan and deviation review, 29 October

**View-weapon driver.** 0x8004EE0C's only caller is 0x8003AD98 in the live
player think, which reads the frame's dt global 0x800B2DB4. Each substep calls
the state machine 0x8004F87C, and every one of its exits stores the result
halfword at 0x800B2B7E. 0x8004FA58 and 0x8004FAB0 store zero; 0x8004FC40
stores `s4`, which is zero unless the idle arm called the fire function at
0x8004FB30. The frame driver 0x8004FEE8 receives that address and the
machinegun, chaingun and hyperblaster arms overwrite it with their own fire
result (0x8004FFE8, 0x80050214, 0x800502E0). 0x8004F6D4 returns the last
substep's value, and 0x8003ADA0..0x8003ADB4 requests ATTACK for any nonzero
low halfword. Native fire functions return 1 for a shot (the blaster's
0x8004C1A4), 0 when the spawn failed and 2 when dry. The port's own enum numbers
a shot 0, so `q2_viewweapon.result` keeps the console's encoding.

The client now runs the driver once per logic tick with that tick's dt for
every player. The body chooser reads `q2_vw_result` instead of a shot counter.
The selection fixture was extended to fire result 2: native requests ATTACK
for it in all 48 relevant rows, matching both PAL and US. `test_viewweapon` pins
the result's life: idle shot, the next call's clear, the stale value on a zero
step, dry 2, an arm's round, and an arm's round erased by a later quiet
substep. The old counter also advanced on attempts the refire clock refused.
Holding a fed railgun for 300 headless PAL frames, the old build posed ATTACK
on 168 frames with 7 shots and the new one on 156 with 6. The muzzle test now
requires the shot on the frame that takes a tick. A frame between ticks fires
nothing, where the old build fired and lit the muzzle on it. A new MATRIX5
regression drives a dry click through the real client loop. It passes on the old
build as well, because that build's counter also counted dry attempts, so it is
a regression check rather than a control.

**Driver write order, 5 November.** `generate_viewweapon_driver_reference.py`
executes the whole PAL and US driver: the substep loop, the state machine, the
model-move driver 0x80050454, the frame driver, the key reload, the barrel ramp
0x8004FC78 and the dry pass's 0x800506C4/0x80050758. After each call with a
held hand grenade it also executes that grenade's think 0x8004A368 in its held
state. It uses a synthetic bank with the retail key counts and events,
synthetic move records, and scripted fire answers 1, 0 and 2.

The 133 sequences cover all eleven weapons, dt 1, 12, 30 and a mixed 1..30
pattern, holds and releases, mid-burst and double-tap selections, and quad.
They also cover moves frozen outside FIRE, swaps with a move on the record, and
grenade cooks, re-presses and failed spawns. That is 7,838 calls and 594 fire
calls, identical between revisions; 1,357 calls have a move on the record. The
HEAD driver diverged from the first 78 sequences in 71 (2,419 calls).

The native order per substep is: `min(left, dt)` comes off the clock, then the
state-machine pass runs. The pass, not the loop, advances the frame when the
key is spent. The idle arm can fire on any substep, not only at a boundary.
Then come the move driver, the frame driver, the key reload when frame or state
moved, and the ramp. The display is computed once after the loop.

The move driver returns before it reads the record unless the weapon in hand is
3, 6, 7, 8 or 11 (0x800504D4) and the state is FIRE (0x80050508). So a move
advances and starts only inside a fire clip. One still playing when its clip
ends stands at its position through IDLE, LOWER and RAISE, and resumes on the
next fire clip. The event that starts a move is read through the clip the
substep began with, so a fire clip's first-key event starts one substep late.
A swap writes +256 and nothing else (0x8004FA3C/0x8004FA44): the record +736
and flags +258 carry over to the new weapon.

The held grenade's think charges +76 by `dt*6` at position 380. It primes on
`pos >= 261` against its mark and throws on `pos >= 411`, then writes the mark
+740 = pos - 1 (0x8004A42C-0x8004A544). The cook arm pins only while that mark
is below 380 (0x80050404), so a re-press past 380 does not drag the arm back.

The port now runs that loop. The following port behaviours were wrong and are
fixed:

- the machinegun's held wrap loaded key 2's clock;
- the LOWER hold replayed its last key instead of halting (0x8004FA50);
- an idle dry answer set the latch instead of asking again;
- a spawn that failed (0) did not play the fire clip;
- the chaingun's released rate was written only at frame 9 (0x800500A0);
- the hyperblaster's accumulator was cleared on frame 6;
- a new shot reset the +218 cache, +44 and +52;
- the constructor's +44/+218 of 1 were missing;
- the countdown was decremented on the frame it was armed (0x8004ECF0 against
  0x8004ED54);
- a playing move advanced in any state;
- a swap cleared the move record;
- the cook ignored +740;
- the grenade's crossings and charge were taken per substep from the arm
  instead of once per frame from the held grenade's mark, and ran with no
  grenade after a failed spawn.

Base and display arithmetic now matches the native `mult`/`div`/halfword form
without clamps. `test_viewweapon` drives every call twice. It matches the fire
calls (substep, arm, state, frame, clock, rounds) and the end state. It checks
all six bases, the display, the barrel, the rand state, sound, quad and swap.
It also checks the move position, flags and record, the +740 mark, and the
grenade's charge, release and liveness. The first pass asks inside the
substep. The second reports before the call the way
`client_advance_view_weapon` does. Mutants that advance a move outside FIRE,
clear the record on a swap or ignore the mark each fail hundreds of checks.

Limit: the dry pass's auto-select is in-driver only through
`q2_viewweapon.auto_select`, which the client does not set. The client still
reselects from `refire` a tick late, and its `q2_vw_select` arms the 70-tick
countdown where 0x800506C4 does not. The port therefore stops asking after a
dry pass, as the console's next pass lowers instead.

The grenade's think is placed after the driver in the same frame, the client's
order; the console's entity order was not traced. The thrown grenade's flight,
its fuse expiry (0x8004AA1C, mirrored by `q2_vw_hand_grenade_expired`) and the
throw sample are outside the fixture. `hand_live` is not in the save chunk, so
a held grenade restored from a save stops thinking. PrimaryBFG 0x8004B2B4
writes the gun's move fields from the BFG projectile's think; the port has no
counterpart. The shotgun's +695 cock path in 0x80050454 is left to the client's
`cock_pending`. The fixture has no hardware timing and no real fire functions.

**Hitscan.** Every native instant weapon calls LineIntersectCreCylinder
0x800544EC, the routine `q2_combat_trace_actors` reproduces, from the world
trace's stop to the shooter's ignore argument. The bullet does this at
0x8004891C, the rail at 0x80049318, the mission laser at 0x80048F8C and the
creature laser at 0x80049AA8. The rail makes one query and one DoDamage
(`a3 = 3`, 0x80049334), so it does not penetrate. The query does not filter
`takedamage`, so an immune body stops a round and shields what is behind it.
`q2_combat_hitscan` implements that shape; the player's bullets and rail, the
creatures' rounds and rail, and both lasers use it. The player's bullet blood
had been placed at the world fraction, on the wall behind the creature, and is
now placed at the entry. `test_combat` and `test_sim` now pin a single rail
victim, immune shielding and entry-ordered selection, replacing assertions of
the old behaviour.

**Knockback.** DoDamage forms `target+0x54 - point` (0x80057EF0..0x80057F28),
and +0x54 is the mid-body origin. `q2_sim_hurt_player` built the player's
actor with the feet there. A reconstructed "blind" point 286 units back toward
the shooter then cancelled the error, so every creature round pushed
horizontally. The actor now carries the origin, the reconstruction is gone,
and rounds pass their real entry point. A level round pushes 533 units back
and none vertically; a round from 1,200 units above pushes 275 back and 456
down. Only mods 3, 13, 15 and 18 read the point as a direction. Still open:
for a client target the bullet arm adjusts the entry with GenMissileHitPlane
(0x80048930..0x80048954) against the previous frame's view plane, a shift of
at most a few dozen units per axis. That adjustment is not reproduced.

**Drowning.** 0x8003D358 `slti v0, a2, 3601` / `bne v0, zero, 0x8003D3E0`
takes the 750-tick ambience arm while air is below 3601. It drowns at
`air >> 10`, capped at 7, only once air reaches 3601. The port had the arms
reversed. A submerged player took 0..3 points per pass for twelve seconds
and then nothing indefinitely. `test_sim` now checks no damage below 3601,
three to seven per pass after it, and continued damage.

**Smaller corrections.** Gib rebounds use SquareRoot0 0x8008A7E8 and
VectorNormal 0x8008A588, as the shared toss mover does (0x800466F4,
0x80046A14). A 3200 rebound becomes 1798, not 1800. The BFG beam's far end is
the target origin plus `(572 - height) / 2`, truncated toward zero
(0x80048D4C..0x80048D74). The secret chime plays from the entity origin
(`s2 + 84`, 0x80028FA0), not the feet.

**Qualified as equivalent or unreachable.** These verdicts come from native
reading and shipped-data censuses; no fixture executes them.

- Secret deduplication: 33 of the 34 INSECRET items are fired by volumes in
  one-shot records (flags 0x48 or 0x50) that nothing re-enables. BASE0's crate
  record fires once.
- Briefing unit: every campaign map's strings carry exactly one unit, which
  equals its level-table id.
- Path corners: native uses linear scans, not an area tree. Every map has at
  most one PathCorner group with at most 32 corners, and no duplicate
  positive targetname.
- Actor effect area, and the BFG beam's area while the beam is refreshed.
- Muzzle roll: RotMatrix is Ry·Rx·Rz with roll innermost, so the native fire
  matrix and the port's camera transpose compose identically. A one-unit
  rounding difference remains: RotateS floors in the Y-negated frame.

The unit-five board camera is reachable through BOSS2 record 0xC0. It reads
the level table's static first entry, and the port keeps the player camera as
host policy.

**Identified, not yet corrected:**

- Player noise; corrected in the next section.
- The BFG beam freezes its endpoints for up to about four ticks when a
  refresh is skipped; native recomputes them every frame (ProcessSplurges).
  This is visual only. Corrected 31 October; see the section below.
- The AI body clip (aiworld.c) still uses the older cylinder arithmetic. Its
  list order against native list membership is unqualified.

  **This first clause was already stale when it was written.** The clip's
  narrow phase has called combat.c's shared transcription of 0x800544EC since
  `ff7795c` (`aiworld.c:103`, `q2_combat_cylinder_interval`); what that commit
  kept separate, deliberately, is the FILTER, because the shot path rejects on
  `takedamage` and 0x800545E8 skips only the ignore entity. The second clause
  stands: list ORDER against native list membership is still unqualified.

All 73 CTests pass. The disc-backed muzzle, weapon (grenade, projectile
contact, bolt, auto-aim), save, player-animation and split-screen suites pass
on both PAL and US.

## Player noise, 29 October

PlayerNoise (0x80062B80) returns in multiplayer (0x800AEBCC) and for a
NOTARGET player (0x80062C14). On its first call it spawns two entities
(0x80062C30, 0x80062C4C): class 201, owner the player (+0x48), svflags 1, and
no client and no object, so no health. It keeps them at who+0xC8 and +0xCC.
A type below 2 goes to `sound_entity`, type 2 to `sound2_entity`
(0x80062C68). The noise is moved to `where` and stamped with
`teleport_time = level.time` (0x80062CA0..0x80062CD4). The port pointed both
slots at the live player stand-in, and three behaviours followed from that:

- FoundTarget publishes a sighting only for a client (0x8005D11C), so a
  hearing creature woke others with a sighting it never made.
- FindTarget's "already got one" (0x8005D4A4) returned before any sight
  check, so a creature hunting a "noise" never re-saw the player.
- While the player kept firing, each shot refreshed the 50-tick window, so
  hearing creatures held fire indefinitely.

`q2_creature_world` now owns the two noise entities, `noise[0]` and
`noise[1]`. FindTarget refuses a heard noise whose owner is NOTARGET
(0x8005D4FC).

The sim raises the requests where the console calls PlayerNoise:

- Type 0 at the entity origin for the jump (0x8003E208), the dive
  (0x8003D2B8) and surfacing (0x8003D460). The dive's noise comes before the
  splash cooldown that can skip its sound. Wading in makes none.
- Type 1 at the shot's start from nine fire functions. The hand grenade's
  and the BFG's make none.
- Type 2 at the end of a player's bullet after its blood or puff
  (0x80048998..0x800489AC).
- Type 2 at a player's bolt hitting the world. This comes from both arms of
  ProcessMissiles_Many's hull impact (0x8004861C, 0x80048650); a bolt
  striking a body makes none.

The client hands these requests to the creature world once per frame.

Cold saves write CAIS version 6, which adds both noise entities' position
and stamp, and references to them from the alert slots and creature targets.
Versions 1..5 still read. Zone history captures a noise reference as none.
Each zone keeps its own AI clock, so a carried stamp would be meaningless,
and a returning creature then has no enemy, which is what a stale noise
leaves it.

The 50-tick expiry reads the noise's health through its null object pointer
(0x8005DE48). That is undefined native state; the port's zero health makes
the creature give the noise up. This matches the expected kernel-RAM read,
but no hardware trace has confirmed it.

Two new `test_creature` regressions cover the entities, the channel split,
the frozen position, the NOTARGET early-out, no sighting propagation,
re-sighting the player, and expiry. A control build pointing the slots back
at the player fails 11 of their checks. `test_save` round-trips a noise
entity through disk. All 73 CTests pass. The disc-backed suites pass on PAL
and US, including the full 851-check creature client.

The table below compares 1,500-frame headless `--demo` PAL runs of the
previous commit (`0b5d7dc`) with this change:

| Map | Creature rounds before -> after | Outcome |
| --- | --- | --- |
| WASTE1 | 0 -> 93 | Before, three creatures hunted the player but never fired; after, the player is killed by a rail |
| JAIL2 | 82 -> 109 | Hits on the player rise from 4 to 12 |
| BASE2 | 29 -> 42 | The player dies in both runs |

These are port demo runs, not matched retail traces.

## Three named gaps, 31 October

This round took the three items the table above named as closable and closed
them, and found that one of its own claims had gone stale.

**The BFG beam's ends are live.** `ProcessSplurges` (0x80048CA8, QUAKEMIS.C) is
one function and it does four things per record per pass: subtract the frame
delta (0x80048CE8), pose the near end off the owner through
`GetAbsoluteModelVert` (0x80048D0C — a negative clip takes the owner's own drawn
position at +0xA4, 0x8006CD98), resolve the area from that owner through
`GetFireLocSpace` (0x80048D24), and read the far end off the target entity's
+0x54..+0x5C raised by `(572 - target+0x96) / 2` (0x80048D4C). The port had the
timer and the submit but kept the two ends the refresh was handed, so a beam
froze the moment `BFGSpecial` stopped re-arming it — when the ball lost sight of
a body, and for the whole tail of each 45-tick timer after the ball had moved
on. `q2_fx_timed_bind` is those two pointers as a hook; the sim answers it from
the projectile slot and the live target list. A 600-frame headless `--demo
--watch --shoot --weapon 11` run of WASTE4 zone 1 queues 154 beams before and
after, and re-derives 62 of them where it re-derived none; 8 of 60 captured
frames differ, the largest by 2,800 pixels.

DEVIATION, stated: native holds raw entity pointers and reads them whatever has
become of the entities, so a beam outliving its ball reads a recycled record.
The hook can say "gone" instead, and the last resolved ends then stand. An
attached record — the Parasite's style-4 beam — is still refreshed by
`client_cre_beams_update` once a display frame, because only the client can pose
its owner's vertex; the resolver declines those rather than resolving them twice.

**A model entity carries a geometric scale.** `QuakeGenExplosion`'s third
argument reaches `MyScaleMatrix` (0x80055F3C, MYMATRIX.C) at 0x8005A96C with
entity+0x2C0 as both source and destination, multiplying all nine elements by it
in 1.0.12. Every caller passes 4096 but the gib dispatcher's two `Explosion`
arms (0x8007CF18, 0x8007D010), which pass 2048 — so a Boss2's, Flyer's, Hover's
or Strider's fireball is half the size of a rocket's. The header recorded this as
NOT REPRODUCED because `q2_entity` had no draw scale (`scale` is the +0xFC light
intensity). It has one now, spent as the model instance's uniform scale, and
`instance_scale` was given MyScaleMatrix's own round-toward-zero
(`bgez` / `addiu 4095` before the `sra 12`) in place of a floor — a unit per
negative matrix element, which at half scale is a 4096th of a turn of tilt.
ZPOP gains version 6 for the field; versions 1..5 read as 4096, which is all
they could ever have held.

**GenMissileHitPlane, and the overflow that is its behaviour.** COLL.C's
0x80054BD4 moves a hit point onto the victim's own view plane before DoDamage
sees it, and only a client has one: both callers load `entity+0x0C`, dereference
it and pass that record and `record + 52`. `UpdateWindowStuff` (CAMERA.C
0x80037F10) fills it at the end of each camera pass — `MYRotMatrixYXZ2`,
`ReverseMatrix`, then `RotateS(transpose, {0,0,4096}, view+58)`, so +58..+62 is
the camera's forward and +52..+56 is that over eight. The plane therefore sits
512 units in front of the eye, facing it, and what a shot reads during the
game's frame is the plane the previous frame left.

It reads as a ray/plane intersection and it is not one. 0x80054CB0 is `mult v0,
v1` then `mflo t1`, so the numerator handed to `div` is the LOW 32 BITS of
`dir[k] * num`. `dir` is the shot's full-length direction — the bullet arm's aim
shifted left two, 16384 units — so the product clears 2^31 once `num` passes
131072, which is 32 units of travel, and the body query puts the entry point on
the victim's 286-unit cylinder. In play the product always wraps: head-on from
the skin the point moves 30 units, not the 226 that would put it on the plane.
That bound is exactly the "few dozen units per axis" this audit had estimated,
and it is why the arm is transcribed instruction for instruction instead of
written as the intersection it resembles. `q2_combat_missile_hit_plane` keeps
the `addu` wraps and the toward-zero divides; `test_combat` pins both the
non-overflowing case and the wrapped one, the `n . dir >= 0` gate that leaves a
shot from behind alone, and the un-ticked view that projects nothing.

The port applies it on the creature-bullet arm (0x80048930..0x80048954), which
is the one the audit named. `--report` now counts `creatures.player_hits` and
`creatures.player_hits_projected`: a 900-frame JAIL2 zone 0 `--demo` run
projects 3 of 9, and BASE2 and WASTE4 project none of theirs because every round
reached the player from behind their own view plane. The projectile arm's call
(0x80047E94..0x80047EB8) and its `LineIntersectCrePlane` fallback are read and
NOT applied: the projected point feeds `ProcessMissiles_Many`'s later reads at
0x80047EDC, so applying it wants that function's ordering established first.

**And one of this file's own claims was stale.** "The AI body clip (aiworld.c)
still uses the older cylinder arithmetic" was true when it was first written and
not when it was last repeated: `ff7795c` moved that narrow phase onto combat.c's
transcription of 0x800544EC and said so in its message. The list-order half of
the same bullet still stands. Corrected in place above rather than deleted,
because a stale claim that was carried forward twice is worth leaving visible.

Validation: all 73 CTests pass, including the three new regressions. The
disc-backed creature, weapon, muzzle, save and client-effect suites pass on both
PAL and US. `.install/q2psx.exe` is staged from this tree and completes a
120-frame WASTE4 capture with zero errors and zero warnings.

## The screen for a controller that went away, 31 October

Found by the symbol-table census rather than by reading code: of the 1,037
functions `/MAIN.SYM` names, only eleven are cited nowhere in this tree, and
four of them belong to one flow — `InGame_GOTO_REINSERT_CONTROLLER`
(0x8001D200), `FrontEndCheckForReinsert` (0x800204F4),
`InGame_PrintMemCardMessage` and `ControlGetPortName` (0x8001FB3C), all
QFR_SING.C. The first two are the console's lost-controller screen, and the
port had the page's data without ever entering it.

**Where it comes from.** `GameCheckPaused` (QUAKE.C 0x8003E538) walks every
player each frame with `SnapShotPadViolation` (INPUT.C 0x80032DCC) at
0x8003E6C4..0x8003E6F8. The first non-zero answer breaks the loop at
0x8003E850: the pause word is set, the player index is stored, `killallsounds`
runs (0x8003E860) and 0x8003E870 enters the page with three arguments — the
violation kind, the port byte the query wrote, and the player.

**What the page is.** 0x8001D238 enters page 38 OUTRIGHT, so no pause page is
built underneath it; 0x8001D41C appends the shared empty terminator table that
every text-only page uses. Two of the three rows are then rewritten. The middle
one is the literal "MULTI TAP INTO" for kind 3 (0x8001D278) and otherwise
"%s INTO" with "CONTROLLER" for kind 1 or "MOUSE" for anything else
(0x8001D284..0x8001D2AC) — note the else: an unnamed kind still says MOUSE. The
bottom one is "CONTROLLER PORT %s" with `ControlGetPortName`'s answer
(0x8001D378), which is "1" or "2" with no multi tap and one of "1-A".."2-D"
with one (0x8009B48C against 0x8009B4AC / 0x8009B4B0). Finally 0x8001D42C
installs `FrontEndCheckForReinsert` as the page's per-frame hook, and that is
the ONLY way off it: no row carries an action and the page has no back action,
so neither X nor triangle does anything. The hook asks the query again and, on
a zero, calls `InGame_GOTO_PAUSED` (0x8001FD88).

**What the port did instead.** `SDL_EVENT_GAMEPAD_REMOVED` opened the PAUSE
menu. That is the right screen at the wrong time — it is what the console shows
when the device comes BACK — and it offered RESTART LEVEL and QUIT GAME to a
player with nothing to drive them with, while saying nothing about what had
happened or which port to look at.

**What it does now.** The removal raises page 38 with the device and the port
named, kills voices first as 0x8003E860 does, and holds: START and Escape are
both refused over it, so a second player or the keyboard cannot dismiss a
screen that is waiting on somebody else's pad. When the pad is back the
per-frame check hands over to the pause page, which is page 43 in a match and
26 otherwise.

DEVIATION, stated: the port learns the violation from SDL rather than by
polling `SnapShotPadViolation`, which is the same fact arriving from the other
direction, and it has no multi tap. `ControlGetPortName`'s rule is applied as
the console's own hardware rule rather than invented — one or two players need
no tap and are ports "1" and "2"; three or four cannot be played on that
machine without a tap in port one, and are "1-A".."1-D". Nothing an SDL pad
carries names a socket, and a USB socket number would be a different fact from
the one the screen asks the player to act on. Only kind 1 is reachable here:
the port's mouse is not a pad the engine can lose, and there is no tap.

One table correction came with it: page 38's record in `pages.c` carried no
second table address where 0x8001D41C installs the shared empty one.

Validation: `test_menu` adds the composition, the three violation kinds, the
empty port name, the "no button leaves the page" sweep and both arms of the
check; a control build with the composition removed fails five of them.
`test_client_menu --reinsert` drives the whole cycle through the real client on
BASE1 — pad added, frames drawn, pad removed, four frames held with the page
still up, pad back, pause page — and writes the three captures. All 73 CTests
pass, and the menu suites pass on PAL and US.

## RESTART LEVEL and the level-start copy, 2 November

This was found by a symbol-table census that also counted a function as covered
when any address inside it is cited. `ReloadAutoSave` (QUAKE.C `0x800400A8`,
1,376 bytes) was cited nowhere, by name or by address. It turned out to be the
whole of RESTART LEVEL, and the port's version of that row did something else.

**What the console does.** The death and pause pages park 21 (RESTART) and 20
(RESUPPLY) at `0x8001FE48` / `0x8001FEEC`. QuakeLoop's tail runs OnLeavingZone
(`0x80039138` → `0x8007CB8C` → `0x80078D1C`), which writes the dying zone's
blocks as any exit would. The outer loop then calls `RestartLevel(0)` or
`RestartLevel(1)` (`0x80018788` / `0x8001876C`). `RestartLevel` (`0x80040608`):

- calls `RestoreLevelStartState` (`0x800806C8`), copying 7,168 bytes from
  `0x800E47A0` back over save memory at `0x800E65A0`, which overwrites the
  blocks just written;
- for a resupply only, opens `"PlayerSave0"` and edits it (below);
- calls `ReloadAutoSave`.

`ReloadAutoSave` reads the restored record: the level name at block+28, the
position at block+52..60 (y raised 286), the yaw at block+66 and the zone digit
at block+20. It builds a synthetic start position named "Reload Start" at
`0x800C8CB4`, makes that the arrival, and sets the state word to 3, so the
reload's OnLeavingZone writes nothing (`0x80078D58`).

The copy is `StoreLevelStartState` (`0x80080620`). InitLevel calls it at
`0x80070F94`, after OnLoadZone, on every full load: a new game, a LOADMAP or a
unit change. `0x80070F80..0x80070F8C` skips it when the arrival reads "Reload
Start", so a restart's reload keeps its copy. A zone gate never reaches it:
LoadZone (`0x8007901C`) is OnLeavingZone and ReloadLevelData, and InitLevel is
not part of that. Restart therefore returns to the level's start, in whichever
zone the player died. `InitialiseQuakePlayer` writes `PlayerSave0` back after
spawning (`GamePlayerLoadSave(…, 1)` at `0x8003B6B0`), so the copy holds the
arrival point. Save memory carries every map's EVE_ blocks, the creature
batches, the unit's mission words, and the player record. That record holds
client+72..+223 at block+76, the entity's health, position and angles, the
level-clock word `0x800B29D4` at block+380, and the six tally rows at
block+228.

**What the resupply edits.** `RestartLevel` indexes block+16, so its offsets are
the client record's minus 12 (`0x800406EC..0x800407E4`):

- for ids 2..11 whose owned bit (`0x8009DC2C`) is set, ammo of the id's type
  (`0x8009DC5C`) is raised to half the BASE capacity at `0x8009C5C8`
  (`sll 16 / sra 17`); the tier is never read;
- health is set from the cap (client+0x48);
- a class byte other than 1 or 2 becomes jacket, with flags `& ~0x6000 | 0x1000`;
- armour rises to the jacket record's maximum (`0x8009C5ED`, 50), whatever the
  class.

**What the port did.** It reloaded `c->zone_index`, the zone the player died
in, with the carry flags down. `q2_sim_spawn` then handed out the blaster and
100 health, except on the map the session was launched into, where the derived
launch loadout stood in. The arrival rule placed the player at that zone's
first start position whenever the arrival lay in another zone. The dying
zone's event block was carried into the reload, so doors and spent records
stayed as they were at death. RESUPPLY spent a continue and then took the same
path.

**What it does now.** `client_load_zone` captures a saved game into
`c->autosave` at the end of every full load in single player. It skips zone
gates, saved-game and restart reloads, the front end and matches. The resident
zone is stashed into a scratch history, not the live one. RESTART and RESUPPLY
apply that save through `client_apply_save`, the card-load path, as the console
does. For a resupply, `q2_player_resupply` edits the save's inventory for the
duration of the apply and the copy is put back. What save memory does not hold
carries on: settings, cheat level, skill, the continues byte `0x800B335D`, and
the three random streams.

DEVIATION, stated: after a game is loaded from a file, a restart returns to
the loaded game. The console's card load is `ReloadAutoSave` too, so it also
skips the copy. A restart would then restore whatever the last full load left
at `0x800E47A0`: another level, or QFRONT's save memory when the game was
loaded from the title. In the second case `ReloadAutoSave` finds no record, and
the loaded level is rebuilt from empty save memory at the card's position.
Module clocks follow the port's existing saved-game policy for that case.

Validation: `test_playerdeath` pins the top-up arithmetic. The new
`test_client_population --restart` arrives on BASE2 from BASE1 carrying a
shotgun, picks up a chaingun, gains a secret and a unit word, goes through the
zone-1 gate and dies. It then checks that RESTART restores the zone, position,
yaw, clock, loadout, secrets, mission words and event store. It checks that
RESUPPLY tops up the same record, that a third restart gets the un-topped
record, that a cold-loaded save becomes the restart point, and that the legacy
reload kept for a session with no copy still lands in zone 1 with the blaster.
It passes on PAL and US. The native-lifecycle test now asserts that an
in-session restart rolls back a unit word set after entry, and that a restart
after a cold load keeps the loaded module state. All 73 CTests pass, as do every
`test_client_population` mode and `test_client_save --headless` on PAL. The
whole-disc `check_headless_disc.py --quick` sweep passes 163 of 163 on PAL. A
real-client run, `--map BASE2 --headless --demo --restart 360`, takes the HUD
from 97/48/28 back to the launch loadout's 102/50/28 at the entry point, with
zero errors and zero warnings. Captures are under
`.agents/tmp/functional-parity/restart/`.

**Census residues checked and found not to be gaps.** These names came up in
the same sweep and need no work:

- `monster_use` (`0x80062354`) is installed at entity+0x9C by `monster_start`,
  but no game code loads that field back. Only LevelBin modules call an actor's
  use, and the BOSS1 and LAB callers are the scripted RiderStand and DeadComm.
- `monster_triggered_spawn` (`0x800623D4`) is a bare `jr ra`.
- `IsEnergyWeapon` (`0x8005B8FC`) has no caller.
- The six `PrintMissionDetails` only print, through `PrintIdentityName` and
  `PrintBool`. The `InitMission*` functions that install them were not read.
- The `PSXB3ROCKS` family is bound, but no map declares it (userfuncs.h).


## Known differences from the console, 4 November

The first three of this round's items are closed; the rest are open and
listed at the end. Every claim below was checked by executing the disc's own code
(the bolt fixture's R3000 machine) or by a whole-disc sweep.

**The projectile arm's contact point.** ProcessMissiles_Many takes the actor
query's entry and moves it before anything reads it (0x80047E84..0x80047ED8):
for a body whose `+0x0C` record has a view, GenMissileHitPlane against that
view's plane with the bolt's raw velocity (+0x52) as the direction; for a client
with no view record, nothing (0x80047EAC); for a body with no client,
LineIntersectCrePlane (0x800542A4). The result is copied into the bolt's origin
(0x80047EE8), which is the point DoDamage's knockback, DoNoise and
GenerateBulletBlood (0x80048B64) all read. LineIntersectCrePlane computes
`start + d * SquareRoot0(s.s - |(s x e) >> 12|^2) >> 12`. That is the closest
point on the line only when |d| is 4096. A 1200-unit step against a body 600
ahead comes out at 79, not at the skin, and executing the disc's own function
says exactly that. `q2_combat_line_cre_plane` transcribes it with the image's
wrapping arithmetic and toward-zero shifts, and `bolt_contact_point` applies
both arms.

DoNoise runs on every arm through 0x80048608, not only on the hull impact:
the flagged actor contact (0x8004861C), the flagged entity-box contact
(0x80048650) and the hull impact. The previous note said a bolt striking a body
makes none; the branches at 0x80047E7C and 0x80047F1C say otherwise.

The bolt fixture (`generate_bolt_reference.py`) now runs four victim kinds: a
client with no view record, no client, and two client view planes. It records
DoNoise's calls and position, giving 276 cases per disc, with PAL and US in
agreement. `test_sim` matches every one.

**Which frame's view plane.** QuakeLoop (0x80038E70) runs QuakeLogic and
ProcessCreatureBatches, then ProcessAllCameras (0x80038FD8, UpdateWindowStuff at
0x8003815C), then ProcessMissiles (0x80039000). So a creature's round, fired
from a think, reads the previous frame's plane, and a bolt reads the plane this
frame's camera pass has just written. The port refreshed the plane only at the
end of the player's tick, and its creatures tick after the sim, so the two arms
were swapped. The tick now runs a camera pass ahead of the combat tick and keeps
the earlier plane in `view_plane_prev` for the creature round.

**The AI body clip.** gitrace (0x8005BD3C) gates its actor pass on mask
0x02000000 alone (0x8005BF18) and hands 0x800544EC the frame's actor list
(0x800B2B90..0x800B2B98); that function skips the ignore entity and nothing
else (0x800545E8). The port's pass ran an older cylinder approximation and
dropped every non-solid body unless the mask carried 0x04000000. Neither rule is
the console's. It now runs the shot path's transcription
(`q2_combat_trace_cylinders`). Membership is `listed`: on the actor list,
corpses included until their dissolve gate answers (0x8007F728..0x8007F740).
That is kept separate from `solid`, which is the separation gate (0x800513F0).
Against HEAD, the whole-disc sweep changes only `creatures.blocked_body` in the
two SECURITY cases (69 to 66 and 17 to 14), which are borderline cylinder
contacts the approximation accepted.

The list's ORDER is the record-slot order of MoveModels (0x8006A4F0), which
walks the 48 records at 0x800CBA28 and calls each handler. Each handler appends
itself through AddCreToNextBumpList (0x800552B4), and InitialiseBumpListFrame
(0x8005525C) swaps the double buffer each frame. Slots come from GetNextCre's
free stack (0x8006C098, 0x800B2BAC), which items and model entities share. The
port does not model that allocator. Instead the order was measured: with every
0x800544EC caller's candidate order reversed, the full PAL sweep's 213 reports
(29,568 values) and every final frame are identical. Order decides only exact
ties and the rounding of a shortened segment, and neither occurs in that sweep.

**One random stream, 5 November.** The console has one generator: every call
of 0x80089E28 jumps to BIOS A(2Fh) rand(), one state for the whole program.
The port kept five: the gameplay stream, `combat.rng`, `fx_rng`,
`ent_world.rng` and the glint's `script_rng`. A `q2_rng` can now be BOUND to
the gameplay state (`shared`), and the sim binds the weapon, effect and entity
streams at init. The glint stays private, because BIGGUN seeds and draws from
its own module-local state (module+0x43B0), not from the BIOS. The multiplayer
spawn tie-break used a host LCG; it now draws rand(). `q2_rng_seed` always makes
a stream private, so a fixture or preview owns its generator; the shared state
is seeded only by `q2_random_seed`, the console's srand().

The seed follows the console. InitLevel's first act is
`srand(InSelfPlayMode ? 13 : GetRCnt(0xF2000001))` (0x80070E20..0x80070E34),
ahead of the level data, the spawns and RestoreSavedData (0x80070F50). The
client seeds at the same point of its zone load: 13 for a headless run, the
self-play arm, which keeps every capture reproducible; the host clock otherwise,
in the root counter's place. A load therefore does not restore the saved
rand() state. The RAND chunk and the CMBT slots are still written (all three
hold the one state) and read, but `q2_save_apply_game_rng` applies nothing.
Because normal play seeds from the root counter, the console's absolute
sequence cannot be observed outside self-play. The attract demo's levels now
take the self-play arm as well (`selfplay_active`, see below).
What can be matched is the structure: one state, every draw on the game
frame, and a reseed at each load.

The draws that ran per DISPLAY frame now run on the logic tick, which is the
console's frame (QuakeLoop runs once per game frame):
- the teleport columns, 45 draws plus 4 per pad, which also spawned a group
  each displayed frame;
- the LAB and BOSS1 sweep velocities, 45;
- the mission lights;
- BIGGUN's lightning, which also advanced while paused;
- the title screen's scene lights.

On a host frame with no tick, the tick's result is redrawn without drawing
numbers: the teleport glow, the light jitter and the scene lights are cached.
`test_sim` checks that one draw from each bound stream advances the single
sequence in call order. The save tests now check that a load keeps the level's
seed.

**Closed later in the round (5 November):**

- **Weapon-driver write order.** See "Driver write order, 5 November" above.
  It is checked against 7,838 native driver calls over every weapon.
- **The muzzle's Y rounding.** Every player fire function negates the
  offset's Y around RotateS (0x8004C01C, 0x8004C04C). With RotateS flooring,
  the height is a ceiling in the port's frame.
- **Multiplayer missile order.** QuakeLoop runs every record's think, then
  ProcessMissiles and ProcessParticles. With `defer_missiles` the client runs
  `q2_sim_missiles_late` after the last player's tick.
- **Trigger bits #25.** `0x0100`, `0x0040` and `0x0020` only select which
  volume answers the first-match query (0x80050EE8..0x80050F7C). The player's
  consumer tests `0x1000`/`0x0200`/`0x2000` and never reads the result again.
  The port keeps the first match and the unmasked return.
- **Vertex rule #2b.** The drawer 0x800B1F90 writes each part's vertices into
  the scratch at vert_base * 8 and emits that part's faces in the same pass.
  That is last writer wins, as `q2_model_bake_indices` does.
- **Gun-arm palette 91.** The console's own BASE0 load, executed with its
  LoadImage captured, puts every texel of the arm's page and every entry of
  CLUT 91 exactly where the port decodes them. The texture data was never the
  difference. `test_vram_native` now compares the client's whole VRAM with
  the console's load for every level on both discs, rectangle by rectangle.
  Every gameplay map matches. The test exposed and fixed one difference:
  InitCluts uploads whole rows, so the slots after the last palette hold the
  buffer's following bytes. The darker arm in the one retail capture is
  therefore not texture data. With lighting already ruled out, that leaves
  the capture itself, which goes on the hardware list.

**Closed after that (5 November): the self-play attract demo.** The title's
thirty seconds (9000 units in QFRONT module+0x12DC0, US +0x12DD8, reset by
player one's pad word and drained by the frame delta through the page hook
module+0xC6AC) now run. At zero, 0x80101B08 puts up module page 19 with the
one-row table at module+0xEBC4 (US +0xEBD8), `DEMO OF GAME`, hides the title
objects, resets the player, sound and video settings and arms the engine's
delayed state change: twelve counted frames (0x8001F964), then state 10.
State 10 is GameSelfPlay (0x80041300, US 0x800412FC). It walks
SelfPlayMapNamePntr (gp+712) through Base3, Jail2 and Waste2 (0x8009D228, US
0x8009D47C), enters the level at the `SelfPlay0` start position and sets
InSelfPlayMode, so InitLevel seeds 13. Each level's init then reads
&InSelfPlayMode through engine+0x78 and, when set, writes a loadout (weapons,
selected weapon, ammo and, on BASE3 and WASTE2, health 45), skill 1, calls
SelfPlayOn (engine+0x58) on its recording (+0x8B4, +0x788, +0x7F4 on both
discs), sets NoInputTime to 900 and installs QuakeNextDemoLevel as the
no-input routine. The port reads all of that out of the module
(`q2_selfplay_scan`, src/game/selfplay.c) rather than restating it.

Playback is SwapButtons mode 1 (0x800323EC, US 0x800323E8). Each halfword
gives the frame's buttons (`& 0xFFF0`) and its length in video fields (the low
nibble, bits 3/0/1/2 worth 1/2/4/8), and the length replaces NewVcounts after
MainGameLoop's clamp. The terminator 0xFFFF, any real pad button or player
one's death ends playback; the zeroed NoInputTime then fires
QuakeNextDemoLevel at once, and its state 6 is ProcessGame's return to
GameTitle. So every demo returns to the title, whose own thirty seconds start
the next level. SpriteAnim(1) (0x80038820) draws the DemoSp overlay, a 64 x 20
sprite at (224, 20) stepping one cell per field through 48 cells of DemoSp2 and
then 48 of DemoSp1, through palette 79. `sim.dt_force` carries the recorded
delta into the tick. The recordings differ by region: PAL 1125 / 900 / 750
frames at 6 units a field, US 1350 / 1056 / 900 at 5.

`test_client_selfplay --headless <disc>` compares every recording's decode
with native SelfPlayOn / SwapAllButtons runs (`selfplay_reference.h`),
including the input, death and no-input exits. It then runs the loop through
the client: the countdown and its reset, the hand-off, the loadout, a
repeatable player position after 100 frames, the key-press exit with settings
restored, and the level order.

One host divergence: the console never restores the settings the demo
resets; the port puts the player's back when the demo returns to the title.

Limits: the port's creatures do not reproduce the console's encounters, so
on both discs the BASE3 and WASTE2 demos end early when player one dies there,
and the console's player presumably does not. JAIL2 plays its whole PAL
recording. The magazine-demo arm and the record path (SelfRecordOn) are not
reached on a retail disc and are not modelled.

**Closed after that (5 November): every level's VRAM is the console's.** The
pending rectangles were one gap in the shared loader, not in the screens.
LoadInGameVram (0x8003FE20) uploads every image in its registration table on
every level load, in table order, through 0x80068FAC (the four 8bpp sheets) and
0x80069018 (the rest), both ending in 0x800691A4. The port uploaded only the
font atlases, the icon sheet and Squiggle. `q2_vram_upload_registered` now runs
the table, apart from the three icon sheets, which the session picks. That puts
the logo and screen pictures, the pad diagrams and the DemoSp pages where the
console's loader puts them. The QENDMIS palette block differed only in palettes
0 and 4: the uplink screen opens during the port's load and writes the unit
tint and the globe palette (module+0x3ED4), where the console's fixture still
shows the section's 0x8000 placeholders. `test_vram_native` checks those two
against the module's data and the rest against the native hash. All 530
rectangles of all 45 loadable maps match on both discs, with nothing pending;
the regenerated fixture adds MATRIX8, which the interpreter can now load.

**Closed after that (5 November): three behaviours the residue pass found.**

- **The help computer after a demo.** The end of playback stores 3 into player
  one's pad mode word (record +0x2C8, 0x800D5EF8 PAL; 0x8003281C and
  0x800328AC), and its only other writers are SelfPlayOn (1), SelfRecordOn (2)
  and SelfPlayRecOff (0), the last reached on a retail disc only through
  QuakeEndSelfPlay, which nothing calls. HelpComputer_NewMessage (0x80021254)
  and HelpComputer_DoDisplay (0x80021524) both return at once while it is
  non-zero. So on the console, once the title has played a demo, no level's
  help computer raises anything for the rest of the power-on session. The
  client keeps the word (`pad_mode`) and gates both; `test_client_selfplay`
  checks a level's raise after a demo and without one.
- **The next unit's first level.** QENDMIS's exit sets NewMission and
  NextMission and no name; GameNewMission empties the name from 0x800AC670 and
  the unit's hub writes its fixed first level at `Default` (QMJAIL.C
  0x80023340 Jail2, QMPOWER.C 0x80023D2C Power1, QMGRAV.C 0x80024538 Command,
  QMBOSS.C 0x80024D40 Boss1). The port carried the last LOADMAP across the
  screen and, without one, went back to the front end. `client_unit_first_level`
  is the hub table now.
- **Sound RAM.** See the FORMATS note on the SPU budget: two sounds never fit
  (FRAGTOWE's last and SECURITY's), and a request for one plays from its slot's
  stale address or not at all. `q2_spu_ram` keeps sound RAM and the slot table
  across levels.

**The attract demos as a parity probe (5 November).** A recording replays the
console's pad against the port's simulation, so it stays in step only while
every tick matches. `tests/fixtures/lockstep_oracle.py` now runs the console's
own QuakeLoop over PAL BASE3's demo, natively, recording every `rand()` call
with its caller, player one, each creature's actor and edict, and the entity
box table, frame by frame. The port was compared against it until each
divergence was explained. The `rand()` stream, player one and every creature now
match through record frame 209, against 29 before; player one's path to 220.

What the comparison found, in the order the run reached it:

- the generator seed per level load, the first-tick all-areas rule and the
  door gate on the area flood;
- monster_start's one-frame deferral of the go routine;
- the Soldier's and Infantry's spawn-time walk draws;
- the teleport columns' 45 draws after the creatures;
- the trigger's held bit 24 and per-pellet spread;
- pain and death inside the damage function;
- corpses handled the next frame;
- the entity sweep after the creatures (`q2_sim_entities_late`);
- BLAST FORCE's boot value of 0 (the knockback the volley's gibs carry);
- the AI trace's start cells and its real entity sweep (`0x80053C58`, mask
  `0x810`, 286 inflation, the monster clips);
- step vectors rounded toward zero;
- turns writing only the target yaw;
- G_TouchTriggers' and `visible`'s pulls of the actor back over the edict;
- the 286 separation radius of players and creatures;
- the AI's `skill` cvar, a static record that always reads 0.

The oracle stubs SnapShotPadViolation (`0x80032DCC`). Without it
GameCheckPaused sees no pad and sets the menu word in frame 1, and every body
part's tumble is skipped.

*Frame 99, object slots, and after.* MoveModels walks the console's object
array (`0x800CBA28`, stride `0x300`) in slot order, and `0x8006C098` hands out
slots from a last-in, first-out stack at `0x800D4A28`, filled 0 to 47 by
`0x8006D0BC`. When the second Infantry gibs, five of its six gibs pop slots
12-8, above its own slot 5, and are moved that frame. The sixth takes slot 2,
which the Soldier's actor freed at frame 30, and waits a frame. The port now
mirrors the stack and its owners (`src/game/objslot.c`). The deal at load
matches the oracle's trace exactly: slots 0 and 1, then the four creatures in
2-5, then the two pickups in 6 and 7. Creatures think in slot order, with the
entity walk (`q2_entity_walk_to`) catching up to each.

Two timing faults surfaced past frame 99:
- **The AI clock** (`0x8007F2B4`, called from QuakeLoop at `0x80038EF8`)
  bumps level.framenum before player one's think. The port bumped it after,
  so the ledge Infantry missed the frame-98 shot at frame 100.
- **The active-area flood** runs before player one moves the doors. The port
  ran it after, so the far Soldier woke at 121 instead of 122.

Three more came after that:
- **The CREBATCH that releases Zone1bob** spawns at the top of the next
  frame (ProcessCreatureBatches, `0x80038F24`), not when the script selects it.
- **A creature's muzzle flash** stays lit, and draws its two random radii in
  the draw pass after the teleports, until its next think drops the bit
  (`0x8007EE20`).
- **The oracle itself** never reached the sound start's variation draws
  (`0x8007270C`), because its loader stubs the sample upload. It now makes
  them as a console with its samples loaded would.

*Frame 210 to 286.* Seven more faults, two of them in the oracle:
- **Spawn flag 4 is ambush.** monster_start rewrites it (`0x800619F0..0x80061A44`),
  so the second batch's flag-4 Soldier skipped ai_stand's idle roll.
- **`visible` tests no entity box** (`0x8005B950`): the console's Soldier saw
  player one through a door the port's could not see through.
- **The oracle's tempgoal was NULL.** Its loader never runs the boot's `main`
  (`0x80018D04`), which points ai_run's scratch goal at `0x800DDDCC`. The
  oracle now makes that store (`boot_globals`); before it, a pursuing creature
  seemed to skip SV_NewChaseDir, where the port, correctly, did not.
- **SquareRoot0** (`0x8008A7E8`) is a table root. It is what VectorLength,
  vectoangles and the separation pass call, and VectorNormalize (`0x8005C634`)
  mixes it with SquareRoot12. Exact roots put player one a unit off the console
  at frame 77 and a creature's pursuit detour on the wrong side at 217.
- **The item sweep** (`0x8002DC04`, doors, lifts, rotators) runs before
  MoveModels. Run after the player, the rising door held player one a frame
  longer, from 220.
- **A side face stops a mover one unit out** (`lo - 1`, `hi + 1` on X and Z in
  `0x80052C70`), which is where the console's player stands at the door (207).
- **The sighting spot and the crumbs** come from QuakeThinkGeneral on the AI
  clock, from the position before the frame's move (`entity+0x3C`,
  `0x8007E2C4..0x8007E3D4`). The port's crumbs were laid every tenth frame at
  the current position.

*Frame 287 to 378.* Five faults in the port and one in the oracle:
- **An unspawned creature has no body.** Its actor stayed in every hit list, so
  a Soldier's pellets struck it instead of the wall (287).
- **A gibbed creature's flash** stayed lit for the next draw (288).
- **The player's edict and body are two positions.** Creatures aim at the
  edict origin, refreshed on the AI tick before the player moves and by
  `visible`'s pull; the port aimed at the moved body (248).
- **A client's view height is a constant -250** (`0x8007E1B0`), 40 above the
  camera's standing eye the port used (248).
- **The knockback's self test is a byte compare** (`0x80057F38`): the
  attacker's last-attacker byte against the victim's index. A creature's byte
  is 0, so its hits shove player one with the self-damage force.
- **The oracle's level clock was frozen.** The VSync callback (`0x80019044`)
  advances it and the interpreter has none; a landing kick never decayed and
  tilted the view plane the bullet path projects onto. It now adds each
  recorded frame's fields.

*Frame 379, and player one through 407.* Two more port faults moved player one's
path from 255 to 407: a creature's muzzle is posed through the object matrix the
handler rebuilds after its think (so a turning Soldier fires from last frame's
facing), and the bullet's body sweep starts from the wall trace's own end, not one
rebuilt from its fraction.

*Frame 379 to 450: all of the recorded run.* Creature 9 (the second batch's
Soldier) chased player one a frame late because its activity gate
(`0x800578D8`) suspends it on the console at 292 and not in the port. The gate
asks whether the creature's area was drawn last frame (`0x800689C0`, stamped by
the SortData region test at `0x80065CC4`) and, if not, whether it is within 8192
of the view. The port's regions differed for two reasons:
- **A sealed door closes its SortData marker.** The marker test reads the node's
  runtime bit 15 (`0x80065D54..0x80065D60`), which a shut door sets on its
  portal as well as OBJDRAWOFF. The port tested OBJDRAWOFF alone, so areas 20
  and 30 got a rectangle behind a closed door.
- **The camera's row scales come from the view.** `0x80055DE4` scales row 0 by
  the view width over 320 and row 1 by the height over 240: ×1.6 and ×1.033 for
  PAL single player's 512×248. The port used ×1.5 and ×1, which moved areas 16
  and 35 a few pixels and counted area 16 as drawn.

With both, the region rectangles at frame 291 match the console's exactly. The
last fault was at 381: a pickup burst (`0x8005AB70`) draws its fifteen velocity
triples whatever the mesh, where the port drew three numbers per group.

The `rand()` stream and player one's path then matched the console's for all
450 frames of that run.

*Frame 451 to the end of the demo.* A longer run (620 frames) found five more
port faults:
- **The AI clock comes first** (`0x80038EF8`), before the item sweep and the
  batch spawn. The port advanced it after player one, so the third batch,
  spawned at 392 in a ticking frame, started its creatures' own clocks
  (edict+0x34, `0x8007ECC4..0x8007ED04`) one tick behind. At 469 a Soldier's
  FindTarget still heard the frame-240 noise that the console's had dropped.
- **The view plane's vertical is `sin(pitch)`.** The port negated it, so at 496
  five pellets were projected onto a plane tilted the wrong way (`0x80054BD4`).
  The first pellet's knockback came out (55, 7, 64) against the console's
  (-20, -25, 79).
- **The slide is measured from the trace's end** (`0x8004545C`, `lw v1,
  40(s0)` into the trace record at `0x800C8FE8`), not from the backed-off
  body. On a sloped floor the port came out a unit short (498).
- **`fire_hit` draws no blood** (`0x80061118`). At 518 the Infantry's smack
  drew 90 numbers in the port.
- **The machinegun's flash radii come after the round** (`0x8004C978`). The
  draw count was right, so the state compare could not see it; the values were
  swapped with the spread's.

With these, the `rand()` stream matches the console's for the whole BASE3 demo:
player one dies to the Soldier's shotgun at record frame 526 on both, and the
demo ends there. Two more faults put player one's path on the console's for
every frame as well:
- **`fire_hit` clears the ground normal** (`memset(object+0x60, 0, 6)` at
  `0x800613F0`), not the velocity. The port read +0x60 as the velocity and
  zeroed it. So after the Infantry's smack at 518, the airborne arm could not
  re-declare the floor and took the 20-unit probe (`0x80045CD0`), and the fall
  at 520 ran at half speed.
- **The player's own rounds sweep bodies to the world trace's end**, as the
  creatures' already did.

The port draws the player's death-animation pick at 526 (`0x8003CEEC`), which
the console's run does not reach. The demo is over by then, so it shows in no
stream.

*The other two demos (6 November).* The oracle now takes `--demo` and loads
JAIL2 or WASTE2 the way GameSelfPlay does, by pointing SelfPlayMapNamePntr
(gp+712) at the entry. WASTE2's load draws one number (a TIMER armed at
STARTLEV), so its compare starts one draw past the seed. Seven more port faults:
- **The start wrappers.** Berserk, Gunner, Arachner and Tank Commander spawns
  end in `walkmonster_start`, and with it the start-frame draw (`0x80061B2C`,
  which needs a move already set: the Gunner's stand comes first).
- **TIMER countdowns** subtract this frame's delta before the test
  (`0x8002739C`). The port's sweep ran a frame behind for a timer armed at load.
- **A player's body is 600 tall** (`object+0x96`). At 572 a round 299 above the
  origin passed over (WASTE2 frame 58).
- **M_CheckAttack traces bodies** (`0x8005D99C`, mask `0x0200001B`) and decides
  only when the trace ends on the enemy. A point trace's entity clip is
  `0x80053974`, not the box arm's inflated `0x80053C58` sweep.
- **The body list is live.** It holds the objects, so a player's body is where
  the player has just moved to.
- **First-frame activity.** Before the first draw every area's stamp equals the
  frame counter, so all creatures run frame 1 (JAIL2: phase 12 for all seven).

BASE3 and WASTE2 now match the console's `rand()` stream and player path to their
ends (record frames 526 and 211).

*JAIL2 from 200 to 322.* Four more:
- **The pitch enters the player's fire matrix and aim un-negated.** The object
  matrix at +704 is RotMatrix of +230 (measured rows at frame 198 for pitch
  -426); the muzzle (`q2_weapon_muzzle_origin`), the auto-aim search and its
  fallback all used the view's opposite sign, so a shot fired looking up went
  down.
- **The level clock is left on the last creature's clock.** `0x8007ED04` writes
  each thinking creature's clock into `level.framenum` and nothing restores it;
  the AI tick copies the global count (`0x800B29E0`, now `q2_level_tick`) back
  in. The player's noise is stamped with whatever it holds.
- **The head gib starts at the draw origin** (`+0xA4`, `0x8005A578`): the
  position lowered by 286 and raised by the model's bias.

JAIL2 now matches to frame 322 of its 891.

*Where JAIL2 parts now: frame 322.* A thrown grenade is an OBJECT on the
console: `0x800CCF28` is slot 7 of the 48-record array (`0x800CBA28`, stride
`0x300`), taken from the same LIFO stack as creatures and gibs, and its toss
(`0x80046B98`, the tumble draws at `0x80046BFC..0x80046C34`) runs at that slot's
turn in MoveModels: at 322, between the second and third creature. The port keeps
grenades and rockets in its own missile pass, with no slot, so their draws come
before every creature's and every later object is dealt a different slot.
Giving projectiles object slots and running them in the slot walk is the next
step. Two of the port's creature steps also fail at 322 where all three of the
console's succeed (`stepret 1`), which may follow from the same order.

Two known differences remain that the run did not reach:
- the trigger-volume adjustment of the sighting spot and crumbs (`0x80061024`,
  `0x80060C94`);
- a pickup burst's groups start at the item, not at fifteen of the posed mesh's
  vertices (`0x8006CC44`).

The US demos have not been run against the oracle.

## Limits

The audit remains open. Passing suites did not demonstrate full functional
parity: the follow-up above found the missing player-setup flow outside their
previous coverage. A script sweep or campaign route is not an exhaustive
playthrough of all possible state combinations. The selector review closes the
identified COMMAND, SECURITY and BOSS2 gaps but does not establish arbitrary
module execution. Static candidates are not proof of register values or branch
coverage. POWER1 and BIGGUN's secondary hook are now accounted for, and the
suspected COMMAND/POWER1 counter consumers proved inert. Shared acid/lava
sound dispatch and its bank fallback are now covered. The BOSS1 entrance branch
and BASE3/MAGDEMO's wall callback are covered as well. The shared ordinary
laser damage path and effect ordering are now checked; other registered frame
branches still need to be assessed
against their effects and persistent state.
Signed damage now follows the measured native arithmetic, but no normal-play
negative-damage caller has been established. Native damage and its feedback
preserve the view-amplitude fields; the port no longer assigns guessed values.
The measured reader is retained without claiming that an exhaustive trace of
every possible writer in every module has been performed.
The creature and entity audits describe their native coverage.

Remaining fidelity/measurement work includes paired retail camera/state captures,
NTSC comparison captures, SPU key-on transitions, host output conversion and measured final audio amplitude,
hardware movie A/V phase, and GPU cycle/scanout timing. See the
[renderer](RENDERER-AUDIT.md) and [audio](AUDIO-AUDIT.md) audits. Physical controller
ergonomics, audible output and physical host-device switching remain hands-on checks.

### Player auto-aim follow-up, 28 October

`FindPlayerTarget` (`0x80039DDC`) now searches eleven elevation rays in native
order: zero, then alternating positive/negative offsets in steps of 512 through
2560, with a forward extent of 8192. Its initial projected-screen filter is
overwritten unconditionally by the full actor list. The implementation therefore
adds no visibility, health or damageability filter. Each ray uses the measured
actor query, followed by a PrimaryColl test to the clipped contact; solid-entity
collision is not part of this native visibility test. The first clear ray wins.
The SDK normalization and ratan2 results feed the firing direction and barrel
target. A miss retains the native half-first-ray direction and targets pitch zero.

Acquisition uses the previous published viewport position and cell, before the
current live player's movement/look input. Easing runs once per logic tick and
the display driver copies its result. All fire substeps retain that tick's aim;
grenade paths that use their separate throw matrix keep that native distinction.
Corpses skip acquisition/easing. The optional versioned PLYR tail preserves the
direction, target, partly eased barrel, viewport origin/cell and validity across
cold saves; older player records remain readable.

The fixture executes complete PAL/US search, rotation, actor-query and SDK math
instructions with synthetic actor/window inputs and explicit hull visibility.
All 256 cases per revision agree, including every elevation, camera index,
occlusion, null viewport, roll and rejected initial screen projections.
`test_combat` checks the exact target, direction, pitch and visibility arguments.
The client regression covers four owners, pre-input acquisition, a viewport
distinct from the current player position, retained fire direction, display-only
updates, target loss, corpse behavior and immediate firing after cold loading.
The disc-backed BASE1 capture changes the barrel from zero to -160 while leaving
the camera level. Both PAL and US pass. Evidence is under
`.agents/tmp/functional-parity/autoaim/`.

These comparisons do not establish whole-session actor-list ordering, posed hit
refinement, full weapon-driver scheduling or matched hardware picture output;
those remain in the corresponding rows below. The original before-run log also
contains five subsequently corrected test assertions that used floor shift for
negative bolt velocity; native bolt division truncates toward zero. The six
barrel-state failures are the actual integration control.

### Consolidated remaining work, 28 October

This is the known remaining scope, not proof that undiscovered defects are
absent. A missing implementation, an unmeasured result and an archival format
question are different kinds of evidence; none is silently counted as complete.

| Area | Current limit | Evidence needed to close it |
| --- | --- | --- |
| Player-body / weapon scheduling | Body transitions, RNG use and draw-independent advancement have native fixtures. The view-weapon driver runs once per logic tick, and ATTACK follows its last-substep return, dry 2 included (29 October section). The port's restructured driver loop can still order writes differently from native within a call that crosses key boundaries. | Execute full native weapon-driver sequences against the port's loop, and compare integrated live/death ordering and RNG histories. |
| Projectile and other collision/toss details | Grenade bounce/owner lifetime, projectile actor queries, owner self-splash, cold-save cells and bolt flight/impact effects are corrected. Hitscan now uses the native query, with a single rail victim, immune shielding and entry-point blood. Knockback uses the entity origin, and gib rebounds use SDK arithmetic. GenMissileHitPlane now moves a client target's entry onto that player's own view plane before DoDamage, with the 32-bit wrap its numerator really has (31 October). The AI body clip's narrow phase has shared combat.c's transcription of 0x800544EC since `ff7795c`; the claim that it had not was stale. Still open: that clip's list ORDER against native list membership, the projectile arm's own GenMissileHitPlane call (0x80047EB8) and its LineIntersectCrePlane fallback, the cached bolt hull marcher, posed contact refinement, final-frame body submission, other entity/contact and simultaneous-sound ordering, and extreme math domains. | Compare the AI clip's list order with native. Read ProcessMissiles_Many's ordering around 0x80047E94 before applying its arm, since the projected point feeds that function's later reads. Qualify the remaining collision, presentation and sound boundaries and reachable numeric domains. |
| Shared gameplay randomness | The port retains separate combat, effect and global gameplay streams. Bolt trail/impact draws now share their saved combat stream, but native BIOS randomness is shared across subsystems. | Reconstruct reachable caller order, unify stream ownership and qualify integrated gameplay plus cold-save histories without claiming isolated seeded fixtures establish whole-frame parity. |
| Other explicit source deviations | Reviewed 29 October. The chime origin, knockback, BFG beam centring and drowning arms are corrected. Secret dedup, path-corner order, briefing-unit selection, actor effect areas and the muzzle's roll composition are equivalent or unreachable. The unit-five camera stays host policy. The BFG beam's endpoint freeze is corrected (31 October): ProcessSplurges' two pointers are a resolver the sim fills, so a live record's ends and area come off the ball and the body every pass. A model entity's geometric scale is carried and spent, so the gib dispatcher's fireball draws at half size. Remaining: a one-unit muzzle rounding difference, and a beam whose owner the resolver can no longer find keeps its last ends where native reads the recycled entity. | Pin the muzzle rounding against a native RotMatrix/RotateS fixture. The stale-owner case needs the allocator's reuse order to be reproduced before it can be anything but a stated deviation. |
| Creature hearing (player noise) | Corrected (Player noise section): native noise entities, sources, saves and FindTarget's owner test. The expiry relies on the noise's zero health standing in for an undefined null-pointer read. Zone history deliberately drops a heard noise. The silencer and rebreather arms are unreachable in shipped data. | Matched retail traces of creatures responding to noise, if the undefined expiry read needs confirming. |
| Native mission and campaign behavior | Named handlers are implemented, but static LevelBin fallback does not execute arbitrary branches or track registers. Registered frame callbacks, trigger combinations and their persistent state are not exhaustively qualified. | Compare reachable native branches and state transitions across difficulties, zone returns, deaths, restarts and saves; extend regressions for each discrepancy. |
| Whole-game behavior | Creature callbacks, movement, weapons, menus and multiplayer have focused native/functional coverage; a scripted campaign route is not a complete retail playthrough. | Matched retail state/input traces for remaining interactions, including boss combat and movement edge cases. |
| PAL rendering | No paired retail scenes with matching camera, animation, lighting and RNG are available for current gameplay output. | Compare matched native frames, including near-eye geometry, translucency, water and split-screen. |
| NTSC and other revisions | Selected US loader states now match independent native execution, and both PAL/US pass all-zone allocation coverage. Matched NTSC picture/audio captures are still missing; these loader comparisons do not qualify every subsystem or revision. | Re-run behavior, picture, movie and audio acceptance against each supported build and qualify additional revisions separately. |
| GPU timing | GPU duration, CPU/DMA contention and scanout racing the draw stream are unmodeled. | Establish shipped-game impact with packet/bus traces, then implement any required timing behavior. |
| GPU/GTE arithmetic coverage | Existing external golden vectors cover a subset of coordinates, windings, gradients and opcode modes. | Broaden independent hardware acceptance data; do not derive expected values from the port. |
| Flat-polygon allocation | The client uses native heap accounting for known layouts. All 82 selected PAL/US load phases match native mission dispatch and loader results, with separate coverage of all 102 direct-level selections. Multiplayer's erroneous Tribute allocation is removed; US four-player TIMS now receives the native 5,664 packets. All 115 zones pass on each disc. Unknown layouts or rejected accounting retain the provisional fallback. | Qualify arbitrary outer-state histories and recording allocations. Intermission retention and view counts are still supplied as fixture context; loader traces and all-zone coverage do not establish whole-session/hardware parity. See the renderer audit. |
| Final audio amplitude | CD/main register values and gain units are fixed; the complete output chain lacks independent hardware measurements. | Measure final balance, clipping order and least-bit rounding with voices and CD mixed. |
| XA and output conversion | The mixer now stays at 44,100 Hz with the CD's 25-tap interpolation; the XA predictor, filter rounding/start state and device conversion remain unqualified against hardware. | Compare the complete CD waveform and final device output against independent hardware measurements. |
| SPU transitions and coverage | Absolute key-on latency/phase/history, remaining fractional filter positions and general ADSR settings are not fully established. Two finite END/repeat captures are excluded from the steady-waveform suite. | Match independent transition captures, including those exclusions. The game-specific ADSR setting is `0000/0000`. |
| Reserved ADPCM shifts | The shift-12-through-15 hardware capture does not match the fallback; all 2,475 PAL samples use valid shifts. | Match an independently verified model, without treating this unused PAL edge case as a current audible defect. |
| Movie synchronization | Software scheduling and fades work; initial hardware A/V phase, downstream device latency and long-run alignment are unmeasured. | Compare independent retail picture/audio captures and host output timing. |
| Host audio devices | Missing/failed streams now retry, with nonfatal initialization and silent movie-clock continuity. Open default-device migration is delegated to SDL; physical switching and real speaker latency remain unqualified. | Exercise actual unplug/replug and default-device switching, and measure audible latency. Automated failures and real SDL dummy-output recovery are covered in the audio audit. |
| Physical controllers | Automated tests cover synthetic events; physical ergonomics and rumble are not qualified. | A specifically authorized hands-on session with the intended controllers. |
| Undefined native memory | Enemy-less Boss1 aim and the stand-on-entity branch can depend on uninitialized PS1 stack history. The port uses documented defined behavior. | Full machine-history evidence if reproducing these invalid/undefined states is required; another actor-state formula cannot establish equivalence. |
| Historical reconstruction notes | `openquestions.md` retains stale open markers and low-impact metadata questions. Scene bytes/bits, resource fields, remap IDs and authored coordinate/bounds provenance are not all classified. | Reconcile each note with current code and native consumers. Count a functional defect only when a reachable behavioral effect is demonstrated. |

The table contains twenty-one known remaining areas. It is an inventory of current
evidence, not an assertion that every undiscovered bug has been enumerated.

CHD/MDS input, perspective-correct rendering, multiple host mice and revival of
cut multiplayer modes are host extensions or intentionally unsupported features,
not missing behavior of the supported retail game. Host file containers, CRT,
bloom and adaptive presentation likewise have explicit divergence policies.
The owner's requested stronger pickup autoswitch is also deliberate; disabling
it restores the native blaster-only pickup switch.

The CD-DA item has been reclassified after executing both native music paths.
All 51 named playlists on each supported disc either select XA ids 2..21 or
stop; 1,170 starts across repeated loops never select negative-file ids 0/1.
Those reserved entries have zero duration, boot TOC setup does not change
them, and the exported `PlayThisXA` function does nothing. CD-DA PCM remains
unimplemented for modified tables or a different revision, but this is not an
identified missing soundtrack in the supported retail game. The PAL audio
track is all zeroes; the US track contains only signed PCM values -1, 0 and 1.
The comparison also fixed an implicit restart at a playlist's zero terminator;
native playback stops there. Both finite control lists failed before that fix,
while shipped nonempty lists already loop explicitly. The client now matches
native selection, channel, timer, cursor and termination results in 3,156 checks
per disc. See the [audio audit](AUDIO-AUDIT.md#cd-da-and-playlist-scope)
for the evidence and its limits.

## Unrelated housekeeping

The current Git inventory, rechecked during the 28 October audio-clock pass,
contains only the main checkout on `codex/functional-parity`. The six entries
listed below are no longer registered. This pass did not remove them;
`audio-clock/worktrees.json` records the empty secondary-worktree inventory.

Historical finding: these worktrees were clean with all commits already in
the current checkout (`a590ce9`). They were reported as removal candidates:

| Path | Branch |
| --- | --- |
| `E:\Repositories\Q2PSX-PC\.claude\worktrees\jovial-easley-421933` | detached at `816e874` |
| `E:\Repositories\Q2PSX-PC\.claude\worktrees\youthful-jennings-369181` | detached at `ac7d59d` |
| `E:\Repositories\Q2PSX-PC-fire` | `claude/creature-fire-secret-b8bfe1` |
| `E:\Repositories\Q2PSX-PC-mg` | `claude/mg-recoil-doors-c4e349` |
| `E:\Repositories\Q2PSX-PC-rail` | `claude/railgun-tank-7c31a2` |
| `C:\Users\djdac\AppData\Local\Temp\claude\E--Repositories-Q2PSX-PC\37c44139-0571-4fd4-a2c6-c44bd7b15f05\scratchpad\headwt` | detached at `d93b1fe` |

All six were rechecked on 27 October during the BOSS1 pass: clean, fully merged,
and with their latest commits dated 22 October. None is checked out on the
default branch. `boss1-entrance/worktrees.json` retains the check.
The LAB lift pass rechecked all six with the same results; its current record
is `lab-lifts/worktrees.json`. The broader native-lift pass confirmed the same
state in `native-lifts/worktrees.json`; the earthquake and laser-course passes
confirmed it again in `earthquakes/worktrees.json` and
`security-lasers/worktrees.json`. The orb/lift pass confirmed all six remain
clean and fully merged, with the same commit dates, in
`security-orb/worktrees.json`. Those removal recommendations are historical;
the current inventory above supersedes them.
The LAB/BOSS1 sweep pass rechecked the same six clean, fully merged checkouts;
`native-sweeps/worktrees.json` records their paths, branches and commit dates.
The lifecycle pass rechecked all six with the same result; its record is
`native-lifecycle/worktrees.json`.
The 28 October audio-gain pass again found all six clean and fully merged,
with no commits newer than 22 October; `audio-gain/worktrees.json` records
their current paths, branches and dates. They remain removal candidates,
and none was removed.

The orb/lift review also found PAL's `LiftRing1` / `ring` asset-name mismatch,
documented above. It remains an explicit source-data discrepancy; there is no
evidence for substituting the differently named node.

The damage pass removed its two converted raw PPM captures and superseded
`view-build.log` / `inspect-build.log` after verifying their absolute paths and
absence of links. The PNGs, failing controls and final build/test logs remain.

The save fixture defaults to Windows TEMP. The focused rerun redirected TEMP,
TMP and TMPDIR into the project task area. Automatic approval review blocked
deleting emptied test directories (reason: "blocked by policy"), including
`C:\Users\djdac\AppData\Local\Temp\q2psx_save_test`. Disposable files were
removed; selected PNGs and final logs were retained.

The BOSS1 pass completed the previously blocked file cleanup with explicit
paths after a manifest-driven request was rejected. It removed 24 converted
raw PPMs, one duplicate PNG, two crosshair-only POWER1 diagnostic PNGs, and
seven superseded logs across `unlocks/`, `selectors/`, `callbacks/`, `power1/`,
`burn-sounds/` and `boss1-entrance/`. Absolute targets and link-free ancestors
were checked before deletion; pixel comparisons verified every retained image.
The cleanup record is `boss1-entrance/cleanup-images.json`. Useful before/after
PNGs, failing controls, final build/test logs and unique sweep captures remain.

The laser-wall pass removed six converted raw PPMs after verifying their PNGs
contain identical pixels, plus one superseded build log. Automatic approval
review rejected removal of the four empty smoke-profile directories and their
parent under `.agents/tmp/functional-parity/laserwall/smoke-saves/` with reason
"blocked by policy". These empty directories remain; PNGs, disassembly, final
test logs and unique headless-sweep evidence are retained.

The ordinary-laser pass removed nine converted raw PPMs after checking their
absolute paths, link-free ancestors and identical retained PNG pixels. The
conversion record, current sweep evidence, disassembly, failing control and
final test/build/stage logs remain in `static-lasers/`.

The LAB lift pass removed 15 converted raw PPMs, 13 disposable regression saves
and its superseded census log after checking exact paths and link-free
ancestors. PNG pixel equality and the duplicate census rows were verified;
`lab-lifts/cleanup.json` lists the removed files. Current logs, both failing
controls, native disassembly and selected PNGs remain.

The broader native-lift pass removed 15 raw PPMs after verifying identical
retained PNG pixels, 13 disposable regression saves and its superseded census
probe. Exact absolute paths and link-free ancestors were checked first;
`native-lifts/cleanup.json` records all 29 removals. The failing control,
corrected census, final logs, disassembly and sweep evidence remain.

The laser-course pass removed 194 converted raw PPMs and 22 disposable disc-sweep
saves after checking the exact task-local paths and every ancestor for reparse
points. Identical PNG pixels, test logs and reports remain;
`security-lasers/cleanup.json` records all 216 removals.

The orb/lift pass removed 207 converted raw PPMs and 35 disposable regression
saves. Exact absolute paths and every ancestor were checked for reparse
points, and retained PNG pixels were compared before deletion.
`security-orb/cleanup.json` records all 242 removals; the before/after PNGs,
native disassembly, failing control and final build/test/stage evidence remain.

The LAB/BOSS1 sweep pass removed 210 converted raw PPMs, 35 disposable test
saves, one duplicate first-frame PNG, its temporary control-source backup and
the superseded control-build log. Every absolute target and ancestor was
checked for reparse points; retained PNG pixels and the restored production
source were verified. `native-sweeps/cleanup.json` records all 248 removals.
The original failing control, native disassembly, matching before/after PNGs
and final build/test/stage evidence remain.

The earthquake pass removed 16 converted raw PPMs, 13 disposable regression
saves and two superseded logs after checking exact paths, link-free ancestors
and identical retained PNG pixels. `earthquakes/cleanup.json` records all 31
removals. Before/after PNGs, native disassembly and final test/stage evidence
remain in that directory.

The lifecycle pass removed 237 converted raw PPMs, 35 disposable regression
saves and one duplicate corrected capture. Exact absolute targets and all
ancestors were checked for reparse points, and retained PNG pixels were
verified. `native-lifecycle/cleanup.json` records all 273 removals. Native
disassembly, the failing control, before/after PNGs and final logs remain.
