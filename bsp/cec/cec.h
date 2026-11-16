// bsp/cec/cec.h — CEC on the TV connector. The board joins the bus as a
// playback device (and, when asked, a second device type): it claims a
// logical address, answers what every device owes, hands the TV remote's keys
// and the TV's standby to the app, and can wake the TV and switch it to this
// input, or put it in standby.
//
// Past that device, the app can ask other devices on the bus: one request at
// a time, started by a cec_request_*() call and carried out by cec_task(); its
// outcome is taken with cec_request_result(). And it can watch the bus: a
// capture keeps the frames for this board, or every frame on the cable, with
// a timestamp and the acknowledge.
//
// The wire is GPIO31 (PIN_CEC) used open drain: the pad's output value
// stays 0 and its output enable pulls the wire low or lets it go.
//
// Interrupts: GPIO31's two edges through a raw handler on IO_IRQ_BANK0, and
// one TIMER1 alarm, both at PICO_HIGHEST_IRQ_PRIORITY and both in RAM.
// IO_IRQ_BANK0 is one vector for every pin of the core, so its priority is
// raised for every GPIO interrupt user on that core, from the first
// cec_start() on. Another GPIO interrupt user must use a raw or shared
// handler and leave GPIO31's bits alone.
//
// Timing: a handler may run about 160 us after its cause before a bit leaves
// the limits of the standard. Code that holds interrupts off longer than
// that, or another interrupt at the highest priority that runs that long,
// makes the engine give up: a send is abandoned and sent again (late_aborts),
// a frame being received is dropped (rx_dropped).
//
// Everything here is called from the main loop of the core that called
// cec_start(). Call after board_init().
#ifndef CEC_H
#define CEC_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The physical address used when nothing better is known: input 1 of the TV
// (1.0.0.0). The real one is in the TV's EDID, which only MAIN reads; see
// cec_address_source_t for where a better one comes from.
#define CEC_PHYS_ADDR_DEFAULT 0x1000u
#define CEC_PHYS_ADDR_NONE    0xFFFFu   // no place in the connection tree; also "not answered"

// Logical addresses (CEC 10.2). A direct request goes to 0..14; 15 as a
// destination is every device.
#define CEC_LOGICAL_TV            0
#define CEC_LOGICAL_RECORDER_1    1
#define CEC_LOGICAL_TUNER_1       3
#define CEC_LOGICAL_PLAYBACK_1    4
#define CEC_LOGICAL_AUDIO_SYSTEM  5
#define CEC_LOGICAL_ALL           15    // as a destination: every device; as an address held: none

// [Device Type]: what this board presents itself as (cec_set_device_types())
// and what a device reports (cec_reply_t, cec_scan_entry_t).
#define CEC_TYPE_TV               0
#define CEC_TYPE_RECORDER         1
#define CEC_TYPE_TUNER            3
#define CEC_TYPE_PLAYBACK         4
#define CEC_TYPE_AUDIO_SYSTEM     5
#define CEC_TYPE_NONE             0xFF  // no second device type; also "not answered"

// The vendor ID this board reports until the app sets another: a 24-bit IEEE
// company ID. CEC_VENDOR_NONE: none is reported; also "not answered".
#define CEC_VENDOR_ID_DEFAULT     0x001582u
#define CEC_VENDOR_NONE           0xFFFFFFFFu

// [UI Command] values of the keys a TV remote sends most.
#define CEC_KEY_SELECT        0x00
#define CEC_KEY_UP            0x01
#define CEC_KEY_DOWN          0x02
#define CEC_KEY_LEFT          0x03
#define CEC_KEY_RIGHT         0x04
#define CEC_KEY_ROOT_MENU     0x09
#define CEC_KEY_EXIT          0x0D
#define CEC_KEY_POWER         0x40
#define CEC_KEY_VOLUME_UP     0x41
#define CEC_KEY_VOLUME_DOWN   0x42
#define CEC_KEY_MUTE          0x43
#define CEC_KEY_PLAY          0x44
#define CEC_KEY_STOP          0x45
#define CEC_KEY_PAUSE         0x46
#define CEC_KEY_F1_BLUE       0x71
#define CEC_KEY_F2_RED        0x72
#define CEC_KEY_F3_GREEN      0x73
#define CEC_KEY_F4_YELLOW     0x74

