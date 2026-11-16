/* Host tests for bsp/cec/cec_engine: the C entry points cec.c calls, built
 * from C against the C++ bit engine and device. */
#include <stdint.h>
#include <string.h>
#include "test_util.h"
#include "cec/cec_engine.h"

static cec_engine_out_t s_out;

static int frame_is(const cec_frame_t *f, const uint8_t *blocks, int len) {
    return f->len == len && memcmp(f->block, blocks, (size_t)len) == 0;
}

static cec_frame_t frame_of(const uint8_t *blocks, int len) {
    cec_frame_t f;
    memset(&f, 0, sizeof f);
    f.len = (uint8_t)len;
    memcpy(f.block, blocks, (size_t)len);
    return f;
}

static void rx(const uint8_t *blocks, int len, uint32_t now_ms) {
    const cec_frame_t f = frame_of(blocks, len);
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_rx(&f, now_ms, &s_out);
}

/* Starts the device at 1.0.0.0 and answers its first poll with no
 * acknowledge, so it takes playback 1 (address 4). */
static void claim_playback_1(const char *name) {
    cec_engine_device_init(name, CEC_VENDOR_ID_DEFAULT, CEC_TYPE_PLAYBACK, CEC_TYPE_NONE);
    cec_engine_out_clear(&s_out);
    cec_engine_device_start(0x1000, &s_out);
    static const uint8_t poll[] = { 0x44 };
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], poll, 1));
    const cec_frame_t sent = s_out.frame[0];
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_tx_done(&sent, CEC_ENGINE_TX_NOT_ACKED, &s_out);
}

static void test_claim(void) {
    claim_playback_1("hello");
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_ADDRESS_CLAIMED);
    ASSERT_EQ(s_out.event[0].own, 4);
    /* <Report Physical Address> 1.0.0.0, playback; <Device Vendor ID>. */
    static const uint8_t report[] = { 0x4F, 0x84, 0x10, 0x00, 0x04 };
    static const uint8_t vendor[] = { 0x4F, 0x87, 0x00, 0x15, 0x82 };
    ASSERT_EQ(s_out.frames, 2);
    ASSERT_TRUE(frame_is(&s_out.frame[0], report, 5));
    ASSERT_TRUE(frame_is(&s_out.frame[1], vendor, 5));
    ASSERT_EQ(cec_engine_device_address(), 4);
    ASSERT_EQ(cec_engine_device_address_mask(), 1u << 4);
}

static void test_osd_name(void) {
    claim_playback_1("hello");
    static const uint8_t give[] = { 0x04, 0x46 };
    static const uint8_t name[] = { 0x40, 0x47, 'h', 'e', 'l', 'l', 'o' };
    rx(give, 2, 0);
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], name, 7));

    /* No printable character: the default name. */
    claim_playback_1("\x01");
    static const uint8_t dflt[] = { 0x40, 0x47, 'F', 'r', 'e', 'e', 'w', 'i', 'l', 'i', ' ', '2' };
    rx(give, 2, 0);
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], dflt, 12));
}

static void test_keys(void) {
    claim_playback_1(NULL);
    static const uint8_t up[] = { 0x04, 0x44, 0x01 };
    static const uint8_t released[] = { 0x04, 0x45 };
    rx(up, 3, 1000);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_KEY_DOWN);
    ASSERT_EQ(s_out.event[0].data[0], 0x01);
    rx(released, 2, 1100);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_KEY_UP);
    ASSERT_EQ(s_out.event[0].data[0], 0x01);

    /* A press with no release is let go 550 ms after it. */
    rx(up, 3, 2000);
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_tick(2549, &s_out);
    ASSERT_EQ(s_out.events, 0);
    cec_engine_device_on_tick(2550, &s_out);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_KEY_UP);

    ASSERT_TRUE(strcmp(cec_engine_key_name(0x01), "up") == 0);
    ASSERT_TRUE(strcmp(cec_engine_key_name(0x00), "select") == 0);
}

static void test_standby_in(void) {
    claim_playback_1(NULL);
    static const uint8_t standby[] = { 0x0F, 0x36 };
    rx(standby, 2, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_STANDBY);
}

