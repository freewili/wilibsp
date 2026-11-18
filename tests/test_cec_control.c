/* Host tests for bsp/cec/cec_control: the requests this board makes of other
 * devices (every cec_request_*() behind cec.h) and the capture, built from C
 * against the C++ core. The test plays the wire: it takes each frame the
 * request hands out, gives it a send result, and answers it or not. */
#include <stdint.h>
#include <string.h>
#include "test_util.h"
#include "cec/cec_control.h"

#define OWN 4   /* this board: playback 1 */

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

/* The next frame is blocks; it is sent with result at now_ms. */
static void expect_sent(const uint8_t *blocks, int len, cec_engine_tx_result_t result, uint32_t now_ms) {
    cec_frame_t f;
    ASSERT_TRUE(cec_control_next(OWN, now_ms, &f));
    ASSERT_TRUE(frame_is(&f, blocks, len));
    ASSERT_TRUE(!cec_control_next(OWN, now_ms, &f));    /* one frame at a time */
    cec_control_on_sent(&f, result, now_ms);
}

static void answer(const uint8_t *blocks, int len, uint32_t now_ms) {
    const cec_frame_t f = frame_of(blocks, len);
    cec_control_on_rx(&f, now_ms);
}

static cec_reply_t take(void) {
    cec_reply_t r;
    memset(&r, 0xEE, sizeof r);
    ASSERT_TRUE(!cec_control_busy());
    ASSERT_TRUE(cec_control_result(&r));
    ASSERT_TRUE(!cec_control_result(&r));               /* handed over once */
    return r;
}

static void test_raw(void) {
    cec_control_init();
    /* A poll: no answer is asked for. */
    ASSERT_TRUE(cec_control_raw(0, NULL, 0, CEC_ANSWER_STANDARD));
    ASSERT_TRUE(cec_control_busy());
    ASSERT_TRUE(!cec_control_raw(0, NULL, 0, CEC_ANSWER_NONE));   /* one request at a time */
    static const uint8_t poll[] = { 0x40 };
    expect_sent(poll, 1, CEC_ENGINE_TX_ACKED, 0);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_SENT);
    ASSERT_EQ(r.answer.len, 0);

    /* <Give Device Vendor ID> with the standard answer. */
    static const uint8_t give[] = { 0x8C };
    ASSERT_TRUE(cec_control_raw(0, give, 1, CEC_ANSWER_STANDARD));
    static const uint8_t give_frame[] = { 0x40, 0x8C };
    expect_sent(give_frame, 2, CEC_ENGINE_TX_ACKED, 10);
    static const uint8_t other[] = { 0x0F, 0x82, 0x00, 0x00 };    /* not the answer */
    answer(other, 4, 20);
    ASSERT_TRUE(cec_control_busy());
    static const uint8_t vendor[] = { 0x0F, 0x87, 0x00, 0xE0, 0x91 };
    answer(vendor, 5, 30);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_TRUE(frame_is(&r.answer, vendor, 5));
    ASSERT_EQ(r.vendor_id, 0x00E091);

    /* An explicit answer opcode, refused. */
    static const uint8_t custom[] = { 0x89, 0x01, 0x02 };
    ASSERT_TRUE(cec_control_raw(5, custom, 3, 0x89));
    static const uint8_t custom_frame[] = { 0x45, 0x89, 0x01, 0x02 };
    expect_sent(custom_frame, 4, CEC_ENGINE_TX_ACKED, 0);
    static const uint8_t refusal[] = { 0x54, 0x00, 0x89, 0x04 };
    answer(refusal, 4, 100);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_REFUSED);
    ASSERT_EQ(r.abort_reason, CEC_REASON_REFUSED);
    ASSERT_TRUE(frame_is(&r.answer, refusal, 4));

    /* No answer asked for, to every device. */
    static const uint8_t standby[] = { 0x36 };
    ASSERT_TRUE(cec_control_raw(15, standby, 1, CEC_ANSWER_NONE));
    static const uint8_t standby_frame[] = { 0x4F, 0x36 };
    expect_sent(standby_frame, 2, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);

    /* Not acknowledged, failed. */
    ASSERT_TRUE(cec_control_raw(3, give, 1, CEC_ANSWER_STANDARD));
    static const uint8_t give3[] = { 0x43, 0x8C };
    expect_sent(give3, 2, CEC_ENGINE_TX_NOT_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_NOT_ACKED);
    ASSERT_TRUE(cec_control_raw(3, give, 1, CEC_ANSWER_STANDARD));
    expect_sent(give3, 2, CEC_ENGINE_TX_FAILED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_FAILED);

    /* Out of range. */
    uint8_t long_data[16] = { 0 };
    ASSERT_TRUE(!cec_control_raw(16, give, 1, CEC_ANSWER_NONE));
    ASSERT_TRUE(!cec_control_raw(0, long_data, 16, CEC_ANSWER_NONE));
    ASSERT_TRUE(!cec_control_raw(0, NULL, 1, CEC_ANSWER_NONE));
    ASSERT_TRUE(!cec_control_raw(0, give, 1, 256));
    ASSERT_TRUE(!cec_control_raw(0, give, 1, -3));
    ASSERT_TRUE(!cec_control_busy());
}

