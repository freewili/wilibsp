/* dualcpu — the DISPLAY half of a combined DISPLAY + ESP32 app.
 *
 * The two halves share nothing but OneWili peer streams: best-effort
 * datagrams that MAIN routes between its clients (onewili_stream.h). This
 * half pings the ESP32 once a second and times the answers, shows the
 * telemetry the ESP32 sends about once a second, and drives the ESP32's RGB
 * LED and Wi-Fi scan from the touch buttons and the keypad.
 *
 * Messages (all fields little-endian, the first byte is the type):
 *   ESP32 -> DISPLAY  0x01 TELEMETRY  seq u16, uptime_s u32, temp_dC i16
 *                                     (INT16_MIN = unknown), int_free u32,
 *                                     psram_free u32, stream_drops u32,
 *                                     gpio_events u32, text_events u32,
 *                                     led mode/r/g/b u8, ap_total u8, n u8
 *                                     (35 bytes so far), then n <= 4 x
 *                                     { rssi i8, channel u8, ssid_len u8,
 *                                     ssid[ssid_len <= 12] }
 *                     0x04 PONG       id u32 (the PING's), esp_ms u32
 *   DISPLAY -> ESP32  0x02 SET_LED    mode u8 (0 off, 1 solid, 2 rainbow), r, g, b
 *                     0x03 PING       id u32, display_ms u32
 *                     0x05 SCAN_NOW   results ride the next TELEMETRY
 * Unknown types are ignored; short or inconsistent ones count as malformed.
 *
 * The bottom row labels the five front-panel buttons, each label directly
 * above its button: grey OFF, yellow RAINBOW, green GREEN, blue BLUE, red RED
 * (solid LED colours). Press the button or touch its label. SCAN, top right,
 * or OK or the nav centre starts a Wi-Fi scan. HOLD HOME 5 s leaves the app;
 * HOLD PAGE 5 s shows About.
 *
 * Bench control rides SEGGER RTT channel 0 (the DIAG channel): a "stats:"
 * line every 2 s with every counter as key=value, and one-line commands on
 * the down buffer -- stats, ping, burst <n>, led <mode> <r> <g> <b>, scan,
 * espmode [0|1], help -- each answered by one line starting with '='.
 *
 * MAIN must run firmware with peer streams, and the ESP32 the dual-CPU app's
 * ESP32 half. This app keeps the ESP32 powered and sets MAIN's ESP32 Mode to
 * OneWili API, without which MAIN ignores the ESP32's OneWili traffic. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_stream.h"
#include "input/app_recovery_onewili.h"
#include "input/picpwr.h"
#include "input/uartkbd.h"
#include "agentio/agentio.h"
#include "SEGGER_RTT.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_VERSION_STR "002"

/* Message types and SET_LED modes: the protocol above. */
#define DC_TELEMETRY    0x01
#define DC_SET_LED      0x02
#define DC_PING         0x03
#define DC_PONG         0x04
#define DC_SCAN_NOW     0x05
#define DC_LED_OFF      0
#define DC_LED_SOLID    1
#define DC_LED_RAINBOW  2
#define TELEMETRY_FIXED 35
#define PING_LEN        9
#define PONG_LEN        9
#define MAX_APS         4
#define SSID_MAX        12

#define LED_LEVEL        64     /* solid colours; the ESP32's rainbow peaks at 96 */
#define PING_PERIOD_MS   1000
#define PING_TIMEOUT_MS  3000   /* a PING unanswered this long counts as lost */
#define PING_SLOTS       128    /* well over one credit window of PINGs (768 / 19 = 40) */
#define STALE_MS         3000
#define UI_PERIOD_MS     200
#define STATS_PERIOD_MS  2000
#define TOUCH_PERIOD_MS  20     /* each poll is an I2C transaction; 50 Hz is plenty */
#define EXPIRE_PERIOD_MS 100
#define ZONES_RESEND_MS  5000   /* MAIN can reboot on its own and forget the rails */
#define FLASH_MS         150    /* button highlight after a press */
#define BURST_MAX        1000
#define DRAIN_MAX        64     /* bounded, so a flood cannot starve the rest of the loop */

/* RGB565, byte-swapped to the panel's wire order. */
#define BE(c)    ((uint16_t)((((c) >> 8) & 0xFFu) | (((c) & 0xFFu) << 8)))
#define C_BG     BE(0x0000)
#define C_WHITE  BE(0xFFFF)
#define C_GREY   BE(0x8410)
#define C_DGREY  BE(0x4208)
#define C_RED    BE(0xF800)
#define C_GREEN  BE(0x07E0)
#define C_YEL    BE(0xFFE0)
#define C_CYAN   BE(0x07FF)
#define C_NAVY   BE(0x0010)
#define C_TEAL   BE(0x0410)
/* The five front-panel buttons, in the colours the stock firmware's menu bar
 * uses for them (AGENTS.md, "The five front-panel buttons"). */
#define C_KEY_GREY   BE(0xD69A)
#define C_KEY_YELLOW BE(0xFF06)
#define C_KEY_GREEN  BE(0x1200)
#define C_KEY_BLUE   BE(0x00F8)
#define C_KEY_RED    BE(0x8007)

/* ~33 KB of link buffers: far too big for the stack. */
static ow_device dev;
static uint8_t   rx_buf[OW_STREAM_MTU];
static char      text_id[32];
static char      text_args[512];
static char      line[1536];            /* one stats line, written to RTT in one piece */

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static const char* st_name(int s) {
    static const char* const names[] = { "ok", "arg", "io", "timeout", "failed", "protocol", "buffer" };
    return s >= 0 && s < (int)(sizeof names / sizeof names[0]) ? names[s] : "?";
}

/* ── what this half sent and saw ───────────────────────────────────────── */
static struct {
    uint32_t tx_ok, tx_refused;           /* ow_stream_write accepted / refused */
    uint32_t rx, telemetry;
    uint32_t tele_gaps, tele_resyncs;     /* TELEMETRY seq skipped / restarted */
    uint32_t unknown, malformed, foreign, poll_errors;
    uint32_t text_lines;
} st;

