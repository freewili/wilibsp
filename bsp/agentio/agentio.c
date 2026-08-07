#include "agentio/agentio.h"

#if FW2_AGENTIO

#include <stdlib.h>
#include <string.h>
#include "agentio/agentio_proto.h"
#include "agentio/agentio_rle.h"
#include "agentio/agentio_shadow.h"
#include "display/hstx_dvi.h"
#include "display/st7796.h"
#include "input/ft6336.h"
#include "input/uartkbd.h"
#include "platform/diag.h"
#include "platform/psram.h"
#include "pico/stdlib.h"
#include "SEGGER_RTT.h"

/* Shadow framebuffer: 480x320 wire-order RGB565 = 307,200 bytes, reserved at a
 * FIXED address in the top of PSRAM rather than placed by the linker.
 *
 * Why not __uninitialized_psram: that emits a `.psram_noload` input section,
 * and ld gives every allocated section a load address (LMA) by continuing from
 * the previous section's. `.psram_load` declares `AT> PSRAM_STORE`;
 * `.psram_noload` declares nothing, so it inherits whatever the LMA counter
 * happened to be. In a `no_flash` build the SDK aliases PSRAM_STORE to RAM
 * (memory_aliases_no_flash.incl: "Can't store in PSRAM"), so that phantom
 * 300 KB load range runs off the end of SRAM and picotool refuses the UF2:
 *
 *     Memory segment 2003fdbc->2008adbc is outside of valid address range
 *
 * The segment carries ZERO file bytes -- it is pure metadata -- but picotool
 * range-checks it anyway. A flash build puts the same phantom range in flash,
 * where nothing notices. That is why this only ever broke RAM apps, and why
 * agentio kept having to be switched off in them.
 *
 * The shadow needs no linker placement to begin with: agentio_init() zeroes it
 * and every use goes through a plain pointer. So it is reserved at the TOP of
 * PSRAM. The linker's PSRAM region grows UP from PSRAM_BASE, so the two meet
 * only if an app linker-allocates nearly the whole 8 MB -- and agentio_init()
 * checks __psram_end__ and disables capture rather than silently aliasing.
 *
 * This is the bounds-checked exception to AGENTS.md invariant 2's "never cast
 * PSRAM_BASE": the hazard there is a pointer at the BASE, which is precisely
 * where the linker starts allocating. */
#define AGENTIO_SHADOW_SIZE ((size_t)ST7796_W * ST7796_H * 2)
#define AGENTIO_SHADOW_ADDR \
    ((uintptr_t)PSRAM_BASE + (uintptr_t)PICO_PSRAM_SIZE_BYTES - AGENTIO_SHADOW_SIZE)

static uint8_t *const s_shadow = (uint8_t *)AGENTIO_SHADOW_ADDR;
static agentio_shadow_t s_shadow_state;
/* False when linker-placed PSRAM objects reach into the shadow window. */
static bool s_shadow_ok;

/* End of everything the linker placed in PSRAM (sections_psram.incl). */
extern char __psram_end__;

/* RTT channel 1. The up buffer blocks when full: a capture must not silently
 * drop pixels, and blocking the app loop during capture is the accepted trade
 * (see the spec's Coherence section). */
static uint8_t s_up_buf[4096];
static uint8_t s_down_buf[256];

static char   s_line[AGENTIO_CMD_MAX];
static size_t s_line_len;

static fw2kb_t *s_kb;

/* Button press queue: each entry is an inject mask applied on one
 * agentio_task() call. A press is {mask, 0} — two entries, two loop
 * iterations, so the app cannot miss the edge. */
#define AGENTIO_QUEUE_MAX 32          /* a chord is up to 10 presses = 20 entries */
static uint16_t s_queue[AGENTIO_QUEUE_MAX];
static int      s_q_head, s_q_count;

/* TYPE state: expanded lazily, one character per drain, so each chord is
 * resolved against the keyboard's real current page. */
static char s_type[AGENTIO_TYPE_MAX + 1];
static int  s_type_pos;

/* Tap state: released once the app has actually observed the point AND the
 * hold time has elapsed. */
#define AGENTIO_TAP_MS 60u
static bool     s_tap_active;
static uint32_t s_tap_deadline;
static uint32_t s_tap_reads0;

