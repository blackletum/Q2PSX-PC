/*
 * save.h — persisting and restoring a game in progress.
 *
 * ---------------------------------------------------------------------------
 * A deliberate divergence, stated up front
 * ---------------------------------------------------------------------------
 * The original saved to a PlayStation memory card: fixed 8 KB blocks, a card
 * directory, and a whole layer of "insufficient free blocks" and "remove memory
 * card" handling. A native port has a filesystem, and reproducing block
 * management would add failure modes the host does not have in exchange for
 * nothing a player can perceive.
 *
 * So this writes an ordinary host file. That is a departure from faithfulness
 * and it is a considered one — unlike the rendering, where the hardware's
 * limits ARE the experience, a save file's container is invisible. What the
 * save CONTAINS still mirrors the original's state.
 *
 * Reading the original's card format is a separate job, worth doing so existing
 * saves can be imported. It is not this.
 *
 * ---------------------------------------------------------------------------
 * Format
 * ---------------------------------------------------------------------------
 * A small header then three sections. Everything is little-endian and
 * fixed-width, so a save written on one host loads on another.
 *
 *     magic     "Q2PS"
 *     version   bumped whenever the layout changes
 *     build     the disc's serial, so a save cannot be loaded against the
 *               wrong release — the level table and script offsets differ
 *               between builds, and a mismatched save would restore a player
 *               into the wrong map's coordinates
 *
 * The event flags are the part that matters and the part easiest to forget. A
 * save that restores position and inventory but not which triggers have already
 * fired drops the player into a level where every door they opened is shut
 * again. They are stored as one byte per record, indexed the same way the
 * runtime indexes them.
 */
#ifndef Q2PSX_SAVE_H
#define Q2PSX_SAVE_H

#include "events_rt.h"
#include "inventory.h"
#include "q2psx.h"
#include "sim.h"

#define Q2_SAVE_MAGIC   "Q2PS"
#define Q2_SAVE_VERSION 1
#define Q2_SAVE_MAP_LEN 16
#define Q2_SAVE_SERIAL_LEN 16

typedef struct q2_save {
    char serial[Q2_SAVE_SERIAL_LEN];  /* the disc this save belongs to */
    char map[Q2_SAVE_MAP_LEN];
    s32  zone;

    /* Player */
    s32  pos[3];
    s32  yaw, pitch;
    q2_inventory inventory;

    /* Script state: one flags byte per event record. Owned. */
    u8  *event_flags;
    u32  event_count;
} q2_save;

void q2_save_free(q2_save *s);

/* Capture the current state. Copies the event flags, so the runtime may change
 * afterwards without disturbing the snapshot. */
q2_result q2_save_capture(q2_save *out, const q2_sim *sim,
                          const q2_inventory *inv,
                          const char *serial, const char *map, s32 zone);

/* Apply a loaded save to a simulation. Fails if the save is for a different
 * disc, or if the map does not match — the caller must load the right map
 * first, because restoring a position into the wrong geometry is worse than
 * refusing. */
q2_result q2_save_apply(const q2_save *s, q2_sim *sim, q2_inventory *inv,
                        const char *serial, const char *map);

q2_result q2_save_write(const q2_save *s, const char *path);
q2_result q2_save_read(q2_save *out, const char *path);

#endif /* Q2PSX_SAVE_H */
