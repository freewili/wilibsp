// bsp/rfid/rfid_em4100.c — EM4100 encode/decode.
#include "rfid/rfid_em4100.h"
#include <string.h>

#define HEADER_ONES   9
#define DATA_ROWS     10
#define DATA_COLS     4
#define ROW_STRIDE    5

bool rfid_em4100_decode(const uint8_t *bits, unsigned count,
                        uint8_t id_out[RFID_EM4100_ID_BYTES]) {
    if (count < RFID_EM4100_FRAME_BITS) return false;

    for (unsigned start = 0; start < count; ++start) {
        bool ok = true;
        for (int i = 0; i < HEADER_ONES; ++i)
            if (bits[(start + i) % count] != 1) { ok = false; break; }
        if (!ok) continue;

        uint8_t nibbles[DATA_ROWS];
        unsigned idx = start + HEADER_ONES;
        bool valid = true;

        for (int row = 0; row < DATA_ROWS && valid; ++row) {
            uint8_t nibble = 0, parity = 0;
            for (int col = 0; col < DATA_COLS; ++col) {
                uint8_t bit = bits[(idx++) % count];
                nibble |= (bit << (DATA_COLS - 1 - col));
                parity ^= bit;
            }
            if (bits[(idx++) % count] != parity) { valid = false; break; }
            nibbles[row] = nibble;
        }
        if (!valid) continue;

        for (int col = 0; col < DATA_COLS && valid; ++col) {
            uint8_t cp = 0;
            for (int row = 0; row < DATA_ROWS; ++row)
                cp ^= bits[(start + HEADER_ONES + row * ROW_STRIDE + col) % count];
            if (bits[(idx++) % count] != cp) { valid = false; break; }
        }
        if (!valid) continue;

        if (bits[idx % count] != 0) continue;

        bool erased = true;
        for (int row = 0; row < DATA_ROWS && erased; ++row)
            if (nibbles[row] != 0) erased = false;
        if (erased) continue;

        for (int i = 0; i < RFID_EM4100_ID_BYTES; ++i)
            id_out[i] = (nibbles[i * 2] << 4) | nibbles[i * 2 + 1];
        return true;
    }
    return false;
}

bool rfid_em4100_encode(const uint8_t id[RFID_EM4100_ID_BYTES],
                        uint8_t bits_out[RFID_EM4100_FRAME_BITS]) {
    memset(bits_out, 0, RFID_EM4100_FRAME_BITS);

    unsigned pos = 0;
    for (int i = 0; i < HEADER_ONES; ++i)
        bits_out[pos++] = 1;

    uint8_t col_parity[DATA_COLS];
    memset(col_parity, 0, sizeof(col_parity));

    for (int row = 0; row < DATA_ROWS; ++row) {
        uint8_t nibble = (row & 1)
            ? (id[row / 2] & 0x0F)
            : ((id[row / 2] >> 4) & 0x0F);
        uint8_t row_parity = 0;
        for (int col = 0; col < DATA_COLS; ++col) {
            uint8_t bit = (nibble >> (DATA_COLS - 1 - col)) & 1;
            bits_out[pos++] = bit;
            row_parity ^= bit;
            col_parity[col] ^= bit;
        }
        bits_out[pos++] = row_parity;
    }

    for (int col = 0; col < DATA_COLS; ++col)
        bits_out[pos++] = col_parity[col];

    bits_out[pos++] = 0;

    return pos == RFID_EM4100_FRAME_BITS;
}

uint32_t rfid_em4100_t5577_config(void) {
    return 0x00148040u;
}