/* PINGs waiting for their PONG, by id modulo PING_SLOTS. */
enum { SLOT_FREE, SLOT_PENDING, SLOT_EXPIRED };
typedef struct { uint32_t id, sent_us; uint8_t state; } ping_slot;
static struct {
    ping_slot slot[PING_SLOTS];
    uint32_t  next_id;
    uint32_t  sent, refused, answered, lost, late, unmatched, evicted;
    uint32_t  last_us, min_us, max_us;
    uint64_t  sum_us;
} ping;

typedef struct { int8_t rssi; uint8_t channel; char ssid[SSID_MAX + 1]; } ap_t;
static struct {
    int      valid;
    uint16_t seq;
    uint32_t uptime_s;
    int16_t  temp_dc;
    uint32_t int_free, psram_free, drops, gpio_events, text_events;
    uint8_t  led_mode, led_r, led_g, led_b;
    uint8_t  ap_total, ap_n;
    ap_t     ap[MAX_APS];
    uint32_t rx_ms;
} tele;

typedef enum { LINK_WAITING, LINK_LIVE, LINK_STALE } link_state;
static int        esp_seen;
static uint32_t   esp_last_rx_ms;
static int        esp_mode_value = -1, esp_mode_result = -1;   /* -1: not tried yet */
static char       last_action[40] = "none";
static int        last_action_ok = 1;
static uint32_t   rails_live;
static int        rails_valid;

static link_state esp_link(uint32_t now) {
    if (!esp_seen) return LINK_WAITING;
    return (uint32_t)(now - esp_last_rx_ms) <= STALE_MS ? LINK_LIVE : LINK_STALE;
}

static const char* link_name(link_state l) {
    return l == LINK_LIVE ? "live" : l == LINK_STALE ? "stale" : "waiting";
}

static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ── sending ───────────────────────────────────────────────────────────── */
static ow_status stream_send(const uint8_t* p, uint32_t n) {
    ow_status s = ow_stream_write(&dev, OW_PEER_ESP32, p, n);
    if (s == OW_OK) st.tx_ok++;
    else st.tx_refused++;
    return s;
}

static void note_action(const char* what, ow_status s) {
    snprintf(last_action, sizeof last_action, "%s %s", what, s == OW_OK ? "sent" : "REFUSED");
    last_action_ok = s == OW_OK;
    DIAG("dualcpu: %s -> %s\n", what, st_name((int)s));
}

static ow_status ping_send(void) {
    uint8_t p[PING_LEN];
    uint32_t id = ++ping.next_id;
    uint32_t t0 = time_us_32();
    ow_status s;
    p[0] = DC_PING;
    ow_stream_put_u32(p + 1, id);
    ow_stream_put_u32(p + 5, now_ms());
    s = stream_send(p, sizeof p);
    if (s != OW_OK) { ping.refused++; return s; }
    {
        ping_slot* slot = &ping.slot[id % PING_SLOTS];
        /* Only a burst far past the credit window can lap the table; that
         * PING is untracked rather than lost. */
        if (slot->state == SLOT_PENDING) ping.evicted++;
        slot->id = id;
        slot->sent_us = t0;
        slot->state = SLOT_PENDING;
    }
    ping.sent++;
    return OW_OK;
}

static ow_status led_send(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, const char* what) {
    uint8_t p[5] = { DC_SET_LED, mode, r, g, b };
    ow_status s = stream_send(p, sizeof p);
    note_action(what, s);
    return s;
}

static ow_status scan_send(void) {
    uint8_t p[1] = { DC_SCAN_NOW };
    ow_status s = stream_send(p, sizeof p);
    note_action("wifi scan", s);
    return s;
}

/* ── receiving ─────────────────────────────────────────────────────────── */
static void pong_rx(uint32_t id) {
    ping_slot* slot = &ping.slot[id % PING_SLOTS];
    uint32_t rtt;
    if (slot->id != id || slot->state == SLOT_FREE) { ping.unmatched++; return; }
    if (slot->state == SLOT_EXPIRED) { ping.late++; slot->state = SLOT_FREE; return; }
    rtt = time_us_32() - slot->sent_us;
    slot->state = SLOT_FREE;
    ping.answered++;
    ping.last_us = rtt;
    ping.sum_us += rtt;
    if (!ping.min_us || rtt < ping.min_us) ping.min_us = rtt;
    if (rtt > ping.max_us) ping.max_us = rtt;
}

static void ping_expire(void) {
    uint32_t t = time_us_32();
    for (unsigned i = 0; i < PING_SLOTS; i++) {
        ping_slot* slot = &ping.slot[i];
        if (slot->state == SLOT_PENDING && (uint32_t)(t - slot->sent_us) >= PING_TIMEOUT_MS * 1000u) {
            slot->state = SLOT_EXPIRED;
            ping.lost++;
        }
    }
}

/* Validates the whole datagram before touching the snapshot, so a bad one
 * leaves the last good telemetry on screen. */
static int telemetry_rx(const uint8_t* d, int n, uint32_t now) {
    unsigned o = TELEMETRY_FIXED, count, i;
    uint16_t seq;
    if (n < TELEMETRY_FIXED) return 0;
    count = d[34];
    if (count > MAX_APS) return 0;
    for (i = 0; i < count; i++) {
        if (o + 3u > (unsigned)n || d[o + 2] > SSID_MAX || o + 3u + d[o + 2] > (unsigned)n) return 0;
        o += 3u + d[o + 2];
    }
    seq = get_u16(d + 1);
    if (tele.valid) {
        uint16_t skipped = (uint16_t)(seq - (uint16_t)(tele.seq + 1u));
        /* A large jump is the ESP32 restarting its count, not lost frames. */
        if (skipped != 0 && skipped < 1000u) st.tele_gaps += skipped;
        else if (skipped != 0) st.tele_resyncs++;
    }
    tele.seq         = seq;
    tele.uptime_s    = ow_stream_get_u32(d + 3);
    tele.temp_dc     = (int16_t)get_u16(d + 7);
    tele.int_free    = ow_stream_get_u32(d + 9);
    tele.psram_free  = ow_stream_get_u32(d + 13);
    tele.drops       = ow_stream_get_u32(d + 17);
    tele.gpio_events = ow_stream_get_u32(d + 21);
    tele.text_events = ow_stream_get_u32(d + 25);
    tele.led_mode    = d[29];
    tele.led_r       = d[30];
    tele.led_g       = d[31];
    tele.led_b       = d[32];
    tele.ap_total    = d[33];
    tele.ap_n        = (uint8_t)count;
    for (i = 0, o = TELEMETRY_FIXED; i < count; i++) {
        ap_t* a = &tele.ap[i];
        unsigned len = d[o + 2], k;
        a->rssi = (int8_t)d[o];
        a->channel = d[o + 1];
        for (k = 0; k < len; k++) {
            char c = (char)d[o + 3 + k];
            a->ssid[k] = c >= 0x20 && c <= 0x7E ? c : '?';   /* the font has ASCII only */
        }
        a->ssid[len] = 0;
        o += 3u + len;
    }
    tele.rx_ms = now;
    tele.valid = 1;
    return 1;
}

