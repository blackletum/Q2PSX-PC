/* LevelBin module behaviour the port reproduces by hand, checked through the
 * real loader: the help-computer briefings the modules raise from init and
 * zone-entry callbacks, BASE2's drawbridge box and the QENDMIS uplink screen. */
int q2_test_levelbin_client_main(int argc, char **argv);
#define main q2_test_levelbin_client_main
#include "../src/client/main.c"
#undef main

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} } while (0)
#include "client_fixture.h"

static bool gate(client *c, int zone)
{
    c->carry_player = c->carry_same_map = true;
    return client_load_zone(c, c->map, zone);
}

/* A LOADMAP from another map carrying `keys` (a full load, init included). */
static bool arrive(client *c, const char *map, int zone, u32 keys)
{
    c->carry_player = true;
    c->carry_same_map = false;
    /* The load takes the carry from the live inventory (client_load_zone). */
    c->sim[0].combat.inv.flags = (c->sim[0].combat.inv.flags & ~0x0FFFu) | keys;
    return client_load_zone(c, map, zone);
}

static bool pending(const client *c, const char *orders, s32 delay)
{
    return c->native_popup_pending && !strcmp(c->native_popup_orders, orders) &&
           c->native_popup_delay == delay;
}

static void test_briefings(const char *disc)
{
    client *c = open_client(disc);
    const u32 red = Q2_KEY_PYRAMID_RED, purple = Q2_KEY_PYRAMID_PURPLE;

    /* JAIL2 +0xCC: every zone-0 entry, delay 10, no latch. */
    CHECK(client_load_zone(c, "JAIL2", 0));
    CHECK(pending(c, "Unit2Curr1", 10));
    CHECK(!strcmp(c->native_popup_objective, "Unit2Miss1"));
    c->native_popup_pending = false;
    CHECK(gate(c, 1));
    CHECK(!c->native_popup_pending);
    CHECK(gate(c, 0));
    CHECK(pending(c, "Unit2Curr1", 10));
    c->native_popup_pending = false;

    /* JAIL3 +0x13C: one-shot on unit word +48 bit 0, which persists. */
    c->native_mission.flags = 0;
    CHECK(arrive(c, "JAIL3", 0, 0));
    CHECK(pending(c, "Unit2Curr2", 50));
    CHECK(c->native_mission.flags & Q2_NATIVE_JAIL3_BRIEFED);
    CHECK(c->jail3_prev_zone == 0);
    c->native_popup_pending = false;
    CHECK(arrive(c, "JAIL3", 0, 0));
    CHECK(!c->native_popup_pending);

    /* Zones 4/5 only on the first callback since init (+0x1218 == -1). */
    CHECK(arrive(c, "JAIL3", 4, 0));
    CHECK(pending(c, "Unit2Curr2", 100));
    CHECK(c->jail3_prev_zone == 4);
    c->native_popup_pending = false;
    CHECK(gate(c, 3) && c->jail3_prev_zone == 3);
    CHECK(gate(c, 4));
    CHECK(!c->native_popup_pending && c->jail3_prev_zone == 4);
    c->native_mission.flags |= Q2_NATIVE_JAIL3_KEYS_SEEN;
    CHECK(arrive(c, "JAIL3", 4, 0));
    CHECK(!c->native_popup_pending);
    CHECK(arrive(c, "JAIL3", 4, red));
    CHECK(pending(c, "Unit2Curr3", 50));
    CHECK(arrive(c, "JAIL3", 4, red | purple));
    CHECK(pending(c, "Unit2Curr7", 50));
    CHECK(arrive(c, "JAIL3", 5, purple));
    CHECK(pending(c, "Unit2Curr6", 50));
    CHECK(arrive(c, "JAIL3", 5, red));
    CHECK(!c->native_popup_pending || !strcmp(c->native_popup_orders, "Unit2Curr6"));
    c->native_popup_pending = false;
    CHECK(arrive(c, "JAIL3", 5, red));
    CHECK(!c->native_popup_pending);       /* bit 1 set: no Unit2Curr2 either */
    c->native_mission.flags &= ~Q2_NATIVE_JAIL3_KEYS_SEEN;
    CHECK(arrive(c, "JAIL3", 5, red));
    CHECK(pending(c, "Unit2Curr2", 100));
    c->native_popup_pending = false;

    /* SECURITY +0x16B0: one-shot on bit 2. */
    CHECK(arrive(c, "SECURITY", 0, 0));
    CHECK(pending(c, "Unit2Curr8", 40));
    CHECK(c->native_mission.flags & Q2_NATIVE_SECURITY_BRIEFED);
    c->native_popup_pending = false;
    CHECK(arrive(c, "SECURITY", 0, 0));
    CHECK(!c->native_popup_pending);

    /* BOSS1/BOSS2 init: every full load, not a gate. */
    CHECK(arrive(c, "BOSS1", 0, 0));
    CHECK(pending(c, "Unit5Curr1", 40));
    c->native_popup_pending = false;
    CHECK(gate(c, 1));
    CHECK(!c->native_popup_pending);
    CHECK(arrive(c, "BOSS2", 0, 0));
    CHECK(pending(c, "Unit5Curr2", 50));
    CHECK(!strcmp(c->native_popup_objective, "Unit5Miss1"));

    /* The raise lands on the first logic tick, with the level's own text. */
    {
        q2_input in;
        memset(&in, 0, sizeof(in));
        c->popup.visible = false;
        c->popup.delay_ticks = 0;
        client_native_popup_dt(c, c->native_popup_orders,
                               c->native_popup_objective, c->native_popup_delay, 10);
        c->native_popup_pending = false;
        CHECK(c->popup.delay_ticks == 500);
        CHECK(!strcmp(c->popup.orders_key, "Unit5Curr2"));
        CHECK(c->leveltext_ready && !strcmp(c->popup.orders,
              q2_leveltext_find(&c->leveltext, "Unit5Curr2")));
        (void)in;
    }
    close_client(c);
}

