// bsp/rfid/rfid_demod.h — RFID signal demodulator.
#ifndef RFID_DEMOD_H
#define RFID_DEMOD_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFID_DEMOD_BIT_PERIOD_US  512
#define RFID_DEMOD_MAX_BITS       128

typedef struct {
    uint8_t  id[5];
    bool     valid;
} rfid_demod_tag_t;

void rfid_demod_reset(void);
bool rfid_demod_feed(const uint16_t *samples, uint32_t count,
                     rfid_demod_tag_t *out);
bool rfid_demod_calibrated(void);

#ifdef __cplusplus
}
#endif
#endif
