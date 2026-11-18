/* Host tests for bsp/cec/cec_watch: the events the app gets from received
 * frames the device does not report. The test plays the device's key
 * reports (cec_watch_on_key) the way cec.c passes them on. */
#include <stdint.h>
#include <string.h>
#include "test_util.h"
#include "cec/cec_watch.h"

static cec_frame_t frame_of(const uint8_t *blocks, int len) {
    cec_frame_t f;
    memset(&f, 0, sizeof f);
    f.len = (uint8_t)len;
    memcpy(f.block, blocks, (size_t)len);
    return f;
}

/* The frame watched; the device must see every frame but accepted vendor ones. */
static int watch(const uint8_t *blocks, int len, cec_event_t *ev) {
    const cec_frame_t f = frame_of(blocks, len);
    bool for_device = false;
    const int got = cec_watch_on_rx(&f, ev, &for_device);
    ASSERT_TRUE(for_device == !(cec_watch_accepts_vendor() && got && ev->type == CEC_EV_VENDOR));
    return got;
}

/* The TV's <Routing Change> 1.0.0.0 -> 2.0.0.0 and <Set Stream Path> 3.0.0.0:
 * the address the route now leads to, the announcing device, the opcode. */
static void test_routing(void) {
    cec_event_t ev;
    const uint8_t change[] = { 0x0F, 0x80, 0x10, 0x00, 0x20, 0x00 };
    ASSERT_TRUE(watch(change, 6, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_ROUTING);
    ASSERT_EQ(ev.data, 0x80);
    ASSERT_EQ(ev.from, 0);
    ASSERT_EQ(ev.to, 15);
    ASSERT_EQ(ev.extra[0], 0x20);
    ASSERT_EQ(ev.extra[1], 0x00);

    const uint8_t path[] = { 0x0F, 0x86, 0x31, 0x20 };
    ASSERT_TRUE(watch(path, 4, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_ROUTING);
    ASSERT_EQ(ev.data, 0x86);
    ASSERT_EQ(ev.extra[0], 0x31);
    ASSERT_EQ(ev.extra[1], 0x20);
}

/* Not broadcast, the wrong length, another opcode, a poll: nothing. */
static void test_not_routing(void) {
    cec_event_t ev;
    const uint8_t direct[] = { 0x04, 0x86, 0x10, 0x00 };
    ASSERT_TRUE(!watch(direct, 4, &ev));
    const uint8_t short_change[] = { 0x0F, 0x80, 0x10, 0x00, 0x20 };
    ASSERT_TRUE(!watch(short_change, 5, &ev));
    const uint8_t long_path[] = { 0x0F, 0x86, 0x10, 0x00, 0x00 };
    ASSERT_TRUE(!watch(long_path, 5, &ev));
    const uint8_t active[] = { 0x4F, 0x82, 0x10, 0x00 };
    ASSERT_TRUE(!watch(active, 4, &ev));
    const uint8_t poll[] = { 0x04 };
    ASSERT_TRUE(!watch(poll, 1, &ev));
}

/* A held key: the first press is the device's (KEY_DOWN), each press of the
 * same key from the same device after it a repeat, until the device reports
 * the key up. */
static void test_key_repeat(void) {
    cec_event_t ev;
    const uint8_t up_key[] = { 0x04, 0x44, 0x01 };
    cec_watch_init();
    ASSERT_TRUE(!watch(up_key, 3, &ev));            /* the press: the device reports it */
    cec_watch_on_key(true, 0x01, 0);
    ASSERT_TRUE(watch(up_key, 3, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_KEY_REPEAT);
    ASSERT_EQ(ev.data, 0x01);
    ASSERT_EQ(ev.from, 0);
    ASSERT_EQ(ev.to, 4);
    ASSERT_TRUE(watch(up_key, 3, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_KEY_REPEAT);

    /* Another key, another device, a broadcast: not a repeat of this one. */
    const uint8_t down_key[] = { 0x04, 0x44, 0x02 };
    ASSERT_TRUE(!watch(down_key, 3, &ev));
    const uint8_t other_device[] = { 0x54, 0x44, 0x01 };
    ASSERT_TRUE(!watch(other_device, 3, &ev));
    const uint8_t broadcast[] = { 0x0F, 0x44, 0x01 };
    ASSERT_TRUE(!watch(broadcast, 3, &ev));
    const uint8_t no_operand[] = { 0x04, 0x44 };
    ASSERT_TRUE(!watch(no_operand, 2, &ev));

    /* Up, from a release or the device's time-out: the next press is new. */
    cec_watch_on_key(false, 0x01, 0);
    ASSERT_TRUE(!watch(up_key, 3, &ev));

    /* A start forgets a key held before it. */
    cec_watch_on_key(true, 0x01, 0);
    cec_watch_init();
    ASSERT_TRUE(!watch(up_key, 3, &ev));
}

/* Vendor frames: the device's while not accepted; accepted, each of the four
 * is the app's, whole, and the device does not see it. */
static void test_vendor(void) {
    cec_event_t ev;
    const uint8_t button_down[] = { 0x04, 0x8A, 0x91, 0x22 };
    const uint8_t button_up[] = { 0x04, 0x8B };
    const uint8_t command[] = { 0x04, 0x89, 0x01 };
    const uint8_t with_id[] = { 0x0F, 0xA0, 0x00, 0x15, 0x82, 0x7F };
    ASSERT_TRUE(!cec_watch_accepts_vendor());
    ASSERT_TRUE(!watch(button_down, 4, &ev));

    cec_watch_accept_vendor(true);
    ASSERT_TRUE(watch(button_down, 4, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_VENDOR);
    ASSERT_EQ(ev.data, 0x8A);
    ASSERT_EQ(ev.from, 0);
    ASSERT_EQ(ev.to, 4);
    ASSERT_EQ(ev.frame.len, 4);
    ASSERT_TRUE(memcmp(ev.frame.block, button_down, 4) == 0);
    ASSERT_TRUE(watch(button_up, 2, &ev));
    ASSERT_EQ(ev.data, 0x8B);
    ASSERT_TRUE(watch(command, 3, &ev));
    ASSERT_EQ(ev.data, 0x89);
    ASSERT_TRUE(watch(with_id, 6, &ev));
    ASSERT_EQ(ev.data, 0xA0);
    ASSERT_EQ(ev.to, 15);
    ASSERT_EQ(ev.frame.len, 6);

    /* Other frames are unaffected, and the setting survives a start. */
    const uint8_t routing[] = { 0x0F, 0x86, 0x20, 0x00 };
    ASSERT_TRUE(watch(routing, 4, &ev));
    ASSERT_TRUE(ev.type == CEC_EV_ROUTING);
    ASSERT_EQ(ev.frame.len, 0);
    cec_watch_init();
    ASSERT_TRUE(cec_watch_accepts_vendor());
    cec_watch_accept_vendor(false);
    ASSERT_TRUE(!watch(button_down, 4, &ev));
}

int main(void) {
    test_routing();
    test_not_routing();
    test_key_repeat();
    test_vendor();
    TEST_RETURN();
}