static void datagram_rx(ow_peer src, const uint8_t* d, int n, uint32_t now) {
    st.rx++;
    if (src != OW_PEER_ESP32) { st.foreign++; return; }
    esp_seen = 1;
    esp_last_rx_ms = now;
    switch (d[0]) {
    case DC_TELEMETRY:
        if (telemetry_rx(d, n, now)) st.telemetry++;
        else st.malformed++;
        break;
    case DC_PONG:
        if (n < PONG_LEN) st.malformed++;
        else pong_rx(ow_stream_get_u32(d + 1));
        break;
    default:
        st.unknown++;
        break;
    }
}

/* Also sends the stream link's HELLO/keepalive: without regular polls MAIN
 * closes this CPU's stream latch after 3 s and drops datagrams for it. */
static void stream_drain(void) {
    ow_peer src = OW_PEER_MAIN;
    uint32_t now = now_ms();
    int n, guard = 0;
    while (guard++ < DRAIN_MAX && (n = ow_stream_poll(&dev, &src, rx_buf, sizeof rx_buf)) != 0) {
        if (n < 0) { st.poll_errors++; continue; }
        datagram_rx(src, rx_buf, n, now);
    }
}

/* MAIN tees its text events to this CPU once the display bridge is active;
 * nobody here wants them, but an undrained FIFO drops whole frames. */
static void text_drain(void) {
    int guard = 0;
    while (guard++ < DRAIN_MAX &&
           ow_poll_text_line(&dev, text_id, sizeof text_id, text_args, sizeof text_args) == 1)
        st.text_lines++;
}

/* ── ESP32 Mode ────────────────────────────────────────────────────────── */
static void esp_mode_set(int32_t v) {
    ow_status s = ow_wireless_e_sp32_mode(&dev, v);
    esp_mode_value = (int)v;
    esp_mode_result = (int)s;
    DIAG("dualcpu: ESP32 Mode <- %d (%s): %s\n", (int)v, v ? "OneWili API" : "Default Firmware",
         st_name((int)s));
}

/* ── screen ────────────────────────────────────────────────────────────── */
/* Left column: the ESP32's telemetry; right column: this side of the link.
 * Every value is a fixed-width padded field redrawn only when its text or
 * colour changes, so the loop never repaints the whole panel. */
#define COL_L    4
#define COL_R    252
#define ROW0     46
#define PITCH2   18
#define ROW1     140      /* first small-text row on the right */
#define PITCH1   11
#define BTN_Y    276
#define BTN_H    40
/* The bottom row puts one label directly above each front-panel button. The
 * buttons are equally spaced along the bottom edge, so the boxes are fixed
 * fifths of the width, never sized to their labels. */
#define KEY_W     ((ST7796_W - 12) / 5)   /* 93 */
#define KEY_PITCH (KEY_W + 3)             /* 96 */
#define KEY_X(i)  ((i) * KEY_PITCH)
#define SCAN_W   80
#define SCAN_H   24
#define SCAN_X   (ST7796_W - SCAN_W - 4)
#define SCAN_Y   2
#define SWATCH_X 214
#define SWATCH_S 16

enum {
    F_UP, F_TEMP, F_HEAP, F_GPIO, F_TEXT, F_EDROPS, F_LED, F_APS, F_AP0, F_AP1, F_AP2, F_AP3,
    F_LINK, F_RTT, F_AVG, F_LOST, F_AGE,
    F_PINGS, F_TXRX, F_DROPS, F_TELE, F_FWRX, F_FWTX, F_MAIN, F_RAILS, F_ESPMODE, F_LAST, F_MISC,
    F_COUNT
};
typedef struct { int16_t x, y; uint8_t scale, width; } field_pos;
static const field_pos FIELDS[F_COUNT] = {
    [F_UP]     = { COL_L, ROW0 + 0 * PITCH2, 2, 20 },
    [F_TEMP]   = { COL_L, ROW0 + 1 * PITCH2, 2, 20 },
    [F_HEAP]   = { COL_L, ROW0 + 2 * PITCH2, 2, 20 },
    [F_GPIO]   = { COL_L, ROW0 + 3 * PITCH2, 2, 20 },
    [F_TEXT]   = { COL_L, ROW0 + 4 * PITCH2, 2, 20 },
    [F_EDROPS] = { COL_L, ROW0 + 5 * PITCH2, 2, 20 },
    [F_LED]    = { COL_L, ROW0 + 6 * PITCH2, 2, 17 },   /* the colour swatch follows */
    [F_APS]    = { COL_L, ROW0 + 7 * PITCH2, 2, 20 },
    [F_AP0]    = { COL_L, ROW0 + 8 * PITCH2, 2, 20 },
    [F_AP1]    = { COL_L, ROW0 + 9 * PITCH2, 2, 20 },
    [F_AP2]    = { COL_L, ROW0 + 10 * PITCH2, 2, 20 },
    [F_AP3]    = { COL_L, ROW0 + 11 * PITCH2, 2, 20 },
    [F_LINK]   = { COL_R, ROW0 + 0 * PITCH2, 2, 18 },
    [F_RTT]    = { COL_R, ROW0 + 1 * PITCH2, 2, 18 },
    [F_AVG]    = { COL_R, ROW0 + 2 * PITCH2, 2, 18 },
    [F_LOST]   = { COL_R, ROW0 + 3 * PITCH2, 2, 18 },
    [F_AGE]    = { COL_R, ROW0 + 4 * PITCH2, 2, 18 },
    [F_PINGS]  = { COL_R, ROW1 + 0 * PITCH1, 1, 37 },
    [F_TXRX]   = { COL_R, ROW1 + 1 * PITCH1, 1, 37 },
    [F_DROPS]  = { COL_R, ROW1 + 2 * PITCH1, 1, 37 },
    [F_TELE]   = { COL_R, ROW1 + 3 * PITCH1, 1, 37 },
    [F_FWRX]   = { COL_R, ROW1 + 4 * PITCH1, 1, 37 },
    [F_FWTX]   = { COL_R, ROW1 + 5 * PITCH1, 1, 37 },
    [F_MAIN]   = { COL_R, ROW1 + 6 * PITCH1, 1, 37 },
    [F_RAILS]  = { COL_R, ROW1 + 7 * PITCH1, 1, 37 },
    [F_ESPMODE]= { COL_R, ROW1 + 8 * PITCH1, 1, 37 },
    [F_LAST]   = { COL_R, ROW1 + 9 * PITCH1, 1, 37 },
    [F_MISC]   = { COL_R, ROW1 + 10 * PITCH1, 1, 37 },
};
static char     shown[F_COUNT][48];
static uint16_t shown_fg[F_COUNT];
static int32_t  shown_swatch = -1;

