/*
 * selfplay.h — the attract demo: the title screen's idle, the three recorded
 * levels, and the pad recording that plays player one through them.
 *
 * ---------------------------------------------------------------------------
 * The chain, end to end
 * ---------------------------------------------------------------------------
 * Addresses are PAL (SLES-01534) unless marked; the US build (SLUS-00757) runs
 * the same code four bytes lower through this stretch of the executable, and
 * MAIN.SYM names every function in it.
 *
 *  1. THE TITLE'S IDLE. QFRONT's title builder (module+0xCEE0) parks 9000 in
 *     module+0x12DC0 (US +0x12DD8) and installs module+0xC6AC as the page hook.
 *     The hook resets the store to 9000 whenever engine+0x2AC — player one's
 *     pad word, copied there by DoFrontEndStuff (0x80019BDC) — is non-zero, and
 *     otherwise subtracts the frame delta (engine+0xD4, &NewVcounts). At zero it
 *     calls 0x80101B08. 9000 of the console's 1/300 s units is thirty seconds.
 *
 *  2. DEMO OF GAME. 0x80101B08 builds module page 19 with the one-row table at
 *     module+0xEBC4 (US +0xEBD8) — `DEMO OF GAME` at (256, 111), (256, 107) on
 *     the 240-line build, the same place once the layout is centred — hides
 *     the five title objects, calls FrontEndResetPlayer/Sound/Video/
 *     MultiplayerDefaults (engine+0x1F0..+0x1FC), Config_InstallVideo
 *     (+0x3E8) and PatchEngineVariables(0) (+0x390), and arms the engine's
 *     delayed state change: engine+0x2C2 = 10, engine+0x2C0 = 12. 0x8001F964
 *     counts the twelve FRAMES down and writes 10 into LevelComplete on the
 *     thirteenth.
 *
 *  3. STATE 10 is GameSelfPlay (0x80041300, US 0x800412FC; MainGameLoop's
 *     dispatch at 0x800186E8). Off the magazine-demo arm it walks
 *     SelfPlayMapNamePntr (gp+712) through the 12-byte names at 0x8009D228
 *     (US 0x8009D47C) — Base3, Jail2, Waste2, then a zero entry that wraps it
 *     back — copying the current one into DirectAccessLevelName and
 *     `SelfPlay0` into DirectAccessZoneName, the START POSITION the level is
 *     entered at, which all three maps carry. NewMission and NextMission are
 *     cleared and InSelfPlayMode (0x800B2A24, US 0x800B2C94) is set. InitLevel
 *     then seeds srand(13) (0x80070E34).
 *
 *  4. THE LEVEL'S OWN INIT does the rest, through the engine vtable. All three
 *     modules carry the same block, gated on *engine+0x78 (&InSelfPlayMode):
 *     a loadout written into the player's client record and entity, skill 1
 *     into engine+0x366, SelfPlayOn(0, module+REC, QuakeEndSelfPlay) through
 *     +0x58, NoInputTime (*engine+0x6C) = 900 and
 *     SetNoInputTimeOutRoutine(QuakeNextDemoLevel) through +0x70. REC is
 *     +0x8B4 on BASE3, +0x788 on JAIL2 and +0x7F4 on WASTE2 — on both discs,
 *     though the recordings themselves differ (`q2_selfplay_scan`).
 *
 *  5. PLAYBACK is SwapButtons (0x800323EC, US 0x800323E8), called for each
 *     player by SwapAllButtons (0x80033030) after MainGameLoop has worked out
 *     the frame's delta. Mode 1 (+0x1C of the pad record at GameWindows+0x2AC,
 *     0x800D5EDC, stride 0x310) forces the pad type to 4 — a digital pad —
 *     and reads one halfword per frame:
 *
 *         0xFFFF            the end of the recording
 *         word & 0xFFF0     the pad's buttons, active high (pad.h); the four
 *                           bits it drops are SELECT, L3, R3 and START
 *         word & 0x000F     the frame's length in VIDEO FIELDS, in a scrambled
 *                           order: bit 3 is 1, bit 0 is 2, bit 1 is 4 and
 *                           bit 2 is 8 (0x8003271C..0x800327FC)
 *
 *     For player one the field count replaces NewVcounts — times PalNtscRate,
 *     6 on PAL and 5 on NTSC — so the recording sets the clock as well as the
 *     pad, and it does so after MainGameLoop's 30-unit clamp (0x800184B8). The
 *     word is consumed and the cursor moves on by two bytes. Nothing tests the
 *     end pointer SelfPlayOn stores at +0x2C.
 *
 *  6. THE END is tested every frame of playback (0x80032824..0x800328AC): the
 *     terminator, ANY button on the real pad (+0x04), or — in single player —
 *     player one's entity at health 0 or below. Any of them zeroes the frame's
 *     buttons, NoInputTime and InSelfPlayMode and puts the record in mode 3.
 *     The last thing SwapAllButtons does is the no-input test
 *     (0x80033058): ChrisCount >= LastInputTime + NoInputTime calls the
 *     routine. With NoInputTime zeroed that is immediate, and the routine is
 *     QuakeNextDemoLevel (0x80040F5C), which sets LevelComplete to 6 — 8 on the
 *     magazine demo — and state 6 is ProcessGame's loop back to GameTitle
 *     (0x80018CC8): the front end. With NoInputTime at 900 the same test
 *     would also end a demo after 900 frames without a button, which no
 *     recording on either disc comes near.
 *
 *     So the next level does not follow the last one directly: every demo
 *     returns to the title, and the title's own thirty seconds start the next
 *     one, because SelfPlayMapNamePntr was left pointing at it.
 *
 *  7. THE OVERLAY is SpriteAnim(1) (0x80038820), called by the frame at
 *     0x80038F8C whenever InSelfPlayMode is set and no loading hook is up. A
 *     free-running halfword at gp+614 advances by NewVcounts / PalNtscRate —
 *     one step a field — and wraps at 96; steps 0..47 come from texture slot
 *     12 (DemoSp2.lbm, x = 832) and 48..95 from slot 11 (DemoSp1.lbm, x =
 *     768), each a 4 x 12 grid of 64 x 20 cells, u = (c & 3) * 64 and
 *     v = (c' >> 2) * 20. One 64 x 20 SPRT at (224, 20), through the bank's
 *     palette 79 (0x800E3FCA), 4-bit, in the overlay's OT entry. The counter
 *     is never reset.
 *
 * The magazine-demo arm (GameRunningMode, 0x800B2AA8) and the record path
 * (SelfRecordOn, mode 2) exist in the executable and are not reached on a
 * retail disc; they are not modelled.
 */