// [Menu Request Type] of cec_request_menu(), and the [Menu State] answered.
#define CEC_MENU_ACTIVATE     0
#define CEC_MENU_DEACTIVATE   1
#define CEC_MENU_QUERY        2
#define CEC_MENU_OPEN         0     // menu_state: activated
#define CEC_MENU_CLOSED       1     // menu_state: deactivated

// [Display Control] of cec_request_osd_string().
#define CEC_OSD_SHOW_DEFAULT        0x00    // for the TV's default time
#define CEC_OSD_SHOW_UNTIL_CLEARED  0x40
#define CEC_OSD_CLEAR               0x80    // no text

// [Abort Reason] of <Feature Abort>.
#define CEC_REASON_UNRECOGNIZED     0       // unrecognized opcode
#define CEC_REASON_WRONG_MODE       1       // not in the right mode to respond
#define CEC_REASON_NO_SOURCE        2       // cannot provide the source
#define CEC_REASON_INVALID_OPERAND  3
#define CEC_REASON_REFUSED          4
#define CEC_REASON_UNDETERMINED     5       // unable to determine

// [Power Status] answered.
#define CEC_POWER_STATUS_ON         0
#define CEC_POWER_STATUS_STANDBY    1
#define CEC_POWER_STATUS_TO_ON      2       // standby to on
#define CEC_POWER_STATUS_TO_STANDBY 3       // on to standby
#define CEC_POWER_STATUS_NONE       0xFF    // not answered

// One frame: block 0 the header (initiator high nibble, destination low),
// block 1 the opcode, then the operands. One block alone is a poll.
#define CEC_FRAME_BLOCKS 16
typedef struct {
    uint8_t len;
    uint8_t block[CEC_FRAME_BLOCKS];
} cec_frame_t;

typedef enum {
    CEC_EV_KEY_DOWN,        // data = [UI Command]; from = the remote's device, to = this board's address it was sent to
    CEC_EV_KEY_UP,          // data = [UI Command]; extra[0] 0: a release (or another key) ended it,
                            // 1: no release came, 550 ms after the last press or repeat
    CEC_EV_STANDBY,         // the TV (or another device, from) sent <Standby>
    CEC_EV_ACTIVE_SOURCE,   // data 1: the TV switched to this device; 0: another source took over
    CEC_EV_ADDRESS,         // data = the logical address claimed, 15: none free; extra[0] = the device type
    CEC_EV_MENU_REQUEST,    // data = [Menu Request Type] (CEC_MENU_*) from `from`; <Menu Status> is answered already
    CEC_EV_MENU_LANGUAGE,   // data, extra[0], extra[1] = the TV's menu language, ISO 639-2 ("eng")
    CEC_EV_DECK_PLAY,       // a playback or recorder address got <Play>; data = [Play Mode]
    CEC_EV_DECK_CONTROL,    // ... <Deck Control>; data = [Deck Control Mode]
    CEC_EV_FEATURE_ABORT,   // `from` refused a message of this board: data = its opcode, extra[0] = the reason
    CEC_EV_ROUTING,         // `from` announced a route: data = 0x80 <Routing Change> or 0x86 <Set Stream Path>;
                            // extra[0], extra[1] = the physical address it now leads to, high byte first
    CEC_EV_KEY_REPEAT,      // data = [UI Command]: the held key's device pressed it again (a held key repeats
                            // within 450 ms, CEC 13.13.3); none come for a tap
    CEC_EV_VENDOR,          // only after cec_accept_vendor_frames(true): data = the opcode (0x89, 0x8A, 0x8B,
                            // 0xA0); frame = the whole frame, header first
} cec_event_type_t;