static void field(int id, uint16_t fg, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void field(int id, uint16_t fg, const char* fmt, ...) {
    char text[48], padded[48];
    const field_pos* f = &FIELDS[id];
    va_list ap;
    int n;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    n = (int)strlen(text);
    if (n > f->width) n = f->width;
    memcpy(padded, text, (size_t)n);
    memset(padded + n, ' ', (size_t)(f->width - n));
    padded[f->width] = 0;
    if (shown_fg[id] == fg && strcmp(shown[id], padded) == 0) return;
    strcpy(shown[id], padded);
    shown_fg[id] = fg;
    st7796_draw_text(f->x, f->y, f->scale, fg, C_BG, padded);
}

static void ui_invalidate(void) {
    memset(shown, 0, sizeof shown);
    shown_swatch = -1;
}

/* The LED as last reported, brightened so a dim colour still reads as that
 * colour on the panel. Off is an empty grey box. */
static void swatch_update(void) {
    int32_t key;
    uint16_t c;
    int y = FIELDS[F_LED].y;
    uint32_t r = tele.led_r, g = tele.led_g, b = tele.led_b, m = r > g ? r : g;
    if (b > m) m = b;
    if (!tele.valid || m == 0) {
        key = 0x1000000;
    } else {
        r = r * 255u / m; g = g * 255u / m; b = b * 255u / m;
        key = (int32_t)((r << 16) | (g << 8) | b);
    }
    if (key == shown_swatch) return;
    shown_swatch = key;
    if (key == 0x1000000) {
        st7796_fill_rect(SWATCH_X, y, SWATCH_S, SWATCH_S, C_GREY);
        st7796_fill_rect(SWATCH_X + 2, y + 2, SWATCH_S - 4, SWATCH_S - 4, C_BG);
        return;
    }
    c = BE((uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3)));
    st7796_fill_rect(SWATCH_X, y, SWATCH_S, SWATCH_S, c);
}

/* ── buttons ───────────────────────────────────────────────────────────── */
typedef enum { ACT_RED, ACT_GREEN, ACT_BLUE, ACT_RAINBOW, ACT_OFF, ACT_SCAN, ACT_COUNT } action_t;
typedef struct { const char* label; uint16_t bg, fg; int x, y, w, h; } button_t;
/* Each bottom-row box sits over its front-panel button and wears that
 * button's colour. SCAN has no button in that row (it is OK or the nav
 * centre), so it lives in the title bar rather than breaking the row. */
static const button_t buttons[ACT_COUNT] = {
    [ACT_OFF]     = { "OFF",     C_KEY_GREY,   C_BG,    KEY_X(0), BTN_Y, KEY_W, BTN_H },
    [ACT_RAINBOW] = { "RAINBOW", C_KEY_YELLOW, C_BG,    KEY_X(1), BTN_Y, KEY_W, BTN_H },
    [ACT_GREEN]   = { "GREEN",   C_KEY_GREEN,  C_WHITE, KEY_X(2), BTN_Y, KEY_W, BTN_H },
    [ACT_BLUE]    = { "BLUE",    C_KEY_BLUE,   C_WHITE, KEY_X(3), BTN_Y, KEY_W, BTN_H },
    [ACT_RED]     = { "RED",     C_KEY_RED,    C_WHITE, KEY_X(4), BTN_Y, KEY_W, BTN_H },
    [ACT_SCAN]    = { "SCAN",    C_TEAL,       C_WHITE, SCAN_X, SCAN_Y, SCAN_W, SCAN_H },
};
/* Front-panel button to action, from UARTKBD_BTN_GREY: the same left-to-right
 * order as the row. */
static const action_t KEY_ACTION[5] = { ACT_OFF, ACT_RAINBOW, ACT_GREEN, ACT_BLUE, ACT_RED };
static int      flash_btn = -1;
static uint32_t flash_until;

static void button_draw(int i, int lit) {
    const button_t* b = &buttons[i];
    int tw = (int)strlen(b->label) * 12;
    st7796_fill_rect(b->x, b->y, b->w, b->h, b->bg);
    st7796_draw_text(b->x + (b->w - tw) / 2, b->y + (b->h - 16) / 2, 2, b->fg, b->bg, b->label);
    if (lit) {   /* in the label colour, which contrasts with every box */
        st7796_fill_rect(b->x, b->y, b->w, 3, b->fg);
        st7796_fill_rect(b->x, b->y + b->h - 3, b->w, 3, b->fg);
        st7796_fill_rect(b->x, b->y, 3, b->h, b->fg);
        st7796_fill_rect(b->x + b->w - 3, b->y, 3, b->h, b->fg);
    }
}

static void button_flash(int i, uint32_t now) {
    if (flash_btn >= 0 && flash_btn != i) button_draw(flash_btn, 0);
    flash_btn = i;
    flash_until = now + FLASH_MS;
    button_draw(i, 1);
}

