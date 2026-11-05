/*
 * The attract loop (selfplay.h), through the real client.
 *
 * Without a disc: the recording's word format, GameSelfPlay's walk, the
 * overlay's cell map and the end-of-playback rules over synthetic words.
 *
 * `--headless <disc> [out]`: every recording on the disc decoded against
 * native executions of SelfPlayOn / SwapAllButtons (fixtures/
 * selfplay_reference.h), the init block read out of all three modules, and
 * then the loop itself — thirty seconds of title, DEMO OF GAME's twelve
 * frames, the level loaded at SelfPlay0 with the module's loadout, the
 * record driving player one the same way on every run, a key press back to
 * the title, and the three levels in their order. With `out`, the DEMO OF
 * GAME page, a self-play frame with its overlay and the title after the exit
 * are written there as PPM.
 */
int q2_test_selfplay_client_main(int argc, char **argv);
#define main q2_test_selfplay_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} } while (0)
#include "client_fixture.h"
#include "fixtures/selfplay_reference.h"

#define FRAME (1.0f / 30.0f)

static u32 fnv(u32 h, u32 v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 0x01000193u;
    }
    return h;
}

/* ------------------------------------------------------------------------- */
/* No disc                                                                    */
/* ------------------------------------------------------------------------- */

static void put_words(u8 *buf, const u16 *w, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        buf[2 * i]     = (u8)(w[i] & 0xFF);
        buf[2 * i + 1] = (u8)(w[i] >> 8);
    }
}

static void test_format(void)
{
    u32 f, cursor = 0;
    u32 slot;
    u8 u, v;

    /* 0x8003271C..0x800327FC: bit 3 one field, bit 0 two, bit 1 four,
     * bit 2 eight — and the record path is its inverse. */
    CHECK(q2_selfplay_fields(0x0011) == 2);
    CHECK(q2_selfplay_fields(0x001A) == 5);
    CHECK(q2_selfplay_fields(0x0019) == 3);
    CHECK(q2_selfplay_fields(0x0008) == 1);
    CHECK(q2_selfplay_fields(0x0004) == 8);
    CHECK(q2_selfplay_buttons(0x4211) == 0x4210);
    for (f = 0; f < 16; f++) {
        u16 w = q2_selfplay_encode(Q2_PAD_CROSS | Q2_PAD_UP, f);

        CHECK(q2_selfplay_fields(w) == f);
        CHECK(q2_selfplay_buttons(w) == (Q2_PAD_CROSS | Q2_PAD_UP));
    }

    /* GameSelfPlay's table and its wrap at the zero entry. */
    CHECK(!strcmp(q2_selfplay_level(0), "Base3"));
    CHECK(!strcmp(q2_selfplay_level(1), "Jail2"));
    CHECK(!strcmp(q2_selfplay_level(2), "Waste2"));
    CHECK(q2_selfplay_level(3) == NULL);
    CHECK(q2_selfplay_next(&cursor) == 0 && cursor == 1);
    CHECK(q2_selfplay_next(&cursor) == 1 && cursor == 2);
    CHECK(q2_selfplay_next(&cursor) == 2 && cursor == 0);
    CHECK(q2_selfplay_next(&cursor) == 0);

    /* SpriteAnim(1): slot 12 for 0..47, slot 11 for 48..95, 4 x 12 cells. */
    q2_selfplay_overlay_cell(0, &slot, &u, &v);
    CHECK(slot == 12 && u == 0 && v == 0);
    q2_selfplay_overlay_cell(5, &slot, &u, &v);
    CHECK(slot == 12 && u == 64 && v == 20);
    q2_selfplay_overlay_cell(47, &slot, &u, &v);
    CHECK(slot == 12 && u == 192 && v == 220);
    q2_selfplay_overlay_cell(48, &slot, &u, &v);
    CHECK(slot == 11 && u == 0 && v == 0);
    q2_selfplay_overlay_cell(95, &slot, &u, &v);
    CHECK(slot == 11 && u == 192 && v == 220);
    CHECK(q2_selfplay_overlay_step(90, 7) == 1);
    CHECK(q2_selfplay_overlay_step(0, 2) == 2);
}