static void test_time_limits(void) {
    cec_control_init();
    /* Acknowledged, and no answer within a second. */
    ASSERT_TRUE(cec_control_power_status(0));
    static const uint8_t give[] = { 0x40, 0x8F };
    expect_sent(give, 2, CEC_ENGINE_TX_ACKED, 1000);
    cec_control_tick(1999);
    ASSERT_TRUE(cec_control_busy());
    cec_control_tick(2000);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_NO_ANSWER);
    ASSERT_EQ(r.power_status, CEC_POWER_STATUS_NONE);

    /* Handed out, and no send result within 4 s. */
    ASSERT_TRUE(cec_control_power_status(0));
    cec_frame_t f;
    ASSERT_TRUE(cec_control_next(OWN, 5000, &f));
    cec_control_tick(8999);
    ASSERT_TRUE(cec_control_busy());
    cec_control_tick(9000);
    ASSERT_EQ(take().result, CEC_RESULT_FAILED);

    /* A result for another frame is not this request's. */
    ASSERT_TRUE(cec_control_power_status(0));
    ASSERT_TRUE(cec_control_next(OWN, 0, &f));
    static const uint8_t other[] = { 0x40, 0x04 };
    const cec_frame_t o = frame_of(other, 2);
    cec_control_on_sent(&o, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_TRUE(cec_control_busy());

    /* CEC stopping ends it. */
    cec_control_stop();
    ASSERT_EQ(take().result, CEC_RESULT_FAILED);
    cec_control_stop();
    ASSERT_TRUE(!cec_control_busy());
}

static void test_power_status(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_power_status(15));
    ASSERT_TRUE(cec_control_power_status(0));
    static const uint8_t give[] = { 0x40, 0x8F };
    expect_sent(give, 2, CEC_ENGINE_TX_ACKED, 0);
    static const uint8_t from_other[] = { 0x34, 0x90, 0x00 };     /* the wrong device */
    answer(from_other, 3, 10);
    ASSERT_TRUE(cec_control_busy());
    static const uint8_t status[] = { 0x04, 0x90, 0x01 };
    answer(status, 3, 20);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.power_status, CEC_POWER_STATUS_STANDBY);
}

