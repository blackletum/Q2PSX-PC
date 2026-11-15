/* Population state retained by OnLeavingZone / OnEnteringZone.
 * Retail saves objects at 0x800569C8 and group flags at 0x80056F8C.
 * A living creature's record is the console's (GeneralRespawnStuffStore,
 * 0x8007D318, decoded as GeneralRespawnStuff 0x8007D158 decodes it) and its
 * return is a fresh spawn; dead bodies carry their runtime. Authored identities
 * key the records; runtime slots are not cross-zone identities. Models,
 * collision nodes and live references are rebound against the destination's
 * resources. */
#ifndef Q2PSX_ZONE_STATE_H
#define Q2PSX_ZONE_STATE_H
#include "q2psx.h"
#include "entity.h"
#include "projectile.h"
#include "modelent.h"
#include "monster.h"
#include "ai.h"
#include "crebind.h"

#define Q2_ZONE_MAP_NAME 16
#define Q2_ZONE_HISTORY_MAPS 64u
#define Q2_ZONE_HISTORY_OBJECTS 4096u
enum { Q2_ZONE_ITEM = 1, Q2_ZONE_CREATURE = 2,
       Q2_ZONE_DYNAMIC = 3, Q2_ZONE_PROJECTILE = 4 };

/* Live presentation uses a rebound move pointer; snapshots retain only the
 * cursor and source. The frame stamp belongs to the new client frame. */
typedef struct q2_zone_animation {
    q2_model_cursor cursor;
    const q2_mmove *move;
    long frame_stamp;
    u8 source;
    bool stamped;
} q2_zone_animation;

enum { Q2_ZREF_NONE, Q2_ZREF_PLAYER, Q2_ZREF_CREATURE, Q2_ZREF_CORNER };
typedef struct q2_zone_ref {
    u32 kind, group, slot, id;
} q2_zone_ref;

enum { Q2_ZTHINK_NONE, Q2_ZTHINK_ITEM, Q2_ZTHINK_SHRINK,
       Q2_ZTHINK_DROP, Q2_ZTHINK_MODEL, Q2_ZTHINK_GIB };

/* All pointer members in these snapshots are NULL. The portable encoder
 * writes named scalar fields, never these structs' padding or addresses. */
typedef struct q2_zone_runtime {
    s32 sim_time, ai_time, ai_frame;
    bool materialized; /* session-only: don't discard a capacity-deferred object */
    union {
        struct {
            q2_monster state;
            q2_zone_ref refs[5];
            char move_module[13];
            u32 move_offset;
            u8 has_move, thinking;
            q2_zone_animation animation;
            q2_cre_shared_state shared;
        } creature;
        struct {
            q2_entity state;
            u32 source_slot;
            u8 think;
            bool model_bound;
            s16 velocity[3];
            q2_gib_body gib;
        } entity;
        struct {
            q2_projectile state;
            q2_zone_ref shooter;
            u32 source_slot;
        } projectile;
    };
} q2_zone_runtime;

/*
 * For a creature, `health`, `yaw`, `target`, `pos` and `spawnflags` are the
 * console's record as GeneralRespawnStuff (0x8007D158) decodes it, written by
 * `store_living_record` (zonestate.c): yaw rounded down to 64 units, health a
 * multiple of four toward zero in the 12-byte form, the target masked to eight
 * bits, and `spawnflags` the eleven-bit record field -- the edict's nine bits
 * (+0x1C >> 18) in bits 0..8 and the Population record's 0x600 (+0xF2) -- not
 * the edict word. ZPOP files before version 7 hold the edict word here and are
 * converted on read. A body keeps full precision, since its runtime comes back.
 */
typedef struct q2_zone_object {
    u32 group, slot;
    s32 zone;
    u16 id;
    u8 kind, removed, dead, hidden, taken, shrink;
    s16 health, scale, target;
    s32 pos[3], yaw, frame, remove_in, respawn_at;
    u32 spawnflags;
    q2_zone_runtime *runtime;
} q2_zone_object;

typedef struct q2_zone_population {
    char map[Q2_ZONE_MAP_NAME];
    u32 identity;
    q2_zone_object *object;
    u32 count, capacity;
    u32 next_serial;
    bool runtime_present;
    s32 ai_time, ai_frame;
    u32 ai_phase_us;
    q2_trail_state trail;
    q2_zone_ref alerts[3];
    s32 alert_frame[3];
} q2_zone_population;

typedef struct q2_zone_history {
    q2_zone_population *map;
    u32 count;
} q2_zone_history;

struct q2_sim;
struct q2_creature_world;
void q2_zone_history_free(q2_zone_history *h);

