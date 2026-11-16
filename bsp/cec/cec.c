// bsp/cec/cec.c — see cec.h.
//
// Layout of this file:
//   1. the pin, the alarm and the two handlers — RAM only
//   2. the send queue and the event queue
//   3. open / close
//   4. task
//   5. device requests, settings, status
//   6. requests of other devices, capture, names
#include "cec/cec.h"
#include "cec/cec_control.h"
#include "cec/cec_engine.h"
#include "cec/cec_place.h"
#include "cec/cec_watch.h"
#include "platform/board.h"
#include "platform/timer1.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/structs/io_bank0.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/watchdog.h"
#include "pico/time.h"

#if PIN_CEC >= 32
#error "the CEC pin is reached through the low SIO word"
#endif
#define CEC_PIN_MASK      (1u << PIN_CEC)
#define CEC_IRQ_REG       (PIN_CEC / 8u)
#define CEC_IRQ_FALL      ((uint32_t)GPIO_IRQ_EDGE_FALL << (4u * (PIN_CEC % 8u)))
#define CEC_IRQ_RISE      ((uint32_t)GPIO_IRQ_EDGE_RISE << (4u * (PIN_CEC % 8u)))
#define CEC_IRQ_EDGES     (CEC_IRQ_FALL | CEC_IRQ_RISE)

#define CEC_TX_QUEUE      16u     // power of two
// What the device or a request adds to the queue needs this much room. The
// device's answer to a finished send is exempt and uses the rest: it is at
// most CEC_ENGINE_OUT_FRAMES_MAX frames.
#define CEC_TX_ROOM       (2u * CEC_ENGINE_OUT_FRAMES_MAX)
#define CEC_SEND_TIMEOUT_MS   2500u   // a frame still waiting for the wire this long after its send began is given up
#define CEC_CLOSE_TIMEOUT_MS  1500u   // how long leaving the wire waits for the queued frames to go out
#define CEC_EVENT_QUEUE   16u     // power of two

static bool     s_wanted;               // cec_start() asked for the device, cec_stop() did not since
static bool     s_running;              // the wire is open
static bool     s_reopen;               // the device leaves the wire and starts again with a new physical address
static bool     s_closing;
static uint32_t s_close_by_ms;
static uint16_t s_phys_addr = CEC_PHYS_ADDR_DEFAULT;
static cec_address_source_t s_source = CEC_ADDRESS_DEFAULT;
static bool     s_firmware_read;        // the firmware's app-info record was looked at
static bool     s_learned;              // an address was learned and not yet taken for saving
static bool     s_announce;             // <Active Source> once the learned address is claimed
static char     s_osd_name[15];         // 14 characters and the terminator; "" = the default
static uint32_t s_vendor_id = CEC_VENDOR_ID_DEFAULT;
static uint8_t  s_types[2] = { CEC_TYPE_PLAYBACK, CEC_TYPE_NONE };
static const char *s_error = "";

static bool     s_irqs_installed;
static uint32_t s_alarm;
static uint32_t s_alarm_irq;

// Written by the handlers.
static bool              s_alarm_timed;     // the alarm is armed for a time still ahead
static uint32_t          s_alarm_at;
static volatile uint32_t s_alarms;
static volatile uint32_t s_late_100;
static volatile uint32_t s_late_300;
static volatile uint32_t s_late_max_us;

// A frame to send, and whether a request sent it (its result goes to the
// request) or the device did.
typedef struct {
    cec_frame_t frame;
    bool        request;
} cec_tx_entry_t;

static cec_tx_entry_t s_tx_queue[CEC_TX_QUEUE];
static uint8_t     s_tx_head;           // the next frame to send
static uint8_t     s_tx_count;
static bool        s_in_flight;         // the engine has a frame whose result is still to come
static bool        s_in_flight_request; // ... sent for a request
static uint32_t    s_in_flight_ms;
static uint32_t    s_tx_acked;
static uint32_t    s_tx_not_acked;
static uint32_t    s_tx_failed;
static uint32_t    s_tx_dropped;
static cec_engine_out_t s_out;          // the task's own