static void test_system_info(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_system_info(15));
    ASSERT_TRUE(cec_control_system_info(0));
    static const uint8_t q_phys[] = { 0x40, 0x83 };
    static const uint8_t a_phys[] = { 0x0F, 0x84, 0x00, 0x00, 0x00 };
    static const uint8_t q_version[] = { 0x40, 0x9F };
    static const uint8_t a_version[] = { 0x04, 0x9E, 0x05 };
    static const uint8_t q_vendor[] = { 0x40, 0x8C };
    static const uint8_t a_vendor[] = { 0x0F, 0x87, 0x00, 0x00, 0xF0 };
    static const uint8_t q_language[] = { 0x40, 0x91 };
    static const uint8_t a_language[] = { 0x0F, 0x32, 'e', 'n', 'g' };
    expect_sent(q_phys, 2, CEC_ENGINE_TX_ACKED, 0);
    answer(a_phys, 5, 10);
    expect_sent(q_version, 2, CEC_ENGINE_TX_ACKED, 20);
    answer(a_version, 3, 30);
    expect_sent(q_vendor, 2, CEC_ENGINE_TX_ACKED, 40);
    answer(a_vendor, 5, 50);
    expect_sent(q_language, 2, CEC_ENGINE_TX_ACKED, 60);
    answer(a_language, 5, 70);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_TRUE(frame_is(&r.answer, a_phys, 5));
    ASSERT_EQ(r.phys_addr, 0x0000);
    ASSERT_EQ(r.device_type, CEC_TYPE_TV);
    ASSERT_EQ(r.cec_version, 0x05);
    ASSERT_EQ(r.vendor_id, 0x0000F0);
    ASSERT_TRUE(strcmp(r.language, "eng") == 0);

    /* Another device: no menu language, and a question unanswered stays unanswered. */
    ASSERT_TRUE(cec_control_system_info(5));
    static const uint8_t q5_phys[] = { 0x45, 0x83 };
    static const uint8_t a5_phys[] = { 0x5F, 0x84, 0x20, 0x00, 0x05 };
    static const uint8_t q5_version[] = { 0x45, 0x9F };
    static const uint8_t q5_vendor[] = { 0x45, 0x8C };
    expect_sent(q5_phys, 2, CEC_ENGINE_TX_ACKED, 0);
    answer(a5_phys, 5, 10);
    expect_sent(q5_version, 2, CEC_ENGINE_TX_ACKED, 20);
    cec_control_tick(1020);
    expect_sent(q5_vendor, 2, CEC_ENGINE_TX_ACKED, 1030);
    cec_control_tick(2030);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.phys_addr, 0x2000);
    ASSERT_EQ(r.device_type, CEC_TYPE_AUDIO_SYSTEM);
    ASSERT_EQ(r.cec_version, 0);
    ASSERT_EQ(r.vendor_id, CEC_VENDOR_NONE);
    ASSERT_EQ(r.language[0], 0);

    /* Nobody there: one question. */
    ASSERT_TRUE(cec_control_system_info(9));
    static const uint8_t q9_phys[] = { 0x49, 0x83 };
    expect_sent(q9_phys, 2, CEC_ENGINE_TX_NOT_ACKED, 0);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_NOT_ACKED);
    ASSERT_EQ(r.phys_addr, CEC_PHYS_ADDR_NONE);
}

static void test_osd_names(void) {
    cec_control_init();
    ASSERT_TRUE(cec_control_osd_name(0));
    static const uint8_t give[] = { 0x40, 0x46 };
    static const uint8_t name[] = { 0x04, 0x47, 'T', 'V', 0x01 };
    expect_sent(give, 2, CEC_ENGINE_TX_ACKED, 0);
    answer(name, 5, 10);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_TRUE(strcmp(r.name, "TV?") == 0);

    /* Telling the TV a new name: nothing back is taken as taken. */
    ASSERT_TRUE(!cec_control_tell_osd_name(""));
    ASSERT_TRUE(!cec_control_tell_osd_name("fifteen chars!!"));
    ASSERT_TRUE(!cec_control_tell_osd_name("bad\x01"));
    ASSERT_TRUE(cec_control_tell_osd_name("Wili"));
    static const uint8_t set[] = { 0x40, 0x47, 'W', 'i', 'l', 'i' };
    expect_sent(set, 6, CEC_ENGINE_TX_ACKED, 0);
    cec_control_tick(1000);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_SENT);
    ASSERT_TRUE(strcmp(r.name, "Wili") == 0);

    /* ... and a refusal is REFUSED. */
    ASSERT_TRUE(cec_control_tell_osd_name("Wili"));
    expect_sent(set, 6, CEC_ENGINE_TX_ACKED, 0);
    static const uint8_t refusal[] = { 0x04, 0x00, 0x47, 0x00 };
    answer(refusal, 4, 10);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_REFUSED);
    ASSERT_EQ(r.abort_reason, CEC_REASON_UNRECOGNIZED);
}