static void action_run(action_t a, uint32_t now) {
    button_flash((int)a, now);
    switch (a) {
    case ACT_RED:     led_send(DC_LED_SOLID, LED_LEVEL, 0, 0, "led red"); break;
    case ACT_GREEN:   led_send(DC_LED_SOLID, 0, LED_LEVEL, 0, "led green"); break;
    case ACT_BLUE:    led_send(DC_LED_SOLID, 0, 0, LED_LEVEL, "led blue"); break;
    case ACT_RAINBOW: led_send(DC_LED_RAINBOW, 0, 0, 0, "led rainbow"); break;
    case ACT_OFF:     led_send(DC_LED_OFF, 0, 0, 0, "led off"); break;
    case ACT_SCAN:    scan_send(); break;
    default:          break;
    }
}

/* The static layer: title bar, headers, dividers and buttons. */
static void screen_static(void) {
    st7796_fill_rect(0, 0, ST7796_W, 28, C_NAVY);
    st7796_draw_text(8, 6, 2, C_WHITE, C_NAVY, "DUAL CPU");
    st7796_draw_text(116, 5, 1, C_CYAN, C_NAVY, "DISPLAY + ESP32 PEER STREAMS");
    st7796_draw_text(116, 16, 1, C_GREY, C_NAVY, "v" APP_VERSION_STR "  HOLD HOME 5S EXIT");
    st7796_draw_text(COL_L, 34, 1, C_CYAN, C_BG, "ESP32 TELEMETRY");
    st7796_draw_text(COL_R, 34, 1, C_CYAN, C_BG, "LINK / RTT / STREAMS");
    st7796_fill_rect(246, 32, 1, 234, C_DGREY);
    st7796_fill_rect(0, 269, ST7796_W, 1, C_DGREY);
    for (int i = 0; i < ACT_COUNT; i++) button_draw(i, 0);
}

/* After the PAGE-hold About page: the snapshot the BSP restores cannot know
 * which fields changed while the page was up, so repaint everything once. */
static void screen_redraw(void) {
    st7796_fill_screen(C_BG);
    screen_static();
    ui_invalidate();
    flash_btn = -1;
}

static void ms_tenths(char* out, size_t cap, uint32_t us) {
    snprintf(out, cap, "%u.%u", (unsigned)(us / 1000u), (unsigned)(us % 1000u / 100u));
}

static void ui_update(uint32_t now) {
    char a[16], b[16];
    ow_fwgui_stats ls;
    link_state link = esp_link(now);
    uint32_t drops = ow_stream_drops(&dev);
    ow_fwgui_get_stats(&ls);

    if (!tele.valid) {
        field(F_UP, C_GREY, "up --");
        field(F_TEMP, C_GREY, "temp --");
        field(F_HEAP, C_GREY, "ram -- ps --");
        field(F_GPIO, C_GREY, "gpio events --");
        field(F_TEXT, C_GREY, "text events --");
        field(F_EDROPS, C_GREY, "esp drops --");
        field(F_LED, C_GREY, "led --");
        field(F_APS, C_GREY, "aps --");
    } else {
        uint32_t u = tele.uptime_s;
        if (u >= 86400u)
            field(F_UP, C_WHITE, "up %ud %02u:%02u:%02u", (unsigned)(u / 86400u), (unsigned)(u / 3600u % 24u),
                  (unsigned)(u / 60u % 60u), (unsigned)(u % 60u));
        else
            field(F_UP, C_WHITE, "up %02u:%02u:%02u", (unsigned)(u / 3600u), (unsigned)(u / 60u % 60u),
                  (unsigned)(u % 60u));
        if (tele.temp_dc == INT16_MIN) {
            field(F_TEMP, C_GREY, "temp --");
        } else {
            int t = tele.temp_dc;
            field(F_TEMP, C_WHITE, "temp %s%d.%d C", t < 0 ? "-" : "", abs(t) / 10, abs(t) % 10);
        }
        field(F_HEAP, C_WHITE, "ram %uK ps %uK", (unsigned)(tele.int_free / 1024u),
              (unsigned)(tele.psram_free / 1024u));
        field(F_GPIO, C_WHITE, "gpio events %u", (unsigned)tele.gpio_events);
        field(F_TEXT, C_WHITE, "text events %u", (unsigned)tele.text_events);
        field(F_EDROPS, tele.drops ? C_YEL : C_WHITE, "esp drops %u", (unsigned)tele.drops);
        if (tele.led_mode == DC_LED_OFF)
            field(F_LED, C_WHITE, "led off");
        else if (tele.led_mode == DC_LED_RAINBOW)
            field(F_LED, C_WHITE, "led rainbow");
        else
            field(F_LED, C_WHITE, "led %u,%u,%u", (unsigned)tele.led_r, (unsigned)tele.led_g,
                  (unsigned)tele.led_b);
        if (tele.ap_n == 0)
            field(F_APS, C_GREY, "no APs yet - SCAN");
        else
            field(F_APS, C_CYAN, "aps %u, top %u:", (unsigned)tele.ap_total, (unsigned)tele.ap_n);
    }
    swatch_update();
    for (int i = 0; i < MAX_APS; i++) {
        if (tele.valid && i < tele.ap_n)
            field(F_AP0 + i, C_WHITE, "%d %u %s", (int)tele.ap[i].rssi, (unsigned)tele.ap[i].channel,
                  tele.ap[i].ssid);
        else
            field(F_AP0 + i, C_WHITE, "%s", "");
    }

    if (link == LINK_LIVE) field(F_LINK, C_GREEN, "esp32 LIVE");
    else if (link == LINK_STALE)
        field(F_LINK, C_RED, "esp32 STALE %us", (unsigned)((now - esp_last_rx_ms) / 1000u));
    else field(F_LINK, C_YEL, "waiting for esp32");
    if (ping.answered) {
        ms_tenths(a, sizeof a, ping.last_us);
        field(F_RTT, C_WHITE, "rtt %s ms", a);
        ms_tenths(a, sizeof a, (uint32_t)(ping.sum_us / ping.answered));
        field(F_AVG, C_WHITE, "avg %s ms", a);
    } else {
        field(F_RTT, C_GREY, "rtt --");
        field(F_AVG, C_GREY, "avg --");
    }
    field(F_LOST, ping.lost ? C_YEL : C_WHITE, "lost %u late %u", (unsigned)ping.lost, (unsigned)ping.late);
    if (tele.valid) {
        uint32_t age = now - tele.rx_ms;
        field(F_AGE, age > STALE_MS ? C_RED : C_WHITE, "tele age %u.%u s", (unsigned)(age / 1000u),
              (unsigned)(age % 1000u / 100u));
    } else {
        field(F_AGE, C_GREY, "tele age --");
    }

    ms_tenths(a, sizeof a, ping.min_us);
    ms_tenths(b, sizeof b, ping.max_us);
    field(F_PINGS, C_WHITE, "ping %u/%u rtt %s-%s ms", (unsigned)ping.sent, (unsigned)ping.answered, a, b);
    field(F_TXRX, st.tx_refused ? C_YEL : C_WHITE, "sent %u recv %u refused %u", (unsigned)st.tx_ok,
          (unsigned)st.rx, (unsigned)st.tx_refused);
    field(F_DROPS, drops ? C_YEL : C_WHITE, "ow_stream_drops %u  malformed %u", (unsigned)drops,
          (unsigned)st.malformed);
    field(F_TELE, C_WHITE, "tele %u gap %u unk %u other %u", (unsigned)st.telemetry, (unsigned)st.tele_gaps,
          (unsigned)st.unknown, (unsigned)st.foreign);
    field(F_FWRX, ls.stream_dropped || ls.stream_malformed ? C_YEL : C_WHITE,
          "fwgui rx %u drop %u mal %u max %u", (unsigned)ls.frames_stream, (unsigned)ls.stream_dropped,
          (unsigned)ls.stream_malformed, (unsigned)ls.stream_max_fill);
    field(F_FWTX, ls.stream_tx_lost ? C_YEL : C_WHITE, "fwgui tx %u ref %u lost %u",
          (unsigned)ls.stream_tx_frames, (unsigned)ls.stream_tx_refused, (unsigned)ls.stream_tx_lost);
    field(F_MAIN, ls.stream_confirmed ? C_GREEN : C_YEL, "main link %s credits %u",
          ls.stream_confirmed ? "CONFIRMED" : "NO CREDIT YET", (unsigned)ls.stream_credits);
    if (rails_valid)
        field(F_RAILS, C_WHITE, "rails esp32 %s  esp led 5v %s",
              (rails_live & picpwr_zone_bit(PICPWR_ZONE_WIFI_BT)) ? "ON" : "OFF",
              (rails_live & picpwr_zone_bit(PICPWR_ZONE_RGB_LEDS)) ? "ON" : "OFF");
    else
        field(F_RAILS, C_GREY, "rails ?");
    if (esp_mode_result < 0)
        field(F_ESPMODE, C_GREY, "esp32 mode: not set");
    else if (esp_mode_result == OW_OK)
        field(F_ESPMODE, C_GREEN, "esp32 mode: %s set", esp_mode_value ? "OneWili API" : "default");
    else
        field(F_ESPMODE, C_RED, "esp32 mode: FAILED (%s)", st_name(esp_mode_result));
    field(F_LAST, last_action_ok ? C_WHITE : C_RED, "last: %s", last_action);
    field(F_MISC, C_GREY, "txt lines %u ring %u cksum %u", (unsigned)st.text_lines,
          (unsigned)ls.ring_max_fill, (unsigned)ls.checksum_errors);
}

