// Host tests for apps/emberward/ew_sfx.c: the sound-effect sequencer and the
// looping note buffers.
#include "ew_sfx.h"
#include "test_util.h"

#include <string.h>

static int16_t left(uint32_t frame) { return (int16_t)(frame >> 16); }
static int16_t right(uint32_t frame) { return (int16_t)(frame & 0xFFFF); }

// Ticks an effect to its end; returns the ticks with sound and counts changes.
static int play_out(ew_sfx_t *s, int *changes) {
    int sounding = 0;
    *changes = 0;
    for (int i = 0; i < 1000; i++) {
        ew_note_t n;
        if (ew_sfx_tick(s, &n)) (*changes)++;
        if (s->id == EW_SFX_NONE && !n.hz) break;
        if (n.hz) sounding++;
    }
    return sounding;
}

static void test_length_matches(void) {
    for (int id = EW_SFX_TURN; id < EW_SFX_COUNT; id++) {
        ew_sfx_t s;
        ew_sfx_init(&s);
        ew_sfx_play(&s, (ew_sfx_id_t)id);
        int changes;
        const int sounding = play_out(&s, &changes);
        ASSERT_TRUE(ew_sfx_length((ew_sfx_id_t)id) > 0);
        ASSERT_TRUE(sounding <= (int)ew_sfx_length((ew_sfx_id_t)id));
        ASSERT_TRUE(sounding > 0);
        ASSERT_TRUE(changes >= 2);   // starts, and falls silent at the end
        ASSERT_EQ(s.id, EW_SFX_NONE);
    }
    ASSERT_EQ(ew_sfx_length(EW_SFX_NONE), 0);
    ASSERT_EQ(ew_sfx_length(EW_SFX_COUNT), 0);
}

static void test_turn_timing(void) {
    // TURN is one 2-tick note: on for two ticks, then silence.
    ew_sfx_t s;
    ew_sfx_init(&s);
    ew_sfx_play(&s, EW_SFX_TURN);
    ew_note_t n;
    ASSERT_TRUE(ew_sfx_tick(&s, &n));
    ASSERT_EQ(n.hz, 1400);
    ASSERT_TRUE(!ew_sfx_tick(&s, &n));
    ASSERT_EQ(n.hz, 1400);
    ASSERT_TRUE(ew_sfx_tick(&s, &n));
    ASSERT_EQ(n.hz, 0);
    ASSERT_TRUE(!ew_sfx_tick(&s, &n));
}

static void test_priority(void) {
    ew_sfx_t s;
    ew_sfx_init(&s);
    ew_sfx_play(&s, EW_SFX_HIT);
    ew_note_t n;
    ew_sfx_tick(&s, &n);
    ew_sfx_play(&s, EW_SFX_TURN);   // lower: ignored
    ASSERT_EQ(s.id, EW_SFX_HIT);
    ew_sfx_play(&s, EW_SFX_GAME_OVER);   // higher: takes over
    ASSERT_EQ(s.id, EW_SFX_GAME_OVER);
    ASSERT_TRUE(ew_sfx_tick(&s, &n));
    ASSERT_EQ(n.hz, 392);
    ew_sfx_play(&s, EW_SFX_NONE);
    ASSERT_EQ(s.id, EW_SFX_NONE);
    ASSERT_TRUE(ew_sfx_tick(&s, &n));   // falls silent
    ASSERT_EQ(n.hz, 0);
    ASSERT_TRUE(ew_sfx_priority(EW_SFX_FLARE) > ew_sfx_priority(EW_SFX_BLOCK));
}

static void test_restrike_same_note(void) {
    // The same effect again while its first note still sounds is re-struck.
    ew_sfx_t s;
    ew_sfx_init(&s);
    ew_note_t n;
    ew_sfx_play(&s, EW_SFX_BLOCK);
    ASSERT_TRUE(ew_sfx_tick(&s, &n));
    ew_sfx_play(&s, EW_SFX_BLOCK);
    ASSERT_TRUE(ew_sfx_tick(&s, &n));
    ASSERT_EQ(n.hz, 660);
}

static void test_rest_step(void) {
    // WAVE_START has rests between its notes.
    ew_sfx_t s;
    ew_sfx_init(&s);
    ew_sfx_play(&s, EW_SFX_WAVE_START);
    ew_note_t n;
    int rests = 0, prev = -1;
    for (int i = 0; i < 40; i++) {
        ew_sfx_tick(&s, &n);
        if (n.hz == 0 && prev > 0) rests++;
        prev = n.hz;
    }
    ASSERT_TRUE(rests >= 2);
}

static int cycles_in(const uint32_t *buf, unsigned frames) {
    int ups = 0;
    for (unsigned i = 0; i < frames; i++) {
        const int16_t a = left(buf[i]), b = left(buf[(i + 1) % frames]);
        if (a < 0 && b >= 0) ups++;
    }
    return ups;
}

static void test_render_whole_cycles(void) {
    static uint32_t buf[512];
    const unsigned fs = 16009;
    uint32_t noise = 0;
    for (int wave = EW_WAVE_SQUARE; wave <= EW_WAVE_TRIANGLE; wave++) {
        const unsigned got = ew_sfx_render(buf, 512, 1000, fs, (ew_wave_t)wave, 5000, &noise);
        const int cycles = (int)((1000u * 512u + fs / 2) / fs);   // 32
        ASSERT_EQ(cycles_in(buf, 512), cycles);
        ASSERT_NEAR(got, 1000, 20);
        for (int i = 0; i < 512; i++) {
            ASSERT_TRUE(left(buf[i]) >= -5000 && left(buf[i]) <= 5000);
            ASSERT_EQ(left(buf[i]), right(buf[i]));
        }
    }
    // The pulse is high a quarter of each cycle.
    ew_sfx_render(buf, 512, 1000, fs, EW_WAVE_PULSE, 5000, &noise);
    int high = 0;
    for (int i = 0; i < 512; i++) high += left(buf[i]) > 0;
    ASSERT_NEAR(high, 128, 4);
}

static void test_render_edges(void) {
    static uint32_t buf[512];
    uint32_t noise = 0;
    memset(buf, 0x55, sizeof buf);
    ASSERT_EQ(ew_sfx_render(buf, 512, 0, 16009, EW_WAVE_SQUARE, 5000, &noise), 0);
    for (int i = 0; i < 512; i++) ASSERT_EQ(buf[i], 0);
    // Very low: at least one cycle. Very high: at most frames / 2 cycles.
    ew_sfx_render(buf, 512, 1, 16009, EW_WAVE_SQUARE, 5000, &noise);
    ASSERT_EQ(cycles_in(buf, 512), 1);
    ew_sfx_render(buf, 512, 15000, 16009, EW_WAVE_SQUARE, 5000, &noise);
    ASSERT_EQ(cycles_in(buf, 512), 256);
    // Noise stays in range and advances its state.
    noise = 0;
    ew_sfx_render(buf, 512, 3000, 16009, EW_WAVE_NOISE, 4000, &noise);
    ASSERT_TRUE(noise != 0);
    int distinct = 0;
    for (int i = 1; i < 512; i++) {
        ASSERT_TRUE(left(buf[i]) >= -4000 && left(buf[i]) <= 4000);
        distinct += buf[i] != buf[i - 1];
    }
    ASSERT_TRUE(distinct > 50);
}

int main(void) {
    test_length_matches();
    test_turn_timing();
    test_priority();
    test_restrike_same_note();
    test_rest_step();
    test_render_whole_cycles();
    test_render_edges();
    printf("test_ew_sfx: %s\n", g_failures ? "FAIL" : "ok");
    TEST_RETURN();
}
