/*
 * icontable.h — the status bar's icon sheet, and the map from a weapon to the
 * ammo icon that stands beside its counter.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists at all
 * ---------------------------------------------------------------------------
 * Until retail capture was compared against the reconstruction, this project
 * stated as a *finding* that Quake II PSX has no status bar (FORMATS.md §11.1,
 * now retracted). It has one: health, ammo and armour in large numerals beside
 * their icons, and a weapon strip along the bottom right.
 *
 * The reason the finding survived so long is worth carrying in the header of
 * the file that refutes it. §11.1 enumerated every `printf`/`sprintf` site and
 * every reader of the font table, exhaustively and correctly, and concluded
 * that nothing formats a player statistic. That is still true. The bar does not
 * format anything: it draws **pre-rendered sprites** — icons and numerals — out
 * of the sheet in VRAM slot 14 through the quad emitter at `0x80033320`, which
 * takes a rect and a position and touches neither the font nor a format string.
 * An enumeration of text sites is structurally unable to see it.
 *
 * ---------------------------------------------------------------------------
 * What IS read, and what is not
 * ---------------------------------------------------------------------------
 * READ, and checkable against the disc:
 *
 *   - the 57-entry rect table at `0x8009C478`, five bytes each, and where it
 *     ends — see the note on the count below
 *   - its geometry: a grid of 32 x 24 cells, eight per row, seven rows at
 *     v = 0, 24, 48, 72, 96, 120, 144, with the rightmost cell of each row one
 *     texel narrow
 *   - the weapon-to-ammo-icon map at `0x800ABE9C` and its companion at
 *     `0x800ABEA8`, twelve bytes each and indexed by the live weapon id, which
 *     is the same 1-based 0..11 space the weapon tables use (FORMATS.md §9.6)
 *   - the split-screen size reduction: in a two-player session an icon is drawn
 *     24 x 18 instead of 32 x 24, and at three or more 16 x 12 (`0x800353C8`
 *     onward). A weapon id of 0 collapses it to 1 x 1, which is the blank.
 *
 *   - the numerals, at `0x8009C598`: ten four-byte {u, v, w, h} records, all
 *     24 x 24 at v = 168, with u = 24 * digit. They are the row below the icon
 *     grid in the same sheet — see statusbar.h.
 *
 * ---------------------------------------------------------------------------
 * The vocabulary — SOLVED. The fifth byte is the item's `effect`
 * ---------------------------------------------------------------------------
 * Each rect record's fifth byte was recorded here as "an id whose value space
 * is unidentified". It is the **item's touch-dispatch index** — the `effect`
 * column of the item table at `0x8009F5CC` (itemtable.h), the one the pickup
 * handler takes as `effect - 2`. Joining the two names every icon.
 *
 * The proof is the weapon-to-ammo table, and it is an eleven-way coincidence
 * that nothing in the decode was arranged to produce. Read as effect ids, its
 * entries are:
 *
 *     Shotgun, Super Shotgun   18  ->  Shells P
 *     Machinegun, Chaingun     19  ->  Bullets P
 *     Grenades, Grenade Lchr   20  ->  Grenade P
 *     Rocket Launcher          21  ->  Rockets P
 *     HyperBlaster, BFG        22  ->  Cells P
 *     Railgun                  23  ->  Slugs P
 *
 * Every weapon names its own ammunition. Read instead as rect INDICES — the
 * other candidate — the same table gives the shotgun "Flame Fuel" and the
 * rocket launcher "Combat Armour", which is how that reading was ruled out.
 *
 * A second confirmation falls out of the same join: a weapon pickup's own
 * effect is its **1-based weapon slot** — Shotgun 2, Super Shotgun 3,
 * Machinegun 4, Chaingun 5, Grenade Launcher 7, Rocket Launcher 8,
 * HyperBlaster 9, Railgun 10, BFG 11 — which is the weapon numbering
 * FORMATS.md §9.6 derived from a completely different direction.
 *
 * So `q2_icon_rect_for_id` is a SCAN, not an index. That is what the runtime
 * does too, and it is why the table carries the id at all: an array indexed by
 * effect would not need one.
 *
 * Where the bar is DRAWN is now known and lives in statusbar.h: `0x800337D0` is
 * the per-viewport draw hook, not a screen.
 */
#ifndef Q2PSX_ICONTABLE_H
#define Q2PSX_ICONTABLE_H

#include "ident.h"
#include "q2psx.h"

/* Where the tables are in the catalogued PAL build. */
#define Q2_ICON_ADDR_RECTS      0x8009C478u
#define Q2_ICON_ADDR_AMMO_ICON  0x800ABE9Cu
#define Q2_ICON_ADDR_AMMO_KIND  0x800ABEA8u

/* Where the rect table stops being rectangles. Not a table this module reads —
 * recorded so the boundary is a stated fact rather than an off-by-N. */
#define Q2_ICON_ADDR_AFTER      0x8009C595u

