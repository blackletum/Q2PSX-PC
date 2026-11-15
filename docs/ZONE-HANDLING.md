# Zone handling audit

The client used to rebuild each zone's Population from `COMMON.DAT` on every
visit. Event latches survived, but items and creatures did not: collected
pickups returned, dead enemies revived, injured enemies healed and activated
ambushes could become dormant again. The JAIL4 zone 3 return regression failed
19 checks before population history was added.

## Retail evidence

These addresses were read from the user's PAL executable, SLES-01534, using
`q2psx-inspect disasm`; they are not inferred from the desktop implementation.

| Routine or call | Behavior |
| --- | --- |
| `OnLeavingZone`, `0x80078D1C` | Saves outgoing state before changing the zone. |
| `0x80078E5C` -> `0x800569C8` | Saves zone objects. |
| `0x80078E68` -> `0x80056F8C` | Saves Population group flags. |
| `0x80029078` | Saves the map's event latches; this was already implemented. |
| `OnEnteringZone`, `0x80078E98`, call at `0x80078F9C` | Restores objects through `0x800567FC`, before the zone callback. |
| Living-creature store, `GeneralRespawnStuffStore` `0x8007D318` | A 12- or 20-byte record: class, position (y+286), yaw>>6, health (>>2 toward zero in the 12-byte form, whole in the 20-byte form), the edict's nine spawn-flag bits plus the record's 0x600, and the target (the path corner's id when the target is zero and the creature is walking to a class-114 corner). Nothing else: no move, no enemy, no think. |
| Living-creature respawn, `GeneralRespawnStuff` `0x8007D158` | Decodes that record into a spawn record and calls `SpawnGeneral` (`0x800565C4`), a fresh spawn through `0x8007E440` and `SetupBoxDrop`; the stored health is written before export 0 (`0x8007E680`) and again after it (`0x8007D304`). |
| Item/corpse serializer, `0x80059B08` | Supplies the separate saved-object path for items and bodies. |

The retail format serializes objects; it does not keep native pointers or resume
an arbitrary C AI call stack. The desktop history follows that distinction. A
living creature's record is quantised exactly as the console's is, because the
yaw and health quantisations are lossy; its position is kept whole where the
console's 16-bit form would have kept it whole.

## Implemented behavior

`src/game/zonestate.c` stores each Population object's state by map, kind, group
and authored slot. A hash of the Population bytes and difficulty prevents a
different layout or skill from inheriting unrelated records. Runtime array
indices are not stable identities: an item's slot can be reused after collection,
and the creature roster can change with the resident zone's modules.

On departure the client records selected items, including removed items whose
runtime slots have been reused, and creatures that actually existed in the
resident zone. Dormant and out-of-zone placeholders do not overwrite a saved
resident. On return it rebuilds the world, restores selected item groups and
object state, and spawns each surviving creature afresh from its record, as
GeneralRespawnStuff does: the stored position goes through the same
`q2_spawn_place` box drop a Population record takes, the stored yaw, target and
spawn-flag bits are written before the wake runs export 0 and monster_start, and
the stored health is written again after them (`q2_zone_history_spawned`). The
creature's AI starts from its stand, with no enemy, no pursuit and no deadline
from before the gate. Dead bodies alone carry a runtime through the gate (below).
Creature residency follows the saved position in the destination PrimaryColl.
Collision nodes, model bindings, actor caches and AI links are rebuilt.

At departure the client first does what OnLeavingZone does before it stores
anything. FinishDying (0x8007F630) turns every creature still carrying deadflag
DEAD_DEAD into a detached class-47 body resting on its death move's last frame,
with no edict, no think and no AI methods; the store/respawn pair (0x80059B08 /
0x8007D558) brings it back as that body under the corpse handler. Then
SaveCreatures' kill hooks run: gibs and boss fragments (class 48, DestroyMe) and
every pickup whose flags carry bit 4 — death drops (0x80020B38) and collected
pickups — are removed, because LoadStoreQuakePickup (0x8005B4EC) refuses to
store them and RememberMe kills the slot regardless. Explosion models and
projectiles in flight, the held grenade included, are carried through an
overlapping doorway with their velocity, fuse, attitude and shooter binding,
because the zone path (LoadZone 0x8007901C → ReloadLevelData 0x800793B8) never
resets the object array or the missile pool; only the level path (LoadLevel
0x8007CA44 → FinishedLoading 0x8007C938) does. An object that does not join the
destination's PrimaryColl is not parked: the console has no suspended objects,
so the history rebuilds its dynamic and projectile records from the live world
at every departure and such an object is simply gone.

A living creature's record is the console's: position (whole, or through the
16-bit form's wrap when every coordinate fits it), yaw rounded down to 64
units, health truncated toward zero to a multiple of four when it lies in
-511..511 and every coordinate fits sixteen bits (the 12-byte form), else
whole, the nine edict spawn-flag bits, and the target or the path corner's id
(masked to eight bits). The move, AI frame, enemy, deadlines and display cursor
the port used to carry are not in the record and do not come back; the
runtime the cold save keeps for the resident zone is discarded when a gate
consumes the record. A creature caught mid-death at the gate becomes a body, as
FinishDying produces, and the body's runtime comes back under the corpse
handler; detached bodies keep their dissolve state. The returning body is posed
where RespawnQuakeDeadBody poses it: LastFrameFromSequenceNum answers the last
tick of the clip the stored frame lies in (the clip lengths through that clip,
in the engine's tenths, minus one), and `client_cre_pose_returned_body` writes
that position into the actor's animation cursor, which the corpse tick never
advances. Measured on the disc every death clip is three animation frames per
AI frame, so the body comes back two animation frames further into its clip
than the one the player walked away from. The map's AI clock, fractional tick,
alert references and player trail also survive. Unloaded objects are suspended:
their remaining deadlines are rebased on re-entry, while objects transferred
through overlapping geometry continue on the active clock. Timers stored as
durations stay durations.

Collected items stay removed. Surviving creatures keep their position, their
wounds, yaw and target as the console's record quantises them, and their nine
spawn-flag bits; casualties remain dead or removed. Activated ambushes
stay selected even when their original CREBATCH came from a repeatable event
which event-history replay does not rerun. A repeated CREBATCH cannot summon
their saved members again. Multiplayer continues to use its existing arena
spawn and respawn rules.

History is carried across both zone gates and map changes. Starting a new game
clears it without recapturing the abandoned game during the first load. Native
saves contain an additive `ZPOP` chunk with a versioned, bounded, explicitly
little-endian payload. Version 2 adds full object runtime and map clocks;
version 3 also retains direct-think creature callback identities, render scale
and the projection's consumed pain response; version 7 stores a creature's
spawn flags as the console's eleven-bit record field (older files are
converted on read). The resident `CAIS` chunk likewise
uses version 3 for these fields. Versions 1 and 2, and files with no ZPOP, remain
readable. Older saves
cannot recover motion or populations which were never recorded. The resident
save reconstructs dynamic entity slots before applying its existing ENTS/ITEM
state, then rebinds the full runtime. No model, actor, move or callback address
is written to the file. Resident projectile slots retain their processing order;
collision nodes are rebuilt. History copies own their runtime records independently.
An empty creature roster no longer discards the zone model bank or bypasses
runtime restoration (BOSS2 zone 0 exercises this path).

The loader now opens both the destination zone and COMMON archive before
changing the live mission row, loading-screen counters, transition serial,
shocks or outgoing history. A missing or unreadable archive leaves the current
world intact and clears the failed carry request. A local copy also protects
the frequent `client_load_zone(c, c->map, ...)` call from overlapping string
copying. Same-map player simulation carries its tick count, fractional dt phase
and item-world time along with the existing level clock.

## Validation

`test_client_zones` always runs a synthetic history test. Its disc mode also
uses the real loader, fixed headless rendering and the user's assets:

```powershell
.\build-msvc\bin\Release\test_client_zones.exe --headless .\.install\disc\game.cue .\.agents\tmp\zone-runtime\after
```

The tests cover reused entity slots, roster reordering, out-of-zone placeholders,
Population identity mismatch, JAIL4 collection and casualties, a cold save/load
in another zone, a full map round trip, a new game, failed loads, fractional
simulation timing and BASE1's LiftRoom ambush. The two JAIL4 captures use the
same camera: the health pickup visible before collection remains absent after
leaving, saving, reopening the save and returning.

The runtime regression adds airborne/landed drops, an explosion, an unbound gib,
all five projectile kinds and a held grenade, a firing creature and a creature
one frame into its death. A cold save in the zone records all of them; a gate
kills the drops and the gib, finishes the dying creature into a corpse, carries
the held grenade, and leaves the rest where the console would — live in the next
zone, not waiting — so nothing but the grenade and the two creatures is there on
return. The firing creature comes back as a fresh spawn: stand move, parked go
think, no enemy, no deadlines, its health and yaw quantised as the record
quantises them and its position re-placed by the spawn's box drop. The corpse
comes back with its animation cursor on the last tick of its death clip, and
neither the corpse tick nor a drawn frame moves it. The same test measures
every named death move of the resident modules against the model's clip chain
(one clip per move, three animation frames per AI frame, the move's last frame
two animation frames short of the clip's end). A corpse-dissolve round trip,
removal followed by slot reuse, repeated BASE1 overlap crossings and a
toss-table deferral test guard against lost objects and duplication. The
runtime image pair shows drops and a projectile before departure and, after a
gate out and back, the authored pickup alone; `attacker-*` and `dying-*` /
`corpse-*` frame the two creatures before the gate and after the return.

`test_save` additionally checks multi-map history encoding, removal/corpse/item
fields, v1 migration, independent runtime copies, older files without ZPOP,
duplicate chunks, unsupported versions, malformed runtime flags, truncation
and bounded object counts. The disc harness checks all 21 multi-zone
maps' gate destinations and the 19 maps with walkable gates. PODCITY and THEVAT
have no inter-zone gate and are explicitly skipped by the crossing test.

## Scope

This is native continuation state, not bit-for-bit compatibility with the
console's packed save representation. Transient particle and audio tails are
not archived; restored objects emit their ongoing effects on subsequent ticks.
Creature-module and LevelBin handler coverage and limits
are tracked in [CREATURE-AUDIT.md](CREATURE-AUDIT.md) and
[ENTITY-AUDIT.md](ENTITY-AUDIT.md).
The record's two spare bytes (byte 5 of the 12-byte form, bytes 6..7 of the
20-byte form) are decoded into the details block but SpawnGeneral is not handed
them; no class-specific store that fills them has been found, and the port
carries nothing for them.