/* ── input ─────────────────────────────────────────────────────────────── */
static void touch_poll(uint32_t now) {
    static int was_down;
    uint16_t x, y;
    int down = ft6336_poll(&x, &y);
    if (down && !was_down) {
        /* 4 px of slack above and below; the gaps between boxes do nothing. */
        for (int i = 0; i < ACT_COUNT; i++) {
            const button_t* b = &buttons[i];
            if (x >= b->x && x < b->x + b->w && y + 4 >= b->y && y < b->y + b->h + 4) {
                action_run((action_t)i, now);
                break;
            }
        }
    }
    was_down = down;
}

static void keys_poll(uint32_t now) {
    static uint32_t page_down_at;
    static int page_down;
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if (ev.btn == UARTKBD_BTN_PAGE) {
            /* About opens after a 5 s hold, timed from the button level; this
             * edge is seen a loop pass late at most. Redrawing after a hold
             * just short of it is harmless, missing one leaves stale fields. */
            if (ev.pressed) { page_down = 1; page_down_at = now; }
            else if (page_down && (uint32_t)(now - page_down_at) >= 4500u) screen_redraw();
            if (!ev.pressed) page_down = 0;
            continue;
        }
        if (!ev.pressed) continue;
        switch (ev.btn) {
        case UARTKBD_BTN_GREY:
        case UARTKBD_BTN_YELLOW:
        case UARTKBD_BTN_GREEN:
        case UARTKBD_BTN_BLUE:
        case UARTKBD_BTN_RED:        action_run(KEY_ACTION[ev.btn - UARTKBD_BTN_GREY], now); break;
        case UARTKBD_BTN_OK:
        case UARTKBD_BTN_NAV_CENTER: action_run(ACT_SCAN, now); break;
        default:                     break;
        }
    }
}

/* ── RTT: stats line and command channel ───────────────────────────────── */
/* One key=value line with every counter, for a host script to parse. It is
 * written with a single SEGGER_RTT_Write so NO_BLOCK_SKIP drops it whole or
 * not at all, never half a line. */