/* Set while the app is inside a region whose timing is the result; see
 * agentio_measure_begin() in agentio.h. Only captures are gated on it --
 * injection is cheap and is often what starts the measured work. */
static volatile bool s_measuring;

void agentio_measure_begin(void) { s_measuring = true; }
void agentio_measure_end(void)   { s_measuring = false; }

/* One output row at a time, sized to the widest surface we can be asked for.
 * Derive it rather than hard-coding 480: HSTX_VID_W_MAX is overridable by a
 * target compile definition (see bsp/display/hstx_dvi.h), so an app that raises
 * it would overflow a buffer pinned to ST7796_W. Both are compile-time, so this
 * costs nothing and stays correct however the DVI maxima are configured. */
#define AGENTIO_ROW_MAX \
    ((ST7796_W) > (HSTX_VID_W_MAX) ? (ST7796_W) : (HSTX_VID_W_MAX))
static uint16_t s_row[AGENTIO_ROW_MAX];
/* PackBits worst case: every unit literal, plus one control byte per 128. */
static uint8_t  s_enc[AGENTIO_ROW_MAX * 2 + AGENTIO_ROW_MAX / 128 + 8];

static bool queue_push(uint16_t mask)
{
    if (s_q_count >= AGENTIO_QUEUE_MAX) return false;
    s_queue[(s_q_head + s_q_count) % AGENTIO_QUEUE_MAX] = mask;
    s_q_count++;
    return true;
}

/* Queue a press: mask on one task call, released on the next. */
static bool queue_press(int btn)
{
    if (btn < 0 || btn >= UARTKBD_BTN_COUNT) return false;
    if (s_q_count + 2 > AGENTIO_QUEUE_MAX) return false;
    queue_push((uint16_t)(1u << btn));
    queue_push(0);
    return true;
}

/* Map a chord-engine button to its uartkbd button index. hello_keyboard feeds
 * UARTKBD_BTN_GREY..RED straight through as FW2KB_BTN_GRAY..RED, and maps
 * UARTKBD_BTN_PAGE to FW2KB_BTN_AI. */
static int fw2kb_btn_to_uartkbd(fw2kb_btn b)
{
    if (b >= FW2KB_BTN_GRAY && b <= FW2KB_BTN_RED) return (int)b;
    if (b == FW2KB_BTN_AI || b == FW2KB_BTN_CTRL_DOWN) return UARTKBD_BTN_PAGE;
    return -1;
}

static void reply(const char *s)
{
    SEGGER_RTT_Write(AGENTIO_RTT_CHANNEL, s, (unsigned)strlen(s));
}

/* Write a payload larger than the RTT up buffer WITHOUT blocking inside
 * SEGGER_RTT_Write. That call holds SEGGER_RTT_LOCK (interrupts off) for the
 * whole transfer, so when the payload exceeds the buffer it spins with IRQs
 * masked until the host drains — and any DMA whose rearm lives in an IRQ
 * handler (e.g. audio_capture's ping-pong) keeps chaining un-rearmed and
 * marches its write address through RAM (root-caused on FW2 2026-07-27:
 * capture DMA sprayed BSS from s_buf to the top of SRAM, corrupting this
 * module's own response buffer mid-transfer). Chunking to the space that is
 * free RIGHT NOW means each SEGGER_RTT_Write completes without ever spinning
 * under the lock, and interrupts breathe between chunks. If the host stops
 * draining for 3 s the remainder is dropped so the UI loop never hangs. */
static void rtt_write_yielding(const void *data, unsigned len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t stall_start = time_us_32();
    while (len) {
        unsigned avail = SEGGER_RTT_GetAvailWriteSpace(AGENTIO_RTT_CHANNEL);
        if (!avail) {
            if (time_us_32() - stall_start > 3000000u) return;   /* host gone */
            continue;
        }
        unsigned k = avail < len ? avail : len;
        SEGGER_RTT_Write(AGENTIO_RTT_CHANNEL, p, k);   /* fits: never blocks */
        p += k; len -= k;
        stall_start = time_us_32();
    }
}

/* Split `line` on single spaces into up to `max` tokens. Returns the count. */
static int split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

/* Surface geometry. Returns false when the surface is unusable (e.g. DVI has
 * not been initialized, in which case its accessors are still zero/NULL). */