static void test_wake_and_standby_out(void) {
    claim_playback_1(NULL);
    cec_engine_out_clear(&s_out);
    ASSERT_TRUE(cec_engine_device_one_touch_play(&s_out));
    static const uint8_t view_on[] = { 0x40, 0x04 };
    static const uint8_t active[] = { 0x4F, 0x82, 0x10, 0x00 };
    ASSERT_EQ(s_out.frames, 2);
    ASSERT_TRUE(frame_is(&s_out.frame[0], view_on, 2));
    ASSERT_TRUE(frame_is(&s_out.frame[1], active, 4));
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_ACTIVE_SOURCE);
    ASSERT_EQ(s_out.event[0].data[0], 1);

    cec_engine_out_clear(&s_out);
    ASSERT_TRUE(cec_engine_device_standby(CEC_ENGINE_ADDR_TV, &s_out));
    static const uint8_t standby[] = { 0x40, 0x36 };
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], standby, 2));

    /* Leaving as the active source says <Inactive Source> to the TV. */
    cec_engine_out_clear(&s_out);
    cec_engine_device_stop(&s_out);
    static const uint8_t inactive[] = { 0x40, 0x9D, 0x10, 0x00 };
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], inactive, 4));
    ASSERT_EQ(cec_engine_device_address_mask(), 0);
}

static void test_wire(void) {
    cec_wire_out_t out;
    cec_engine_wire_init(0);
    cec_engine_wire_clear_counts();
    static const uint8_t blocks[] = { 0x40, 0x04 };
    const cec_frame_t f = frame_of(blocks, 2);
    ASSERT_TRUE(cec_engine_wire_send(&f, 0, &out));
    ASSERT_TRUE(!out.drive_low);
    ASSERT_TRUE(out.alarm);                     /* the line must be free first */
    ASSERT_TRUE(!cec_engine_wire_send(&f, 0, &out));
    const cec_frame_t empty = frame_of(blocks, 0);
    cec_engine_wire_cancel(&out);
    ASSERT_TRUE(!cec_engine_wire_send(&empty, 0, &out));

    cec_frame_t done;
    cec_engine_tx_result_t result;
    ASSERT_TRUE(cec_engine_wire_pop_tx_done(&done, &result));
    ASSERT_EQ(result, CEC_ENGINE_TX_FAILED);
    ASSERT_TRUE(frame_is(&done, blocks, 2));
    ASSERT_TRUE(!cec_engine_wire_pop_tx_done(&done, &result));

    uint8_t flags;
    ASSERT_TRUE(!cec_engine_wire_pop_rx(&done, &flags));
    cec_engine_wire_counts_t counts;
    cec_engine_wire_counts(&counts);
    ASSERT_EQ(counts.late_aborts, 0);
    ASSERT_EQ(counts.rx_frames, 0);
}

/* A playback device and an audio system: two claims, two addresses, and
 * keys to the audio system's address say so. */
static void test_second_type(void) {
    cec_engine_device_init("two", CEC_VENDOR_ID_DEFAULT, CEC_TYPE_PLAYBACK, CEC_TYPE_AUDIO_SYSTEM);
    cec_engine_out_clear(&s_out);
    cec_engine_device_start(0x1000, &s_out);
    static const uint8_t poll4[] = { 0x44 };
    static const uint8_t poll5[] = { 0x55 };
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], poll4, 1));
    ASSERT_TRUE(!cec_engine_device_ready());

    cec_frame_t sent = s_out.frame[0];
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_tx_done(&sent, CEC_ENGINE_TX_NOT_ACKED, &s_out);
    static const uint8_t report4[] = { 0x4F, 0x84, 0x10, 0x00, 0x04 };
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_ADDRESS_CLAIMED);
    ASSERT_EQ(s_out.event[0].own, 4);
    ASSERT_EQ(s_out.event[0].data[0], CEC_TYPE_PLAYBACK);
    ASSERT_EQ(s_out.frames, 2);
    ASSERT_TRUE(frame_is(&s_out.frame[0], report4, 5));
    ASSERT_TRUE(frame_is(&s_out.frame[1], poll5, 1));

    sent = s_out.frame[1];
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_tx_done(&sent, CEC_ENGINE_TX_NOT_ACKED, &s_out);
    static const uint8_t report5[] = { 0x5F, 0x84, 0x10, 0x00, 0x05 };
    static const uint8_t vendor[] = { 0x4F, 0x87, 0x00, 0x15, 0x82 };
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].own, 5);
    ASSERT_EQ(s_out.event[0].data[0], CEC_TYPE_AUDIO_SYSTEM);
    ASSERT_EQ(s_out.frames, 2);
    ASSERT_TRUE(frame_is(&s_out.frame[0], report5, 5));
    ASSERT_TRUE(frame_is(&s_out.frame[1], vendor, 5));
    ASSERT_TRUE(cec_engine_device_ready());
    ASSERT_EQ(cec_engine_device_address_mask(), (1u << 4) | (1u << 5));
    ASSERT_EQ(cec_engine_device_address(), 4);

    /* The TV's volume key to the audio system. */
    static const uint8_t vol_up[] = { 0x05, 0x44, 0x41 };
    rx(vol_up, 3, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_KEY_DOWN);
    ASSERT_EQ(s_out.event[0].peer, 0);
    ASSERT_EQ(s_out.event[0].own, 5);
    ASSERT_EQ(s_out.event[0].data[0], CEC_KEY_VOLUME_UP);
}