static void stats_emit(const char* prefix) {
    ow_fwgui_stats ls;
    uint32_t now = now_ms();
    int n;
    ow_fwgui_get_stats(&ls);
    n = snprintf(line, sizeof line,
        "%s ms=%u link=%s main=%u espmode=%d espmode_st=%s"
        " tx=%u txref=%u rx=%u tele=%u telegap=%u teleresync=%u pong=%u unk=%u mal=%u foreign=%u pollerr=%u"
        " ping=%u pingref=%u lost=%u late=%u unmatched=%u evicted=%u"
        " rtt_last_us=%u rtt_avg_us=%u rtt_min_us=%u rtt_max_us=%u tele_age_ms=%d"
        " drops=%u fs=%u sdrop=%u smal=%u smax=%u stx=%u sref=%u slost=%u cred=%u"
        " txt=%u ring=%u ovr=%u hwovr=%u cksum=%u lenerr=%u fdrop=%u"
        " esp_seq=%u esp_up=%u esp_temp_dc=%d esp_ram=%u esp_psram=%u esp_drops=%u esp_gpio=%u esp_text=%u"
        " led=%u,%u,%u,%u aps=%u/%u rails=%x\n",
        prefix, (unsigned)now, link_name(esp_link(now)), (unsigned)ls.stream_confirmed, esp_mode_value,
        esp_mode_result < 0 ? "none" : st_name(esp_mode_result),
        (unsigned)st.tx_ok, (unsigned)st.tx_refused, (unsigned)st.rx, (unsigned)st.telemetry,
        (unsigned)st.tele_gaps, (unsigned)st.tele_resyncs, (unsigned)ping.answered, (unsigned)st.unknown,
        (unsigned)st.malformed, (unsigned)st.foreign, (unsigned)st.poll_errors,
        (unsigned)ping.sent, (unsigned)ping.refused, (unsigned)ping.lost, (unsigned)ping.late,
        (unsigned)ping.unmatched, (unsigned)ping.evicted,
        (unsigned)ping.last_us, (unsigned)(ping.answered ? ping.sum_us / ping.answered : 0),
        (unsigned)ping.min_us, (unsigned)ping.max_us, tele.valid ? (int)(now - tele.rx_ms) : -1,
        (unsigned)ow_stream_drops(&dev), (unsigned)ls.frames_stream, (unsigned)ls.stream_dropped,
        (unsigned)ls.stream_malformed, (unsigned)ls.stream_max_fill, (unsigned)ls.stream_tx_frames,
        (unsigned)ls.stream_tx_refused, (unsigned)ls.stream_tx_lost, (unsigned)ls.stream_credits,
        (unsigned)st.text_lines, (unsigned)ls.ring_max_fill, (unsigned)ls.ring_overrun_bytes,
        (unsigned)ls.hw_overruns, (unsigned)ls.checksum_errors, (unsigned)ls.length_errors,
        (unsigned)ls.dropped_frames,
        (unsigned)tele.seq, (unsigned)tele.uptime_s, (int)tele.temp_dc, (unsigned)tele.int_free,
        (unsigned)tele.psram_free, (unsigned)tele.drops, (unsigned)tele.gpio_events, (unsigned)tele.text_events,
        (unsigned)tele.led_mode, (unsigned)tele.led_r, (unsigned)tele.led_g, (unsigned)tele.led_b,
        (unsigned)tele.ap_total, (unsigned)tele.ap_n, (unsigned)rails_live);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) { n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    SEGGER_RTT_Write(0, line, (unsigned)n);
}

static uint32_t arg_u(char** argv, int argc, int i, uint32_t dflt) {
    return i < argc ? (uint32_t)strtoul(argv[i], 0, 0) : dflt;
}

static int led_mode_arg(const char* s) {
    if (strcmp(s, "off") == 0) return DC_LED_OFF;
    if (strcmp(s, "solid") == 0) return DC_LED_SOLID;
    if (strcmp(s, "rainbow") == 0) return DC_LED_RAINBOW;
    return (int)strtol(s, 0, 0);
}

