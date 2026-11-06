// bsp/rfid/rfid.h — 125 kHz LF RFID reader.
// Call rfid_reader_service() from your main loop.
#ifndef BSP_RFID_H
#define BSP_RFID_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RFID_STATE_IDLE     = 0,
    RFID_STATE_STARTING = 1,
    RFID_STATE_READING  = 2,
    RFID_STATE_FAULTED  = 3,
} rfid_state_t;

#define RFID_TAG_ID_MAX 8

typedef struct {
    uint8_t  id_len;
    uint8_t  id[RFID_TAG_ID_MAX];
} rfid_tag_t;

bool     rfid_reader_start(void);
void     rfid_reader_stop(void);
void     rfid_reader_service(void);
bool     rfid_reader_poll(rfid_tag_t *out);

int      rfid_reader_state(void);
bool     rfid_reader_calibrated(void);
unsigned rfid_reader_carrier_hz(void);

#ifdef __cplusplus
}
#endif
#endif