static cec_event_t s_events[CEC_EVENT_QUEUE];
static uint8_t     s_event_head;        // the oldest event
static uint8_t     s_event_count;
static uint32_t    s_events_dropped;

static inline uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// ---------------------------------------------------------------------------
// 1. The pin, the alarm and the two handlers — RAM only. Nothing below may
//    call a flash- or PSRAM-resident function.
// ---------------------------------------------------------------------------

static __force_inline io_bank0_irq_ctrl_hw_t *irq_ctrl(void) {
    return get_core_num() ? &io_bank0_hw->proc1_irq_ctrl : &io_bank0_hw->proc0_irq_ctrl;
}

// The pin and the alarm as the engine asked for them.
static __force_inline void apply(const cec_wire_out_t *out) {
    if (out->drive_low)
        sio_hw->gpio_oe_set = CEC_PIN_MASK;
    else
        sio_hw->gpio_oe_clr = CEC_PIN_MASK;

    const uint32_t mask = 1u << s_alarm;
    if (!out->alarm) {
        timer1_hw->armed = mask;                // disarm
        s_alarm_timed = false;
        return;
    }
    timer1_hw->alarm[s_alarm] = out->alarm_us;
    s_alarm_at = out->alarm_us;
    // A time already reached never matches: the handler is asked for directly.
    s_alarm_timed = (int32_t)(out->alarm_us - timer1_hw->timerawl) > 1;
    if (!s_alarm_timed)
        hw_set_bits(&timer1_hw->intf, mask);
}

static void __not_in_flash_func(alarm_isr)(void) {
    const uint32_t mask = 1u << s_alarm;
    timer1_hw->intr = mask;                     // latched match
    hw_clear_bits(&timer1_hw->intf, mask);      // a direct request
    const uint32_t now = timer1_hw->timerawl;

    const uint32_t late = now - s_alarm_at;
    if (s_alarm_timed && (int32_t)late >= 0) {
        s_alarm_timed = false;
        s_alarms++;
        if (late >= 100u) s_late_100++;
        if (late >= 300u) s_late_300++;
        if (late > s_late_max_us) s_late_max_us = late;
    }

    // The level is read before apply(): a pull this alarm ends is still on,
    // and a high there means the pull does not reach the wire.
    cec_wire_out_t out;
    cec_engine_on_alarm(now, (sio_hw->gpio_in & CEC_PIN_MASK) != 0, &out);
    apply(&out);
}

// Interrupts held off delay this handler and the alarm's alike. Released
// together, the alarm runs first (TIMER1's vectors are below IO_IRQ_BANK0 at
// the same priority), so the engine sees its own lateness before the edges.
static void __not_in_flash_func(edge_isr)(void) {
    uint32_t events = irq_ctrl()->ints[CEC_IRQ_REG] & CEC_IRQ_EDGES;
    if (events == 0) return;
    const uint32_t now = timer1_hw->timerawl;
    const bool high_now = (sio_hw->gpio_in & CEC_PIN_MASK) != 0;
    // One edge with the wire already back where it was before it: the edge
    // that took it back came after the flags were read.
    if (events != CEC_IRQ_EDGES && high_now == (events == CEC_IRQ_FALL))
        events = CEC_IRQ_EDGES;
    io_bank0_hw->intr[CEC_IRQ_REG] = events;

    cec_wire_out_t out;
    if (events == CEC_IRQ_EDGES) {
        // Both edges since the handler last ran: the level now is where the second one went.
        cec_engine_on_edge_pair(high_now, now, &out);
    } else {
        cec_engine_on_edge(events == CEC_IRQ_FALL, now, &out);
    }
    apply(&out);
}