static void dispatch(char* cmd) {
    char* argv[8];
    int argc = 0;
    char* p = cmd;
    while (*p && argc < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;
    for (p = argv[0]; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

    if (strcmp(argv[0], "stats") == 0) {
        stats_emit("=stats:");
    } else if (strcmp(argv[0], "ping") == 0) {
        ow_status s = ping_send();
        SEGGER_RTT_printf(0, "=ping id=%u %s\n", (unsigned)ping.next_id, st_name((int)s));
    } else if (strcmp(argv[0], "burst") == 0) {
        /* Back-to-back PINGs: once the credit window (OW_STREAM_WINDOW
         * accounting bytes, OW_STREAM_WIRE_BYTES(9) = 19 per PING) is full the
         * rest are refused locally until MAIN's next CREDIT, which is what
         * this is for. */
        uint32_t n = arg_u(argv, argc, 1, 10), ok = 0, refused = 0, i;
        ow_fwgui_stats before, after;
        if (n > BURST_MAX) n = BURST_MAX;
        ow_fwgui_get_stats(&before);
        for (i = 0; i < n; i++) {
            if (ping_send() == OW_OK) ok++;
            else refused++;
            /* Keep taking datagrams (the PONGs) off the receive FIFO while
             * the burst runs, as the main loop would. */
            stream_drain();
            if ((i & 63u) == 63u) fw2_app_recovery_task();
        }
        ow_fwgui_get_stats(&after);
        SEGGER_RTT_printf(0, "=burst n=%u accepted=%u refused=%u link_refused=%u credits=%u window=%u\n",
                          (unsigned)n, (unsigned)ok, (unsigned)refused,
                          (unsigned)(after.stream_tx_refused - before.stream_tx_refused),
                          (unsigned)(after.stream_credits - before.stream_credits), (unsigned)OW_STREAM_WINDOW);
    } else if (strcmp(argv[0], "led") == 0) {
        int mode = argc > 1 ? led_mode_arg(argv[1]) : -1;
        uint32_t r = arg_u(argv, argc, 2, 0), g = arg_u(argv, argc, 3, 0), b = arg_u(argv, argc, 4, 0);
        ow_status s;
        if (mode < DC_LED_OFF || mode > DC_LED_RAINBOW || r > 255 || g > 255 || b > 255) {
            SEGGER_RTT_printf(0, "=err usage: led <0|1|2|off|solid|rainbow> <r> <g> <b>\n");
            return;
        }
        s = led_send((uint8_t)mode, (uint8_t)r, (uint8_t)g, (uint8_t)b, "led (rtt)");
        SEGGER_RTT_printf(0, "=led mode=%d rgb=%u,%u,%u %s\n", mode, (unsigned)r, (unsigned)g, (unsigned)b,
                          st_name((int)s));
    } else if (strcmp(argv[0], "scan") == 0) {
        SEGGER_RTT_printf(0, "=scan %s\n", st_name((int)scan_send()));
    } else if (strcmp(argv[0], "espmode") == 0) {
        /* Blocks for one MAIN round trip (up to the 5 s command timeout). */
        esp_mode_set((int32_t)arg_u(argv, argc, 1, 1));
        SEGGER_RTT_printf(0, "=espmode %d %s\n", esp_mode_value, st_name(esp_mode_result));
    } else if (strcmp(argv[0], "help") == 0) {
        SEGGER_RTT_printf(0, "=cmds: stats | ping | burst <n> | led <mode> <r> <g> <b> | scan | espmode [0|1]\n");
    } else {
        SEGGER_RTT_printf(0, "=err unknown command %s\n", argv[0]);
    }
}

static void rtt_poll(void) {
    static char cmd[128];
    static unsigned len;
    uint8_t b[64];
    unsigned n;
    while ((n = SEGGER_RTT_Read(0, b, sizeof b)) > 0) {
        for (unsigned i = 0; i < n; i++) {
            char c = (char)b[i];
            if (c == '\n' || c == '\r') {
                if (len) { cmd[len] = 0; dispatch(cmd); }
                len = 0;
            } else if (len + 1 < sizeof cmd) {
                cmd[len++] = c;
            }
        }
    }
}

/* Logs the transitions a bench operator wants to see in the RTT stream. */
static void link_watch(uint32_t now) {
    static int last_link = -1, last_main = -1;
    ow_fwgui_stats ls;
    int link = (int)esp_link(now);
    ow_fwgui_get_stats(&ls);
    if (link != last_link) {
        DIAG("dualcpu: esp32 link %s\n", link_name((link_state)link));
        last_link = link;
    }
    if ((int)ls.stream_confirmed != last_main) {
        DIAG("dualcpu: MAIN stream link %s\n", ls.stream_confirmed ? "confirmed (CREDIT received)" : "unconfirmed");
        last_main = (int)ls.stream_confirmed;
    }
}

/* Keeps MAIN's view of the rails current: it reads what is powered from
 * this event, and a BSP app is the only thing that can send it. */
static void zones_report(uint32_t now) {
    static uint32_t sent_mask, sent_at;
    static int sent;
    uint32_t rails;
    if (!picpwr_rails(&rails)) return;
    rails_live = rails;
    rails_valid = 1;
    if (sent && rails == sent_mask && (uint32_t)(now - sent_at) < ZONES_RESEND_MS) return;
    ow_fwgui_send_power_zones(rails);
    sent = 1;
    sent_mask = rails;
    sent_at = now;
}

int main(void) {
    uint32_t now, next_ping, next_ui, next_stats, next_touch, next_expire;
    int touch_ok;
    /* RTT channel 0 keeps the BSP's compiled-in buffers (1 KB up, 16 B
     * down): SEGGER_RTT_Config*Buffer cannot move channel 0's buffers at run
     * time. Stats lines fit, and the bench's writer tops up the small down
     * buffer as the app reads commands out of it. */

    board_init();              /* must precede ow_open_fwgui: uart_init reads clk_peri */
    fw2_app_recovery_init();   /* waits for DISPLAY and RGB_LEDS (zone 10, the ESP32 LED's 5 V) */
    st7796_init();
    fw2_app_about_use_lcd();
    agentio_init();            /* before the first clear: it owns the capture shadow */
    st7796_fill_screen(C_BG);
    board_backlight_set(1);
    touch_ok = ft6336_init();
    screen_static();
    field(F_LINK, C_YEL, "starting...");
    field(F_ESPMODE, C_YEL, "powering the esp32...");

    /* The ESP32-C5 is zone 5. Under the stock DISPLAY firmware MAIN's zone
     * demand keeps it on; a BSP app discards that demand, so nothing else
     * will re-assert the rail after a sleep, USB attach or watchdog. */
    picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_WIFI_BT));
    picpwr_release_unused();

    field(F_ESPMODE, C_YEL, "opening the MAIN link...");
    while (fw2_app_recovery_open_onewili(&dev) != OW_OK) {
        fw2_app_recovery_task();
        DIAG("dualcpu: FwGUI link open failed (is the main CPU running stock fw?), retry in 1 s\n");
        fw2_app_recovery_sleep_ms(1000);
    }
    zones_report(now_ms());
    /* Once per launch: MAIN answers the ESP32's OneWili traffic only in this
     * mode. The call is also the first real command, which proves MAIN is
     * listening and opens the display bridge for text events. */
    field(F_ESPMODE, C_YEL, "esp32 mode: setting...");
    esp_mode_set(1);
    DIAG("dualcpu: up (touch %s), pinging the ESP32 every %u ms once it reports\n",
         touch_ok ? "ok" : "MISSING", (unsigned)PING_PERIOD_MS);

    now = now_ms();
    next_ping = now + PING_PERIOD_MS;
    next_ui = now;
    next_stats = now + STATS_PERIOD_MS;
    next_touch = now;
    next_expire = now + EXPIRE_PERIOD_MS;
    for (;;) {
        fw2_app_recovery_task();
        agentio_task();
        stream_drain();
        text_drain();
        now = now_ms();
        if ((int32_t)(now - next_touch) >= 0) {
            next_touch = now + TOUCH_PERIOD_MS;
            touch_poll(now);
        }
        keys_poll(now);
        rtt_poll();
        if ((int32_t)(now - next_ping) >= 0) {
            ow_fwgui_stats ls;
            next_ping = now + PING_PERIOD_MS;
            ow_fwgui_get_stats(&ls);
            /* Before MAIN confirms the link every write is refused, and before
             * the ESP32 has said anything a PING can only be lost: neither
             * would say anything about the round trip. */
            if (ls.stream_confirmed && esp_seen) ping_send();
            zones_report(now);
        }
        if ((int32_t)(now - next_expire) >= 0) {
            next_expire = now + EXPIRE_PERIOD_MS;
            ping_expire();
        }
        if (flash_btn >= 0 && (int32_t)(now - flash_until) >= 0) {
            button_draw(flash_btn, 0);
            flash_btn = -1;
        }
        if ((int32_t)(now - next_ui) >= 0) {
            next_ui = now + UI_PERIOD_MS;
            link_watch(now);
            ui_update(now);
        }
        if ((int32_t)(now - next_stats) >= 0) {
            next_stats = now + STATS_PERIOD_MS;
            stats_emit("stats:");
        }
    }
}