static s32 settle(client *c, s32 x, s32 feet_y, s32 z)
{
    q2_player *p = &c->sim[0].player[0];
    q2_input in;
    int i;
    memset(&in, 0, sizeof(in));
    p->pos[0] = x; p->pos[1] = feet_y; p->pos[2] = z;
    memset(p->vel, 0, sizeof(p->vel));
    p->ent.pos[0] = x; p->ent.pos[2] = z;
    p->ent.pos[1] = q2_sim_origin_y(feet_y);
    p->ent.node = q2_coll_find_node(&c->sim[0].coll, p->ent.pos, -1, true);
    p->on_ground = false;
    for (i = 0; i < 90; i++) q2_sim_advance(&c->sim[0], &in, 1.0 / 30.0);
    return p->pos[1];
}

static bool has_bridge_box(const client *c)
{
    u32 i;
    for (i = 0; i < c->sim[0].volume_count; i++) {
        const q2_move_target *t = &c->sim[0].volumes[i];
        if (t->active && t->min[0] == -5063 && t->min[1] == 100 && t->min[2] == 7039 &&
            t->max[0] == -3918 && t->max[1] == 304 && t->max[2] == 9530) return true;
    }
    return false;
}

static void test_base2_bridge(const char *disc)
{
    client *c = open_client(disc);
    s32 open_y, closed_y;
    CHECK(client_load_zone(c, "BASE2", 2));
    CHECK(!has_bridge_box(c));
    closed_y = settle(c, -4490, -200, 8285);
    CHECK(client_load_zone(c, "BASE2", 1));
    CHECK(!has_bridge_box(c));
    CHECK(q2_event_rt_trigger_named(&c->sim[0].event_rt, "EDrawbridge"));
    q2_event_rt_update(&c->sim[0].event_rt);
    CHECK(gate(c, 2));
    CHECK(has_bridge_box(c));
    open_y = settle(c, -4490, -200, 8285);
    printf("BASE2 bridge walk: feet y %d before EDrawbridge, %d after (box top 100)\n",
           (int)closed_y, (int)open_y);
    CHECK(open_y <= 100 && open_y > -200);
    CHECK(closed_y > 304);
    CHECK(gate(c, 1));
    CHECK(has_bridge_box(c));
    CHECK(gate(c, 0));
    CHECK(!has_bridge_box(c));
    close_client(c);
}