// The two engine calls the task makes with the handlers held off.
static bool __no_inline_not_in_flash_func(send_locked)(const cec_frame_t *frame) {
    cec_wire_out_t out;
    const uint32_t save = save_and_disable_interrupts();
    const bool ok = cec_engine_wire_send(frame, timer1_hw->timerawl, &out);
    apply(&out);
    restore_interrupts(save);
    return ok;
}

static bool __no_inline_not_in_flash_func(cancel_locked)(void) {
    cec_wire_out_t out;
    const uint32_t save = save_and_disable_interrupts();
    const bool ok = cec_engine_wire_cancel(&out);
    apply(&out);
    restore_interrupts(save);
    return ok;
}

// ---------------------------------------------------------------------------
// 2. The send queue and the event queue
// ---------------------------------------------------------------------------

static inline uint32_t queue_room(void) { return CEC_TX_QUEUE - s_tx_count; }

static void queue_push(const cec_frame_t *frame, bool request) {
    if (s_tx_count >= CEC_TX_QUEUE) {
        s_tx_dropped++;
        return;
    }
    cec_tx_entry_t *entry = &s_tx_queue[(s_tx_head + s_tx_count) & (CEC_TX_QUEUE - 1u)];
    entry->frame = *frame;
    entry->request = request;
    s_tx_count++;
}

static void event_queue(const cec_event_t *ev) {
    if (s_event_count >= CEC_EVENT_QUEUE) {
        s_events_dropped++;
        return;
    }
    cec_event_t *out = &s_events[(s_event_head + s_event_count) & (CEC_EVENT_QUEUE - 1u)];
    *out = *ev;
    out->time_ms = now_ms();
    s_event_count++;
}

// data and extra[]: what each event type carries, as cec.h lists it.
static void event_push(cec_event_type_t type, const cec_engine_event_t *ev, uint8_t data, uint8_t extra0) {
    const cec_event_t out = {
        .type = type, .data = data, .from = ev->peer, .to = ev->own, .extra = { extra0, ev->data[2] },
    };
    event_queue(&out);
}

// The device's events, as the app sees them. timeout: they come from the
// device's clock, not from a frame: a key-up then means no release came.
static void event_from_device(const cec_engine_event_t *ev, bool timeout) {
    if (ev->type == CEC_ENGINE_EV_KEY_DOWN || ev->type == CEC_ENGINE_EV_KEY_UP)
        cec_watch_on_key(ev->type == CEC_ENGINE_EV_KEY_DOWN, ev->data[0], ev->peer);
    switch (ev->type) {
    case CEC_ENGINE_EV_ADDRESS_CLAIMED: event_push(CEC_EV_ADDRESS, ev, ev->own, ev->data[0]); break;
    case CEC_ENGINE_EV_ADDRESS_NONE:    event_push(CEC_EV_ADDRESS, ev, CEC_ENGINE_ADDR_NONE, ev->data[0]); break;
    case CEC_ENGINE_EV_STANDBY:         event_push(CEC_EV_STANDBY, ev, 0, 0); break;
    case CEC_ENGINE_EV_ACTIVE_SOURCE:   event_push(CEC_EV_ACTIVE_SOURCE, ev, ev->data[0], 0); break;
    case CEC_ENGINE_EV_KEY_DOWN:        event_push(CEC_EV_KEY_DOWN, ev, ev->data[0], 0); break;
    case CEC_ENGINE_EV_KEY_UP:          event_push(CEC_EV_KEY_UP, ev, ev->data[0], timeout ? 1u : 0u); break;
    case CEC_ENGINE_EV_MENU_REQUEST:    event_push(CEC_EV_MENU_REQUEST, ev, ev->data[0], 0); break;
    case CEC_ENGINE_EV_MENU_LANGUAGE:   event_push(CEC_EV_MENU_LANGUAGE, ev, ev->data[0], ev->data[1]); break;
    case CEC_ENGINE_EV_DECK_PLAY:       event_push(CEC_EV_DECK_PLAY, ev, ev->data[0], 0); break;
    case CEC_ENGINE_EV_DECK_CONTROL:    event_push(CEC_EV_DECK_CONTROL, ev, ev->data[0], 0); break;
    case CEC_ENGINE_EV_FEATURE_ABORT:   event_push(CEC_EV_FEATURE_ABORT, ev, ev->data[0], ev->data[1]); break;
    default: break;
    }
}