/* The vendor ID and the name of the device running, changed under it. */
static void test_vendor_and_name(void) {
    cec_engine_device_init("first", CEC_VENDOR_NONE, CEC_TYPE_PLAYBACK, CEC_TYPE_NONE);
    cec_engine_out_clear(&s_out);
    cec_engine_device_start(0x1000, &s_out);
    const cec_frame_t sent = s_out.frame[0];
    cec_engine_out_clear(&s_out);
    cec_engine_device_on_tx_done(&sent, CEC_ENGINE_TX_NOT_ACKED, &s_out);
    ASSERT_EQ(s_out.frames, 1);                 /* <Report Physical Address> only: no vendor ID */

    static const uint8_t give_vendor[] = { 0x04, 0x8C };
    static const uint8_t refused[] = { 0x40, 0x00, 0x8C, 0x00 };
    rx(give_vendor, 2, 0);
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], refused, 4));

    cec_engine_device_set_vendor_id(0x123456);
    static const uint8_t vendor[] = { 0x4F, 0x87, 0x12, 0x34, 0x56 };
    rx(give_vendor, 2, 0);
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], vendor, 5));

    char name[15];
    cec_engine_device_osd_name(name);
    ASSERT_TRUE(strcmp(name, "first") == 0);
    cec_engine_device_set_osd_name("abc");
    cec_engine_device_osd_name(name);
    ASSERT_TRUE(strcmp(name, "abc") == 0);
    static const uint8_t give_name[] = { 0x04, 0x46 };
    static const uint8_t abc[] = { 0x40, 0x47, 'a', 'b', 'c' };
    rx(give_name, 2, 0);
    ASSERT_TRUE(frame_is(&s_out.frame[0], abc, 5));
    cec_engine_device_set_osd_name(NULL);
    cec_engine_device_osd_name(name);
    ASSERT_TRUE(strcmp(name, "Freewili 2") == 0);
}

/* The device events past keys and standby. */
static void test_device_events(void) {
    claim_playback_1(NULL);
    static const uint8_t menu[] = { 0x04, 0x8D, 0x00 };
    static const uint8_t status[] = { 0x40, 0x8E, 0x00 };
    rx(menu, 3, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_MENU_REQUEST);
    ASSERT_EQ(s_out.event[0].data[0], CEC_MENU_ACTIVATE);
    ASSERT_EQ(s_out.frames, 1);
    ASSERT_TRUE(frame_is(&s_out.frame[0], status, 3));

    static const uint8_t language[] = { 0x0F, 0x32, 'e', 'n', 'g' };
    rx(language, 5, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_MENU_LANGUAGE);
    ASSERT_EQ(s_out.event[0].data[0], 'e');
    ASSERT_EQ(s_out.event[0].data[2], 'g');

    static const uint8_t play[] = { 0x04, 0x41, 0x24 };
    rx(play, 3, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_DECK_PLAY);
    ASSERT_EQ(s_out.event[0].data[0], 0x24);

    static const uint8_t deck[] = { 0x04, 0x42, 0x03 };
    rx(deck, 3, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_DECK_CONTROL);

    static const uint8_t abort_of_osd[] = { 0x04, 0x00, 0x64, 0x04 };
    rx(abort_of_osd, 4, 0);
    ASSERT_EQ(s_out.events, 1);
    ASSERT_EQ(s_out.event[0].type, CEC_ENGINE_EV_FEATURE_ABORT);
    ASSERT_EQ(s_out.event[0].peer, 0);
    ASSERT_EQ(s_out.event[0].data[0], 0x64);
    ASSERT_EQ(s_out.event[0].data[1], CEC_REASON_REFUSED);
}

static void test_names(void) {
    ASSERT_TRUE(strcmp(cec_engine_opcode_name(0x46), "give_osd_name") == 0);
    ASSERT_TRUE(strcmp(cec_engine_device_type_name(CEC_TYPE_AUDIO_SYSTEM), "audio_system") == 0);
    ASSERT_TRUE(strcmp(cec_engine_power_status_name(CEC_POWER_STATUS_STANDBY), "standby") == 0);
    ASSERT_TRUE(strcmp(cec_engine_abort_reason_name(CEC_REASON_REFUSED), "refused") == 0);
    ASSERT_TRUE(strcmp(cec_engine_opcode_name(0x02), "unknown") == 0);
}

int main(void) {
    test_claim();
    test_osd_name();
    test_keys();
    test_standby_in();
    test_wake_and_standby_out();
    test_wire();
    test_second_type();
    test_vendor_and_name();
    test_device_events();
    test_names();
    TEST_RETURN();
}