#ifndef Q2PSX_SELFPLAY_H
#define Q2PSX_SELFPLAY_H

#include "q2psx.h"

/* module+0x12DC0's reload value at 0x8010C6C8, in 1/300 s units. */
#define Q2_SELFPLAY_TITLE_IDLE        9000

/* engine+0x2C0 at 0x80101C8C: frames 0x8001F964 counts before the state. */
#define Q2_SELFPLAY_HANDOFF_FRAMES    12

/* engine+0x2C2 at 0x80101C80, and QuakeNextDemoLevel's answer. */
#define Q2_SELFPLAY_STATE_SELFPLAY    10
#define Q2_SELFPLAY_STATE_TITLE        6

/* InitLevel's srand argument while InSelfPlayMode is set (0x80070E34). */
#define Q2_SELFPLAY_SEED              13

/* DirectAccessZoneName, written by GameSelfPlay (0x80041440). */
#define Q2_SELFPLAY_ARRIVAL           "SelfPlay0"

#define Q2_SELFPLAY_LEVELS             3
#define Q2_SELFPLAY_END_WORD      0xFFFFu
#define Q2_SELFPLAY_BUTTON_MASK   0xFFF0u

/* SpriteAnim(1), 0x80038920..0x80038A0C. */
#define Q2_SELFPLAY_OVERLAY_X         224
#define Q2_SELFPLAY_OVERLAY_Y          20
#define Q2_SELFPLAY_OVERLAY_W          64
#define Q2_SELFPLAY_OVERLAY_H          20
#define Q2_SELFPLAY_OVERLAY_STEPS      96
#define Q2_SELFPLAY_OVERLAY_HALF       48
#define Q2_SELFPLAY_OVERLAY_PALETTE    79
#define Q2_SELFPLAY_OVERLAY_SLOT_A     12   /* steps  0..47, x = 832 */
#define Q2_SELFPLAY_OVERLAY_SLOT_B     11   /* steps 48..95, x = 768 */
#define Q2_SELFPLAY_OVERLAY_PAGE_Y    256

/*
 * GameSelfPlay's table, 0x8009D228 (US 0x8009D47C): twelve-byte LEVEL TABLE
 * display names, resolved to a directory the way any level request is.
 */
const char *q2_selfplay_level(u32 index);

/*
 * GameSelfPlay's walk over SelfPlayMapNamePntr. `*cursor` is the pointer as an
 * index (0 = Base3); it starts at 0 because the console initialises the
 * pointer to NULL and the first call points it at Base3. Returns the index to
 * play and leaves `*cursor` on the one after it, wrapping at the zero entry.
 */
