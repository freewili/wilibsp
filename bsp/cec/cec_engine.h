// bsp/cec/cec_engine.h — C entry points to the CEC bit engine (fwCECWire)
// and the device (fwCECCore) for cec.c and the host tests. No hardware
// access: the caller owns the pin and the alarm, reports every edge and
// alarm, and applies the cec_wire_out_t that comes back.
//
// One engine and one device, held in this module. The handler entries
// (cec_engine_on_*) and the two called with the handlers held off
// (cec_engine_wire_send, cec_engine_wire_cancel) are placed in RAM on the
// target and call nothing in flash.
#ifndef CEC_ENGINE_H
#define CEC_ENGINE_H
#include <stdbool.h>
#include <stdint.h>

#include "cec.h"            // cec_frame_t

#ifdef __cplusplus
extern "C" {
#endif

#define CEC_ENGINE_FRAME_MAX       16   // header + opcode + operands
#define CEC_ENGINE_OUT_FRAMES_MAX  6
#define CEC_ENGINE_OUT_EVENTS_MAX  4
#define CEC_ENGINE_ADDR_NONE       15   // no logical address; also the broadcast destination
#define CEC_ENGINE_ADDR_TV         0
#define CEC_ENGINE_PHYS_ADDR_NONE  0xFFFFu

// What the caller applies after every engine call: hold the line low or
// release it, and when to call cec_engine_on_alarm() next.
typedef struct {
    bool     drive_low;
    bool     alarm;
    uint32_t alarm_us;
} cec_wire_out_t;

// Flags of a received frame.
#define CEC_ENGINE_RX_ACKED   0x01      // every block acknowledged; a broadcast: not rejected
#define CEC_ENGINE_RX_FOR_US  0x02      // a broadcast, or addressed to an address the device holds

// Result of a frame sent, after the engine's own retries.
typedef enum {
    CEC_ENGINE_TX_ACKED = 0,            // a broadcast: nobody rejected it
    CEC_ENGINE_TX_NOT_ACKED,
    CEC_ENGINE_TX_FAILED,               // arbitration lost or a line error on every attempt
} cec_engine_tx_result_t;

// Events of the device, the values of its own event list.
typedef enum {
    CEC_ENGINE_EV_ADDRESS_CLAIMED = 0,  // own = the logical address taken
    CEC_ENGINE_EV_ADDRESS_NONE,         // no free address: the device stays at 15
    CEC_ENGINE_EV_STANDBY,              // <Standby> received
    CEC_ENGINE_EV_ACTIVE_SOURCE,        // data[0] 1: this device became the active source; 0: it stopped being it
    CEC_ENGINE_EV_KEY_DOWN,             // data[0] = [UI Command]
    CEC_ENGINE_EV_KEY_UP,               // data[0] = [UI Command]; also after the 550 ms follower timeout
    CEC_ENGINE_EV_MENU_REQUEST,         // data[0] = [Menu Request Type]
    CEC_ENGINE_EV_MENU_LANGUAGE,        // data[0..2] = the TV's ISO 639-2 language code
    CEC_ENGINE_EV_DECK_PLAY,            // data[0] = [Play Mode]
    CEC_ENGINE_EV_DECK_CONTROL,         // data[0] = [Deck Control Mode]
    CEC_ENGINE_EV_FEATURE_ABORT,        // peer refused a message of ours: data[0] = its opcode, data[1] = the reason
} cec_engine_event_type_t;

typedef struct {
    uint8_t type;                       // cec_engine_event_type_t; later values are other device events
    uint8_t peer;                       // the other device; 15 = none
    uint8_t own;                        // this device's address the event concerns; 15 = none
    uint8_t data[3];
} cec_engine_event_t;

// Frames to send, in order, and events. A call appends; the caller clears.
typedef struct {
    uint8_t            frames;
    uint8_t            events;
    bool               overflow;        // something did not fit and was dropped
    cec_frame_t        frame[CEC_ENGINE_OUT_FRAMES_MAX];
    cec_engine_event_t event[CEC_ENGINE_OUT_EVENTS_MAX];
} cec_engine_out_t;

typedef struct {
    uint32_t rx_frames;                 // complete frames seen on the line
    uint32_t rx_errors;
    uint32_t rx_overruns;
    uint32_t error_signals;
    uint32_t tx_retries;
    uint32_t arbitration_lost;
    uint32_t line_errors;
    uint32_t pull_not_seen;             // lows this device pulled that the line did not show
    uint32_t late_aborts;               // sends abandoned by an alarm served too late
    uint32_t rx_dropped;                // frames dropped silently: their times could not be trusted
} cec_engine_wire_counts_t;

// Past this lateness the engine abandons a send (late_aborts) or drops a
// frame being received (rx_dropped) instead of putting a wrong bit on the line.
#define CEC_ENGINE_LATE_MAX_US 200u

static inline void cec_engine_out_clear(cec_engine_out_t *out) {
    out->frames = 0;
    out->events = 0;
    out->overflow = false;
}

// ---- engine: main loop ----------------------------------------------------
// The line counts as busy until now_us.
void cec_engine_wire_init(uint32_t now_us);
void cec_engine_wire_set_ack_mask(uint16_t mask);
// True: every frame on the line is handed over, polls and frames for other
// devices included. cec_engine_wire_init() turns it off.
void cec_engine_wire_set_monitor_all(bool monitor_all);
bool cec_engine_wire_pop_rx(cec_frame_t *frame, uint8_t *flags);
bool cec_engine_wire_pop_tx_done(cec_frame_t *frame, cec_engine_tx_result_t *result);
void cec_engine_wire_counts(cec_engine_wire_counts_t *out);
void cec_engine_wire_clear_counts(void);

// ---- engine: main loop, with the two handlers held off ---------------------
// False while a frame is already waiting or being sent, or for a bad length.
bool cec_engine_wire_send(const cec_frame_t *frame, uint32_t now_us, cec_wire_out_t *out);
// Gives up a frame still waiting for the line (reported Failed). False once it is being sent.
bool cec_engine_wire_cancel(cec_wire_out_t *out);

// ---- engine: handlers -------------------------------------------------------
void cec_engine_on_edge(bool falling, uint32_t now_us, cec_wire_out_t *out);
void cec_engine_on_edge_pair(bool first_falling, uint32_t now_us, cec_wire_out_t *out);
void cec_engine_on_alarm(uint32_t now_us, bool line_high, cec_wire_out_t *out);

// ---- device ------------------------------------------------------------------
// One device of type first_type, and of second_type unless that is
// CEC_TYPE_NONE (cec.h), CEC 1.4, every standard answer, vendor_id
// (CEC_VENDOR_NONE: none), OSD name osd_name (up to 14 printable characters;
// NULL or none printable: "Freewili 2").
void cec_engine_device_init(const char *osd_name, uint32_t vendor_id, uint8_t first_type, uint8_t second_type);
// The name and the vendor ID of the device running now, as init takes them.
void cec_engine_device_set_osd_name(const char *osd_name);
void cec_engine_device_set_vendor_id(uint32_t vendor_id);
// The name in use, terminated: out holds 15 characters.
void cec_engine_device_osd_name(char *out);
// The claim is over (a device type may still be at address 15).
bool cec_engine_device_ready(void);
// Starts the address claim. CEC_ENGINE_PHYS_ADDR_NONE: no claim, address 15.
void cec_engine_device_start(uint16_t phys_addr, cec_engine_out_t *out);
// Gives up the address; an active source says <Inactive Source> first.
void cec_engine_device_stop(cec_engine_out_t *out);
void cec_engine_device_on_tx_done(const cec_frame_t *sent, cec_engine_tx_result_t result, cec_engine_out_t *out);
void cec_engine_device_on_rx(const cec_frame_t *frame, uint32_t now_ms, cec_engine_out_t *out);
void cec_engine_device_on_tick(uint32_t now_ms, cec_engine_out_t *out);
// Bit n set = logical address n is held: what the engine acknowledges.
uint16_t cec_engine_device_address_mask(void);
// The address frames leave from: the claimed one, else 15.
uint8_t cec_engine_device_address(void);
// <Image View On> to the TV, then <Active Source> (at address 15 only the first).
bool cec_engine_device_one_touch_play(cec_engine_out_t *out);
// <Standby> to destination (CEC_ENGINE_ADDR_NONE: to every device).
bool cec_engine_device_standby(uint8_t destination, cec_engine_out_t *out);

// Names of the core's tables; "unknown" past them.
const char *cec_engine_key_name(uint8_t ui_command);
const char *cec_engine_opcode_name(uint8_t opcode);
const char *cec_engine_device_type_name(uint8_t device_type);
const char *cec_engine_power_status_name(uint8_t power_status);
const char *cec_engine_abort_reason_name(uint8_t reason);

#ifdef __cplusplus
}
#endif
#endif