typedef struct {
    cec_event_type_t type;
    uint8_t data;
    uint8_t from;           // the other device's logical address; 15: none
    uint8_t to;             // this board's logical address the event concerns; 15: none, or a broadcast
    uint8_t extra[2];
    uint32_t time_ms;       // to_ms_since_boot() when cec_task() made the event
    cec_frame_t frame;      // CEC_EV_VENDOR: the frame received; len 0 for the other types
} cec_event_t;

typedef struct {
    uint32_t rx_frames;         // complete frames seen on the wire
    uint32_t rx_errors;
    uint32_t rx_overruns;
    uint32_t error_signals;
    uint32_t tx_acked;
    uint32_t tx_not_acked;
    uint32_t tx_failed;
    uint32_t tx_dropped;        // frames the send queue had no room for
    uint32_t tx_retries;
    uint32_t arbitration_lost;
    uint32_t line_errors;
    uint32_t alarms;            // alarms that fired on their time
    uint32_t late_100us;        // ... 100 us late or more
    uint32_t late_300us;        // ... 300 us late or more
    uint32_t late_max_us;
    uint32_t pull_not_seen;     // lows this device pulled that the wire did not show
    uint32_t late_aborts;       // sends abandoned by an alarm served too late
    uint32_t rx_dropped;        // frames dropped silently: their times could not be trusted
    uint32_t events_dropped;    // events the app had not taken when the queue was full
    uint32_t capture_dropped;   // captured frames not taken, dropped oldest first
} cec_stats_t;

// Starts the device on the next cec_task(): the address claim, then the
// standard answers. osd_name is the name in the TV's input list, up to 14
// printable characters; NULL: "Freewili 2". False, with cec_last_error()
// naming the cause, if TIMER1 is not counting microseconds. True also when
// it already runs (the name then applies from the next start).
bool cec_start(const char *osd_name);

// Leaves the wire: an active source says <Inactive Source> first. The queued
// frames go out on the following cec_task() calls (at most 1.5 s), then the
// wire is released; cec_running() turns false. A request still running then
// ends CEC_RESULT_FAILED.
void cec_stop(void);

// True from the first cec_task() after cec_start() until the wire is released.
bool cec_running(void);

// The cause of the last refused start, or "".
const char *cec_last_error(void);

// This device's place in the connection tree, A.B.C.D as 0xABCD. A change
// while running claims the address again. Set by the app, it is final: the
// firmware's, saved and learned addresses no longer replace it.
void cec_set_physical_address(uint16_t phys_addr);
uint16_t cec_physical_address(void);

// Where the physical address in use came from. A source replaces the address
// only when it ranks the same or higher, in this order:
typedef enum {
    CEC_ADDRESS_DEFAULT = 0,    // CEC_PHYS_ADDR_DEFAULT: nothing better known
    CEC_ADDRESS_SAVED,          // the setting on the SD card, through cec_use_saved_address()
    CEC_ADDRESS_LEARNED,        // the TV switched to an input no other device answered for
                                // within 2 s; the board then says <Active Source>
    CEC_ADDRESS_FIRMWARE,       // the DISPLAY firmware's, from the TV's EDID, handed over at
                                // the app's launch; read at the first cec_start()
    CEC_ADDRESS_APP,            // cec_set_physical_address()
} cec_address_source_t;
cec_address_source_t cec_physical_address_source(void);
const char *cec_address_source_name(cec_address_source_t source);  // "default", "saved", ...; "unknown"

// The address saved on the SD card (cec_saved_onewili.h reads it). Used unless
// a higher-ranked source gave one.
void cec_use_saved_address(uint16_t phys_addr);

// True once per address learned, with it in *phys_addr: for saving it
// (cec_saved_onewili.h does).
bool cec_take_learned_address(uint16_t *phys_addr);