/*
 * OnLeavingZone's prelude (0x80078D1C), applied to the LIVE world before the
 * departing zone is captured; it is not run for a cold save.
 *
 * FinishDying (0x8007F630, called first at 0x80078D2C) walks the 48 object
 * slots and turns every creature whose edict still carries deadflag DEAD_DEAD
 * (`(edict+0x20 >> 22) & 3 == 2`, 0x8007F688..0x8007F690) into a class-47 dead
 * body: ProcessQuakeDeath (0x8007CD24) counts the kill, the edict is freed
 * (0x8007F288) and unlinked (slot+0x2EC = 0), the hit volume is rescaled
 * (0x8007F77C), the think becomes the corpse handler 0x8007F71C, slot+0x10C
 * gains 0x8000 and the class moves to slot+0xDA with 47 at slot+0xD2. The frame
 * is left where the death move had it; QuakeStoreInanimate (0x80059B08) stores
 * the CLIP that frame is in (FindAnimSequenceNumFromFrame, 0x8006DA3C, at
 * 0x80059BC4) and RespawnQuakeDeadBody (0x8007D558) poses the returning body on
 * that clip's last tick: LastFrameFromSequenceNum (0x8006DAB4, at 0x8007D748)
 * sums the clip lengths through it -- in the tenths BodgeAnimData scaled them
 * to, the unit of +0x100 -- and subtracts one, and 0x8007D75C writes that into
 * the body's +0x100, with the corpse handler installed (0x8007D6E8) and no
 * edict. The port's body comes back with its frame on the move's last AI frame
 * and its animation cursor on that tick (client_cre_pose_returned_body), two
 * animation frames further than the last AI frame on every death clip the
 * disc has (tests/test_client_zones.c measures them).
 *
 * A LIVING creature's +0x18 hook is GeneralRespawnStuffStore (0x8007D318),
 * which keeps class, position, yaw >> 6, health, nine spawn-flag bits with the
 * record's 0x600, and the target; GeneralRespawnStuff (0x8007D158) turns it
 * back into a spawn record for SpawnGeneral (0x800565C4) -- a fresh spawn --
 * and writes the stored health after export 0 (0x8007D304). The port stores
 * that record (store_living_record) and spawns afresh on return
 * (q2_zone_history_creatures, then q2_zone_history_spawned after the wake).
 *
 * Then SaveCreatures (0x800569C8) calls each live slot's class-table +0x1C
 * hook (0x80056B74..0x80056B88). Table 0x800A3368: creatures, pickups (46)
 * and dead bodies (47) carry RememberMe (0x8007F8CC), which stores through the
 * +0x18 hook and then KillModel (0x8006D280); fragments (48, entry 36 at
 * 0x800A3A28) carry DestroyMe (0x8007F928), KillModel alone. LoadStoreQuakePickup
 * (0x8005B4EC) refuses to store a pickup whose +0x44 carries 4 (0x8005B500..
 * 0x8005B50C): every dropped item has it (0x80020B38, except item 21; landing
 * ORs it again at 0x80020D00 unless OBJECTIVE 8), so gibs and timed drops die
 * at the gate. Explosion and debris slots carry class 0 and match no table
 * entry; the missile pool is only reset by InitialiseMissiles (0x80048C54 from
 * InitEveryLevel 0x80070748), and InitCreFreeList (0x8006D0BC) runs only from
 * FinishedLoading (LoadLevel path, 0x8007C9A4) and the level screen
 * (0x8006E0A4) -- none of which MaybeLoadZoneName's LoadZone (0x8007901C ->
 * ReloadLevelData 0x800793B8) reaches. So those live on through a zone gate.
 */
void q2_zone_leaving(struct q2_sim *sim, struct q2_creature_world *world);
q2_result q2_zone_history_copy(q2_zone_history *dst, const q2_zone_history *src);
bool q2_zone_history_valid(const q2_zone_history *h);
q2_result q2_zone_history_capture(q2_zone_history *h, const char *map, int zone,
                                  struct q2_sim *sim,
                                  const struct q2_creature_world *world,
                                  const q2_actor *actors,
                                  const q2_zone_animation *animation, u32 phase_us);
void q2_zone_history_items(const q2_zone_history *h, const char *map, int zone,
                            struct q2_sim *sim, const q2_model_bank *zone_bank);
/* Before the wake: places every surviving record. A living creature's runtime,
 * if a cold save left one, is consumed here -- the console's record has none. */
void q2_zone_history_creatures(q2_zone_history *h, const char *map,
                                int zone, struct q2_sim *sim,
                                struct q2_creature_world *world);
/* After the wake has run export 0 and monster_start: the stored health again,
 * as GeneralRespawnStuff writes it over SpawnGeneral's result (0x8007D304). */
void q2_zone_history_spawned(const q2_zone_history *h, const char *map,
                             struct q2_sim *sim, struct q2_creature_world *world);
void q2_zone_history_resume(q2_zone_history *h, const char *map, int zone,
                             struct q2_sim *sim, struct q2_creature_world *world,
                             q2_actor *actors, q2_zone_animation *animation,
                             u32 *phase_us);
const q2_zone_object *q2_zone_saved_entity(const q2_zone_history *h,
                                           const char *map, int zone, u32 slot);
bool q2_zone_apply_entity(const q2_zone_object *o, struct q2_sim *sim, u32 slot,
                           const q2_model_bank *zone_bank);

/* Shared with the native save's existing AI scalar/move codec. */
bool q2_zone_runtime_valid(u8 kind, const q2_zone_runtime *r);
bool q2_zone_capture_monster(q2_zone_runtime *r,
                              const struct q2_creature_world *w,
                              const q2_monster *m);
bool q2_zone_apply_monster(const q2_zone_runtime *r,
                            struct q2_creature_world *w, q2_monster *m);
q2_zone_ref q2_zone_ref_capture(const struct q2_creature_world *w,
                                const q2_monster *m);
q2_monster *q2_zone_ref_resolve(struct q2_creature_world *w, q2_zone_ref ref);
#endif