static void test_rules(void)
{
    static const u16 words[] = { 0x0011, 0x4019, 0x0001, 0xFFFF };
    u8 mod[64];
    q2_selfplay_setup s;
    q2_selfplay sp;
    int i;

    memset(mod, 0, sizeof(mod));
    put_words(mod + 16, words, 4);
    memset(&s, 0, sizeof(s));
    s.record = 16;
    s.no_input = 900;

    /* The terminator: the frame after the last word ends it and fires. */
    q2_selfplay_start(&sp, mod, sizeof(mod), &s);
    CHECK(q2_selfplay_peek_fields(&sp) == 2);
    CHECK(!q2_selfplay_frame(&sp, 0, false));
    CHECK(sp.buttons == 0x0010 && sp.fields == 2);
    CHECK(!q2_selfplay_frame(&sp, 0, false));
    CHECK(sp.buttons == 0x4010 && sp.fields == 3);
    CHECK(!q2_selfplay_frame(&sp, 0, false));
    CHECK(sp.buttons == 0 && sp.fields == 2);
    CHECK(q2_selfplay_peek_fields(&sp) == 0);
    CHECK(q2_selfplay_frame(&sp, 0, false));
    CHECK(sp.end == Q2_SELFPLAY_END_DATA && !sp.playing && sp.no_input == 0);
    CHECK(sp.cursor == 16 + 6);

    /* A real press consumes the frame's word and then ends it. */
    q2_selfplay_start(&sp, mod, sizeof(mod), &s);
    CHECK(!q2_selfplay_frame(&sp, 0, false));
    CHECK(q2_selfplay_frame(&sp, Q2_PAD_START, false));
    CHECK(sp.end == Q2_SELFPLAY_END_INPUT && sp.buttons == 0 && !sp.playing);
    CHECK(sp.cursor == 16 + 4);

    /* So does a dead player. */
    q2_selfplay_start(&sp, mod, sizeof(mod), &s);
    CHECK(q2_selfplay_frame(&sp, 0, true));
    CHECK(sp.end == Q2_SELFPLAY_END_DEATH);

    /* NoInputTime: ChrisCount >= LastInputTime + NoInputTime fires, and the
     * record stays in mode 1 — it is the level going that ends it. */
    {
        static const u16 idle[] = { 0x0011, 0x0001, 0x0001, 0x0001, 0x0001,
                                    0xFFFF };

        put_words(mod + 16, idle, 6);
        s.no_input = 3;
        q2_selfplay_start(&sp, mod, sizeof(mod), &s);
        for (i = 0; i < 3; i++)                        /* frames 0..2 */
            CHECK(!q2_selfplay_frame(&sp, 0, false));
        CHECK(sp.last_input == 0);
        CHECK(q2_selfplay_frame(&sp, 0, false));       /* 3 >= 0 + 3 */
        CHECK(sp.end == Q2_SELFPLAY_END_IDLE && sp.playing);
    }
}

/* ------------------------------------------------------------------------- */
/* The disc's recordings against native execution                             */
/* ------------------------------------------------------------------------- */

static bool open_levelbin(const disc *d, const char *map, q2_common_file *cf,
                          const dat_chunk **lb)
{
    char path[96];
    q2_buf buf;

    snprintf(path, sizeof(path), "Q2DATA/LEVELS/%s/COMMON.DAT", map);
    if (disc_read_file(d, path, &buf) != Q2_OK)
        return false;
    if (q2_common_open(cf, &buf) != Q2_OK) {
        q2_buf_free(&buf);
        return false;
    }
    *lb = cf->chunk[Q2_COMMON_LEVEL_BIN];
    return *lb && (*lb)->data && (*lb)->size;
}

/* SwapAllButtons' frame, as the fixture runs it: NewVcounts is two fields
 * unless the record set it. */
static void play_native(const native_sp_run *ref, const u8 *data, u32 size,
                        const q2_selfplay_setup *s, s32 rate)
{
    q2_selfplay sp;
    u32 h = 0x811C9DC5u, units = 0, pressed = 0, frames = 0, cp = 0;
    bool fired = false;

    q2_selfplay_start(&sp, data, size, s);
    while (frames < 4000) {
        u32 newv;

        if (q2_selfplay_frame(&sp, 0, false)) {
            fired = true;
            break;
        }
        newv = sp.fields ? sp.fields * (u32)rate : 2u * (u32)rate;
        h = fnv(fnv(h, sp.buttons), newv);
        units += newv;
        pressed += sp.buttons != 0;
        frames++;
        if (frames % NATIVE_SP_CHECKPOINT == 0) {
            CHECK(cp < ref->checkpoint_count &&
                  h == ref->checkpoints[cp]);
            cp++;
        }
    }
    CHECK(fired);
    CHECK(frames == ref->frames);
    CHECK(h == ref->hash);
    CHECK(units == ref->units);
    CHECK(pressed == ref->pressed);
    CHECK(cp == ref->checkpoint_count);
    CHECK(sp.cursor - s->record == ref->cursor);
    CHECK(sp.end == Q2_SELFPLAY_END_DATA);
}