static void test_osd_string(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_osd_string(CEC_OSD_SHOW_DEFAULT, ""));
    ASSERT_TRUE(!cec_control_osd_string(CEC_OSD_SHOW_DEFAULT, "fourteen chars"));
    ASSERT_TRUE(!cec_control_osd_string(0x20, "Hi"));
    ASSERT_TRUE(!cec_control_osd_string(CEC_OSD_CLEAR, "Hi"));
    ASSERT_TRUE(cec_control_osd_string(CEC_OSD_SHOW_UNTIL_CLEARED, "Hi"));
    static const uint8_t show[] = { 0x40, 0x64, 0x40, 'H', 'i' };
    expect_sent(show, 5, CEC_ENGINE_TX_ACKED, 0);
    static const uint8_t refusal[] = { 0x04, 0x00, 0x64, 0x04 };
    answer(refusal, 4, 10);
    ASSERT_EQ(take().result, CEC_RESULT_REFUSED);

    ASSERT_TRUE(cec_control_osd_string(CEC_OSD_CLEAR, NULL));
    static const uint8_t clear[] = { 0x40, 0x64, 0x80 };
    expect_sent(clear, 3, CEC_ENGINE_TX_ACKED, 0);
    cec_control_tick(1000);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);
}

static void test_volume(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_volume(21, 0));
    ASSERT_TRUE(!cec_control_volume(-21, 0));
    ASSERT_TRUE(!cec_control_volume(1, 15));
    static const uint8_t up[] = { 0x40, 0x44, 0x41 };
    static const uint8_t released[] = { 0x40, 0x45 };
    ASSERT_TRUE(cec_control_volume(2, 0));
    expect_sent(up, 3, CEC_ENGINE_TX_ACKED, 0);
    expect_sent(released, 2, CEC_ENGINE_TX_ACKED, 10);
    cec_frame_t f;
    ASSERT_TRUE(!cec_control_next(OWN, 309, &f));       /* presses 300 ms apart */
    expect_sent(up, 3, CEC_ENGINE_TX_ACKED, 310);
    expect_sent(released, 2, CEC_ENGINE_TX_ACKED, 320);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_SENT);
    ASSERT_EQ(r.sent, 2);

    /* Down to the audio system; the second press is not acknowledged. */
    static const uint8_t down5[] = { 0x45, 0x44, 0x42 };
    static const uint8_t released5[] = { 0x45, 0x45 };
    ASSERT_TRUE(cec_control_volume(-3, 5));
    expect_sent(down5, 3, CEC_ENGINE_TX_ACKED, 0);
    expect_sent(released5, 2, CEC_ENGINE_TX_ACKED, 0);
    expect_sent(down5, 3, CEC_ENGINE_TX_NOT_ACKED, 300);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_SENT);
    ASSERT_EQ(r.sent, 1);

    /* Mute: one press. */
    static const uint8_t mute[] = { 0x40, 0x44, 0x43 };
    ASSERT_TRUE(cec_control_volume(0, 0));
    expect_sent(mute, 3, CEC_ENGINE_TX_ACKED, 0);
    expect_sent(released, 2, CEC_ENGINE_TX_ACKED, 0);
    r = take();
    ASSERT_EQ(r.sent, 1);

    /* Nobody there. */
    ASSERT_TRUE(cec_control_volume(1, 0));
    expect_sent(up, 3, CEC_ENGINE_TX_NOT_ACKED, 0);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_NOT_ACKED);
    ASSERT_EQ(r.sent, 0);
}

