// Host tests for apps/emberward/ew_game.c: the game rules.
#include "ew_game.h"
#include "test_util.h"

#include <string.h>

// Drops every queued event, counting those of one type.
static int drain(ew_game_t *g, int type) {
    ew_event_t ev;
    int n = 0;
    while (ew_next_event(g, &ev))
        if (ev.type == type) n++;
    return n;
}

static void run_to_play(ew_game_t *g) {
    for (int i = 0; i < EW_INTRO_TICKS && g->phase == EW_PHASE_INTRO; i++) ew_step(g);
}

// A game in play that spawns nothing for a long time (and does not end the
// wave): the test places sparks itself.
static void quiet_game(ew_game_t *g) {
    ew_init(g, 7);
    run_to_play(g);
    g->to_spawn = 1;
    g->spawn_timer = 60000;
    drain(g, -1);
}

static ew_mote_t *place(ew_game_t *g, int slot, unsigned kind, unsigned lane, int r) {
    ew_mote_t *m = &g->motes[slot];
    m->active = true;
    m->kind = (uint8_t)kind;
    m->lane = (uint8_t)lane;
    m->r_q8 = EW_Q8(r);
    m->speed_q8 = EW_Q8(1);
    return m;
}

static void step_n(ew_game_t *g, int n) {
    for (int i = 0; i < n; i++) ew_step(g);
}

static void test_init(void) {
    ew_game_t g;
    ew_init(&g, 0);
    ASSERT_TRUE(g.rng != 0);
    ASSERT_EQ(g.lives, EW_LIVES_START);
    ASSERT_EQ(g.wave, 1);
    ASSERT_EQ(g.phase, EW_PHASE_INTRO);
    ASSERT_EQ(g.score, 0);
    ew_event_t ev;
    ASSERT_TRUE(ew_next_event(&g, &ev));
    ASSERT_EQ(ev.type, EW_EV_WAVE_START);
    ASSERT_EQ(ev.points, 1);
    run_to_play(&g);
    ASSERT_EQ(g.phase, EW_PHASE_PLAY);
}

static void test_wave_params_ramp(void) {
    ew_wave_params_t p1 = ew_wave_params(1), p5 = ew_wave_params(5), p30 = ew_wave_params(30);
    ASSERT_EQ(p1.count, 16);
    ASSERT_EQ(p1.dart_pct, 0);
    ASSERT_EQ(p1.heart_pct, 0);
    ASSERT_EQ(p1.pair_pct, 0);
    ASSERT_TRUE(p5.speed_q8 > p1.speed_q8);
    ASSERT_TRUE(p5.interval < p1.interval);
    ASSERT_TRUE(p5.dart_pct > 0 && p5.heart_pct > 0 && p5.pair_pct > 0);
    // Capped late in the game.
    ASSERT_EQ(p30.speed_q8, 820);
    ASSERT_EQ(p30.interval, 16);
    ASSERT_EQ(p30.dart_pct, 25);
    ASSERT_EQ(p30.pair_pct, 40);
    // Wave 0 reads as wave 1.
    ASSERT_EQ(ew_wave_params(0).count, p1.count);
}

static void test_turn_wraps(void) {
    ew_game_t g;
    ew_init(&g, 1);
    g.ward = 0;
    ew_turn(&g, -1);
    ASSERT_EQ(g.ward, 7);
    ASSERT_TRUE(ew_ward_covers(&g, 7));
    ASSERT_TRUE(ew_ward_covers(&g, 0));
    ASSERT_TRUE(!ew_ward_covers(&g, 1));
    ew_turn(&g, 4);
    ASSERT_EQ(g.ward, 3);
    ew_turn(&g, +1);
    ASSERT_EQ(g.ward, 4);
    ASSERT_EQ(drain(&g, EW_EV_TURN), 3);
}

static void test_ember_blocked(void) {
    ew_game_t g;
    quiet_game(&g);
    g.ward = 2;   // covers lanes 2 and 3
    place(&g, 0, EW_EMBER, 3, EW_R_WARD + 2);
    step_n(&g, 3);
    ASSERT_TRUE(!g.motes[0].active);
    ASSERT_EQ(g.score, 10);
    ASSERT_EQ(g.combo, 1);
    ASSERT_EQ(g.lives, EW_LIVES_START);
    ASSERT_EQ(drain(&g, EW_EV_BLOCK), 1);
}

static void test_ember_hits_core(void) {
    ew_game_t g;
    quiet_game(&g);
    g.ward = 2;
    g.combo = 9;
    place(&g, 0, EW_EMBER, 6, EW_R_WARD + 2);
    step_n(&g, 40);
    ASSERT_TRUE(!g.motes[0].active);
    ASSERT_EQ(g.lives, EW_LIVES_START - 1);
    ASSERT_EQ(g.combo, 0);
    ASSERT_EQ(g.score, 0);
    ASSERT_TRUE(g.grace > 0);
    ASSERT_EQ(drain(&g, EW_EV_HIT), 1);
}

