#include "uartkbd.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include <string.h>

/* On GPIO38/39 the plain UART function is UART1 CTS/RTS; the UART-AUX
 * function routes UART1 TX/RX here instead (RP2350 datasheet GPIO muxing). */
#define UARTKBD_UART   uart1
#define UARTKBD_TX_PIN 38
#define UARTKBD_RX_PIN 39
#define UARTKBD_BAUD   62500

/* DMA drains the UART RX FIFO into this ring continuously (endless mode,
 * no IRQ), so keyboard bytes survive arbitrarily long CPU stalls up to one
 * full ring (~164 ms of line traffic at 62500 baud). uartkbd_task() reads
 * strictly behind the DMA write pointer. Alignment is required for the
 * DMA write-address ring wrap. */
#define RING_BITS 10
#define RING_SIZE (1u << RING_BITS)
static uint8_t __attribute__((aligned(RING_SIZE))) s_ring[RING_SIZE];
static uint32_t s_rd;          /* software read index into s_ring */
static int      s_dma_chan = -1;

static uartkbd_parser_t s_parser;

/* agentio button injection. A synthetic frame is emitted on demand — exactly
 * once per uartkbd_inject_set() call, plus once at init to prime the parser —
 * never on a timer. The real keyboard reports on change or ~2x/second (see
 * Wilikeyboard.md), i.e. up to ~500 ms apart during a steady hold; a
 * time-based heartbeat shorter than that would misfire mid-hold and inject a
 * spurious release/re-press into ordinary input. */
static bool     s_synth_pending;

/* If the wire goes silent while the parser is mid-frame (disconnect at the
 * wrong instant, or garbage that coincidentally matches the sync bytes),
 * state would otherwise never return to hunt and a pending synthetic frame
 * could stall forever. Track the last time any RX byte was observed and
 * force the parser back to hunt if it has sat mid-frame too long. 250 ms
 * comfortably exceeds the ~3.7 ms it takes 23 bytes to arrive at 62500 baud,
 * and is comfortably shorter than the ~500 ms real-frame cadence. */
#define UARTKBD_MIDFRAME_TIMEOUT_MS 250u
static uint32_t s_last_byte_ms;
static uint32_t s_last_real_frame_ms;
static bool     s_have_real_frame;

/* Idle (all-released) wire values for the button bytes. Injected buttons are
 * OR'd in by the parser, so this frame never needs per-button bit knowledge. */
static void synth_frame(void)
{
    uint8_t f[UARTKBD_FRAME_LEN];
    memset(f, 0, sizeof f);
    f[0] = 0xBD; f[1] = 0x1D;
    f[2] = 0x3F; f[3] = 0x39; f[4] = 0x80; f[5] = 0x07;
    /* Carry the last real charger bytes through so injection never clobbers
     * charger telemetry with zeros. */
    memcpy(&f[10], s_parser.charger_raw, sizeof s_parser.charger_raw);
    uint8_t sum = 0;
    for (int i = 0; i < UARTKBD_FRAME_LEN - 1; i++) sum = (uint8_t)(sum + f[i]);
    f[UARTKBD_FRAME_LEN - 1] = sum;
    /* accept_frame() overwrites both charger_valid and flags on ANY
     * checksum-valid frame, synthetic or real, and f[3]/f[4] above are the
     * idle wire bytes for the connection-detect bits too — decode_flags()
     * reads f[3]&0x04 (AUDIO), f[3]&0x02 (HOTPLUG), f[4]&0x04 (USB), all
     * zero in the synthetic frame. Left alone, injecting a button would
     * momentarily report all three as absent until the next real frame (up
     * to ~500 ms). Save/restore both charger_valid and flags around the feed
     * so a synthetic frame preserves them exactly rather than setting them:
     * before any real charger data has arrived, restoring charger_valid
     * false stops uartkbd_charger() from fabricating engineering-unit
     * readings out of the zeroed charger_raw above; restoring flags always
     * keeps the last real AUDIO/HOTPLUG/USB state intact across the
     * synthetic frame. Once real data has arrived, both restores are a
     * no-op and the charger_raw passthrough still works as before. */
    /* The 20-byte status payload needs the same treatment, and for a sharper
     * reason: it is where picpwr_rails_decode() reads live RAIL STATE, and the
     * synthetic frame carries none — bytes 6..9 above are zero. Letting it
     * land makes picpwr_rails() report every rail OFF. An app running
     * picpwr_keep_awake() + picpwr_task() then sees its kept rails missing on
     * two agreeing frames (a `TAP` injects press AND release, so two synthetic
     * frames arrive back to back) and re-asserts an awake mask built from that
     * all-zero snapshot — which switches off every rail it failed to report,
     * including the LCD (zone 2), the USB hub (8) and the on-board debug probe
     * (16). Observed on hardware 2026-08-06: one `fw press` took the whole
     * board off USB until it was re-plugged. Restore it, exactly as above. */
    bool    had_charger = s_parser.charger_valid;
    uint8_t had_flags   = s_parser.flags;
    bool    had_status  = s_parser.status_valid;
    uint32_t had_frames = s_parser.frames;
    uint8_t had_raw[sizeof s_parser.status_raw];
    memcpy(had_raw, s_parser.status_raw, sizeof had_raw);

    for (int i = 0; i < UARTKBD_FRAME_LEN; i++) uartkbd_parse_byte(&s_parser, f[i]);

    s_parser.charger_valid = had_charger;
    s_parser.flags         = had_flags;
    s_parser.status_valid  = had_status;
    s_parser.frames        = had_frames;
    memcpy(s_parser.status_raw, had_raw, sizeof had_raw);
}