static void test_records(const char *disc_path)
{
    disc *d = NULL;
    q2_build_id build;
    bool ntsc;
    s32 rate;
    u32 i;
    int seen = 0;

    if (disc_open(&d, disc_path) != Q2_OK || q2_identify(d, &build) != Q2_OK) {
        CHECK(!"disc");
        return;
    }
    ntsc = build.video != Q2_VIDEO_PAL;
    rate = 300 / q2_build_tick_rate(&build);
    CHECK(rate == (ntsc ? 5 : 6));

    for (i = 0; i < Q2PSX_ARRAY_COUNT(native_sp_runs); i++) {
        const native_sp_run *ref = &native_sp_runs[i];
        q2_common_file cf;
        const dat_chunk *lb;
        q2_selfplay_setup s;

        if (ref->ntsc != (int)ntsc)
            continue;
        seen++;
        if (!open_levelbin(d, ref->map, &cf, &lb)) {
            CHECK(!"LevelBin");
            continue;
        }

        /* The init block, read out of the module. */
        CHECK(q2_selfplay_scan(lb->data, lb->size, &s));
        CHECK(s.record == ref->record);
        CHECK(s.no_input == 900);
        CHECK(s.skill == 1);
        CHECK((s.set & Q2_SELFPLAY_SET_WEAPON) && s.weapon == 3);
        if (!strcmp(ref->map, "JAIL2")) {
            /* blaster, super shotgun, machinegun, grenades; 14 / 40 / 3 */
            CHECK(s.weapons == 0x2D);
            CHECK(s.ammo[0] == 14 && s.ammo[1] == 40 && s.ammo[2] == 3);
            CHECK(!(s.set & Q2_SELFPLAY_SET_HEALTH));
        } else {
            /* blaster to machinegun; 20 shells, 150 bullets, health 45 */
            CHECK(s.weapons == 0x0F);
            CHECK(s.ammo[0] == 20 && s.ammo[1] == 150);
            CHECK(!(s.set & (Q2_SELFPLAY_SET_AMMO0 << 2)));
            CHECK((s.set & Q2_SELFPLAY_SET_HEALTH) && s.health == 45);
        }
        CHECK(q2_selfplay_length(lb->data, lb->size, s.record) == ref->frames);

        play_native(ref, lb->data, lb->size, &s, rate);

        /* The three exits, on BASE3. */
        if (!strcmp(ref->map, "BASE3")) {
            u32 k;

            for (k = 0; k < Q2PSX_ARRAY_COUNT(native_sp_exits); k++) {
                const native_sp_exit *e = &native_sp_exits[k];
                q2_selfplay_setup t = s;
                q2_selfplay sp;
                u8 *copy = NULL;
                const u8 *data = lb->data;
                int f = 0;

                if (e->ntsc != (int)ntsc)
                    continue;
                if (e->kind == 3) {
                    /* NoInputTime 20 over forty zero-button words. */
                    int w;

                    copy = (u8 *)malloc(lb->size);
                    if (!copy) continue;
                    memcpy(copy, lb->data, lb->size);
                    for (w = 0; w < 40; w++) {
                        copy[t.record + 2 * w]     = 0x01;
                        copy[t.record + 2 * w + 1] = 0x00;
                    }
                    copy[t.record + 80] = 0xFF;
                    copy[t.record + 81] = 0xFF;
                    data = copy;
                    t.no_input = 20;
                }
                q2_selfplay_start(&sp, data, lb->size, &t);
                for (f = 0; f < 4000; f++) {
                    u16 real = (e->kind == 1 && f >= e->at) ? 0x40 : 0;
                    bool dead = e->kind == 2 && f >= e->at;

                    if (q2_selfplay_frame(&sp, real, dead))
                        break;
                }
                CHECK(f == e->frame);
                CHECK(sp.buttons == e->buttons);
                CHECK((sp.playing ? 1u : 3u) == e->mode);
                CHECK((u32)sp.no_input == e->no_input);
                CHECK(sp.cursor - t.record == e->cursor);
                CHECK(sp.end == (e->kind == 1 ? Q2_SELFPLAY_END_INPUT :
                                 e->kind == 2 ? Q2_SELFPLAY_END_DEATH :
                                                Q2_SELFPLAY_END_IDLE));
                free(copy);
            }
        }
        q2_common_close(&cf);
    }
    CHECK(seen == 3);
    disc_close(d);
}