static bool surface_dims(int surface, int *w, int *h)
{
    if (surface == AGENTIO_SURFACE_LCD) {
        *w = ST7796_W;
        *h = ST7796_H;
        return true;
    }
    if (surface == AGENTIO_SURFACE_DVI) {
        /* hstx_dvi_video_base()/_w()/_h() describe the MOVIE sub-rect, which
         * the app can shrink for aspect-ratio letterboxing via
         * hstx_dvi_set_geometry(). Per hstx_dvi.h, the OSD deliberately draws
         * outside that sub-rect, in the margin of the FIXED region -- so a
         * crop of the movie rect can never reach it. Use the region
         * accessors (region_base/region_h) and HSTX_VID_W_MAX (the header
         * exposes no separate "region width" accessor; the stored region's
         * row width is fixed at build time and this is its upper bound) so
         * the whole capturable area, movie plus OSD margin, is in play. */
        *w = HSTX_VID_W_MAX;
        *h = hstx_dvi_region_h();
        return hstx_dvi_region_base() != 0 && *h > 0;
    }
    return false;
}

/* Read one output row into s_row as RGB565 colour VALUES, applying the crop
 * origin and the nearest-neighbour downscale. The two surfaces differ in
 * storage: the LCD shadow holds wire-order bytes (MSB first), the DVI
 * framebuffer holds native-endian values on a strided row pitch. */
static void read_row(int surface, int x, int y, int out_w, int scale)
{
    if (surface == AGENTIO_SURFACE_LCD) {
        const uint8_t *fb = agentio_shadow_fb();
        const uint8_t *row = &fb[((size_t)y * ST7796_W) * 2];
        for (int i = 0; i < out_w; i++) {
            const uint8_t *p = &row[(size_t)(x + i * scale) * 2];
            s_row[i] = (uint16_t)((p[0] << 8) | p[1]);
        }
    } else {
        /* Region base/stride, not video_base/_stride -- see the comment in
         * surface_dims(). The stride is still the per-row element count
         * hstx_dvi_video_stride() reports; only the base pointer and the
         * addressable width/height change to the fixed region. */
        const uint16_t *row = hstx_dvi_region_base()
                            + (size_t)y * (size_t)hstx_dvi_video_stride();
        for (int i = 0; i < out_w; i++) s_row[i] = row[x + i * scale];
    }
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* CAP <surface> <x> <y> <w> <h> <scale> */
static void do_capture(int surface, int x, int y, int w, int h, int scale)
{
    int sw, sh;
    if (!surface_dims(surface, &sw, &sh)) {
        reply("ERR surface\n");
        return;
    }
    if (surface == AGENTIO_SURFACE_LCD && !s_shadow_ok) {
        reply("ERR no-shadow\n");
        return;
    }
    if (scale < 1) scale = 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= sw || y >= sh) {          /* origin outside the surface */
        reply("ERR rect\n");
        return;
    }
    /* Clamp with subtraction, never `x + w`: both are host-controlled ints and
     * their sum can overflow, wrapping negative and skipping the clamp
     * entirely. After the origin check above, 0 <= x < sw, so `sw - x` is a
     * safe positive int and the comparison cannot overflow. */
    if (w <= 0 || w > sw - x) w = sw - x;
    if (h <= 0 || h > sh - y) h = sh - y;
    if (w <= 0 || h <= 0) {
        reply("ERR rect\n");
        return;
    }

    int out_w = w / scale;
    int out_h = h / scale;
    if (out_w <= 0 || out_h <= 0) {
        reply("ERR scale\n");
        return;
    }

    /* Pass 1: total payload size (the header carries it, so it must be known
     * before the first pixel goes out). */
    uint32_t total = 0;
    for (int r = 0; r < out_h; r++) {
        read_row(surface, x, y + r * scale, out_w, scale);
        total += (uint32_t)agentio_rle_encode(s_row, (size_t)out_w,
                                              s_enc, sizeof s_enc);
    }

    uint8_t hdr[AGENTIO_HEADER_LEN];
    memcpy(hdr, AGENTIO_MAGIC, 4);
    hdr[4] = (uint8_t)surface;
    hdr[5] = AGENTIO_FORMAT_PACKBITS16;
    put_u16(&hdr[6],  (uint16_t)x);
    put_u16(&hdr[8],  (uint16_t)y);
    put_u16(&hdr[10], (uint16_t)out_w);
    put_u16(&hdr[12], (uint16_t)out_h);
    put_u32(&hdr[14], total);
    rtt_write_yielding(hdr, sizeof hdr);

    /* Pass 2: stream, yielding between chunks so interrupts stay live while
     * the host drains (see rtt_write_yielding). */
    for (int r = 0; r < out_h; r++) {
        read_row(surface, x, y + r * scale, out_w, scale);
        size_t n = agentio_rle_encode(s_row, (size_t)out_w, s_enc, sizeof s_enc);
        rtt_write_yielding(s_enc, (unsigned)n);
    }
}