static void test_key(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_key(15, CEC_KEY_UP, 0));
    ASSERT_TRUE(!cec_control_key(0, CEC_KEY_UP, 10001));
    static const uint8_t press[] = { 0x40, 0x44, 0x01 };
    static const uint8_t released[] = { 0x40, 0x45 };
    ASSERT_TRUE(cec_control_key(0, CEC_KEY_UP, 0));
    expect_sent(press, 3, CEC_ENGINE_TX_ACKED, 0);
    expect_sent(released, 2, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);

    /* Held 1000 ms: presses at 0, 400, 800, the release at 1000. */
    cec_frame_t f;
    ASSERT_TRUE(cec_control_key(0, CEC_KEY_UP, 1000));
    expect_sent(press, 3, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_TRUE(!cec_control_next(OWN, 399, &f));
    expect_sent(press, 3, CEC_ENGINE_TX_ACKED, 400);
    expect_sent(press, 3, CEC_ENGINE_TX_ACKED, 800);
    ASSERT_TRUE(!cec_control_next(OWN, 999, &f));
    expect_sent(released, 2, CEC_ENGINE_TX_ACKED, 1000);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);

    /* A first press nobody takes ends it without a release. */
    ASSERT_TRUE(cec_control_key(0, CEC_KEY_UP, 1000));
    expect_sent(press, 3, CEC_ENGINE_TX_NOT_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_NOT_ACKED);
}

static void test_routing(void) {
    cec_control_init();
    ASSERT_TRUE(cec_control_active_source());
    static const uint8_t request[] = { 0x4F, 0x85 };
    static const uint8_t active[] = { 0x8F, 0x82, 0x21, 0x00 };
    expect_sent(request, 2, CEC_ENGINE_TX_ACKED, 0);
    answer(active, 4, 10);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.source, 8);
    ASSERT_EQ(r.phys_addr, 0x2100);

    ASSERT_TRUE(cec_control_stream_path(0x2100));
    static const uint8_t path[] = { 0x4F, 0x86, 0x21, 0x00 };
    expect_sent(path, 4, CEC_ENGINE_TX_ACKED, 0);
    answer(active, 4, 10);
    r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.source, 8);
}

static void test_menu(void) {
    cec_control_init();
    ASSERT_TRUE(!cec_control_menu(4, 3));
    ASSERT_TRUE(!cec_control_menu(15, CEC_MENU_QUERY));
    ASSERT_TRUE(cec_control_menu(8, CEC_MENU_QUERY));
    static const uint8_t request[] = { 0x48, 0x8D, 0x02 };
    static const uint8_t status[] = { 0x84, 0x8E, 0x01 };
    expect_sent(request, 3, CEC_ENGINE_TX_ACKED, 0);
    answer(status, 3, 10);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.menu_state, CEC_MENU_CLOSED);
}

static void test_aborts(void) {
    cec_control_init();
    ASSERT_TRUE(cec_control_abort(0));
    static const uint8_t abort_frame[] = { 0x40, 0xFF };
    static const uint8_t refusal[] = { 0x04, 0x00, 0xFF, 0x04 };
    expect_sent(abort_frame, 2, CEC_ENGINE_TX_ACKED, 0);
    answer(refusal, 4, 10);
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_ANSWERED);
    ASSERT_EQ(r.abort_reason, CEC_REASON_REFUSED);

    ASSERT_TRUE(!cec_control_feature_abort(15, 0x8F, CEC_REASON_REFUSED));
    ASSERT_TRUE(!cec_control_feature_abort(0, 0x8F, 6));
    ASSERT_TRUE(cec_control_feature_abort(0, 0x8F, CEC_REASON_REFUSED));
    static const uint8_t feature_abort[] = { 0x40, 0x00, 0x8F, 0x04 };
    expect_sent(feature_abort, 4, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);
}

static void test_vendor(void) {
    cec_control_init();
    static const uint8_t data[] = { 0xF0, 0x01 };
    ASSERT_TRUE(!cec_control_vendor_command(15, data, 2));
    ASSERT_TRUE(cec_control_vendor_command(0, data, 2));
    static const uint8_t command[] = { 0x40, 0x89, 0xF0, 0x01 };
    expect_sent(command, 4, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);

    uint8_t twelve[12] = { 0 };
    ASSERT_TRUE(!cec_control_vendor_command_with_id(0, 0x1000000, data, 2));
    ASSERT_TRUE(!cec_control_vendor_command_with_id(0, 0x001582, twelve, 12));
    ASSERT_TRUE(cec_control_vendor_command_with_id(15, 0x001582, data, 2));
    static const uint8_t with_id[] = { 0x4F, 0xA0, 0x00, 0x15, 0x82, 0xF0, 0x01 };
    expect_sent(with_id, 7, CEC_ENGINE_TX_ACKED, 0);
    ASSERT_EQ(take().result, CEC_RESULT_SENT);
}