static void test_grace_period(void) {
    ew_game_t g;
    quiet_game(&g);
    g.ward = 2;
    place(&g, 0, EW_EMBER, 6, EW_R_CORE + 1);
    place(&g, 1, EW_EMBER, 5, EW_R_CORE + 3);
    step_n(&g, 5);
    ASSERT_EQ(g.lives, EW_LIVES_START - 1);   // the second one passed in the grace period
    ew_event_t ev;
    int hits = 0, passes = 0;
    while (ew_next_event(&g, &ev)) {
        if (ev.type == EW_EV_HIT) hits++;
        if (ev.type == EW_EV_GRACE_PASS) passes++;
    }
    ASSERT_EQ(hits, 1);
    ASSERT_EQ(passes, 1);
    // After the grace period another hit costs a life again.
    step_n(&g, EW_GRACE_TICKS);
    place(&g, 0, EW_EMBER, 6, EW_R_CORE + 1);
    step_n(&g, 2);
    ASSERT_EQ(g.lives, EW_LIVES_START - 2);
}

static void test_glim_absorbed_and_shattered(void) {
    ew_game_t g;
    quiet_game(&g);
    g.ward = 2;
    place(&g, 0, EW_GLIM, 6, EW_R_WARD + 2);
    step_n(&g, 40);
    ASSERT_EQ(g.score, 25);
    ASSERT_EQ(g.flare, EW_FLARE_PER_GLIM);
    ASSERT_EQ(g.combo, 1);
    ASSERT_EQ(drain(&g, EW_EV_ABSORB), 1);

    place(&g, 0, EW_GLIM, 2, EW_R_WARD + 2);
    step_n(&g, 3);
    ASSERT_TRUE(!g.motes[0].active);
    ASSERT_EQ(g.combo, 0);
    ASSERT_EQ(g.score, 25);
    ASSERT_EQ(g.lives, EW_LIVES_START);
    ASSERT_EQ(drain(&g, EW_EV_SHATTER), 1);
}

static void test_multiplier(void) {
    ew_game_t g;
    quiet_game(&g);
    ASSERT_EQ(ew_multiplier(&g), 1);
    g.combo = 8;
    ASSERT_EQ(ew_multiplier(&g), 2);
    g.combo = 100;
    ASSERT_EQ(ew_multiplier(&g), 5);
    g.combo = 16;   // x3
    g.ward = 0;
    place(&g, 0, EW_DART, 0, EW_R_WARD + 2);
    step_n(&g, 3);
    ASSERT_EQ(g.score, 60);
    ASSERT_EQ(g.best_combo, 17);
}

static void test_heart(void) {
    ew_game_t g;
    quiet_game(&g);
    g.ward = 2;
    g.lives = 2;
    place(&g, 0, EW_HEART, 6, EW_R_CORE + 2);
    step_n(&g, 3);
    ASSERT_EQ(g.lives, 3);
    ASSERT_EQ(drain(&g, EW_EV_HEART), 1);
    g.lives = EW_LIVES_MAX;
    place(&g, 0, EW_HEART, 6, EW_R_CORE + 2);
    step_n(&g, 3);
    ASSERT_EQ(g.lives, EW_LIVES_MAX);
}

static void test_flare(void) {
    ew_game_t g;
    quiet_game(&g);
    ASSERT_TRUE(!ew_flare(&g));
    // Five glims fill the meter; the event fires once.
    g.flare = EW_FLARE_FULL - EW_FLARE_PER_GLIM;
    g.ward = 2;
    place(&g, 0, EW_GLIM, 6, EW_R_CORE + 2);
    step_n(&g, 3);
    ASSERT_EQ(g.flare, EW_FLARE_FULL);
    ASSERT_EQ(drain(&g, EW_EV_FLARE_READY), 1);
    g.combo = 0;
    place(&g, 0, EW_EMBER, 0, 150);
    place(&g, 1, EW_DART, 4, 120);
    place(&g, 2, EW_GLIM, 5, 100);
    const uint32_t before = g.score;
    ASSERT_TRUE(ew_flare(&g));
    ASSERT_EQ(g.flare, 0);
    for (int i = 0; i < 3; i++) ASSERT_TRUE(!g.motes[i].active);
    // 15 + 15 for the embers, 25 for the glim, all at x1.
    ASSERT_EQ(g.score - before, 55);
    ASSERT_EQ(g.combo, 3);
    ew_event_t ev;
    int flares = 0;
    uint32_t total = 0;
    while (ew_next_event(&g, &ev)) {
        if (ev.type == EW_EV_FLARE) { flares++; total = ev.points; }
        if (ev.type == EW_EV_BLOCK || ev.type == EW_EV_ABSORB) ASSERT_EQ(ev.pad, 1);
    }
    ASSERT_EQ(flares, 1);
    ASSERT_EQ(total, 55);
    // An absorbed glim does not refill the meter through the flare.
    ASSERT_EQ(g.flare, 0);
}

