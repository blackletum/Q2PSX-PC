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
 * The vocabulary — RETRACTED. The fifth byte is a PALETTE INDEX
 * ---------------------------------------------------------------------------
 * This block used to read "SOLVED — the fifth byte is the item's `effect`",
 * and everything that followed from it was wrong. The retraction is kept in
 * full, argument and all, because the argument is a good one and someone will
 * otherwise rebuild it from the same data and reach the same wrong place.
 *
 * WHAT IT CLAIMED. That the fifth byte is the item's touch-dispatch index —
 * the `effect` column of the item table at `0x8009F5CC` — and that the runtime
 * therefore resolves an icon by SCANNING for a record whose fifth byte matches.
 * Its evidence was the weapon-to-ammo table read as effect ids:
 *
 *     Shotgun, Super Shotgun   18  ->  Shells P
 *     Machinegun, Chaingun     19  ->  Bullets P
 *     Grenades, Grenade Lchr   20  ->  Grenade P
 *     Rocket Launcher          21  ->  Rockets P
 *     HyperBlaster, BFG        22  ->  Cells P
 *     Railgun                  23  ->  Slugs P
 *
 * Every weapon naming its own ammunition, six ways, is not nothing. It is also
 * not enough, and the reason is that the rival reading was never tested against
 * the machine code — only against ITSELF. "Read as rect indices the shotgun
 * gets Flame Fuel" names the cell using the effect join, which is the very
 * thing in dispute. The refutation was circular.
 *
 * WHAT THE CODE DOES. All three status-bar sub-draws index. None scans:
 *
 *     80035190  lbu  v0, 170(t0)     t0 = 0x8009C478   health, ALWAYS 170
 *     8003565C  lbu  v0, 150(a0)     a0 = 0x8009C478   the POWER SHIELD arm
 *     80035374  sll  v0, a0, 2       a0 = ammoIcon[weapon]
 *     80035378  addu v0, v0, a0                        ammo, a0 * 5
 *     80035380  addu v1, a0, t0                        &rect[a0]
 *
 * A five-byte stride on a table of five-byte records, and hard-coded multiples
 * of five for the fixed icons. There is no compare instruction against the
 * fifth byte anywhere in any of them.
 *
 * "ALWAYS" holds for health and NOT for armour, which is a second correction
 * layered on this one. 0x8003565C is one of five arms of a select on the
 * inventory's flag word, and 150 is only the arm the POWER SHIELD bit takes;
 * the other four read 130, 135, 140 and 0 for body, combat, jacket and none.
 * See the block in statusbar.h. Reading it as unconditional put a power shield
 * on the bar for every armoured player — the same error as the one below in a
 * different field: an instruction correctly read, and a premise about its
 * reachability never tested.
 *
 * WHAT THE FIFTH BYTE IS. The sub-draws store it into byte +8 of a field
 * record (0x80035210, 0x8003540C), and byte +8 is the field's PALETTE index
 * into the built-in bank — the same slot a counter running low takes 7 in
 * (0x8003524C). Reading the three that the bar actually selects settles it:
 * 8 is a pale cyan ramp, 38 a blue one, 7 a red one, which are the numerals,
 * the health cross and the low-value flash. See statusbar.h.
 *
 * WHY THE JOIN LOOKED SO GOOD. Both sequences run near-monotonically over the
 * grid, so palette indices and effect ids stay a small constant apart across
 * long stretches, and any window of them lines up. Two checks break the tie
 * without appealing to either reading: under the effect reading the cell
 * assigned to the power shield is EMPTY, and under the index reading the six
 * cells the ammo table selects are six ammo boxes. Look at the sheet.
 *
 * WHAT IT COST. `q2_sbar_icon_field` asked for effect 34 to get health's
 * cross. The scan returns rect 30 — whose fifth byte is 34 — and rect 30 is
 * the POWER SHIELD, so the health field drew a red-lamp device for as long as
 * the bar has existed. It looked like a mis-picked icon rather than a mis-read
 * table because the wrong answer was still a real icon.
 *
 * `q2_icon_rect_for_id` is kept: it is a useful lookup for tooling that wants
 * to ask "which rect carries palette N". It is NOT what the runtime does, and
 * a renderer reaching for it is almost certainly making this mistake again —
 * use `q2_icon_rect_get` with an index.
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
    /*
     * The record's fifth byte: the PALETTE INDEX this sprite is drawn with,
     * into the built-in bank (hudtables.h). Named `id` from when its meaning
     * was unread; kept because renaming it would silently retarget every
     * caller. NOT an item effect id — see the retraction at the top.
     */
    u8 id;
} q2_icon_rect;

typedef struct q2_icon_tables {
    q2_buf       exe;                      /* owns the executable image */

    q2_icon_rect rect[Q2_ICON_COUNT];
    u32          rect_count;

    /*
     * Indexed by the live weapon id. `ammo_icon` is a rect INDEX — the ammo
     * sub-draw multiplies it by five and adds the rect base (0x80035374), so
     * it is fed to q2_icon_rect_get and never to the effect-id scan.
     * `ammo_kind` picks which of the player's six ammo counters is read.
     */
    u8           ammo_icon[Q2_ICON_WEAPONS];
    u8           ammo_kind[Q2_ICON_WEAPONS];
} q2_icon_tables;

q2_result q2_icon_tables_load(q2_icon_tables *out, const disc *d,
                              const q2_build_id *id);
void      q2_icon_tables_free(q2_icon_tables *t);

/* The rect for `index`, or NULL. */
const q2_icon_rect *q2_icon_rect_get(const q2_icon_tables *t, u32 index);

/*
 * The first rect whose fifth byte equals `id`, or NULL. `index_out` receives
 * the rect index when it is not NULL.
 *
 * NOT WHAT THE RUNTIME DOES. Every path in the console indexes this table;
 * nothing scans it. This is a reverse lookup for tooling — "which rect uses
 * palette N" — and a renderer calling it is very likely repeating the mistake
 * the header's retraction describes. Draw with q2_icon_rect_get.
 */
const q2_icon_rect *q2_icon_rect_for_id(const q2_icon_tables *t, u8 effect,
                                        u32 *index_out);

/*
 * The ammo icon a weapon shows, as a RECT INDEX — feed it to
 * `q2_icon_rect_get`. Weapon 0 and the blaster both give 0, which is the 1 x 1
 * blank, so a weapon with no ammunition draws no icon. That is the table's own
 * doing, not a guard here.
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