/* ------------------------------------------------------------------------- */
/* The loop, through the client                                               */
/* ------------------------------------------------------------------------- */

static void shot(client *c, const char *out, const char *name)
{
    char path[512];

    if (!out)
        return;
    q2_screen_tick_dt(&c->screen, 1.0 / 30.0);
    client_frame(c);
    snprintf(path, sizeof(path), "%s/%s.ppm", out, name);
    CHECK(psx_fb_write_ppm(q2_screen_front(&c->screen), path) == Q2_OK);
}

static void spend_loading(client *c);

static client *front_end(const char *disc_path)
{
    client *c = open_client(disc_path);

    CHECK(q2_loading_open(&c->loading, c->disc, &c->hud_tables,
                          &c->settings) == Q2_OK);
    c->running = true;
    client_enter_front_end(c);
    CHECK(c->in_front_end && client_name_eq(c->map, "QFRONT"));
    CHECK(c->menu.open && c->menu.page_id == Q2_PAGE_FRONT_TITLE);
    spend_loading(c);
    return c;
}

static void finish_client(client *c)
{
    q2_loading_close(&c->loading);
    close_client(c);
}

/* Run until the loading screen's hold is spent, as the frame loop does. */
static void spend_loading(client *c)
{
    int n;

    for (n = 0; n < 200 && q2_loading_step(&c->loading, FRAME); n++)
        ;
}

/* Thirty seconds of title, then DEMO OF GAME, then state 10. */
static void reach_demo(client *c, const char *out)
{
    int n;

    for (n = 0; n < 899; n++) {
        client_attract_frame(c, FRAME, true);
        CHECK(!c->attract_armed);
    }
    client_attract_frame(c, FRAME, true);
    CHECK(c->attract_armed && c->menu.page_id == Q2_PAGE_DEMO);
    shot(c, out, "demo-of-game");

    /* 0x8001F964: twelve counted frames, and a frame the scene did not
     * take is not one of them. */
    for (n = 0; n < Q2_SELFPLAY_HANDOFF_FRAMES; n++) {
        client_attract_frame(c, FRAME, true);
        client_attract_frame(c, FRAME, false);
        CHECK(c->attract_armed && c->in_front_end);
    }
    client_attract_frame(c, FRAME, true);
    CHECK(!c->attract_armed && c->selfplay_active && !c->in_front_end);
}

/* The player's position after `ticks` record frames, and the overlay. */
static void run_record(client *c, int frames, s32 pos[3], u32 *cursor,
                       u32 *ticks)
{
    int n;

    for (n = 0; n < frames && c->selfplay_active && !c->selfplay_go_title; n++)
        client_input_simulated(c, FRAME);
    memcpy(pos, c->sim[0].player[0].pos, sizeof(s32) * 3);
    *cursor = c->selfplay.cursor;
    *ticks  = c->selfplay.frames;
}

