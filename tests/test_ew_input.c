// Host tests for apps/emberward/ew_input.c: per-source holds and key repeat.
#include "ew_input.h"
#include "test_util.h"

static int count(ew_input_t *in, ew_key_t key) {
    int n = 0;
    while (ew_input_take(in, key)) n++;
    return n;
}

static void test_press_once(void) {
    ew_input_t in;
    ew_input_init(&in);
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_OK, true, 100);
    ew_input_tick(&in, 2000);   // OK does not repeat
    ASSERT_EQ(count(&in, EW_KEY_OK), 1);
    ASSERT_TRUE(ew_input_held(&in, EW_KEY_OK));
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_OK, false, 2100);
    ASSERT_TRUE(!ew_input_held(&in, EW_KEY_OK));
    ASSERT_EQ(count(&in, EW_KEY_OK), 0);
}

static void test_repeat(void) {
    ew_input_t in;
    ew_input_init(&in);
    ew_input_set(&in, EW_SRC_REMOTE, EW_KEY_LEFT, true, 1000);
    ASSERT_EQ(count(&in, EW_KEY_LEFT), 1);
    ew_input_tick(&in, 1000 + EW_REPEAT_DELAY_MS - 1);
    ASSERT_EQ(count(&in, EW_KEY_LEFT), 0);
    ew_input_tick(&in, 1000 + EW_REPEAT_DELAY_MS);
    ASSERT_EQ(count(&in, EW_KEY_LEFT), 1);
    ew_input_tick(&in, 1000 + EW_REPEAT_DELAY_MS + EW_REPEAT_RATE_MS);
    ASSERT_EQ(count(&in, EW_KEY_LEFT), 1);
    // Released: no more repeats.
    ew_input_set(&in, EW_SRC_REMOTE, EW_KEY_LEFT, false, 1500);
    ew_input_tick(&in, 5000);
    ASSERT_EQ(count(&in, EW_KEY_LEFT), 0);
}

static void test_long_gap_is_bounded(void) {
    ew_input_t in;
    ew_input_init(&in);
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_RIGHT, true, 0);
    count(&in, EW_KEY_RIGHT);
    // A loop stalled for seconds (an SD save) does not unload a burst of turns.
    ew_input_tick(&in, 10000);
    ASSERT_TRUE(count(&in, EW_KEY_RIGHT) <= 3);
    ew_input_tick(&in, 10000 + EW_REPEAT_RATE_MS);
    ASSERT_EQ(count(&in, EW_KEY_RIGHT), 1);
}

static void test_sources_independent(void) {
    ew_input_t in;
    ew_input_init(&in);
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_UP, true, 0);
    ew_input_set(&in, EW_SRC_REMOTE, EW_KEY_UP, true, 10);   // already held: no second press
    ASSERT_EQ(count(&in, EW_KEY_UP), 1);
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_UP, false, 20);
    ASSERT_TRUE(ew_input_held(&in, EW_KEY_UP));             // the remote still holds it
    ew_input_release_source(&in, EW_SRC_REMOTE);
    ASSERT_TRUE(!ew_input_held(&in, EW_KEY_UP));
}

static void test_flush_and_bounds(void) {
    ew_input_t in;
    ew_input_init(&in);
    for (int i = 0; i < 10; i++) {
        ew_input_set(&in, EW_SRC_TOUCH, EW_KEY_PAUSE, true, (uint32_t)i * 10);
        ew_input_set(&in, EW_SRC_TOUCH, EW_KEY_PAUSE, false, (uint32_t)i * 10 + 5);
    }
    ASSERT_EQ(count(&in, EW_KEY_PAUSE), 3);   // pending presses are capped
    ew_input_set(&in, EW_SRC_TOUCH, EW_KEY_VIEW, true, 200);
    ew_input_flush(&in);
    ASSERT_EQ(count(&in, EW_KEY_VIEW), 0);
    ASSERT_TRUE(!ew_input_take(&in, EW_KEY_COUNT));
    ew_input_set(&in, EW_SRC_COUNT, EW_KEY_OK, true, 0);    // ignored
    ASSERT_EQ(count(&in, EW_KEY_OK), 0);
}

static void test_wraparound(void) {
    ew_input_t in;
    ew_input_init(&in);
    const uint32_t t = 0xFFFFFF00u;
    ew_input_set(&in, EW_SRC_PANEL, EW_KEY_DOWN, true, t);
    count(&in, EW_KEY_DOWN);
    ew_input_tick(&in, t + 100);
    ASSERT_EQ(count(&in, EW_KEY_DOWN), 0);
    ew_input_tick(&in, t + EW_REPEAT_DELAY_MS);   // past the 32-bit wrap
    ASSERT_EQ(count(&in, EW_KEY_DOWN), 1);
}

int main(void) {
    test_press_once();
    test_repeat();
    test_long_gap_is_bounded();
    test_sources_independent();
    test_flush_and_bounds();
    test_wraparound();
    printf("test_ew_input: %s\n", g_failures ? "FAIL" : "ok");
    TEST_RETURN();
}