/*
 * FIFTY-SEVEN records, and the number is measured rather than assumed.
 *
 * An earlier guess ran the table to `0x8009C658` — the next address anything
 * references — which would be 96 records. Walking it and testing each record
 * against the grid says otherwise: records 0…56 are the 1 x 1 blank plus
 * fifty-six cells across seven rows, and record 57 onward is not rectangles at
 * all (widths of zero, heights over 200). The table ends at `0x8009C595`, and
 * what lies between there and `0x8009C658` is a different structure that has
 * not been identified.
 *
 * **The last column of every row is 31 wide, not 32.** `u` is 224 there, and
 * 224 + 32 is 256, which wraps to zero in the u8 the primitive carries — so the
 * sheet's rightmost cell is deliberately one texel narrow to stay inside the
 * page. Six records look like errors and are the opposite of errors; a grid
 * check that does not know this rejects one cell per row.
 *
 * That is exactly the sort of boundary a fixed count gets wrong silently, so
 * the inspect command re-derives it from the disc and fails if the run of valid
 * records is not this long.
 */
#define Q2_ICON_COUNT       57
#define Q2_ICON_LAST_COL_W  31   /* the rightmost cell, at u = 224 */
#define Q2_ICON_RECORD      5    /* {u, v, w, h, id}                          */
#define Q2_ICON_CELL_W     32
#define Q2_ICON_CELL_H     24
#define Q2_ICON_PER_ROW     8
#define Q2_ICON_WEAPONS    12    /* the 1-based weapon id space, 0 = none     */

/* The sheet is VRAM slot 14 — `qk_menu.lbm` and its two multiplayer variants
 * (vram.h). It is 4bpp, so the whole sheet is one texture page. */
#define Q2_ICON_SLOT       14

/*
 * How big an icon is drawn, by player count (0x800353C8…0x800353E4). This is
 * the only part of split screen the bar expresses, and it is a size change
 * rather than a layout change.
 */
typedef struct q2_icon_size { u8 w, h; } q2_icon_size;

/*
 * The drawn size of a cell whose own rect is `src_w` x `src_h`.
 *
 * Single player keeps the record's own dimensions — which matters because the
 * sheet is not uniform: icons are 32 x 24 but the numerals are 24 x 24, and
 * forcing everything to the icon size stretches every digit by a third. Two
 * players force 24 x 18 and three or more 16 x 12 REGARDLESS of the source, so
 * the reduction is a clamp rather than a scale, and a numeral and an icon end
 * up the same size in split screen.
 *
 * A weapon id of 0 collapses to the 1 x 1 blank.
 */
q2_icon_size q2_icon_draw_size_of(int players, int weapon_id,
                                  u8 src_w, u8 src_h);

/* The icon case: the source is a full 32 x 24 grid cell. */
q2_icon_size q2_icon_draw_size(int players, int weapon_id);

typedef struct q2_icon_rect {
    u8 u, v, w, h;
    u8 id;          /* the record's own fifth byte; its meaning is unread */
} q2_icon_rect;

typedef struct q2_icon_tables {
    q2_buf       exe;                      /* owns the executable image */

    q2_icon_rect rect[Q2_ICON_COUNT];
    u32          rect_count;

    /* Indexed by the live weapon id. `ammo_icon` is a rect index; `ammo_kind`
     * selects which counter the bar reads, and its value space is unread. */
    u8           ammo_icon[Q2_ICON_WEAPONS];
    u8           ammo_kind[Q2_ICON_WEAPONS];
} q2_icon_tables;

q2_result q2_icon_tables_load(q2_icon_tables *out, const disc *d,
                              const q2_build_id *id);
void      q2_icon_tables_free(q2_icon_tables *t);

/* The rect for `index`, or NULL. */
const q2_icon_rect *q2_icon_rect_get(const q2_icon_tables *t, u32 index);

/*
 * The rect whose fifth byte is `effect`, or NULL. A scan, because that is what
 * the id being stored in the record rather than implied by its position means.
 * `index_out` receives the rect index when it is not NULL.
 */
const q2_icon_rect *q2_icon_rect_for_id(const q2_icon_tables *t, u8 effect,
                                        u32 *index_out);

/*
 * The ammo a weapon shows, as an item EFFECT id — feed it to
 * `q2_icon_rect_for_id`. Weapon 0, "no weapon", gives 0, which matches no
 * record, so an unarmed player's field is empty rather than reading off the
 * front of the table.
 */
u8 q2_icon_ammo_for_weapon(const q2_icon_tables *t, int weapon_id);

/* The same thing already resolved, for callers that only want to draw. */
const q2_icon_rect *q2_icon_ammo_rect(const q2_icon_tables *t, int weapon_id);

/*
 * What a rect depicts, as the item table's model name — "Shells P", "Jacket P".
 * Reads the BUILT-IN item table, so it works without a disc in hand and a
 * caller can label an icon in a tool or a test.
 *
 * Returns NULL for the blank and for the five rects whose effect no item
 * claims. Those five are not a decode failure: the sheet carries art the item
 * table has no record for, which is the same shape of finding as the eight
 * inert pickup handlers (itemtable.h).
 */
const char *q2_icon_item_name(const q2_icon_tables *t, u32 rect_index);

/* And by effect id directly. */
const char *q2_icon_name_for_id(u8 effect);

/*
 * Does `index` sit on the 32 x 24 grid the sheet is laid out on? Record 0 is
 * the deliberate exception — a 1 x 1 blank at (255, 255) — so this reports the
 * grid's own regularity rather than asserting every record obeys it.
 */
bool q2_icon_on_grid(const q2_icon_rect *r);

#endif /* Q2PSX_ICONTABLE_H */
