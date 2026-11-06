// bsp/rfid/rfid.c
#include "rfid/rfid.h"
#include "rfid/rfid_carrier.h"
#include "rfid/rfid_capture.h"
#include "rfid/rfid_demod.h"
#include <string.h>

static rfid_state_t s_state = RFID_STATE_IDLE;
static rfid_tag_t   s_pending;
static bool         s_pending_valid;
static uint32_t     s_read_pos;

bool rfid_reader_start(void) {
    if (s_state != RFID_STATE_IDLE) return s_state != RFID_STATE_FAULTED;

    rfid_demod_reset();
    rfid_carrier_start();

    if (!rfid_capture_start()) {
        rfid_carrier_stop();
        s_state = RFID_STATE_FAULTED;
        return false;
    }

    s_read_pos = rfid_capture_write_pos();
    s_pending_valid = false;
    s_state = RFID_STATE_STARTING;
    return true;
}

void rfid_reader_stop(void) {
    rfid_capture_stop();
    rfid_carrier_stop();
    rfid_demod_reset();
    s_state = RFID_STATE_IDLE;
    s_pending_valid = false;
}

void rfid_reader_service(void) {
    if (s_state == RFID_STATE_IDLE || s_state == RFID_STATE_FAULTED) return;
    if (!rfid_capture_active()) return;

    const uint16_t *ring = rfid_capture_ring();
    const uint32_t  wpos = rfid_capture_write_pos();
    const uint32_t  mask = RFID_ADC_RING_MASK;

    uint32_t avail = (wpos - s_read_pos) & mask;
    if (avail > RFID_ADC_RING_WORDS / 2) {
        s_read_pos = (wpos - RFID_ADC_RING_WORDS / 4) & mask;
        avail = RFID_ADC_RING_WORDS / 4;
    }

    while (avail > 0) {
        uint32_t run = RFID_ADC_RING_WORDS - s_read_pos;
        if (run > avail) run = avail;

        rfid_demod_tag_t d;
        if (rfid_demod_feed(ring + s_read_pos, run, &d) && d.valid
            && !s_pending_valid) {
            s_pending.id_len = 5;
            memcpy(s_pending.id, d.id, 5);
            s_pending_valid = true;
        }

        s_read_pos = (s_read_pos + run) & mask;
        avail -= run;
    }

    if (s_state == RFID_STATE_STARTING && rfid_demod_calibrated())
        s_state = RFID_STATE_READING;
}

bool rfid_reader_poll(rfid_tag_t *out) {
    if (!s_pending_valid) return false;
    *out = s_pending;
    s_pending_valid = false;
    return true;
}

int      rfid_reader_state(void)      { return (int)s_state; }
bool     rfid_reader_calibrated(void) { return rfid_demod_calibrated(); }
unsigned rfid_reader_carrier_hz(void)  { return rfid_carrier_hz(); }