u32 q2_selfplay_next(u32 *cursor);

/* One recorded halfword, read the way 0x80032704..0x800327FC reads it. */
u16 q2_selfplay_buttons(u16 word);
u32 q2_selfplay_fields(u16 word);
/* The record path's inverse (0x80032928..0x80032A0C), for tests. */
u16 q2_selfplay_encode(u16 buttons, u32 fields);

/*
 * What the level's own init asks for, read out of its LevelBin without
 * running it. `module` may be relocated or not: every operand used is an
 * immediate that survives either way.
 */
typedef struct q2_selfplay_setup {
    u32  record;          /* module offset SelfPlayOn is handed            */
    s32  no_input;        /* *engine+0x6C, frames                          */
    s32  skill;           /* engine+0x366, -1 when the block sets none     */

    u32  weapons;         /* client+0x68, the owned mask (1 << (id - 1))   */
    s16  weapon;          /* client+0x66, the selected weapon, 1-based     */
    s16  ammo[6];         /* client+0x6C.. — shells, bullets, grenades...  */
    s16  health;          /* entity+0x108                                  */
    u32  set;             /* Q2_SELFPLAY_SET_* for the fields written      */
} q2_selfplay_setup;

enum {
    Q2_SELFPLAY_SET_WEAPONS = 1u << 0,
    Q2_SELFPLAY_SET_WEAPON  = 1u << 1,
    Q2_SELFPLAY_SET_HEALTH  = 1u << 2,
    Q2_SELFPLAY_SET_AMMO0   = 1u << 3     /* ... << 3 + n for ammo[n] */
};

/* True when the module carries the self-play block: the store of
 * &InSelfPlayMode's test through engine+0x78 and a SelfPlayOn call. */
bool q2_selfplay_scan(const u8 *module, u32 size, q2_selfplay_setup *out);

/* Number of halfwords before the terminator, or 0 when there is none inside
 * the module. */
u32 q2_selfplay_length(const u8 *module, u32 size, u32 record);

typedef enum q2_selfplay_end {
    Q2_SELFPLAY_END_NONE = 0,
    Q2_SELFPLAY_END_DATA,      /* the 0xFFFF terminator                     */
    Q2_SELFPLAY_END_INPUT,     /* a button on the real pad                  */
    Q2_SELFPLAY_END_DEATH,     /* player one at health <= 0                 */
    Q2_SELFPLAY_END_IDLE       /* NoInputTime frames without a button       */
} q2_selfplay_end;

typedef struct q2_selfplay {
    const u8 *module;
    u32       size;
    u32       cursor;      /* +0x20, a module offset                        */
    bool      playing;     /* +0x1C == 1                                    */
    bool      routine;     /* NoInputTimeOutRoutine installed               */

    u32       frames;      /* ChrisCount, +1 a world frame                  */
    u32       last_input;  /* LastInputTime                                 */
    s32       no_input;    /* NoInputTime                                   */

    /* What the last frame was given. */
    u16       buttons;
    u32       fields;      /* 0 when the frame's clock was not the record's */
    q2_selfplay_end end;
} q2_selfplay;

/* SelfPlayOn plus the init's two NoInput stores. */
void q2_selfplay_start(q2_selfplay *sp, const u8 *module, u32 size,
                       const q2_selfplay_setup *setup);

/*
 * The next word's clock, in fields, without consuming it: 0 at the
 * terminator or when playback is off.
 */
u32 q2_selfplay_peek_fields(const q2_selfplay *sp);

/*
 * One frame of SwapAllButtons as it concerns player one, and the ChrisCount
 * increment of the world frame after it.
 *
 * `real` is the real pad's buttons (+0x04), `dead` the single-player health
 * test at 0x80032854..0x80032874. On return `sp->buttons` is the frame's pad
 * word and `sp->fields` the frame's length in fields when the record set it.
 * Returns true when NoInputTimeOutRoutine fires this frame — the request for
 * the title.
 */
bool q2_selfplay_frame(q2_selfplay *sp, u16 real, bool dead);

/* SpriteAnim(1)'s counter after `fields` more fields (0x8003892C..0x80038994). */
u32 q2_selfplay_overlay_step(u32 step, u32 fields);

/* The cell SpriteAnim(1) draws for counter value `step`: slot and texels. */
void q2_selfplay_overlay_cell(u32 step, u32 *slot, u8 *u, u8 *v);

#endif /* Q2PSX_SELFPLAY_H */