static void submit(const cec_engine_out_t *out, bool tick) {
    for (int i = 0; i < out->frames; i++) queue_push(&out->frame[i], false);
    for (int i = 0; i < out->events; i++) event_from_device(&out->event[i], tick);
}

// ---------------------------------------------------------------------------
// 3. Open / close
// ---------------------------------------------------------------------------

// The alarm and the edge handler stay installed for good; both are inert
// while closed (alarm interrupt disabled, the pin's edges masked).
static void install_irqs_once(void) {
    if (s_irqs_installed) return;
    s_irqs_installed = true;

    s_alarm = (uint32_t)timer_hardware_alarm_claim_unused(timer1_hw, true);
    s_alarm_irq = timer_hardware_alarm_get_irq_num(timer1_hw, s_alarm);
    irq_set_exclusive_handler(s_alarm_irq, alarm_isr);
    irq_set_priority(s_alarm_irq, PICO_HIGHEST_IRQ_PRIORITY);

    // Raw handler: it checks and acknowledges only its own pin. The vector is
    // shared by every pin of this core; the two handlers must not interrupt
    // each other, so it gets the alarm's priority.
    gpio_add_raw_irq_handler(PIN_CEC, edge_isr);
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
}

static void open_wire(void) {
    install_irqs_once();

    // SIO, input, output value 0: the output enable alone moves the wire.
    gpio_init(PIN_CEC);
    gpio_disable_pulls(PIN_CEC);

    cec_engine_wire_init(timer1_hw->timerawl);
    cec_engine_wire_set_ack_mask(0);
    cec_engine_wire_set_monitor_all(cec_control_capture_mode_now() == CEC_CAPTURE_ALL);

    s_tx_head = 0;
    s_tx_count = 0;
    s_in_flight = false;
    s_closing = false;
    s_reopen = false;
    s_alarm_timed = false;
    s_running = true;

    cec_engine_device_init(s_osd_name, s_vendor_id, s_types[0], s_types[1]);
    cec_watch_init();
    cec_engine_out_clear(&s_out);
    cec_engine_device_start(s_phys_addr, &s_out);
    submit(&s_out, false);
    cec_place_learn_reset();

    io_bank0_hw->intr[CEC_IRQ_REG] = CEC_IRQ_EDGES;         // drop edges latched meanwhile
    hw_set_bits(&irq_ctrl()->inte[CEC_IRQ_REG], CEC_IRQ_EDGES);
    hw_set_bits(&timer1_hw->inte, 1u << s_alarm);
    irq_set_enabled(s_alarm_irq, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
}

static void close_wire(void) {
    const uint32_t mask = 1u << s_alarm;
    hw_clear_bits(&irq_ctrl()->inte[CEC_IRQ_REG], CEC_IRQ_EDGES);
    irq_set_enabled(s_alarm_irq, false);
    timer1_hw->armed = mask;
    hw_clear_bits(&timer1_hw->intf, mask);
    timer1_hw->intr = mask;
    sio_hw->gpio_oe_clr = CEC_PIN_MASK;         // the wire is never left pulled

    s_running = false;
    s_closing = false;
    s_in_flight = false;
    s_tx_count = 0;
    cec_control_stop();
}

// ---------------------------------------------------------------------------
// 4. Task
// ---------------------------------------------------------------------------

// The physical address from source, when source ranks the same as the one in
// use or higher (cec_address_source_t).
static void use_address(uint16_t phys_addr, cec_address_source_t source) {
    if (source < s_source) return;
    s_source = source;
    if (phys_addr == s_phys_addr) return;
    s_phys_addr = phys_addr;
    if (s_running) s_reopen = true;
}

// The DISPLAY firmware's app-info record (cec_place.h), once per boot. The
// record is cleared once read, as the firmware's bootloader clears its own
// one-shot command in the same registers: an app started later some other
// way does not find it.
static void take_firmware_address(void) {
    if (s_firmware_read) return;
    s_firmware_read = true;
    const uint32_t magic = watchdog_hw->scratch[CEC_PLACE_INFO_SCRATCH];
    if (magic != CEC_PLACE_INFO_MAGIC) return;
    watchdog_hw->scratch[CEC_PLACE_INFO_SCRATCH] = 0;
    uint16_t phys_addr;
    if (cec_place_from_firmware(magic, watchdog_hw->scratch[CEC_PLACE_INFO_ADDR_SCRATCH], &phys_addr))
        use_address(phys_addr, CEC_ADDRESS_FIRMWARE);
}

// A route the TV announced that no device answered: this board's input.
static void learn(uint32_t ms) {
    uint16_t phys_addr;
    if (!cec_place_learned(ms, &phys_addr) || CEC_ADDRESS_LEARNED < s_source) return;
    use_address(phys_addr, CEC_ADDRESS_LEARNED);
    s_learned = true;
    s_announce = true;
}

void cec_task(void) {
    if (!s_running) {
        if (!s_wanted) return;
        open_wire();
    }
    const uint32_t ms = now_ms();

    if ((!s_wanted || s_reopen) && !s_closing) {
        // Leaving: the device gives up its address, and what is queued goes out first.
        s_closing = true;
        s_reopen = false;
        s_close_by_ms = ms + CEC_CLOSE_TIMEOUT_MS;
        cec_engine_out_clear(&s_out);
        cec_engine_device_stop(&s_out);
        submit(&s_out, false);
    }

    // The results of frames sent.
    cec_frame_t frame;
    cec_engine_tx_result_t result;
    while (cec_engine_wire_pop_tx_done(&frame, &result)) {
        s_in_flight = false;
        if (result == CEC_ENGINE_TX_ACKED)
            s_tx_acked++;
        else if (result == CEC_ENGINE_TX_NOT_ACKED)
            s_tx_not_acked++;
        else
            s_tx_failed++;
        cec_control_capture_push(&frame, (uint8_t)(CEC_FRAME_SENT | (result == CEC_ENGINE_TX_ACKED ? CEC_FRAME_ACKED : 0)),
                                 time_us_32());
        if (s_in_flight_request) {
            cec_control_on_sent(&frame, result, ms);
        } else {
            cec_engine_out_clear(&s_out);
            cec_engine_device_on_tx_done(&frame, result, &s_out);
            submit(&s_out, false);
        }
    }

    // The frames received. The capture takes them all; the device and the
    // request take those for this board.
    uint8_t flags;
    while (queue_room() >= CEC_TX_ROOM && cec_engine_wire_pop_rx(&frame, &flags)) {
        static_assert(CEC_ENGINE_RX_ACKED == CEC_FRAME_ACKED && CEC_ENGINE_RX_FOR_US == CEC_FRAME_FOR_US,
                      "frame flags differ");
        cec_control_capture_push(&frame, flags, time_us_32());
        if ((flags & CEC_ENGINE_RX_FOR_US) != 0) {
            cec_event_t watched;
            bool for_device;
            if (cec_watch_on_rx(&frame, &watched, &for_device)) event_queue(&watched);
            if (for_device) {
                cec_engine_out_clear(&s_out);
                cec_engine_device_on_rx(&frame, ms, &s_out);
                submit(&s_out, false);
            }
            cec_control_on_rx(&frame, ms);
            cec_place_on_rx(&frame, s_phys_addr, ms);
        }
    }
    learn(ms);

    if (queue_room() >= CEC_TX_ROOM) {
        cec_engine_out_clear(&s_out);
        cec_engine_device_on_tick(ms, &s_out);
        submit(&s_out, true);
    }
    // The engine acknowledges what the device holds.
    cec_engine_wire_set_ack_mask(cec_engine_device_address_mask());

    // The TV shows the learned input: the device there says so (CEC 11.1.2),
    // once it holds an address; a claim waiting to retry holds none.
    if (s_announce && !s_reopen && !s_closing && queue_room() >= CEC_TX_ROOM && cec_engine_device_ready() &&
        cec_engine_device_address() != CEC_ENGINE_ADDR_NONE) {
        cec_engine_out_clear(&s_out);
        if (cec_engine_device_one_touch_play(&s_out)) submit(&s_out, false);
        s_announce = false;
    }

    // The request: its time limits, and its next frame.
    cec_control_tick(ms);
    if (!s_closing && queue_room() >= CEC_TX_ROOM && cec_control_next(cec_engine_device_address(), ms, &frame))
        queue_push(&frame, true);

    // The next frame.
    if (s_in_flight) {
        if ((uint32_t)(ms - s_in_flight_ms) >= CEC_SEND_TIMEOUT_MS && cancel_locked())
            s_in_flight_ms = ms;                // its result is in the ring
    } else if (s_tx_count != 0) {
        const cec_tx_entry_t *entry = &s_tx_queue[s_tx_head];
        if (send_locked(&entry->frame)) {
            s_in_flight = true;
            s_in_flight_request = entry->request;
            s_in_flight_ms = ms;
        } else {
            s_tx_dropped++;                     // a frame the engine does not take
        }
        s_tx_head = (uint8_t)((s_tx_head + 1u) & (CEC_TX_QUEUE - 1u));
        s_tx_count--;
    }

    if (s_closing && ((s_tx_count == 0 && !s_in_flight) || (int32_t)(ms - s_close_by_ms) >= 0))
        close_wire();
}

// ---------------------------------------------------------------------------
// 5. Device requests, settings, status
// ---------------------------------------------------------------------------

// The name for the next start, up to 14 characters; NULL: the default.
static void keep_osd_name(const char *osd_name) {
    int n = 0;
    if (osd_name != NULL)
        for (; osd_name[n] != 0 && n < (int)sizeof(s_osd_name) - 1; n++) s_osd_name[n] = osd_name[n];
    s_osd_name[n] = 0;
}

bool cec_start(const char *osd_name) {
    if (!timer1_counts_us()) {
        s_error = "TIMER1 is not counting microseconds";
        return false;
    }
    s_error = "";
    take_firmware_address();
    keep_osd_name(osd_name);
    s_wanted = true;
    return true;
}

void cec_stop(void) {
    s_wanted = false;
    s_announce = false;
}

bool cec_running(void) { return s_running; }

const char *cec_last_error(void) { return s_error; }

void cec_set_physical_address(uint16_t phys_addr) { use_address(phys_addr, CEC_ADDRESS_APP); }

uint16_t cec_physical_address(void) { return s_phys_addr; }

cec_address_source_t cec_physical_address_source(void) { return s_source; }

const char *cec_address_source_name(cec_address_source_t source) {
    switch (source) {
    case CEC_ADDRESS_DEFAULT:  return "default";
    case CEC_ADDRESS_SAVED:    return "saved";
    case CEC_ADDRESS_LEARNED:  return "learned";
    case CEC_ADDRESS_FIRMWARE: return "firmware";
    case CEC_ADDRESS_APP:      return "app";
    }
    return "unknown";
}

void cec_use_saved_address(uint16_t phys_addr) { use_address(phys_addr, CEC_ADDRESS_SAVED); }

bool cec_take_learned_address(uint16_t *phys_addr) {
    if (!s_learned) return false;
    s_learned = false;
    *phys_addr = s_phys_addr;
    return s_source == CEC_ADDRESS_LEARNED;
}

static bool is_claimable_type(uint8_t type) {
    return type == CEC_TYPE_PLAYBACK || type == CEC_TYPE_RECORDER || type == CEC_TYPE_TUNER ||
           type == CEC_TYPE_AUDIO_SYSTEM;
}

bool cec_set_device_types(uint8_t first, uint8_t second) {
    if (!is_claimable_type(first) || (second != CEC_TYPE_NONE && !is_claimable_type(second)) || first == second)
        return false;
    if ((first == CEC_TYPE_PLAYBACK && second == CEC_TYPE_RECORDER) ||
        (first == CEC_TYPE_RECORDER && second == CEC_TYPE_PLAYBACK))
        return false;
    if (first == s_types[0] && second == s_types[1]) return true;
    s_types[0] = first;
    s_types[1] = second;
    if (s_running) s_reopen = true;
    return true;
}

void cec_set_vendor_id(uint32_t vendor_id) {
    s_vendor_id = vendor_id == CEC_VENDOR_NONE ? CEC_VENDOR_NONE : (vendor_id & 0xFFFFFFu);
    if (s_running) cec_engine_device_set_vendor_id(s_vendor_id);
}

void cec_accept_vendor_frames(bool on) { cec_watch_accept_vendor(on); }

bool cec_next_event(cec_event_t *out) {
    if (s_event_count == 0) return false;
    *out = s_events[s_event_head];
    s_event_head = (uint8_t)((s_event_head + 1u) & (CEC_EVENT_QUEUE - 1u));
    s_event_count--;
    return true;
}

static bool can_request(void) {
    return s_running && s_wanted && !s_closing && queue_room() >= CEC_TX_ROOM;
}

bool cec_wake_tv(void) {
    if (!can_request()) return false;
    cec_engine_out_clear(&s_out);
    if (!cec_engine_device_one_touch_play(&s_out)) return false;
    submit(&s_out, false);
    return true;
}

bool cec_tv_standby(void) {
    if (!can_request()) return false;
    cec_engine_out_clear(&s_out);
    if (!cec_engine_device_standby(CEC_ENGINE_ADDR_TV, &s_out)) return false;
    submit(&s_out, false);
    return true;
}

uint8_t cec_logical_address(void) {
    return s_running ? cec_engine_device_address() : CEC_ENGINE_ADDR_NONE;
}

uint16_t cec_logical_addresses(void) {
    return s_running ? cec_engine_device_address_mask() : 0;
}

void cec_get_stats(cec_stats_t *out) {
    cec_engine_wire_counts_t w;
    cec_engine_wire_counts(&w);
    out->rx_frames = w.rx_frames;
    out->rx_errors = w.rx_errors;
    out->rx_overruns = w.rx_overruns;
    out->error_signals = w.error_signals;
    out->tx_acked = s_tx_acked;
    out->tx_not_acked = s_tx_not_acked;
    out->tx_failed = s_tx_failed;
    out->tx_dropped = s_tx_dropped;
    out->tx_retries = w.tx_retries;
    out->arbitration_lost = w.arbitration_lost;
    out->line_errors = w.line_errors;
    out->alarms = s_alarms;
    out->late_100us = s_late_100;
    out->late_300us = s_late_300;
    out->late_max_us = s_late_max_us;
    out->pull_not_seen = w.pull_not_seen;
    out->late_aborts = w.late_aborts;
    out->rx_dropped = w.rx_dropped;
    out->events_dropped = s_events_dropped;
    out->capture_dropped = cec_control_capture_dropped();
}

// The handlers add to some of these while they are cleared: a count may then
// be one too low.
void cec_clear_stats(void) {
    cec_engine_wire_clear_counts();
    s_tx_acked = 0;
    s_tx_not_acked = 0;
    s_tx_failed = 0;
    s_tx_dropped = 0;
    s_alarms = 0;
    s_late_100 = 0;
    s_late_300 = 0;
    s_late_max_us = 0;
    s_events_dropped = 0;
    cec_control_capture_clear_dropped();
}

// ---------------------------------------------------------------------------
// 6. Requests of other devices, capture, names
// ---------------------------------------------------------------------------

// Each starts where a device request could (can_request()).

bool cec_request_raw(uint8_t destination, const uint8_t *data, int len, int answer) {
    return can_request() && cec_control_raw(destination, data, len, answer);
}

bool cec_request_power_status(uint8_t destination) {
    return can_request() && cec_control_power_status(destination);
}

bool cec_request_system_info(uint8_t destination) {
    return can_request() && cec_control_system_info(destination);
}

bool cec_request_osd_name(uint8_t destination) {
    return can_request() && cec_control_osd_name(destination);
}

bool cec_request_set_osd_name(const char *name) {
    keep_osd_name(name);
    if (!s_running) return false;
    cec_engine_device_set_osd_name(s_osd_name);
    char in_use[sizeof s_osd_name];
    cec_engine_device_osd_name(in_use);
    return can_request() && cec_control_tell_osd_name(in_use);
}

bool cec_request_osd_string(uint8_t display_control, const char *text) {
    return can_request() && cec_control_osd_string(display_control, text);
}

bool cec_request_volume(int steps, uint8_t destination) {
    return can_request() && cec_control_volume(steps, destination);
}

bool cec_request_key(uint8_t destination, uint8_t ui_command, uint32_t hold_ms) {
    return can_request() && cec_control_key(destination, ui_command, hold_ms);
}

bool cec_request_active_source(void) {
    return can_request() && cec_control_active_source();
}

bool cec_request_stream_path(uint16_t phys_addr) {
    return can_request() && cec_control_stream_path(phys_addr);
}

bool cec_request_menu(uint8_t destination, uint8_t menu_request) {
    return can_request() && cec_control_menu(destination, menu_request);
}

bool cec_request_abort(uint8_t destination) {
    return can_request() && cec_control_abort(destination);
}

bool cec_request_feature_abort(uint8_t destination, uint8_t opcode, uint8_t reason) {
    return can_request() && cec_control_feature_abort(destination, opcode, reason);
}

bool cec_request_vendor_command(uint8_t destination, const uint8_t *data, int len) {
    return can_request() && cec_control_vendor_command(destination, data, len);
}

bool cec_request_vendor_command_with_id(uint8_t destination, uint32_t vendor_id, const uint8_t *data, int len) {
    return can_request() && cec_control_vendor_command_with_id(destination, vendor_id, data, len);
}

bool cec_request_bus_scan(void) {
    return can_request() && cec_engine_device_ready() && cec_control_bus_scan(cec_engine_device_address_mask());
}

bool cec_request_busy(void) { return cec_control_busy(); }

bool cec_request_result(cec_reply_t *out) { return cec_control_result(out); }

bool cec_scan_entry(uint8_t address, cec_scan_entry_t *out) { return cec_control_scan_entry(address, out); }

void cec_capture(cec_capture_mode_t mode) {
    cec_control_capture_mode(mode);
    if (s_running) cec_engine_wire_set_monitor_all(mode == CEC_CAPTURE_ALL);
}

bool cec_next_frame(cec_capture_frame_t *out) { return cec_control_capture_pop(out); }

const char *cec_key_name(uint8_t ui_command) { return cec_engine_key_name(ui_command); }
const char *cec_opcode_name(uint8_t opcode) { return cec_engine_opcode_name(opcode); }
const char *cec_result_name(cec_result_t result) { return cec_control_result_name(result); }
const char *cec_device_type_name(uint8_t device_type) { return cec_engine_device_type_name(device_type); }
const char *cec_power_status_name(uint8_t power_status) { return cec_engine_power_status_name(power_status); }
const char *cec_abort_reason_name(uint8_t reason) { return cec_engine_abort_reason_name(reason); }
