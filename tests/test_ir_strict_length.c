// A capture ends when a gap closes it, so a decoded frame has exactly the
// edge count its protocol defines. Anything longer is a second burst, an echo
// or line noise sharing the capture, and decoding the prefix anyway reports a
// clean result for a frame that was never clean.
#include "test_util.h"
#include "ir_synth.h"
#include "ir_decode.h"
#include "ir_encode.h"

typedef bool (*decoder_fn)(const uint32_t *, uint32_t, ir_message_t *);

static void reject_trailing(decoder_fn decode, ir_message_t in,
                            uint32_t expect_n) {
    uint32_t t[IR_MAX_TIMINGS];
    ir_message_t m;

    uint32_t n = ir_encode(&in, t, IR_MAX_TIMINGS);
    ASSERT_EQ(n, expect_n);
    ASSERT_TRUE(decode(t, n, &m));
    ASSERT_EQ(m.protocol, in.protocol);
    ASSERT_EQ(m.address, in.address);
    ASSERT_EQ(m.command, in.command);

    // One extra edge past the stop mark.
    t[n] = 560;
    ASSERT_TRUE(!decode(t, n + 1, &m));

    // A whole spurious burst appended.
    for (uint32_t i = 0; i < 20; i++) t[n + i] = (i & 1u) ? 1690 : 560;
    ASSERT_TRUE(!decode(t, n + 20, &m));

    // Losing the stop mark is still a short frame.
    ASSERT_TRUE(!decode(t, n - 1, &m));
}

int main(void) {
    uint32_t t[IR_MAX_TIMINGS];
    ir_message_t m;

    reject_trailing(ir_decode_nec,
                    (ir_message_t){IR_PROTO_NEC, 0x04, 0x08, false}, 67);
    reject_trailing(ir_decode_samsung32,
                    (ir_message_t){IR_PROTO_SAMSUNG32, 0x07, 0x02, false}, 67);
    reject_trailing(ir_decode_rca,
                    (ir_message_t){IR_PROTO_RCA, 0x0A, 0x35, false}, 51);
    reject_trailing(ir_decode_kaseikyo,
                    (ir_message_t){IR_PROTO_KASEIKYO, 0x2002, 0x30D, false}, 99);

    // The NEC repeat frame is three edges and must keep decoding: it is
    // shorter than a data frame by design, not a truncated one.
    t[0] = 9000; t[1] = 2250; t[2] = 560;
    ASSERT_TRUE(ir_decode_nec(t, 3, &m));
    ASSERT_EQ(m.protocol, IR_PROTO_NEC);
    ASSERT_TRUE(m.repeat);

    // A repeat frame with an extra edge is not a repeat frame.
    t[3] = 560;
    ASSERT_TRUE(!ir_decode_nec(t, 4, &m));

    // The dispatcher must not report a match for a padded frame either.
    ir_message_t in = {IR_PROTO_NEC, 0x04, 0x08, false};
    uint32_t n = ir_encode(&in, t, IR_MAX_TIMINGS);
    for (uint32_t i = 0; i < 20; i++) t[n + i] = (i & 1u) ? 1690 : 560;
    ASSERT_TRUE(!ir_decode(t, n + 20, &m));
    ASSERT_EQ(ir_decode_all(t, n + 20, &m, 1), 0u);
    TEST_RETURN();
}
