// bsp/rfid/rfid_capture.h — RFID signal capture.
#ifndef RFID_CAPTURE_H
#define RFID_CAPTURE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFID_ADC_CHANNEL    6
#define RFID_ADC_SAMPLE_HZ  250000
#define RFID_ADC_RING_WORDS 8192u
#define RFID_ADC_RING_MASK  (RFID_ADC_RING_WORDS - 1u)

bool             rfid_capture_start(void);
void             rfid_capture_stop(void);
bool             rfid_capture_active(void);
const uint16_t  *rfid_capture_ring(void);
uint32_t         rfid_capture_write_pos(void);

#ifdef __cplusplus
}
#endif
#endif