static void dispatch(char *line)
{
    /* TYPE's argument is the rest of the line verbatim — it may contain
     * spaces, so it must be taken before split() rewrites separators to NULs
     * (tokenizing then rejoining silently truncated at split()'s token cap). */
    if (strncmp(line, AGENTIO_CMD_TYPE " ", sizeof AGENTIO_CMD_TYPE) == 0) {
        char *text = line + sizeof AGENTIO_CMD_TYPE;   /* past "TYPE " */
        if (!s_kb) {
            reply("ERR no-keyboard-bound\n");
            return;
        }
        if (s_type[s_type_pos] != 0 || s_q_count > 0) {
            reply("ERR busy\n");
            return;
        }
        size_t len = strlen(text);
        if (len > AGENTIO_TYPE_MAX) {
            reply("ERR too-long\n");
            return;
        }
        memcpy(s_type, text, len + 1);
        s_type_pos = 0;
        reply("OK\n");
        return;
    }

    char *argv[8];
    int argc = split(line, argv, 8);
    if (argc == 0) return;

    if (strcmp(argv[0], AGENTIO_CMD_PING) == 0) {
        reply("OK\n");
        return;
    }

    if (strcmp(argv[0], AGENTIO_CMD_BTN) == 0 && argc == 2) {
        uartkbd_inject_set((uint16_t)strtoul(argv[1], 0, 16));
        reply("OK\n");
        return;
    }

    if (strcmp(argv[0], AGENTIO_CMD_TAP) == 0 && argc == 2) {
        reply(queue_press((int)strtol(argv[1], 0, 10)) ? "OK\n" : "ERR queue\n");
        return;
    }

    if (strcmp(argv[0], AGENTIO_CMD_TCH) == 0 && argc == 4) {
        uint16_t x = (uint16_t)strtoul(argv[1], 0, 10);
        uint16_t y = (uint16_t)strtoul(argv[2], 0, 10);
        int mode = (int)strtol(argv[3], 0, 10);
        if (mode == AGENTIO_TOUCH_UP) {
            ft6336_inject_set(0, 0, false);
            s_tap_active = false;
        } else if (mode == AGENTIO_TOUCH_DOWN) {
            ft6336_inject_set(x, y, true);
            s_tap_active = false;
        } else if (mode == AGENTIO_TOUCH_TAP) {
            ft6336_inject_set(x, y, true);
            s_tap_reads0 = 0;
            s_tap_deadline = to_ms_since_boot(get_absolute_time()) + AGENTIO_TAP_MS;
            s_tap_active = true;
        } else {
            reply("ERR mode\n");
            return;
        }
        reply("OK\n");
        return;
    }

    if (strcmp(argv[0], AGENTIO_CMD_CAP) == 0 && argc == 7) {
        /* do_capture() runs synchronously right here, but the TAP/TYPE queue
         * only drains one edge per agentio_task() call, which runs *after*
         * dispatch() returns. A capture issued while a press or a TYPE
         * string is still pending would see the screen before, or mid-way
         * through, that injection landing. Refuse it with a distinct error
         * so the host knows to retry rather than silently racing it. */
        if (s_q_count > 0 || s_type[s_type_pos] != 0) {
            reply("ERR busy\n");
            return;
        }
        /* A capture streams the whole shadow from this main loop. Inside a
         * timed region that cost lands between the app's measurements instead
         * of inside them, so its own timing looks fine while wall-clock
         * throughput collapses -- see agentio_measure_begin() in agentio.h.
         * Refuse rather than silently corrupt the result. */
        if (s_measuring) {
            reply("ERR measuring\n");
            return;
        }
        do_capture((int)strtol(argv[1], 0, 10), (int)strtol(argv[2], 0, 10),
                   (int)strtol(argv[3], 0, 10), (int)strtol(argv[4], 0, 10),
                   (int)strtol(argv[5], 0, 10), (int)strtol(argv[6], 0, 10));
        return;
    }

    reply("ERR unknown\n");
}

