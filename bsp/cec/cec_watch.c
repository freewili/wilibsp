// bsp/cec/cec_watch.c — see cec_watch.h.
#include "cec_watch.h"

#include <string.h>

#define OP_USER_CONTROL_PRESSED 0x44    // [UI Command]
#define OP_ROUTING_CHANGE       0x80    // [Original Address][New Address], broadcast
#define OP_SET_STREAM_PATH      0x86    // [Physical Address], broadcast
#define OP_VENDOR_COMMAND       0x89
#define OP_VENDOR_BUTTON_DOWN   0x8A
#define OP_VENDOR_BUTTON_UP     0x8B
#define OP_VENDOR_COMMAND_ID    0xA0

static bool s_accept_vendor;

// The key the device holds down.
static bool s_key_down;
static uint8_t s_key;
static uint8_t s_key_from;

void cec_watch_init(void) { s_key_down = false; }

void cec_watch_accept_vendor(bool on) { s_accept_vendor = on; }

bool cec_watch_accepts_vendor(void) { return s_accept_vendor; }

static bool is_vendor(uint8_t opcode) {
    return opcode == OP_VENDOR_COMMAND || opcode == OP_VENDOR_BUTTON_DOWN || opcode == OP_VENDOR_BUTTON_UP ||
           opcode == OP_VENDOR_COMMAND_ID;
}

void cec_watch_on_key(bool down, uint8_t key, uint8_t from) {
    s_key_down = down;
    s_key = key;
    s_key_from = from;
}

static void event_of(const cec_frame_t *frame, cec_event_type_t type, uint8_t data, cec_event_t *event) {
    memset(event, 0, sizeof *event);
    event->type = type;
    event->data = data;
    event->from = (uint8_t)(frame->block[0] >> 4);
    event->to = (uint8_t)(frame->block[0] & 0x0Fu);
}

bool cec_watch_on_rx(const cec_frame_t *frame, cec_event_t *event, bool *for_device) {
    *for_device = true;
    if (frame->len < 2) return false;
    const uint8_t opcode = frame->block[1];
    const bool broadcast = (frame->block[0] & 0x0Fu) == CEC_LOGICAL_ALL;

    if (s_accept_vendor && is_vendor(opcode)) {
        *for_device = false;
        event_of(frame, CEC_EV_VENDOR, opcode, event);
        event->frame = *frame;
        return true;
    }

    // The device's key state is the one before this frame: a press of the
    // held key from its device is a repeat; anything else it reports itself.
    if (opcode == OP_USER_CONTROL_PRESSED && frame->len >= 3 && !broadcast && s_key_down &&
        frame->block[2] == s_key && (frame->block[0] >> 4) == s_key_from) {
        event_of(frame, CEC_EV_KEY_REPEAT, s_key, event);
        return true;
    }

    // The address a route now leads to: the new address of <Routing Change>,
    // the address of <Set Stream Path>.
    int at = -1;
    if (broadcast && opcode == OP_ROUTING_CHANGE && frame->len == 6) at = 4;
    if (broadcast && opcode == OP_SET_STREAM_PATH && frame->len == 4) at = 2;
    if (at < 0) return false;
    event_of(frame, CEC_EV_ROUTING, opcode, event);
    event->extra[0] = frame->block[at];
    event->extra[1] = frame->block[at + 1];
    return true;
}
