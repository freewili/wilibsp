// bsp/cec/cec_control.h — the requests this board makes of other devices on
// the bus, and the capture, for cec.c and the host tests. No hardware and no
// clock: the caller hands in the time, the results of the frames it sent and
// the frames it received, and sends the frames handed out.
//
// One request at a time, as a chain of exchanges: a frame, then the answer
// it waits for, if any. A frame handed out by cec_control_next() must come
// back through cec_control_on_sent() with its result; an answer is matched in
// cec_control_on_rx(). The result codes and the reply are those of cec.h.
//
// Called from the main loop only.
#ifndef CEC_CONTROL_H
#define CEC_CONTROL_H
#include <stdbool.h>
#include <stdint.h>

#include "cec.h"
#include "cec_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// How long a frame handed out may take to report its result, and how long
// an answer is waited for after the frame is acknowledged.
#define CEC_CONTROL_SEND_MS     4000u
#define CEC_CONTROL_ANSWER_MS   1000u
// A held key is pressed again this often (within the 450 ms a follower
// allows, CEC 13.13.3); volume presses are this far apart.
#define CEC_CONTROL_REPEAT_MS   400u
#define CEC_CONTROL_HOLD_MAX_MS 10000u
#define CEC_CONTROL_VOLUME_GAP_MS 300u
#define CEC_CONTROL_VOLUME_MAX  20
#define CEC_CONTROL_CAPTURE     32      // frames the capture keeps; power of two

// No request, no result waiting, capture off and empty: the state at load.
void cec_control_init(void);

// ---- requests: as cec.h, without the CEC-running check --------------------------
bool cec_control_raw(uint8_t destination, const uint8_t *data, int len, int answer);
bool cec_control_power_status(uint8_t destination);
bool cec_control_system_info(uint8_t destination);
bool cec_control_osd_name(uint8_t destination);
// <Set OSD Name> with name (1..14 printable characters) to the TV, a refusal
// awaited; reply.name = name.
bool cec_control_tell_osd_name(const char *name);
bool cec_control_osd_string(uint8_t display_control, const char *text);
bool cec_control_volume(int steps, uint8_t destination);
bool cec_control_key(uint8_t destination, uint8_t ui_command, uint32_t hold_ms);
bool cec_control_active_source(void);
bool cec_control_stream_path(uint16_t phys_addr);
bool cec_control_menu(uint8_t destination, uint8_t menu_request);
bool cec_control_abort(uint8_t destination);
bool cec_control_feature_abort(uint8_t destination, uint8_t opcode, uint8_t reason);
bool cec_control_vendor_command(uint8_t destination, const uint8_t *data, int len);
bool cec_control_vendor_command_with_id(uint8_t destination, uint32_t vendor_id, const uint8_t *data, int len);
// skip_mask: bit n set = address n is not polled (this board's own).
bool cec_control_bus_scan(uint16_t skip_mask);

bool cec_control_busy(void);
bool cec_control_result(cec_reply_t *out);
bool cec_control_scan_entry(uint8_t address, cec_scan_entry_t *out);

// ---- driving the request ---------------------------------------------------------
// The next frame to send, from initiator, when one is due at now_ms.
bool cec_control_next(uint8_t initiator, uint32_t now_ms, cec_frame_t *out);
// The result of a frame cec_control_next() handed out.
void cec_control_on_sent(const cec_frame_t *frame, cec_engine_tx_result_t result, uint32_t now_ms);
// A frame for this board (or a broadcast).
void cec_control_on_rx(const cec_frame_t *frame, uint32_t now_ms);
// The time limits.
void cec_control_tick(uint32_t now_ms);
// CEC stopped: a request running ends CEC_RESULT_FAILED.
void cec_control_stop(void);

// ---- capture ----------------------------------------------------------------------
// The mode, as cec.h; what is waiting is dropped and the drop count cleared.
void cec_control_capture_mode(cec_capture_mode_t mode);
cec_capture_mode_t cec_control_capture_mode_now(void);
// A frame taken in (flags CEC_FRAME_*): kept when the mode wants it. Full:
// the oldest is dropped and counted.
void cec_control_capture_push(const cec_frame_t *frame, uint8_t flags, uint32_t time_us);
bool cec_control_capture_pop(cec_capture_frame_t *out);
uint32_t cec_control_capture_dropped(void);
void cec_control_capture_clear_dropped(void);

const char *cec_control_result_name(cec_result_t result);

#ifdef __cplusplus
}
#endif
#endif
