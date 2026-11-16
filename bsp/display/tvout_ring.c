// bsp/display/tvout_ring.c — see tvout_ring.h.
//
// Two TIMER1 alarms are claimed once and kept: one is the reader's tick, the
// other is never armed and is raised through the force register to hold
// core 1. Each tick is set a time after the previous one has ended, so other
// interrupts of the same priority are served between two ticks.
#include "display/tvout_ring.h"
#include "display/tvout_internal.h"
#include "display/tvout_ring_core.h"
#include "platform/psram.h"
#include "platform/timer1.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/xip_aux.h"
#include "hardware/structs/xip_ctrl.h"
#include "pico/stdlib.h"

_Static_assert(TVOUT_RING_WIDTH_MAX % 2 == 0 && TVOUT_RING_WIDTH_MAX <= 640,
               "TVOUT_RING_WIDTH_MAX: an even width of at most 640");

#define PSRAM_END            (PSRAM_BASE + (uintptr_t)PICO_PSRAM_SIZE_BYTES)
#define NOCACHE_OFFSET       (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE)
#define READ_TIMEOUT_US      300u
#define HOLD_WAIT_US         4u
#define SWITCH_TIMEOUT_US    2000u

// Everything the two interrupt handlers touch.
typedef struct {
    volatile bool hold_req;
    volatile bool held;
    volatile bool core1_ready;            // the hold interrupt is installed on core 1
    uint32_t (*scan_piece)(void);
    uint32_t channel;                     // the stream channel
    uint32_t tick_alarm;
    uint32_t tick_mask;
    uint32_t hold_mask;
    uint32_t stream_base;                 // the picture's cached address
    uint32_t row_bytes;
    uint32_t row_words;
    uint32_t stride_bytes;
    uint32_t width;
    // Counts since the last ring_attach().
    uint32_t reads;
    uint32_t timeouts;
    uint32_t read_us_max;
    uint32_t reader_us;
    uint32_t since_us;
} ring_state_t;

static ring_state_t s_ring;
static uint16_t s_lines[TVOUT_RING_ROWS * TVOUT_RING_WIDTH_MAX] __attribute__((aligned(4)));
static bool s_alarms_claimed;
static uint s_tick_irq;
static uint s_hold_irq;
static bool s_reader_installed;
static bool s_open;
static const char *s_error = "";

// One row from PSRAM into its line buffer, through the XIP streaming FIFO.
// The DMA channel only ever reads that FIFO. False after READ_TIMEOUT_US.
static bool stream_row(uint32_t row) {
    const uint32_t channel = s_ring.channel;
    const uint32_t mask = 1u << channel;
    xip_ctrl_hw->stream_ctr = 0;
    for (uint32_t drain = 0; drain < 16u && !(xip_ctrl_hw->stat & XIP_STAT_FIFO_EMPTY_BITS); drain++)
        (void)xip_ctrl_hw->stream_fifo;
    xip_ctrl_hw->stream_addr = s_ring.stream_base + row * s_ring.stride_bytes;
    dma_hw->ch[channel].al1_write_addr =
        (uint32_t)(uintptr_t)(s_lines + tvout_ring_core_slot(row) * s_ring.width);
    xip_ctrl_hw->stream_ctr = s_ring.row_words;
    dma_hw->ch[channel].al1_transfer_count_trig = s_ring.row_words;
    const uint32_t from = timer1_hw->timerawl;
    while (dma_hw->ch[channel].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
        if (timer1_hw->timerawl - from > READ_TIMEOUT_US) {
            xip_ctrl_hw->stream_ctr = 0;
            dma_hw->abort = mask;
            while (dma_hw->abort & mask) tight_loop_contents();
            return false;
        }
    }
    return true;
}

// Core 1: stays here, off PSRAM, while core 0 reads a row.
static void hold_irq(void) {
    hw_clear_bits(&timer1_hw->intf, s_ring.hold_mask);
    s_ring.held = true;
    while (s_ring.hold_req) tight_loop_contents();
    s_ring.held = false;
}