/* Count module frames from the open to the exit, pressing fire on `press`. */
static u32 run_uplink(client *c, u32 press)
{
    u32 n;
    for (n = 1; n < 2000; n++)
        if (client_uplink_step(c, n == press)) return n;
    return 0;
}

static void test_uplink(const char *disc)
{
    client *c = open_client(disc);
    const dat_chunk *lb;
    int unit;

    for (unit = 1; unit <= 5; unit++) {
        char map[16];
        snprintf(map, sizeof(map), "QENDMIS%d", unit);
        CHECK(client_load_zone(c, map, 0));
        CHECK(c->uplink_open && c->uplink_unit == unit && c->uplink_state == 0);
        lb = c->common.chunk[Q2_COMMON_LEVEL_BIN];
        CHECK(lb && lb->size == Q2_UPLINK_MODULE_SIZE);
        if (!lb) break;
        /* Init's two LoadImages: the unit's tint at (0,256), the globe at
         * (0,257). */
        CHECK(c->vram->px[256][0] == q2_rd_u16(lb->data + 0x52C8 + 0x20 * (unit - 1)));
        CHECK(c->vram->px[256][15] == q2_rd_u16(lb->data + 0x52C8 + 0x20 * (unit - 1) + 30));
        CHECK(c->vram->px[257][5] == q2_rd_u16(lb->data + 0x52A8 + 10));
    }
    CHECK(client_load_zone(c, "QENDMIS1", 0));
    CHECK(!strcmp(c->uplink_text[0], "establishing uplink...."));
    CHECK(!strcmp(c->uplink_text[1], "uplink established...."));
    CHECK(!strcmp(c->uplink_text[2], "scanning area...."));
    CHECK(!strcmp(c->uplink_text[3], "closing uplink...."));

    /* Unpressed: 1 + 91 + 1 + 65 + 1 + 20 + 240 + 61 + 1 frames. */
    CHECK(run_uplink(c, 0) == 481);
    /* The fade-out has had 62 of its 64 steps when state 8 exits. */
    CHECK(c->uplink_scroll == 240 && c->uplink_fade == 4 && c->uplink_fade_out);

    /* Brightness: 2 on state 2's frame (frame 93), 4 drawn on the next. */
    CHECK(client_load_zone(c, "QENDMIS2", 0));
    {
        u32 n;
        for (n = 1; n <= 93; n++) CHECK(!client_uplink_step(c, false));
        CHECK(c->uplink_state == 3 && c->uplink_fade == 2 && c->uplink_wait == 64);
        CHECK(!client_uplink_step(c, false) && c->uplink_fade == 4);
        for (n = 95; n <= 158; n++) client_uplink_step(c, false);
        CHECK(c->uplink_fade == 128);
    }

    /* Fire is refused while ChrisCount < 31, then ends the screen at once. */
    CHECK(client_load_zone(c, "QENDMIS3", 0));
    CHECK(run_uplink(c, 30) == 481);
    CHECK(client_load_zone(c, "QENDMIS3", 0));
    CHECK(run_uplink(c, 31) == 31);
    close_client(c);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--headless")) {
        test_briefings(argv[2]);
        test_base2_bridge(argv[2]);
        test_uplink(argv[2]);
    }
    else if (argc != 1) return 2;
    printf("%d levelbin checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