/* Two devices present: the TV at 0 and an audio system at 5; this board (4) skipped. */
static void test_bus_scan(void) {
    cec_control_init();
    cec_scan_entry_t e;
    ASSERT_TRUE(!cec_control_scan_entry(0, &e));
    ASSERT_TRUE(cec_control_bus_scan(1u << OWN));
    uint32_t t = 0;
    for (uint8_t a = 0; a < 15; a++) {
        if (a == OWN) continue;
        const uint8_t poll[] = { (uint8_t)(OWN << 4 | a) };
        if (a != 0 && a != 5) {
            expect_sent(poll, 1, CEC_ENGINE_TX_NOT_ACKED, t);
            continue;
        }
        expect_sent(poll, 1, CEC_ENGINE_TX_ACKED, t);
        const uint8_t q_phys[] = { (uint8_t)(OWN << 4 | a), 0x83 };
        expect_sent(q_phys, 2, CEC_ENGINE_TX_ACKED, t);
        const uint8_t a_phys[] = { (uint8_t)(a << 4 | 15), 0x84, (uint8_t)(a == 0 ? 0x00 : 0x10), 0x00, a };
        answer(a_phys, 5, t);
        const uint8_t q_vendor[] = { (uint8_t)(OWN << 4 | a), 0x8C };
        expect_sent(q_vendor, 2, CEC_ENGINE_TX_ACKED, t);
        const uint8_t a_vendor[] = { (uint8_t)(a << 4 | 15), 0x87, 0x00, 0x00, (uint8_t)(0xF0 + a) };
        answer(a_vendor, 5, t);
        const uint8_t q_name[] = { (uint8_t)(OWN << 4 | a), 0x46 };
        expect_sent(q_name, 2, CEC_ENGINE_TX_ACKED, t);
        if (a == 0) {
            const uint8_t a_name[] = { 0x04, 0x47, 'T', 'V' };
            answer(a_name, 4, t);
        } else {
            t += 1000;
            cec_control_tick(t);                        /* no name from the audio system */
        }
        const uint8_t q_power[] = { (uint8_t)(OWN << 4 | a), 0x8F };
        expect_sent(q_power, 2, CEC_ENGINE_TX_ACKED, t);
        const uint8_t a_power[] = { (uint8_t)(a << 4 | OWN), 0x90, 0x00 };
        answer(a_power, 3, t);
        const uint8_t q_version[] = { (uint8_t)(OWN << 4 | a), 0x9F };
        expect_sent(q_version, 2, CEC_ENGINE_TX_ACKED, t);
        const uint8_t a_version[] = { (uint8_t)(a << 4 | OWN), 0x9E, 0x05 };
        answer(a_version, 3, t);
    }
    cec_reply_t r = take();
    ASSERT_EQ(r.result, CEC_RESULT_SENT);
    ASSERT_EQ(r.found, 2);
    ASSERT_EQ(r.found_mask, (1u << 0) | (1u << 5));

    ASSERT_TRUE(cec_control_scan_entry(0, &e));
    ASSERT_EQ(e.phys_addr, 0x0000);
    ASSERT_EQ(e.device_type, CEC_TYPE_TV);
    ASSERT_EQ(e.vendor_id, 0x0000F0);
    ASSERT_TRUE(strcmp(e.name, "TV") == 0);
    ASSERT_EQ(e.power_status, CEC_POWER_STATUS_ON);
    ASSERT_EQ(e.cec_version, 0x05);
    ASSERT_TRUE(cec_control_scan_entry(5, &e));
    ASSERT_EQ(e.phys_addr, 0x1000);
    ASSERT_EQ(e.device_type, CEC_TYPE_AUDIO_SYSTEM);
    ASSERT_EQ(e.name[0], 0);
    ASSERT_TRUE(!cec_control_scan_entry(3, &e));
    ASSERT_TRUE(!cec_control_scan_entry(OWN, &e));
    ASSERT_TRUE(!cec_control_scan_entry(15, &e));

    /* Another request keeps the scan's devices. */
    ASSERT_TRUE(cec_control_power_status(0));
    cec_control_stop();
    take();
    ASSERT_TRUE(cec_control_scan_entry(0, &e));
}