void uartkbd_init(void)
{
    uartkbd_parse_init(&s_parser);
    s_rd = 0;
    s_last_byte_ms  = to_ms_since_boot(get_absolute_time());
    s_last_real_frame_ms = 0;
    s_have_real_frame = false;
    s_synth_pending = false;

    uart_init(UARTKBD_UART, UARTKBD_BAUD);
    gpio_set_function(UARTKBD_TX_PIN, GPIO_FUNC_UART_AUX);
    gpio_set_function(UARTKBD_RX_PIN, GPIO_FUNC_UART_AUX);
    uart_set_format(UARTKBD_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(UARTKBD_UART, false, false);
    uart_set_fifo_enabled(UARTKBD_UART, true);

    s_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);      /* wrap write addr */
    channel_config_set_dreq(&c, DREQ_UART1_RX);
    dma_channel_configure(s_dma_chan, &c,
                          s_ring,                        /* write */
                          &uart_get_hw(UARTKBD_UART)->dr,/* read  */
                          0, false);                     /* count set below */
    /* Endless mode: TRANS_COUNT.MODE = 0xF -> count never decrements, the
     * channel runs forever. Writing the trigger alias starts it. */
    dma_channel_hw_addr(s_dma_chan)->al1_transfer_count_trig =
        ((uint32_t)DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS
             << DMA_CH0_TRANS_COUNT_MODE_LSB) | 1u;

    /* Prime the parser immediately so the very first injected press (with no
     * keyboard attached) produces a real edge rather than being swallowed by
     * the parser's own first-frame priming. */
    synth_frame();
}

void uartkbd_task(void)
{
    if (s_dma_chan < 0) return;
    uint32_t wr = (uint32_t)(dma_channel_hw_addr(s_dma_chan)->write_addr
                             - (uintptr_t)s_ring) & (RING_SIZE - 1);
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (s_rd != wr) s_last_byte_ms = now;
    uint32_t frames_before = uartkbd_parse_frames(&s_parser);
    while (s_rd != wr) {
        uartkbd_parse_byte(&s_parser, s_ring[s_rd]);
        s_rd = (s_rd + 1) & (RING_SIZE - 1);
    }
    if (uartkbd_parse_frames(&s_parser) != frames_before) {
        s_last_real_frame_ms = now;
        s_have_real_frame = true;
    }

    /* Mid-frame silence timeout: force the parser back to hunt if it has sat
     * outside hunt state too long with no bytes arriving (see comment on
     * UARTKBD_MIDFRAME_TIMEOUT_MS above). Keeps uartkbd_parse.c pure and
     * free of time. */
    if (s_parser.state != 0 && (now - s_last_byte_ms) >= UARTKBD_MIDFRAME_TIMEOUT_MS)
        s_parser.state = 0;

    /* Emit exactly one pending synthetic frame once the parser reaches hunt
     * state, so it can never corrupt a partially-received real frame. */
    if (s_synth_pending && s_parser.state == 0) {
        synth_frame();
        s_synth_pending = false;
    }
}

bool     uartkbd_next_event(uartkbd_event_t *ev) { return uartkbd_parse_next_event(&s_parser, ev); }
uint16_t uartkbd_buttons(void) { return uartkbd_parse_buttons(&s_parser); }
uint8_t  uartkbd_flags(void)   { return uartkbd_parse_flags(&s_parser); }
bool     uartkbd_charger(uartkbd_charger_t *out) { return uartkbd_parse_charger(&s_parser, out); }
uint32_t uartkbd_frames(void)  { return uartkbd_parse_frames(&s_parser); }
uint32_t uartkbd_errors(void)  { return uartkbd_parse_errors(&s_parser); }

bool uartkbd_button_down_fresh(uartkbd_btn_t button, uint32_t max_age_ms)
{
    return uartkbd_button_down_fresh_state(
        uartkbd_parse_buttons(&s_parser), uartkbd_parse_inject(&s_parser),
        button, s_have_real_frame,
        to_ms_since_boot(get_absolute_time()), s_last_real_frame_ms, max_age_ms);
}

void uartkbd_inject_set(uint16_t mask)
{
    uartkbd_parse_set_inject(&s_parser, mask);
    /* One synthetic frame on the next task() call carries the edge — no
     * timer, no periodic re-emission. If the parser is mid-frame right now,
     * the flag stays raised and the frame goes out on a later task() call. */
    s_synth_pending = true;
}

bool uartkbd_status_raw(uint8_t out[20])
{
    return uartkbd_parse_status_raw(&s_parser, out);
}

bool uartkbd_cmd_send(const uint8_t *frame, size_t len)
{
    if (s_dma_chan < 0) return false;
    /* Command gate: break wakes the receiver, then it announces readiness
     * with a bare 0xC9. */
    uart_set_break(UARTKBD_UART, true);
    busy_wait_us_32(2000);
    uart_set_break(UARTKBD_UART, false);
    /* The activity byte can land mid-frame from the parser's point of
     * view: scan raw ring bytes ahead of the parser, then drop whatever
     * partial frame was in flight. Bytes consumed here are discarded —
     * the next status frame refreshes every latched field. */
    bool active = false;
    unsigned polls = 50000;
    while (!active && polls-- != 0) {
        uint32_t wr = (uint32_t)(dma_channel_hw_addr(s_dma_chan)->write_addr
                                 - (uintptr_t)s_ring) & (RING_SIZE - 1);
        while (s_rd != wr) {
            uint8_t b = s_ring[s_rd];
            s_rd = (s_rd + 1) & (RING_SIZE - 1);
            if (b == 0xC9) { active = true; break; }
        }
        busy_wait_us_32(10);
    }
    uartkbd_parse_resync(&s_parser);
    busy_wait_us_32(5000);
    uart_write_blocking(UARTKBD_UART, frame, len);
    busy_wait_us_32(50000);             /* let the frame drain the link */
    return active;
}
