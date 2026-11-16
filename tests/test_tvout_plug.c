/* Host tests for bsp/display/tvout_plug: the hot-plug settle times. */
#include <stdbool.h>
#include <stdint.h>
#include "test_util.h"
#include "display/tvout_plug.h"

/* Feeds `line` every 10 ms from `from` up to and including `to`; returns the
 * first event, and its time in *at. */
static tvout_plug_event_t hold(tvout_plug_t *p, bool line, uint32_t from, uint32_t to, uint32_t *at) {
    for (uint32_t t = from; t <= to; t += 10) {
        const tvout_plug_event_t ev = tvout_plug_update(p, line, t);
        if (ev != TVOUT_PLUG_NONE) {
            if (at) *at = t;
            return ev;
        }
    }
    return TVOUT_PLUG_NONE;
}

static void test_starts_removed_and_quiet(void) {
    tvout_plug_t p = {0};
    ASSERT_TRUE(!p.connected);
    ASSERT_EQ(hold(&p, false, 0, 5000, NULL), TVOUT_PLUG_NONE);
    ASSERT_TRUE(!p.connected);
}

static void test_connect_after_300ms(void) {
    tvout_plug_t p = {0};
    uint32_t at = 0;
    ASSERT_EQ(hold(&p, true, 1000, 2000, &at), TVOUT_PLUG_CONNECTED);
    ASSERT_EQ(at, 1300);
    ASSERT_TRUE(p.connected);
    ASSERT_EQ(hold(&p, true, at + 10, 5000, NULL), TVOUT_PLUG_NONE);
}

static void test_remove_after_1000ms(void) {
    tvout_plug_t p = {0};
    uint32_t at = 0;
    ASSERT_EQ(hold(&p, true, 0, 1000, &at), TVOUT_PLUG_CONNECTED);
    ASSERT_EQ(hold(&p, false, 2000, 4000, &at), TVOUT_PLUG_REMOVED);
    ASSERT_EQ(at, 3000);
    ASSERT_TRUE(!p.connected);
}

/* A drop shorter than the removal settle (a TV changing input) is not a
 * removal, and the settle restarts after it. */
static void test_short_drop_ignored(void) {
    tvout_plug_t p = {0};
    ASSERT_EQ(hold(&p, true, 0, 1000, NULL), TVOUT_PLUG_CONNECTED);
    ASSERT_EQ(hold(&p, false, 2000, 2990, NULL), TVOUT_PLUG_NONE);
    ASSERT_EQ(hold(&p, true, 3000, 3500, NULL), TVOUT_PLUG_NONE);
    uint32_t at = 0;
    ASSERT_EQ(hold(&p, false, 4000, 6000, &at), TVOUT_PLUG_REMOVED);
    ASSERT_EQ(at, 5000);
}

/* A blip shorter than the connect settle is not a TV. */
static void test_short_blip_ignored(void) {
    tvout_plug_t p = {0};
    ASSERT_EQ(hold(&p, true, 0, 290, NULL), TVOUT_PLUG_NONE);
    ASSERT_EQ(hold(&p, false, 300, 2000, NULL), TVOUT_PLUG_NONE);
    ASSERT_TRUE(!p.connected);
}

static void test_clock_wrap(void) {
    tvout_plug_t p = {0};
    ASSERT_EQ(hold(&p, true, 0xFFFFFF00u, 0xFFFFFFF0u, NULL), TVOUT_PLUG_NONE);
    ASSERT_EQ(tvout_plug_update(&p, true, 0x00000100u), TVOUT_PLUG_CONNECTED);
}

int main(void) {
    test_starts_removed_and_quiet();
    test_connect_after_300ms();
    test_remove_after_1000ms();
    test_short_drop_ignored();
    test_short_blip_ignored();
    test_clock_wrap();
    TEST_RETURN();
}
