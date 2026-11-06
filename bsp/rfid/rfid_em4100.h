// bsp/rfid/rfid_em4100.h — EM4100 encode/decode.
#ifndef RFID_EM4100_H
#define RFID_EM4100_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFID_EM4100_ID_BYTES  5
#define RFID_EM4100_FRAME_BITS 64

// Decode an EM4100 bitstream. Returns true and writes the 5-byte ID on success.
bool rfid_em4100_decode(const uint8_t *bits, unsigned count,
                        uint8_t id_out[RFID_EM4100_ID_BYTES]);

// Encode: given a 5-byte ID, produce the 64-bit frame (one bit per byte).
bool rfid_em4100_encode(const uint8_t id[RFID_EM4100_ID_BYTES],
                        uint8_t bits_out[RFID_EM4100_FRAME_BITS]);

// T5577 config word for an EM4100-compatible tag.
uint32_t rfid_em4100_t5577_config(void);

#ifdef __cplusplus
}
#endif
#endif