static void test_loop(const char *disc_path, const char *out)
{
    client *c = front_end(disc_path);
    bool ntsc = c->build.video != Q2_VIDEO_PAL;
    s32 rate = client_field_units(c);
    s32 pos[3], pos2[3];
    u32 cursor, cursor2, ticks, ticks2, fields = 0, at;
    int n;

    CHECK(rate == (ntsc ? 5 : 6));

    /* The idle: any input puts the whole thirty seconds back. */
    c->settings.v[Q2_SET_PAD_STYLE] = Q2_PAD_STYLE_RIGHT_MOUSE;
    c->settings.v[Q2_SET_MUSIC] = 7;
    for (n = 0; n < 600; n++)
        client_attract_frame(c, FRAME, true);
    CHECK(c->attract_idle > 2990.0 && c->attract_idle < 3010.0);
    c->selfplay_input = true;
    client_attract_frame(c, FRAME, true);
    c->selfplay_input = false;
    CHECK(c->attract_idle == (double)Q2_SELFPLAY_TITLE_IDLE);
    /* And leaving the title page stops it; coming back re-arms it. */
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_OPTIONS);
    client_attract_frame(c, FRAME, true);
    CHECK(!c->attract_on_title);
    q2_menu_goto(&c->menu, Q2_PAGE_FRONT_TITLE);

    reach_demo(c, out);
    CHECK(client_name_eq(c->map, "BASE3"));
    CHECK(c->selfplay_level == 0 && c->attract_next == 1);
    CHECK(c->loading.open && c->loading.menu.page &&
          c->loading.menu.page->id == Q2_PAGE_DEMO);
    spend_loading(c);
    /* The defaults the recording was made on, and the module's loadout. */
    CHECK(c->settings.v[Q2_SET_PAD_STYLE] == Q2_PAD_STYLE_STANDARD_A);
    CHECK(c->sim[0].combat.inv.weapons == 0x0F);
    CHECK(c->sim[0].combat.weapon_id == 3);
    CHECK(c->sim[0].combat.inv.health == 45);
    CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_SHELLS] == 20);
    CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_BULLETS] == 150);
    CHECK(q2_cre_skill() == 1);
    CHECK(!strcmp(c->arrival, Q2_SELFPLAY_ARRIVAL));
    CHECK(c->zone_index == 1);

    /* The record drives the player: 100 rendered frames, one word a tick,
     * each tick as long as its word says. */
    {
        s32 start[3];
        u32 rec = c->selfplay_setup.record;
        const dat_chunk *lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
        u32 k, sum = 0;

        memcpy(start, c->sim[0].player[0].pos, sizeof(start));
        run_record(c, 100, pos, &cursor, &ticks);
        CHECK(c->selfplay_active && !c->selfplay_go_title);
        CHECK(ticks > 60 && ticks < 100);
        CHECK(cursor == rec + 2 * ticks);
        CHECK(pos[0] != start[0] || pos[2] != start[2]);
        for (k = 0; k < ticks; k++) {
            u16 w = (u16)(lb->data[rec + 2 * k] | (lb->data[rec + 2 * k + 1] << 8));

            sum += q2_selfplay_fields(w);
        }
        fields = sum;
        /* 100 frames of 10 units: the record's time, less what is banked. */
        CHECK(sum * (u32)rate <= 1000 && sum * (u32)rate + 30 >= 1000);
        CHECK(c->selfplay_overlay == fields % Q2_SELFPLAY_OVERLAY_STEPS);
        CHECK(c->sim[0].dt_force == (s32)c->selfplay.fields * rate ||
              c->sim[0].dt_force == -1);
    }

    /* DemoSp1/2 are resident where SpriteAnim(1) samples them, and the
     * overlay is one more primitive in the frame. */
    {
        u32 y, x, lit = 0;

        for (y = 256; y < 496; y++)
            for (x = 768; x < 896; x++)
                lit += c->vram->px[y][x] != 0;
        CHECK(lit > 1000);
    }
    shot(c, out, "selfplay");

    /* A key press: the word is consumed, playback ends, and the frame loop
     * goes back to the title with the player's settings restored. */
    at = c->selfplay.cursor;
    c->selfplay_input = true;
    client_input_simulated(c, FRAME);
    c->selfplay_input = false;
    CHECK(c->selfplay_go_title);
    CHECK(c->selfplay_last_end == Q2_SELFPLAY_END_INPUT);
    CHECK(c->selfplay.cursor == at + 2);
    client_selfplay_finish(c);
    CHECK(!c->selfplay_active && c->in_front_end);
    CHECK(client_name_eq(c->map, "QFRONT"));
    CHECK(c->menu.open && c->menu.page_id == Q2_PAGE_FRONT_TITLE);
    CHECK(c->settings.v[Q2_SET_PAD_STYLE] == Q2_PAD_STYLE_RIGHT_MOUSE);
    CHECK(c->settings.v[Q2_SET_MUSIC] == 7);
    CHECK(c->sim[0].dt_force == 0);
    spend_loading(c);
    shot(c, out, "title-after-exit");

    /* The next thirty seconds play the next level, and so on round. */
    reach_demo(c, NULL);
    CHECK(client_name_eq(c->map, "JAIL2") && c->selfplay_level == 1);
    CHECK(c->sim[0].combat.inv.weapons == 0x2D);
    CHECK(c->sim[0].combat.inv.ammo[Q2_AMMO_GRENADES] == 3);
    CHECK(c->sim[0].combat.inv.health == 100);
    client_selfplay_finish(c);
    reach_demo(c, NULL);
    CHECK(client_name_eq(c->map, "WASTE2") && c->selfplay_level == 2);
    CHECK(c->zone_index == 0);
    client_selfplay_finish(c);
    reach_demo(c, NULL);
    CHECK(client_name_eq(c->map, "BASE3") && c->selfplay_level == 0);
    CHECK(c->attract_demos == 4);

    /* The same record, the same seed (13), the same player: a second BASE3
     * from a fresh client lands where the first did. */
    spend_loading(c);
    run_record(c, 100, pos2, &cursor2, &ticks2);
    CHECK(ticks2 == ticks && cursor2 == cursor);
    CHECK(pos2[0] == pos[0] && pos2[1] == pos[1] && pos2[2] == pos[2]);
    finish_client(c);

    {
        client *d = front_end(disc_path);

        reach_demo(d, NULL);
        spend_loading(d);
        run_record(d, 100, pos2, &cursor2, &ticks2);
        CHECK(ticks2 == ticks && cursor2 == cursor);
        CHECK(pos2[0] == pos[0] && pos2[1] == pos[1] && pos2[2] == pos[2]);
        printf("%s BASE3: %u record frames in 100, player at (%d,%d,%d), "
               "overlay step %u\n", ntsc ? "US" : "PAL", ticks, pos[0], pos[1],
               pos[2], fields % Q2_SELFPLAY_OVERLAY_STEPS);

        /* Run the record out: it ends on its terminator, or earlier on the
         * player's death, and either way the request is for the title. */
        for (n = 0; n < 4000 && !d->selfplay_go_title; n++)
            client_input_simulated(d, FRAME);
        CHECK(d->selfplay_go_title);
        CHECK(d->selfplay_last_end == Q2_SELFPLAY_END_DATA ||
              d->selfplay_last_end == Q2_SELFPLAY_END_DEATH);
        printf("%s BASE3: playback over after %u frames (%s)\n",
               ntsc ? "US" : "PAL", d->selfplay.frames,
               d->selfplay_last_end == Q2_SELFPLAY_END_DATA ? "end of record"
                                                            : "player dead");

        /* The end of playback leaves player one's pad in mode 3, and nothing
         * a retail disc runs clears it: HelpComputer_NewMessage and
         * HelpComputer_DoDisplay both return on it. So a level's own
         * HelpComputer call (import +0x3D8) raises nothing in a game started
         * after a demo, and raises the board in one that was not. */
        client_selfplay_finish(d);
        CHECK(d->pad_mode == 3);
        CHECK(client_load_zone(d, "BASE0", 0));
        d->popup_raises = d->popup_muted = 0;
        d->sim[0].event_rt.initial_pass = false;
        d->restoring_save = false;
        client_native_popup(d, "Unit1Curr1", "Unit1Miss1", 5);
        CHECK(d->popup_raises == 0 && d->popup_muted == 1 && !d->popup.visible);
        CHECK(d->pad_mode == 3);
        printf("%s BASE0 after a demo: %u help-computer calls skipped, %u raised\n",
               ntsc ? "US" : "PAL", d->popup_muted, d->popup_raises);
        finish_client(d);
    }
    {
        client *e = front_end(disc_path);

        CHECK(e->pad_mode == 0);
        CHECK(client_load_zone(e, "BASE0", 0));
        e->popup_raises = e->popup_muted = 0;
        e->sim[0].event_rt.initial_pass = false;
        e->restoring_save = false;
        client_native_popup(e, "Unit1Curr1", "Unit1Miss1", 5);
        CHECK(e->popup_raises == 1 && e->popup_muted == 0);
        printf("%s BASE0 with no demo: %u raised\n", ntsc ? "US" : "PAL",
               e->popup_raises);
        finish_client(e);
    }
}

int main(int argc, char **argv)
{
    test_format();
    test_rules();
    if (argc >= 3 && !strcmp(argv[1], "--headless")) {
        test_records(argv[2]);
        test_loop(argv[2], argc >= 4 ? argv[3] : NULL);
    } else if (argc != 1) {
        return 2;
    }
    printf("%d selfplay checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
