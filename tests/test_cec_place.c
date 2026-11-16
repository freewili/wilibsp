/* Host tests for bsp/cec/cec_place: the DISPLAY firmware's app-info record,
 * the saved text form, and learning the input from the routes the TV
 * announces. */
#include <stdint.h>
#include <string.h>
#include "test_util.h"
#include "cec/cec_place.h"

static cec_frame_t frame_of(const uint8_t *blocks, int len) {
    cec_frame_t f;
    memset(&f, 0, sizeof f);
    f.len = (uint8_t)len;
    memcpy(f.block, blocks, (size_t)len);
    return f;
}

static void rx(const uint8_t *blocks, int len, uint16_t own, uint32_t now_ms) {
    const cec_frame_t f = frame_of(blocks, len);
    cec_place_on_rx(&f, own, now_ms);
}

/* The record as fwDisplayAppLaunch.cpp writes it: magic, then the address
 * with its complement above it. */
static void test_firmware_record(void) {
    uint16_t pa = 0;
    ASSERT_TRUE(cec_place_from_firmware(0xCEC0A101u, 0xBFFF4000u, &pa));
    ASSERT_EQ(pa, 0x4000);
    ASSERT_TRUE(!cec_place_from_firmware(0xCEC0A102u, 0xBFFF4000u, &pa));   /* another layout */
    ASSERT_TRUE(!cec_place_from_firmware(0, 0xBFFF4000u, &pa));             /* none */
    ASSERT_TRUE(!cec_place_from_firmware(0xCEC0A101u, 0xBFFE4000u, &pa));   /* complement wrong */
    ASSERT_TRUE(!cec_place_from_firmware(0xCEC0A101u, 0x0000FFFFu, &pa));   /* not known */
}

static void test_text(void) {
    char s[8];
    cec_place_format(0x4000, s);
    ASSERT_TRUE(strcmp(s, "4.0.0.0") == 0);
    cec_place_format(0x12AF, s);
    ASSERT_TRUE(strcmp(s, "1.2.A.F") == 0);

    uint16_t pa = 0;
    ASSERT_TRUE(cec_place_parse("4.0.0.0\r\n", 9, &pa));
    ASSERT_EQ(pa, 0x4000);
    ASSERT_TRUE(cec_place_parse(" 1.2.a.f", 8, &pa));
    ASSERT_EQ(pa, 0x12AF);
    ASSERT_TRUE(!cec_place_parse("0.0.0.0", 7, &pa));     /* the TV's own */
    ASSERT_TRUE(!cec_place_parse("F.F.F.F", 7, &pa));
    ASSERT_TRUE(!cec_place_parse("4.0.0", 5, &pa));
    ASSERT_TRUE(!cec_place_parse("4-0-0-0", 7, &pa));
    ASSERT_TRUE(!cec_place_parse("4.0.0.G", 7, &pa));
    ASSERT_TRUE(!cec_place_parse("", 0, &pa));
}

/* The TV switches to input 4, where this board sits on 1.0.0.0, and nobody
 * answers: 4.0.0.0 is learned once the wait runs out, once. */
static void test_learn_unanswered(void) {
    uint16_t pa = 0;
    cec_place_learn_reset();
    const uint8_t change[] = { 0x0F, 0x80, 0x10, 0x00, 0x40, 0x00 };
    rx(change, 6, 0x1000, 100);
    ASSERT_TRUE(!cec_place_learned(100 + CEC_PLACE_ANSWER_MS - 1, &pa));
    ASSERT_TRUE(cec_place_learned(100 + CEC_PLACE_ANSWER_MS, &pa));
    ASSERT_EQ(pa, 0x4000);
    ASSERT_TRUE(!cec_place_learned(100 + 2 * CEC_PLACE_ANSWER_MS, &pa));

    const uint8_t path[] = { 0x0F, 0x86, 0x30, 0x00 };
    rx(path, 4, 0x1000, 0xFFFFFF00u);                      /* across the clock's wrap */
    ASSERT_TRUE(!cec_place_learned(0xFFFFFF00u + 10, &pa));
    ASSERT_TRUE(cec_place_learned(0xFFFFFF00u + CEC_PLACE_ANSWER_MS, &pa));
    ASSERT_EQ(pa, 0x3000);
}

/* A device at the route, or below it, answers: nothing is learned. */
static void test_learn_answered(void) {
    uint16_t pa = 0;
    const uint8_t change[] = { 0x0F, 0x80, 0x10, 0x00, 0x20, 0x00 };
    const uint8_t active[] = { 0x4F, 0x82, 0x20, 0x00 };
    const uint8_t below[] = { 0x5F, 0x81, 0x21, 0x00 };
    const uint8_t elsewhere[] = { 0x4F, 0x82, 0x30, 0x00 };

    cec_place_learn_reset();
    rx(change, 6, 0x1000, 0);
    rx(active, 4, 0x1000, 500);
    ASSERT_TRUE(!cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));

    rx(change, 6, 0x1000, 0);
    rx(below, 4, 0x1000, 500);
    ASSERT_TRUE(!cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));

    rx(change, 6, 0x1000, 0);
    rx(elsewhere, 4, 0x1000, 500);                        /* another input: no answer for 2.0.0.0 */
    ASSERT_TRUE(cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));
    ASSERT_EQ(pa, 0x2000);
}

/* Routes that start no wait: to this board, to the TV itself, from another
 * device than the TV, sent to one device, or of the wrong length. A route to
 * this board ends a wait. */
static void test_learn_ignored(void) {
    uint16_t pa = 0;
    cec_place_learn_reset();
    const uint8_t to_own[] = { 0x0F, 0x86, 0x10, 0x00 };
    const uint8_t to_tv[] = { 0x0F, 0x80, 0x10, 0x00, 0x00, 0x00 };
    const uint8_t from_other[] = { 0x4F, 0x86, 0x20, 0x00 };
    const uint8_t direct[] = { 0x04, 0x86, 0x20, 0x00 };
    const uint8_t short_path[] = { 0x0F, 0x86, 0x20 };
    rx(to_own, 4, 0x1000, 0);
    rx(to_tv, 6, 0x1000, 0);
    rx(from_other, 4, 0x1000, 0);
    rx(direct, 4, 0x1000, 0);
    rx(short_path, 3, 0x1000, 0);
    ASSERT_TRUE(!cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));

    const uint8_t to_two[] = { 0x0F, 0x86, 0x20, 0x00 };
    rx(to_two, 4, 0x1000, 0);
    rx(to_own, 4, 0x1000, 100);
    ASSERT_TRUE(!cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));

    rx(to_two, 4, 0x1000, 0);
    cec_place_learn_reset();
    ASSERT_TRUE(!cec_place_learned(CEC_PLACE_ANSWER_MS, &pa));
}

int main(void) {
    test_firmware_record();
    test_text();
    test_learn_unanswered();
    test_learn_answered();
    test_learn_ignored();
    TEST_RETURN();
}