// What the board presents itself as: first is CEC_TYPE_PLAYBACK,
// CEC_TYPE_RECORDER, CEC_TYPE_TUNER or CEC_TYPE_AUDIO_SYSTEM; second one of
// those or CEC_TYPE_NONE. Each claims a logical address of its own (an audio
// system: 5 only). Default: playback alone. False, nothing changed, for
// another value, the same type twice, or playback with recorder (a recorder
// does not also take a playback address, CEC 10.2.1). A change while running
// claims the addresses again. Requests leave from the first address held.
bool cec_set_device_types(uint8_t first, uint8_t second);

// The vendor ID answered to <Give Device Vendor ID> and announced after the
// claim; CEC_VENDOR_NONE: none, and <Give Device Vendor ID> is refused.
// Applies at once and to later starts. Default CEC_VENDOR_ID_DEFAULT.
void cec_set_vendor_id(uint32_t vendor_id);

// Every main-loop pass: carries frames between the wire, the device, the
// request running and the capture.
void cec_task(void);

// The next event, oldest first. False when none is waiting. Events past 16
// not taken are dropped and counted (events_dropped).
bool cec_next_event(cec_event_t *out);

// Vendor frames addressed to this board (or a broadcast <Vendor Command With
// ID>): <Vendor Command>, <Vendor Remote Button Down>, <Vendor Remote Button
// Up>, <Vendor Command With ID>. false (the default): the device answers the
// directly addressed ones with <Feature Abort> [Unrecognized opcode], which
// may make a TV stop sending them. true: they are acknowledged on the wire,
// left unanswered and handed to the app as CEC_EV_VENDOR. Applies at once
// and across starts.
void cec_accept_vendor_frames(bool on);

// One Touch Play: <Image View On> to the TV, then <Active Source> naming this
// device's physical address. False while not running or the send queue is full.
bool cec_wake_tv(void);

// <Standby> to the TV. False while not running or the send queue is full.
bool cec_tv_standby(void);

// The logical address requests leave from; 15 while none is held (claiming,
// none free, stopped).
uint8_t cec_logical_address(void);
// Bit n set: logical address n is held, one per device type claimed.
uint16_t cec_logical_addresses(void);

// ---- requests ---------------------------------------------------------------
// One at a time. A cec_request_*() call starts one and returns true, or
// returns false and starts nothing: CEC not running, a request still running,
// the send queue short of room, or an argument out of range. cec_task() sends
// its frames and waits for the answers; cec_request_result() hands over the
// outcome, once. A follower answers within a second (CEC 9.2): a request
// waits that long for each answer, and up to 4 s for each frame to get onto
// the wire.

typedef enum {
    CEC_RESULT_ANSWERED = 0,    // the answer came (cec_reply_t.answer)
    CEC_RESULT_SENT,            // acknowledged; no answer asked for (a broadcast: nobody rejected it)
    CEC_RESULT_REFUSED,         // the destination answered <Feature Abort> (abort_reason)
    CEC_RESULT_NO_ANSWER,       // acknowledged, and no answer within a second
    CEC_RESULT_NOT_ACKED,       // nobody acknowledged it; a broadcast: a device rejected it
    CEC_RESULT_FAILED,          // it did not get onto the wire, or CEC stopped
} cec_result_t;