// Keeps the line buffers ahead of the TV.
static void reader_irq(void) {
    const uint32_t tick_mask = s_ring.tick_mask;
    hw_clear_bits(&timer1_hw->intf, tick_mask);
    timer1_hw->intr = tick_mask;
    if (!tvout_ring_core_reading()) return;

    const uint32_t entered = timer1_hw->timerawl;
    uint32_t first_row = 0;
    const uint32_t count = tvout_ring_core_reader_begin(s_ring.scan_piece(), &first_row);
    for (uint32_t i = 0; i < count; i++) {
        if (s_ring.core1_ready) {
            s_ring.hold_req = true;
            hw_set_bits(&timer1_hw->intf, s_ring.hold_mask);
            const uint32_t from = timer1_hw->timerawl;
            while (!s_ring.held && timer1_hw->timerawl - from <= HOLD_WAIT_US) tight_loop_contents();
        }
        const uint32_t from = timer1_hw->timerawl;
        if (!stream_row(first_row + i)) s_ring.timeouts++;
        const uint32_t took = timer1_hw->timerawl - from;
        s_ring.hold_req = false;
        if (took > s_ring.read_us_max) s_ring.read_us_max = took;
        s_ring.reads++;
        tvout_ring_core_reader_fetched(first_row + i, s_ring.scan_piece());
    }
    const uint32_t gap_us = tvout_ring_core_reader_end(first_row, count);

    timer1_arm_after(s_ring.tick_alarm, tick_mask, gap_us);
    s_ring.reader_us += timer1_hw->timerawl - entered;
}

static bool alarms_claim(void) {
    if (s_alarms_claimed) return true;
    const int tick = timer_hardware_alarm_claim_unused(timer1_hw, false);
    const int hold = timer_hardware_alarm_claim_unused(timer1_hw, false);
    if (tick < 0 || hold < 0) {
        if (tick >= 0) timer_hardware_alarm_unclaim(timer1_hw, (uint)tick);
        if (hold >= 0) timer_hardware_alarm_unclaim(timer1_hw, (uint)hold);
        return false;
    }
    s_ring.tick_alarm = (uint32_t)tick;
    s_ring.tick_mask = 1u << tick;
    s_tick_irq = timer_hardware_alarm_get_irq_num(timer1_hw, (uint)tick);
    s_ring.hold_mask = 1u << hold;
    s_hold_irq = timer_hardware_alarm_get_irq_num(timer1_hw, (uint)hold);
    s_alarms_claimed = true;
    return true;
}

static uintptr_t cached_address(const uint16_t *pixels) {
    const uintptr_t addr = (uintptr_t)pixels;
    return addr >= PSRAM_BASE + NOCACHE_OFFSET ? addr - NOCACHE_OFFSET : addr;
}

// The first pixel lies in PSRAM, through the cached or the uncached window.
static bool in_psram(const uint16_t *pixels) {
    const uintptr_t addr = (uintptr_t)pixels;
    return (addr >= PSRAM_BASE && addr < PSRAM_END) ||
           (addr >= PSRAM_BASE + NOCACHE_OFFSET && addr < PSRAM_END + NOCACHE_OFFSET);
}

// The picture wholly in PSRAM, 4-byte aligned, of an even width at most
// TVOUT_RING_WIDTH_MAX; TIMER1 counting microseconds; two of its alarms and
// one DMA channel free, and taken.
static bool ring_open(const uint16_t *pixels, unsigned width, unsigned height) {
    if (s_open) return true;
    s_error = "";

    const uintptr_t first = cached_address(pixels);
    const uintptr_t end = first + (uintptr_t)width * height * sizeof(uint16_t);
    if (!in_psram(pixels) || end > PSRAM_END || (first & 3u) != 0) {
        s_error = "The picture could not start: the pixels are not wholly in PSRAM, 4-byte aligned.";
        return false;
    }
    if (width == 0 || height == 0 || width % 2u != 0 || width > TVOUT_RING_WIDTH_MAX) {
        s_error = "The picture could not start: a PSRAM picture this wide does not fit the line buffers.";
        return false;
    }
    if (psram_init() == 0) {
        s_error = "The picture could not start: no PSRAM.";
        return false;
    }
    if (!timer1_counts_us()) {
        s_error = "The picture could not start: TIMER1 does not count microseconds.";
        return false;
    }
    if (!alarms_claim()) {
        s_error = "The picture could not start: no free TIMER1 alarm.";
        return false;
    }
    const int channel = dma_claim_unused_channel(false);
    if (channel < 0) {
        s_error = "The picture could not start: no free video channel.";
        return false;
    }

    s_ring.channel = (uint32_t)channel;
    s_ring.stream_base = (uint32_t)first;
    s_ring.width = width;
    s_ring.row_bytes = width * (uint32_t)sizeof(uint16_t);
    s_ring.row_words = s_ring.row_bytes / 4u;
    s_ring.stride_bytes = s_ring.row_bytes;
    dma_channel_config c = dma_channel_get_default_config((uint)channel);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_XIP_STREAM);
    dma_channel_configure((uint)channel, &c, s_lines, &xip_aux_hw->stream, s_ring.row_words, false);

    for (unsigned i = 0; i < TVOUT_RING_ROWS * TVOUT_RING_WIDTH_MAX; i++) s_lines[i] = 0;
    s_open = true;
    return true;
}