static void test_capture(void) {
    cec_control_init();
    static const uint8_t ours[] = { 0x04, 0x44, 0x01 };
    static const uint8_t others[] = { 0x08, 0x46 };
    static const uint8_t poll[] = { 0x00 };
    const cec_frame_t f_ours = frame_of(ours, 3);
    const cec_frame_t f_others = frame_of(others, 2);
    const cec_frame_t f_poll = frame_of(poll, 1);
    cec_capture_frame_t c;

    /* Off: nothing kept. */
    cec_control_capture_push(&f_ours, CEC_FRAME_ACKED | CEC_FRAME_FOR_US, 1);
    ASSERT_TRUE(!cec_control_capture_pop(&c));

    /* Ours: frames for this board and frames it sent. */
    cec_control_capture_mode(CEC_CAPTURE_OURS);
    ASSERT_EQ(cec_control_capture_mode_now(), CEC_CAPTURE_OURS);
    cec_control_capture_push(&f_ours, CEC_FRAME_ACKED | CEC_FRAME_FOR_US, 10);
    cec_control_capture_push(&f_others, CEC_FRAME_ACKED, 20);
    cec_control_capture_push(&f_others, CEC_FRAME_SENT, 30);
    ASSERT_TRUE(cec_control_capture_pop(&c));
    ASSERT_EQ(c.time_us, 10);
    ASSERT_EQ(c.flags, CEC_FRAME_ACKED | CEC_FRAME_FOR_US);
    ASSERT_TRUE(frame_is(&c.frame, ours, 3));
    ASSERT_TRUE(cec_control_capture_pop(&c));
    ASSERT_EQ(c.time_us, 30);
    ASSERT_EQ(c.flags, CEC_FRAME_SENT);
    ASSERT_TRUE(!cec_control_capture_pop(&c));

    /* All: everything, polls too; full drops the oldest. */
    cec_control_capture_mode(CEC_CAPTURE_ALL);
    cec_control_capture_push(&f_poll, 0, 0);
    for (uint32_t i = 1; i <= CEC_CONTROL_CAPTURE + 2; i++)
        cec_control_capture_push(&f_others, 0, i);
    ASSERT_EQ(cec_control_capture_dropped(), 3);
    ASSERT_TRUE(cec_control_capture_pop(&c));
    ASSERT_EQ(c.time_us, 3);
    int n = 1;
    while (cec_control_capture_pop(&c)) n++;
    ASSERT_EQ(n, CEC_CONTROL_CAPTURE);
    ASSERT_EQ(c.time_us, CEC_CONTROL_CAPTURE + 2);
    cec_control_capture_clear_dropped();
    ASSERT_EQ(cec_control_capture_dropped(), 0);

    /* A new mode drops what is waiting. */
    cec_control_capture_push(&f_poll, 0, 7);
    cec_control_capture_mode(CEC_CAPTURE_ALL);
    ASSERT_TRUE(!cec_control_capture_pop(&c));
}

static void test_names(void) {
    ASSERT_TRUE(strcmp(cec_control_result_name(CEC_RESULT_ANSWERED), "answered") == 0);
    ASSERT_TRUE(strcmp(cec_control_result_name(CEC_RESULT_NOT_ACKED), "not_acked") == 0);
    ASSERT_TRUE(strcmp(cec_control_result_name((cec_result_t)9), "unknown") == 0);
}

int main(void) {
    test_raw();
    test_time_limits();
    test_power_status();
    test_system_info();
    test_osd_names();
    test_osd_string();
    test_volume();
    test_key();
    test_routing();
    test_menu();
    test_aborts();
    test_vendor();
    test_bus_scan();
    test_capture();
    test_names();
    TEST_RETURN();
}
