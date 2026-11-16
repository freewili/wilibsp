// Host tests for apps/emberward/ew_mix.c: the TV sound mixer (several effects
// at once, 48 kHz stereo) and its feed into a writer that takes a little at a
// time, like tvout_audio_write().
#include "ew_mix.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

#define AMP 6000

static int16_t s_out[2 * 65536];

static int peak(const int16_t *st, unsigned frames) {
    int p = 0;
    for (unsigned i = 0; i < frames; i++) {
        const int a = abs(st[2 * i]);
        if (a > p) p = a;
    }
    return p;
}

static int rising_edges(const int16_t *st, unsigned frames) {
    int n = 0;
    for (unsigned i = 1; i < frames; i++) n += st[2 * (i - 1)] < 0 && st[2 * i] >= 0;
    return n;
}

static void test_silent_when_idle(void) {
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    memset(s_out, 0x55, sizeof s_out);
    ew_mix_render(&m, s_out, 1000);
    ASSERT_EQ(peak(s_out, 1000), 0);
    ASSERT_EQ(ew_mix_active(&m), 0);
}

static void test_effect_length_and_pitch(void) {
    // MENU: one 880 Hz pulse note of 3 ticks = 2400 frames, then silence.
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    ASSERT_TRUE(ew_mix_post(&m, EW_SFX_MENU));
    ew_mix_render(&m, s_out, 2400);
    ASSERT_EQ(peak(s_out, 2400), AMP);
    ASSERT_NEAR(rising_edges(s_out, 2400), 880 * 2400 / 48000, 1);   // 44 cycles
    for (unsigned i = 0; i < 2400; i++) ASSERT_EQ(s_out[2 * i], s_out[2 * i + 1]);
    ASSERT_EQ(ew_mix_active(&m), 0);
    ew_mix_render(&m, s_out, 100);
    ASSERT_EQ(peak(s_out, 100), 0);

    // Every effect ends after exactly its length in ticks.
    for (int id = EW_SFX_TURN; id < EW_SFX_COUNT; id++) {
        ew_mix_init(&m, AMP);
        ew_mix_post(&m, (ew_sfx_id_t)id);
        const unsigned frames = ew_sfx_length((ew_sfx_id_t)id) * EW_MIX_TICK;
        ew_mix_render(&m, s_out, frames - 1);
        ASSERT_EQ(ew_mix_active(&m), 1);
        ew_mix_render(&m, s_out, 1);
        ASSERT_EQ(ew_mix_active(&m), 0);
    }
}

static void test_overlap_mixes(void) {
    // CONFIRM and BLOCK both open with a 660 Hz square at +amp: the first
    // frame is their sum, clipped when it passes full scale.
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    ew_mix_post(&m, EW_SFX_CONFIRM);
    ew_mix_post(&m, EW_SFX_BLOCK);
    ew_mix_render(&m, s_out, 1);
    ASSERT_EQ(ew_mix_active(&m), 2);
    ASSERT_EQ(s_out[0], 2 * AMP);

    ew_mix_init(&m, 20000);
    ew_mix_post(&m, EW_SFX_CONFIRM);
    ew_mix_post(&m, EW_SFX_BLOCK);
    ew_mix_render(&m, s_out, 8);
    ASSERT_EQ(s_out[0], 32767);
    ASSERT_EQ(s_out[1], 32767);
}

static void test_same_effect_restarts(void) {
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    ew_mix_post(&m, EW_SFX_GAME_OVER);
    ew_mix_render(&m, s_out, 5000);
    ew_mix_post(&m, EW_SFX_GAME_OVER);
    ew_mix_render(&m, s_out, 1);
    ASSERT_EQ(ew_mix_active(&m), 1);
    // Restarted: it lasts its whole length again from here.
    ew_mix_render(&m, s_out, ew_sfx_length(EW_SFX_GAME_OVER) * EW_MIX_TICK - 2);
    ASSERT_EQ(ew_mix_active(&m), 1);
}

static void test_voice_takeover(void) {
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    // Four voices busy: BLOCK (2), DART (2), ABSORB (3), HIT (5).
    ew_mix_post(&m, EW_SFX_BLOCK);
    ew_mix_post(&m, EW_SFX_DART);
    ew_mix_post(&m, EW_SFX_ABSORB);
    ew_mix_post(&m, EW_SFX_HIT);
    ew_mix_render(&m, s_out, 1);
    ASSERT_EQ(ew_mix_active(&m), 4);
    // TURN ranks 1: dropped.
    ew_mix_post(&m, EW_SFX_TURN);
    ew_mix_render(&m, s_out, 1);
    for (int i = 0; i < EW_MIX_VOICES; i++) ASSERT_TRUE(m.voice[i].id != EW_SFX_TURN);
    // FLARE ranks 6: takes the oldest of the lowest rank, BLOCK.
    ew_mix_post(&m, EW_SFX_FLARE);
    ew_mix_render(&m, s_out, 1);
    int block = 0, dart = 0, flare = 0;
    for (int i = 0; i < EW_MIX_VOICES; i++) {
        block += m.voice[i].id == EW_SFX_BLOCK;
        dart += m.voice[i].id == EW_SFX_DART;
        flare += m.voice[i].id == EW_SFX_FLARE;
    }
    ASSERT_EQ(block, 0);
    ASSERT_EQ(dart, 1);
    ASSERT_EQ(flare, 1);
    // NONE silences everything.
    ew_mix_post(&m, EW_SFX_NONE);
    ew_mix_render(&m, s_out, 10);
    ASSERT_EQ(ew_mix_active(&m), 0);
    ASSERT_EQ(peak(s_out, 10), 0);
}