static const uint16_t *ring_lines(void) { return s_lines; }

static void ring_attach(const tvout_list_picture_t *pic, uint32_t (*scan_piece)(void)) {
    for (uint32_t row = 0; row < pic->rows; row++) {
        const uint32_t line = (uint32_t)(uintptr_t)(s_lines + tvout_ring_core_slot(row) * s_ring.width);
        if (row == 0)
            *pic->row0_read_addr = line;
        else
            pic->row_read_addrs[(row - 1u) * 2u] = line;
    }
    tvout_ring_core_init(pic->rows, pic->first_row, pic->pieces);
    s_ring.scan_piece = scan_piece;
    s_ring.hold_req = false;
    s_ring.reads = 0;
    s_ring.timeouts = 0;
    s_ring.read_us_max = 0;
    s_ring.reader_us = 0;
    s_ring.since_us = timer1_hw->timerawl;

    if (!s_reader_installed) {
        s_reader_installed = true;
        irq_set_exclusive_handler(s_tick_irq, reader_irq);
        irq_set_priority(s_tick_irq, PICO_HIGHEST_IRQ_PRIORITY);
        hw_set_bits(&timer1_hw->inte, s_ring.tick_mask);
        irq_set_enabled(s_tick_irq, true);
    }
}

// The rows already read their line buffers, so the TV shows them from the
// first frame; the late count starts once the reader is ahead. At most 2 ms
// for each of the two waits.
static bool ring_run(void) {
    uint32_t from = time_us_32();
    while (!tvout_ring_core_switch_start(s_ring.scan_piece())) {
        if (time_us_32() - from > SWITCH_TIMEOUT_US) {
            s_error = "The picture could not start: the TV position did not come round.";
            return false;
        }
    }
    hw_set_bits(&timer1_hw->intf, s_ring.tick_mask);

    from = time_us_32();
    while (!tvout_ring_core_switch_ready(s_ring.scan_piece)) {
        if (time_us_32() - from > SWITCH_TIMEOUT_US) {
            s_error = "The picture could not start: the PSRAM reader did not keep up.";
            return false;
        }
    }
    tvout_ring_core_switch_done();
    return true;
}

static void ring_close(void) {
    if (!s_open) return;
    tvout_ring_core_release(s_ring.scan_piece != NULL ? s_ring.scan_piece() : 0u);
    busy_wait_us(2u * READ_TIMEOUT_US);     // a read in flight on the other core ends; the tick is not re-armed
    timer1_hw->armed = s_ring.tick_mask;
    dma_channel_unclaim((uint)s_ring.channel);
    s_ring.scan_piece = NULL;
    s_open = false;
}

static bool ring_is_open(void) { return s_open; }

static const char *ring_error(void) { return s_error; }

static void ring_stats(tvout_stats_t *out) {
    out->rows_late = tvout_ring_core_late();
    out->row_reads = s_ring.reads;
    out->row_read_timeouts = s_ring.timeouts;
    out->row_read_us_max = s_ring.read_us_max;
    out->reader_us = s_ring.reader_us;
    out->reader_window_us = s_open ? timer1_hw->timerawl - s_ring.since_us : 0;
}

void tvout_core1_init(void) {
    if (s_ring.core1_ready || !alarms_claim()) return;
    irq_set_exclusive_handler(s_hold_irq, hold_irq);
    irq_set_priority(s_hold_irq, PICO_HIGHEST_IRQ_PRIORITY);
    hw_set_bits(&timer1_hw->inte, s_ring.hold_mask);
    irq_set_enabled(s_hold_irq, true);
    s_ring.core1_ready = true;
}

static const tvout_ring_ops_t RING_OPS = {
    .open = ring_open,
    .lines = ring_lines,
    .attach = ring_attach,
    .run = ring_run,
    .close = ring_close,
    .is_open = ring_is_open,
    .error = ring_error,
    .stats = ring_stats,
};

bool tvout_start_psram(const uint16_t *pixels, int width, int height) {
    return tvout_start_ring(pixels, width, height, &RING_OPS);
}

uint16_t *tvout_psram_uncached(uint16_t *pixels) {
    const uintptr_t addr = (uintptr_t)pixels;
    if (addr >= PSRAM_BASE && addr < PSRAM_END) return (uint16_t *)(addr + NOCACHE_OFFSET);
    return pixels;
}