void agentio_init(void)
{
    /* Refuse to alias: the shadow window is only safe while the linker's PSRAM
     * allocations stop below it. Requires an app to place ~7.7 MB there, but
     * silently corrupting its buffers would be a miserable thing to debug. */
    s_shadow_ok = (uintptr_t)&__psram_end__ <= AGENTIO_SHADOW_ADDR;
    if (s_shadow_ok) {
        memset(s_shadow, 0, AGENTIO_SHADOW_SIZE);
        agentio_shadow_init(&s_shadow_state, s_shadow, ST7796_W, ST7796_H);
    }
    s_line_len = 0;
    s_kb = 0;

    SEGGER_RTT_ConfigUpBuffer(AGENTIO_RTT_CHANNEL, "agentio",
                              s_up_buf, sizeof s_up_buf,
                              SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);
    SEGGER_RTT_ConfigDownBuffer(AGENTIO_RTT_CHANNEL, "agentio",
                                s_down_buf, sizeof s_down_buf,
                                SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    if (s_shadow_ok)
        DIAG("agentio: up on RTT ch%u, shadow %u bytes at %x\n",
             (unsigned)AGENTIO_RTT_CHANNEL, (unsigned)AGENTIO_SHADOW_SIZE,
             (unsigned)AGENTIO_SHADOW_ADDR);
    else
        DIAG("agentio: up on RTT ch%u, CAPTURE DISABLED - linker PSRAM ends at "
             "%x, inside the shadow window at %x\n",
             (unsigned)AGENTIO_RTT_CHANNEL, (unsigned)(uintptr_t)&__psram_end__,
             (unsigned)AGENTIO_SHADOW_ADDR);
}

void agentio_bind_keyboard(fw2kb_t *kb)
{
    s_kb = kb;
}

void agentio_task(void)
{
    uint8_t buf[64];
    unsigned n;

    while ((n = SEGGER_RTT_Read(AGENTIO_RTT_CHANNEL, buf, sizeof buf)) > 0) {
        for (unsigned i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                s_line[s_line_len] = 0;
                if (s_line_len > 0) dispatch(s_line);
                s_line_len = 0;
                continue;
            }
            if (s_line_len < sizeof s_line - 1) s_line[s_line_len++] = c;
        }
    }

    /* One injected edge per call — the app's loop runs between every edge. */
    if (s_q_count > 0) {
        uartkbd_inject_set(s_queue[s_q_head]);
        s_q_head = (s_q_head + 1) % AGENTIO_QUEUE_MAX;
        s_q_count--;
    } else if (s_type[s_type_pos] != 0) {
        /* Expand the next character against the keyboard's LIVE state. */
        fw2kb_btn seq[FW2KB_CHORD_MAX];
        int n = 0;
        if (s_kb && fw2kb_chord_for(s_kb, s_type[s_type_pos], seq, &n)) {
            for (int i = 0; i < n; i++) {
                int u = fw2kb_btn_to_uartkbd(seq[i]);
                if (u >= 0 && !queue_press(u)) break;
            }
        }
        s_type_pos++;
        if (s_type[s_type_pos] == 0) {     /* string finished: reset for the next */
            s_type_pos = 0;
            s_type[0] = 0;
        }
    }

    /* Release a tap once it has been seen at least once and held long enough. */
    if (s_tap_active) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (ft6336_inject_reads() > s_tap_reads0 &&
            (int32_t)(now - s_tap_deadline) >= 0) {
            ft6336_inject_set(0, 0, false);
            s_tap_active = false;
        }
    }
}

void agentio_shadow_note_window(int x0, int y0, int x1, int y1)
{
    if (!s_shadow_ok) return;
    agentio_shadow_set_window(&s_shadow_state, x0, y0, x1, y1);
}

void agentio_shadow_note_pixels(const uint8_t *bytes, size_t n)
{
    if (!s_shadow_ok) return;
    agentio_shadow_write(&s_shadow_state, bytes, n);
}

const uint8_t *agentio_shadow_fb(void)
{
    /* NULL when disabled, per the contract in agentio.h. */
    return s_shadow_ok ? s_shadow : 0;
}

#endif /* FW2_AGENTIO */