// The outcome of a request. result is that of its first frame; the fields a
// request does not fill keep their "not answered" values.
typedef struct {
    cec_result_t result;
    cec_frame_t  answer;        // the first frame's answer or <Feature Abort>, header first; len 0: none
    uint8_t  abort_reason;      // REFUSED, or the answer to cec_request_abort(): CEC_REASON_*; 0xFF none
    uint8_t  power_status;      // cec_request_power_status(): CEC_POWER_STATUS_*
    uint8_t  menu_state;        // cec_request_menu(): CEC_MENU_OPEN / CEC_MENU_CLOSED; 0xFF none
    uint8_t  source;            // routing: the logical address of the device that answered; 15 none
    uint16_t phys_addr;         // system information: the device's; routing: the source's; CEC_PHYS_ADDR_NONE
    uint8_t  device_type;       // system information: CEC_TYPE_*; CEC_TYPE_NONE
    uint8_t  cec_version;       // system information: 4 for 1.3a, 5 for 1.4, 6 for 2.0; 0 none
    uint32_t vendor_id;         // system information: 24 bits; CEC_VENDOR_NONE
    char     language[4];       // system information of the TV: its menu language, "eng"; "" none
    char     name[15];          // cec_request_osd_name(): the answer; cec_request_set_osd_name(): the name in use
    uint8_t  sent;              // cec_request_volume(): presses acknowledged
    uint8_t  found;             // cec_request_bus_scan(): devices that acknowledged their poll
    uint16_t found_mask;        // ... bit n: logical address n
} cec_reply_t;

// Any frame: data is the opcode and up to 14 operands, len 0..15 (0: a poll
// of destination), to destination 0..15, from this board's address. answer:
// CEC_ANSWER_NONE, CEC_ANSWER_STANDARD (the message the standard pairs with
// the opcode; none for a message nothing answers), or the opcode awaited
// (0..255). A <Feature Abort> naming the opcode is the refusal.
#define CEC_ANSWER_NONE      (-1)
#define CEC_ANSWER_STANDARD  (-2)
bool cec_request_raw(uint8_t destination, const uint8_t *data, int len, int answer);

// <Give Device Power Status> to destination 0..14: power_status.
bool cec_request_power_status(uint8_t destination);

// System information of destination 0..14: <Give Physical Address>
// (phys_addr, device_type; its outcome is result), then, if that was
// acknowledged, <Get CEC Version> (cec_version), <Give Device Vendor ID>
// (vendor_id) and, to the TV, <Get Menu Language> (language).
bool cec_request_system_info(uint8_t destination);

// <Give OSD Name> to destination 0..14: name.
bool cec_request_osd_name(uint8_t destination);

// Sets the name this board gives for the TV's input list (up to 14 printable
// characters; NULL or none printable: "Freewili 2") at once and for later
// starts, then tells the TV with <Set OSD Name>: result SENT, or REFUSED.
// False when the request cannot start; the name is set even then.
bool cec_request_set_osd_name(const char *name);

// <Set OSD String> to the TV: 1..13 printable characters to show
// (CEC_OSD_SHOW_*), or CEC_OSD_CLEAR with text NULL or "". Result SENT, or
// REFUSED (many TVs refuse it).
bool cec_request_osd_string(uint8_t display_control, const char *text);

// Volume to destination 0..14 (CEC_LOGICAL_TV, CEC_LOGICAL_AUDIO_SYSTEM):
// steps 1..20 presses of volume up, -1..-20 of volume down, 0 one press of
// mute. Each press is <User Control Pressed> then <User Control Released>,
// presses 300 ms apart. sent: presses acknowledged; the first press not
// acknowledged ends it.
bool cec_request_volume(int steps, uint8_t destination);

// A remote key (CEC_KEY_*) to destination 0..14: <User Control Pressed>,
// repeated every 400 ms while hold_ms (0..10000) lasts, then <User Control
// Released>.
bool cec_request_key(uint8_t destination, uint8_t ui_command, uint32_t hold_ms);

// <Request Active Source> to every device: source and phys_addr of the
// device that answers.
bool cec_request_active_source(void);

// <Set Stream Path> to every device, naming phys_addr: the TV switches there
// and the device there answers <Active Source> (source, phys_addr). This
// board answers for its own address itself: that is NO_ANSWER here.
bool cec_request_stream_path(uint16_t phys_addr);

// <Menu Request> (CEC_MENU_ACTIVATE, _DEACTIVATE, _QUERY) to destination
// 0..14: menu_state.
bool cec_request_menu(uint8_t destination, uint8_t menu_request);