static void test_queue_full(void) {
    ew_mix_t m;
    ew_mix_init(&m, AMP);
    for (int i = 0; i < EW_MIX_QUEUE - 1; i++) ASSERT_TRUE(ew_mix_post(&m, EW_SFX_TURN));
    ASSERT_TRUE(!ew_mix_post(&m, EW_SFX_TURN));
    ASSERT_TRUE(!ew_mix_post(&m, EW_SFX_COUNT));
    ew_mix_render(&m, s_out, 1);
    ASSERT_TRUE(ew_mix_post(&m, EW_SFX_TURN));
}

static void test_pieces_match_whole(void) {
    // Rendered in odd pieces, the samples are those of one long render, noise
    // and rests included.
    static int16_t whole[2 * 12000], parts[2 * 12000];
    ew_mix_t a, b;
    ew_mix_init(&a, AMP);
    ew_mix_init(&b, AMP);
    const ew_sfx_id_t ids[] = { EW_SFX_WAVE_START, EW_SFX_HIT, EW_SFX_FLARE };
    for (int i = 0; i < 3; i++) {
        ew_mix_post(&a, ids[i]);
        ew_mix_post(&b, ids[i]);
    }
    ew_mix_render(&a, whole, 12000);
    for (unsigned at = 0, n = 1; at < 12000; at += n, n = n % 37 + 3) {
        if (at + n > 12000) n = 12000 - at;
        ew_mix_render(&b, &parts[2 * at], n);
    }
    ASSERT_TRUE(memcmp(whole, parts, sizeof whole) == 0);
}

// A writer that, like the TV feeder, takes only what its small buffer has room
// for: `room` frames per call, then nothing until the "TV" drains it.
static int16_t s_sink[2 * 20000];
static unsigned s_sunk, s_room;

static unsigned sink_write(const int16_t *st, unsigned frames) {
    unsigned n = frames < s_room ? frames : s_room;
    if (s_sunk + n > 20000) n = 20000 - s_sunk;
    memcpy(&s_sink[2 * s_sunk], st, (size_t)n * 4);
    s_sunk += n;
    s_room -= n;
    return n;
}

static void test_feed_loses_nothing(void) {
    static int16_t ref[2 * 20000];
    ew_mix_t a, b;
    ew_mix_init(&a, AMP);
    ew_mix_init(&b, AMP);
    ew_mix_post(&a, EW_SFX_NEW_BEST);
    ew_mix_post(&b, EW_SFX_NEW_BEST);
    ew_mix_render(&a, ref, 20000);

    s_sunk = 0;
    unsigned calls = 0;
    while (s_sunk < 20000 && calls < 10000) {
        s_room = 48 + (calls * 7) % 41;   // what the TV drained since the last call
        const unsigned before = s_sunk;
        ASSERT_EQ(ew_mix_feed(&b, sink_write), s_sunk - before);
        calls++;
    }
    ASSERT_EQ(s_sunk, 20000);
    ASSERT_TRUE(memcmp(ref, s_sink, sizeof ref) == 0);

    // A writer with room for everything still gets at most EW_MIX_FEED_MAX
    // blocks per call; a stopped one (takes 0) costs one block, kept for later.
    ew_mix_init(&b, AMP);
    s_sunk = 0;
    s_room = 100000;
    ASSERT_EQ(ew_mix_feed(&b, sink_write), EW_MIX_BLOCK * EW_MIX_FEED_MAX);
    s_room = 0;
    ASSERT_EQ(ew_mix_feed(&b, sink_write), 0);
    ASSERT_EQ(b.pending_count - b.pending_at, EW_MIX_BLOCK);
}

int main(void) {
    test_silent_when_idle();
    test_effect_length_and_pitch();
    test_overlap_mixes();
    test_same_effect_restarts();
    test_voice_takeover();
    test_queue_full();
    test_pieces_match_whole();
    test_feed_loses_nothing();
    printf("test_ew_mix: %s\n", g_failures ? "FAIL" : "ok");
    TEST_RETURN();
}
