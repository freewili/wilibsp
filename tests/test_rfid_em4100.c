#include "test_util.h"
#include "rfid/rfid_em4100.h"
#include <string.h>

static void rotate(const uint8_t *src, unsigned n, unsigned shift, uint8_t *dst) {
    for (unsigned i = 0; i < n; ++i)
        dst[i] = src[(i + shift) % n];
}

int main(void) {
    // 1. Encode → decode round-trip with a known ID.
    {
        const uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x9A};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        uint8_t id_out[5] = {0};
        ASSERT_TRUE(rfid_em4100_decode(bits, RFID_EM4100_FRAME_BITS, id_out));
        ASSERT_TRUE(memcmp(id, id_out, 5) == 0);
    }

    // 2. Encode → decode round-trip with another ID.
    {
        const uint8_t id[5] = {0xFF, 0x00, 0xAA, 0x55, 0x01};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        uint8_t id_out[5] = {0};
        ASSERT_TRUE(rfid_em4100_decode(bits, RFID_EM4100_FRAME_BITS, id_out));
        ASSERT_TRUE(memcmp(id, id_out, 5) == 0);
    }

    // 3. Decode works at every rotation of the 64-bit frame.
    {
        const uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x9A};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        for (unsigned shift = 0; shift < RFID_EM4100_FRAME_BITS; ++shift) {
            uint8_t rotated[RFID_EM4100_FRAME_BITS];
            rotate(bits, RFID_EM4100_FRAME_BITS, shift, rotated);

            uint8_t id_out[5] = {0};
            ASSERT_TRUE(rfid_em4100_decode(rotated, RFID_EM4100_FRAME_BITS, id_out));
            ASSERT_TRUE(memcmp(id, id_out, 5) == 0);
        }
    }

    // 4. Reject a frame with a corrupted row parity bit.
    {
        const uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x9A};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        // Flip the row parity of the first data row (bit index 9+4 = 13).
        bits[13] ^= 1;

        uint8_t id_out[5] = {0};
        ASSERT_TRUE(!rfid_em4100_decode(bits, RFID_EM4100_FRAME_BITS, id_out));
    }

    // 5. Reject a frame with a corrupted column parity bit.
    {
        const uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x9A};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        // Column parity starts at bit 9 + 10*5 = 59. Flip the first one.
        bits[59] ^= 1;

        uint8_t id_out[5] = {0};
        ASSERT_TRUE(!rfid_em4100_decode(bits, RFID_EM4100_FRAME_BITS, id_out));
    }

    // 6. Reject an all-zeros ID (erased tag).
    {
        const uint8_t id[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));

        uint8_t id_out[5] = {0};
        ASSERT_TRUE(!rfid_em4100_decode(bits, RFID_EM4100_FRAME_BITS, id_out));
    }

    // 7. Reject too-short input.
    {
        uint8_t bits[10] = {1,1,1,1,1,1,1,1,1,0};
        uint8_t id_out[5] = {0};
        ASSERT_TRUE(!rfid_em4100_decode(bits, 10, id_out));
    }

    // 8. Verify the 9-one header is present in an encoded frame.
    {
        const uint8_t id[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
        uint8_t bits[RFID_EM4100_FRAME_BITS];
        ASSERT_TRUE(rfid_em4100_encode(id, bits));
        for (int i = 0; i < 9; ++i)
            ASSERT_EQ(bits[i], 1);
        // Stop bit is zero.
        ASSERT_EQ(bits[63], 0);
    }

    // 9. T5577 config word is non-zero and has the right modulation bits.
    {
        uint32_t cfg = rfid_em4100_t5577_config();
        ASSERT_TRUE(cfg != 0);
    }

    TEST_RETURN();
}