// <Abort> to destination 0..14, which a device must refuse: result ANSWERED
// with abort_reason when it does.
bool cec_request_abort(uint8_t destination);

// <Feature Abort> to destination 0..14, naming opcode, with reason
// (CEC_REASON_*). No answer is asked for.
bool cec_request_feature_abort(uint8_t destination, uint8_t opcode, uint8_t reason);

// <Vendor Command> with 0..14 bytes to destination 0..14. No answer is asked for.
bool cec_request_vendor_command(uint8_t destination, const uint8_t *data, int len);

// <Vendor Command With ID>: the 24-bit vendor_id, then 0..11 bytes, to
// destination 0..15. No answer is asked for.
bool cec_request_vendor_command_with_id(uint8_t destination, uint32_t vendor_id, const uint8_t *data, int len);

// Bus scan: polls every logical address but this board's own; a device that
// acknowledges is asked for its physical address, vendor ID, name, power
// status and CEC version. result SENT once every address is done; found,
// found_mask; each device in cec_scan_entry(). A few seconds for a few
// devices (a question nobody answers costs a second). Needs the claim to be
// over.
bool cec_request_bus_scan(void);

// A request is running.
bool cec_request_busy(void);

// True once, when the request started last has finished: *out holds its outcome.
bool cec_request_result(cec_reply_t *out);

// What the last bus scan found at logical address 0..14. False for an
// address that did not acknowledge its poll.
typedef struct {
    uint16_t phys_addr;         // CEC_PHYS_ADDR_NONE: not answered
    uint8_t  device_type;       // CEC_TYPE_NONE
    uint8_t  power_status;      // CEC_POWER_STATUS_NONE
    uint8_t  cec_version;       // 0
    uint32_t vendor_id;         // CEC_VENDOR_NONE
    char     name[15];          // ""
} cec_scan_entry_t;
bool cec_scan_entry(uint8_t address, cec_scan_entry_t *out);

// ---- capture ----------------------------------------------------------------
typedef enum {
    CEC_CAPTURE_OFF = 0,
    CEC_CAPTURE_OURS,           // frames for this board, broadcasts, and the frames it sends
    CEC_CAPTURE_ALL,            // every frame on the cable, polls included, and the frames it sends
} cec_capture_mode_t;

#define CEC_FRAME_ACKED  0x01   // every block acknowledged; a broadcast: nobody rejected it
#define CEC_FRAME_FOR_US 0x02   // a broadcast, or addressed to an address this board holds
#define CEC_FRAME_SENT   0x04   // sent by this board

typedef struct {
    uint32_t    time_us;        // time_us_32() when cec_task() took it in
    uint8_t     flags;          // CEC_FRAME_*
    cec_frame_t frame;
} cec_capture_frame_t;

// What the capture keeps, from now on; frames waiting are dropped. Off by
// default. The newest 32 frames are kept: older ones not taken are dropped
// and counted (capture_dropped). Runs beside the device: this board still
// acknowledges only its own addresses.
void cec_capture(cec_capture_mode_t mode);

// The oldest captured frame, taken off. False when none is waiting.
bool cec_next_frame(cec_capture_frame_t *out);

// ---- names ------------------------------------------------------------------
const char *cec_key_name(uint8_t ui_command);            // "select", "up", "f2_red", ...; "unknown"
const char *cec_opcode_name(uint8_t opcode);             // "give_osd_name", ...; "unknown"
const char *cec_result_name(cec_result_t result);        // "answered", "sent", "refused", ...
const char *cec_device_type_name(uint8_t device_type);   // "playback", "audio_system", ...; "unknown"
const char *cec_power_status_name(uint8_t power_status); // "on", "standby", ...; "unknown"
const char *cec_abort_reason_name(uint8_t reason);       // "unrecognized", "refused", ...; "unknown"

void cec_get_stats(cec_stats_t *out);
void cec_clear_stats(void);

#ifdef __cplusplus
}
#endif
#endif