static void test_game_over(void) {
    ew_game_t g;
    quiet_game(&g);
    g.lives = 1;
    g.ward = 2;
    place(&g, 0, EW_EMBER, 6, EW_R_CORE + 1);
    place(&g, 1, EW_EMBER, 0, 150);
    step_n(&g, 2);
    ASSERT_EQ(g.lives, 0);
    ASSERT_EQ(g.phase, EW_PHASE_OVER);
    ASSERT_TRUE(!g.motes[1].active);
    ASSERT_EQ(drain(&g, EW_EV_GAME_OVER), 1);
    // Nothing moves once over.
    const uint8_t ward = g.ward;
    ew_turn(&g, 1);
    ASSERT_EQ(g.ward, ward);
    ASSERT_TRUE(!ew_flare(&g));
}

static void test_wave_clear_and_next(void) {
    ew_game_t g;
    quiet_game(&g);
    g.to_spawn = 0;
    step_n(&g, 1);
    ASSERT_EQ(g.phase, EW_PHASE_CLEAR);
    ASSERT_EQ(g.score, 100);
    ASSERT_EQ(drain(&g, EW_EV_WAVE_CLEAR), 1);
    step_n(&g, EW_CLEAR_TICKS);
    ASSERT_EQ(g.wave, 2);
    ASSERT_EQ(g.phase, EW_PHASE_INTRO);
    ASSERT_EQ(g.to_spawn, ew_wave_params(2).count);
}

// Two games from the same seed play out identically, no two sparks sit on
// the same spot, and a ward that never moves ends the game.
static void test_full_waves_deterministic(void) {
    ew_game_t a, b;
    ew_init(&a, 12345);
    ew_init(&b, 12345);
    int spawned = 0, guard = 0;
    while (a.phase != EW_PHASE_OVER && guard++ < 200000) {
        ew_step(&a);
        ew_step(&b);
        for (int i = 0; i < EW_MAX_MOTES; i++) {
            for (int j = i + 1; j < EW_MAX_MOTES; j++) {
                const ew_mote_t *m = &a.motes[i], *n = &a.motes[j];
                if (m->active && n->active && m->lane == n->lane && m->kind == n->kind &&
                    m->r_q8 == n->r_q8)
                    ASSERT_TRUE(false);
            }
        }
        ew_event_t ev;
        while (ew_next_event(&a, &ev))
            if (ev.type == EW_EV_SPAWN) spawned++;
        drain(&b, -1);
    }
    ASSERT_EQ(a.phase, EW_PHASE_OVER);
    ASSERT_EQ(a.score, b.score);
    ASSERT_EQ(a.tick, b.tick);
    ASSERT_TRUE(spawned >= EW_LIVES_START);   // at least one spark per life lost
}

// Wave 1's count all spawns when the player blocks everything.
static void test_wave_spawn_count(void) {
    ew_game_t g;
    ew_init(&g, 99);
    g.lives = 200;   // never ends
    int spawned = 0, guard = 0;
    while (g.wave == 1 && guard++ < 100000) {
        ew_step(&g);
        ew_event_t ev;
        while (ew_next_event(&g, &ev))
            if (ev.type == EW_EV_SPAWN) spawned++;
    }
    ASSERT_EQ(spawned, ew_wave_params(1).count);
}

static void test_pairs_answerable(void) {
    // Over many seeds, every pair spawned at once sits in adjacent lanes.
    for (uint32_t seed = 1; seed < 200; seed++) {
        ew_game_t g;
        ew_init(&g, seed);
        g.lives = 200;
        while (g.wave < 3) {
            ew_step(&g);
            ew_event_t ev, prev = { 0 };
            uint32_t prev_tick = 0;
            bool have_prev = false;
            while (ew_next_event(&g, &ev)) {
                if (ev.type != EW_EV_SPAWN) continue;
                if (have_prev && prev_tick == g.tick) {
                    ASSERT_EQ((prev.lane + 1) % EW_LANES, ev.lane);
                    ASSERT_TRUE(prev.kind == EW_EMBER || ev.kind == EW_EMBER);
                }
                prev = ev;
                prev_tick = g.tick;
                have_prev = true;
            }
        }
    }
}

static void test_event_queue_bounded(void) {
    ew_game_t g;
    ew_init(&g, 3);
    drain(&g, -1);
    for (int i = 0; i < 100; i++) ew_turn(&g, 1);
    ASSERT_EQ(drain(&g, EW_EV_TURN), EW_EVENT_QUEUE);
}

int main(void) {
    test_init();
    test_wave_params_ramp();
    test_turn_wraps();
    test_ember_blocked();
    test_ember_hits_core();
    test_grace_period();
    test_glim_absorbed_and_shattered();
    test_multiplier();
    test_heart();
    test_flare();
    test_game_over();
    test_wave_clear_and_next();
    test_full_waves_deterministic();
    test_wave_spawn_count();
    test_pairs_answerable();
    test_event_queue_bounded();
    printf("test_ew_game: %s\n", g_failures ? "FAIL" : "ok");
    TEST_RETURN();
}
