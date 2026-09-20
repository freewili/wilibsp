/* canblast — CAN FD blaster for the FreeWili 2 display CPU.
 *
 * Drives the main CPU's CAN FD controller through the OneWili C API over the
 * FwGUI display link (UART0 @ 8 Mbaud, HW flow control on GPIO 0-3) as fast
 * as the link and the firmware allow, and shows live counters on the LCD:
 *
 *   RX  binary canRxReport events (stream mode 2) or text can0 events
 *       (mode 1), each self-checking payload validated (sequence, run id,
 *       pattern, XOR) so loss / duplication / corruption is visible on-screen.
 *   TX  pipelined one-shot write_canfd commands (onewili_fast.h): several
 *       commands in flight instead of one synchronous round trip per frame.
 *
 * Buttons: GREEN start/stop a transmit flood, BLUE cycle the flood payload
 * length, RED reset the counters, YELLOW cycle the RX stream mode
 * (binary / text / both). HOLD HOME 5 s leaves the app.
 *
 * Bench control rides SEGGER RTT channel 0 (the DIAG channel): the PC sends
 * one-line commands on the down buffer (ID, RESET, STREAM n, RX run count arb,
 * TX run count len arb fd xtd pipe, TXABORT, STAT, PERIODIC ..., CFG ...) and
 * every reply line starts with '='. canfdvalidation/canfdval/wili_display.py
 * is the client.
 *
 * The main CPU must run the 2026-09-19 (or later) FreeWili 2 firmware, which
 * mirrors binary canRxReport frames onto the display link. */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "onewili.h"
#include "onewili_fwgui.h"
#include "onewili_binary.h"
#include "onewili_fast.h"
#include "input/app_recovery_onewili.h"
#include "input/picpwr.h"
#include "input/uartkbd.h"
#include "agentio/agentio.h"
#include "SEGGER_RTT.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_VERSION_STR "001"
#define CAN_CHANNEL     0
#define ARB_RX_DEFAULT  0x123u      /* ValueCAN -> FreeWili (matches canfdval Sides.arb_v2w) */
#define ARB_TX_DEFAULT  0x321u      /* FreeWili -> ValueCAN (matches canfdval Sides.arb_w2v) */
#define FLOOD_RUN_ID    0x5AF0u

static inline uint16_t be16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
#define C_BLACK be16(0x0000)
#define C_WHITE be16(0xFFFF)
#define C_GREEN be16(0x07E0)
#define C_RED   be16(0xF800)
#define C_YEL   be16(0xFFE0)
#define C_CYAN  be16(0x07FF)
#define C_GREY  be16(0x8410)

/* ~37 KB of link buffers plus the parser: far too big for the stack. */
static ow_device        dev;
static ow_binary_device bdev;
static ow_fast_tx       ftx;
static uint8_t          rtt_up[4096];
static uint8_t          rtt_down[256];

static const uint8_t DLC_SIZES[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

/* ── self-checking payload (canfdval/frames.py) ────────────────────────── */
static void build_payload(uint8_t* out, unsigned len, uint32_t seq, uint32_t run) {
    unsigned i;
    memset(out, 0, len);
    for (i = 0; i < len && i < 4; i++) out[i] = (uint8_t)(seq >> (8 * i));
    if (len >= 6) { out[4] = (uint8_t)run; out[5] = (uint8_t)(run >> 8); }
    for (i = 6; i < len; i++) out[i] = (uint8_t)((seq * 7 + i) & 0xFF);
    if (len >= 8) {
        uint8_t x = 0;
        for (i = 0; i + 1 < len; i++) x ^= out[i];
        out[len - 1] = x;
    }
}

/* ── receive sequence check (canfdval/frames.py SequenceCheck) ─────────── */
typedef struct {
    uint32_t arb_id, run_id, expected;
    uint32_t received, unique, duplicates, out_of_order, integrity_errors, wrong_run, other_arb;
    uint32_t wrapped;                 /* DLC too short to identify frames: count only */
    uint32_t last_seq; int have_last;
    int have_first;
    uint32_t first_us, last_us;       /* display clock at first/last frame */
    uint64_t first_ns, last_ns;       /* MAIN's timestamps (binary reports only) */
} seqcheck_t;
static seqcheck_t chk;
static uint8_t    seen[65536 / 8];    /* bitmap of sequence numbers received */

static void chk_reset(uint32_t arb, uint32_t run, uint32_t expected) {
    memset(&chk, 0, sizeof chk);
    memset(seen, 0, sizeof seen);
    chk.arb_id = arb; chk.run_id = run; chk.expected = expected;
}

static uint32_t chk_lost(void) {
    uint32_t got = chk.wrapped ? chk.received : chk.unique;
    return chk.expected > got ? chk.expected - got : 0;
}

static void chk_add(uint32_t arb, const uint8_t* d, unsigned n, uint64_t dev_ns) {
    uint32_t now = time_us_32();
    unsigned i, sb;
    uint32_t seq = 0;
    int ok = 1;
    if (arb != chk.arb_id) { chk.other_arb++; return; }
    chk.received++;
    if (!chk.have_first) { chk.have_first = 1; chk.first_us = now; chk.first_ns = dev_ns; }
    chk.last_us = now; chk.last_ns = dev_ns;
    if (n == 0) { chk.wrapped = 1; return; }   /* no payload: count-only, like any DLC too short to number frames */
    sb = n < 4 ? n : 4;
    for (i = 0; i < sb; i++) seq |= (uint32_t)d[i] << (8 * i);
    if (n >= 8) {
        uint8_t x = 0;
        for (i = 0; i + 1 < n; i++) x ^= d[i];
        if (x != d[n - 1]) ok = 0;
    }
    if (ok && n > 6) {
        unsigned limit = n >= 8 ? n - 1 : n;
        for (i = 6; i < limit; i++)
            if (d[i] != (uint8_t)((seq * 7 + i) & 0xFF)) { ok = 0; break; }
    }
    if (!ok) chk.integrity_errors++;
    if (n >= 6) {
        uint32_t run = (uint32_t)d[4] | ((uint32_t)d[5] << 8);
        if (run != (chk.run_id & 0xFFFF)) { chk.wrong_run++; return; }
    }
    if ((8 * sb < 32 && chk.expected > (1u << (8 * sb))) || seq >= 65536) { chk.wrapped = 1; return; }
    if (seen[seq >> 3] & (uint8_t)(1u << (seq & 7))) chk.duplicates++;
    else { seen[seq >> 3] |= (uint8_t)(1u << (seq & 7)); chk.unique++; }
    if (chk.have_last && seq < chk.last_seq) chk.out_of_order++;
    chk.last_seq = seq; chk.have_last = 1;
}

/* ── counters ──────────────────────────────────────────────────────────── */
static struct {
    uint32_t rx_bin, rx_txt, tx_echo, other_ev, parse_err, bin_err;
    uint32_t rx_rate, tx_rate;            /* frames in the last second */
    uint32_t rx_prev, tx_prev;
} st;
static int      stream_mode = 2;          /* 1 text, 2 binary, 3 both */
static int      link_ok;
static uint32_t rails_live;
static int      rails_valid;

/* ── transmit job (pipelined write_canfd) ──────────────────────────────── */
typedef struct {
    int active, flood;
    uint32_t run_id, count, len, arb, next_seq;
    int fd, xtd;
    uint32_t t0_us, t1_us;
    uint32_t retry[OW_FAST_MAX_IN_FLIGHT]; unsigned nretry;
    uint32_t done_ok, rejected, errors, stalls;
    uint32_t last_progress_us;
    uint32_t backoff_until_us, backoff_us;   /* after a reject: let MAIN drain before sending more */
} txjob_t;
static txjob_t  job;
static uint32_t flood_len = 8;
static uint32_t pipe_depth = 1;   /* synchronous measured fastest: MAIN refuses rather than queues when its FIFO is full */

static void on_tx_result(void* u, uint32_t token, int ok) {
    (void)u;
    job.last_progress_us = time_us_32();
    if (ok) { job.done_ok++; job.backoff_us = 0; return; }
    /* MAIN refused the frame (transmit FIFO full): send that sequence again
     * so delivery stays complete; the reject count shows the pressure. */
    job.rejected++;
    if (job.nretry < OW_FAST_MAX_IN_FLIGHT) job.retry[job.nretry++] = token;
    /* MAIN's one-shot FIFO drains once per main-loop pass; resending at once
     * only lengthens that pass. Back off, growing to ~2 ms while rejects
     * continue, and reset on the next accepted frame (below). */
    if (job.backoff_us < 250) job.backoff_us = 250;
    else if (job.backoff_us < 2000) job.backoff_us *= 2;
    job.backoff_until_us = time_us_32() + job.backoff_us;
}

static void tx_start(uint32_t run, uint32_t count, uint32_t len, uint32_t arb, int fd, int xtd,
                     uint32_t pipe, int flood) {
    memset(&job, 0, sizeof job);
    job.active = 1; job.flood = flood; job.run_id = run; job.count = count;
    job.len = len > 64 ? 64 : len; job.arb = arb; job.fd = fd; job.xtd = xtd;
    if (pipe < 1) pipe = 1;
    if (pipe > OW_FAST_MAX_IN_FLIGHT) pipe = OW_FAST_MAX_IN_FLIGHT;
    ftx.max_in_flight = pipe;
    ow_fast_tx_reset(&ftx);
    job.t0_us = time_us_32();
    job.last_progress_us = job.t0_us;
}

static void tx_stop(void) {
    job.active = 0;
    job.flood = 0;
    job.t1_us = time_us_32();
    ow_fast_tx_reset(&ftx);
}

static void tx_step(void) {
    uint8_t buf[64];
    int r;
    if (!job.active) {
        if (ftx.in_flight) ow_fast_tx_reap(&ftx, 0);
        return;
    }
    r = ow_fast_tx_reap(&ftx, 0);
    if (r < 0) job.errors++;
    if (r > 0) job.last_progress_us = time_us_32();
    if (ftx.in_flight && (int32_t)(time_us_32() - job.last_progress_us) > 2000000) {
        /* MAIN answered nothing for 2 s: those commands are gone. Resend them. */
        uint32_t i = ftx.q_r;
        while (i != ftx.q_w && job.nretry < OW_FAST_MAX_IN_FLIGHT) { job.retry[job.nretry++] = ftx.q_token[i]; i = (i + 1) % OW_FAST_MAX_IN_FLIGHT; }
        job.stalls++;
        ow_fast_tx_reset(&ftx);
        job.last_progress_us = time_us_32();
        DIAG("canblast: tx pipeline stalled, %u commands resent\n", (unsigned)job.nretry);
    }
    while (!job.backoff_us || (int32_t)(time_us_32() - job.backoff_until_us) >= 0) {
        uint32_t seq;
        int from_retry = 0;
        if (job.nretry) { seq = job.retry[job.nretry - 1]; from_retry = 1; }
        else if (job.flood || job.next_seq < job.count) seq = job.next_seq;
        else break;
        build_payload(buf, job.len, seq, job.run_id);
        r = ow_fast_canfd_write(&ftx, CAN_CHANNEL, job.arb, job.fd, job.xtd, buf, job.len, seq);
        if (r <= 0) { if (r < 0) job.errors++; break; }
        if (from_retry) job.nretry--; else job.next_seq++;
    }
    if (!job.flood && job.next_seq >= job.count && job.nretry == 0 && ftx.in_flight == 0) {
        job.active = 0;
        job.t1_us = time_us_32();
    }
}

/* ── receive path ──────────────────────────────────────────────────────── */
static void rx_binary(const ow_evt_can_rx_report* r) {
    uint32_t r0 = r->r0_canid, r1 = r->r1_filter_header_bits;
    int ext = (int)((r1 >> 4) & 1u);
    uint32_t arb = ext ? (((r0 & 0x7FFu) << 18) | ((r0 >> 11) & 0x3FFFFu)) : (r0 & 0x7FFu);
    unsigned len = DLC_SIZES[r1 & 0xFu];
    const uint8_t* data = (const uint8_t*)r->data_words;   /* little-endian words = bytes in order */
    st.rx_bin++;
    if (r->error) st.bin_err++;
    chk_add(arb, data, len, r->time_stamp_ns);
}

/* args = "<hexTs> <seq> <arb>[x] <byte>... <ok>" (the firmware's can0 event) */
static void rx_text(const char* args) {
    char* tok[80];
    int ntok = 0;
    static char buf[520];
    char* p;
    strncpy(buf, args, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    p = buf;
    while (*p && ntok < 80) {
        while (*p == ' ') p++;
        if (!*p) break;
        tok[ntok++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (ntok < 4) { st.parse_err++; return; }
    {
        uint8_t data[64];
        unsigned n = 0;
        uint32_t arb;
        char* end = 0;
        size_t idlen = strlen(tok[2]);
        if (idlen && tok[2][idlen - 1] == 'x') tok[2][idlen - 1] = 0;
        arb = (uint32_t)strtoul(tok[2], &end, 16);
        if (!end || *end) { st.parse_err++; return; }
        for (int i = 3; i < ntok - 1 && n < 64; i++) {
            unsigned long v = strtoul(tok[i], &end, 16);
            if (!end || *end || v > 0xFF) { st.parse_err++; return; }
            data[n++] = (uint8_t)v;
        }
        st.rx_txt++;
        chk_add(arb, data, n, 0);
    }
}

static void rx_poll(void) {
    ow_event ev;
    int n = 0;
    while (n < 256 && ow_binary_poll(&bdev, &ev) == 1) {
        n++;
        if (ev.kind == OW_EV_CAN_RX_REPORT) rx_binary(&ev.u.can_rx_report);
        else st.other_ev++;
    }
    {
        static char id[24];
        static char args[512];
        n = 0;
        while (n < 64 && ow_poll_text_line(&dev, id, sizeof id, args, sizeof args) == 1) {
            n++;
            if (strcmp(id, "can0") == 0) rx_text(args);
            else if (strcmp(id, "canTx0") == 0) st.tx_echo++;
            else st.other_ev++;
        }
    }
}

/* ── OneWili control ───────────────────────────────────────────────────── */
static ow_status stream_set(int mode) {
    ow_status s;
    if (ftx.in_flight) ow_fast_tx_drain(&ftx, 500);   /* a sync call must not race pipelined responses */
    s = ow_io_canfd_enable_canfd_stream(&dev, CAN_CHANNEL, mode);
    if (s == OW_OK) stream_mode = mode;
    return s;
}

static void can_configure(int mode, int baud, int fd_baud, int retry) {
    ow_status a = ow_hardware_settings_home_neptune_settings_c_an1_mode(&dev, mode);
    ow_status b = ow_hardware_settings_home_neptune_settings_c_an1_rate(&dev, baud);
    ow_status c = ow_hardware_settings_home_neptune_settings_c_an1fdd_rate(&dev, fd_baud);
    ow_status d = ow_hardware_settings_home_neptune_settings_c_an1_tx_retry(&dev, retry);
    DIAG("canblast: CAN1 mode %d rate %d fd %d retry %d -> %d %d %d %d\n",
         mode, baud, fd_baud, retry, (int)a, (int)b, (int)c, (int)d);
}

static void counters_reset(void) {
    memset(&st, 0, sizeof st);
    ow_fast_tx_reset(&ftx);
    chk_reset(chk.arb_id ? chk.arb_id : ARB_RX_DEFAULT, chk.run_id, 0);
    ftx.sent = ftx.ok = ftx.failed = ftx.proto_errors = ftx.io_errors = 0;
    bdev.unknown_frames = bdev.size_mismatches = 0;
    dev.dropped_text_events = 0;
}

/* ── RTT command channel ───────────────────────────────────────────────── */
static void reply_stat(void) {
    ow_fwgui_stats ls;
    uint32_t rx_span = chk.have_first ? chk.last_us - chk.first_us : 0;
    uint32_t dev_span = chk.have_first && chk.last_ns >= chk.first_ns ? (uint32_t)((chk.last_ns - chk.first_ns) / 1000u) : 0;
    uint32_t tx_span = job.active ? time_us_32() - job.t0_us : job.t1_us - job.t0_us;
    ow_fwgui_get_stats(&ls);
    SEGGER_RTT_printf(0, "=STAT rx=%u uniq=%u lost=%u dup=%u ooo=%u bad=%u wrong=%u other=%u wrapped=%u exp=%u",
                      chk.received, chk.unique, chk_lost(), chk.duplicates, chk.out_of_order,
                      chk.integrity_errors, chk.wrong_run, chk.other_arb, chk.wrapped, chk.expected);
    SEGGER_RTT_printf(0, " bin=%u txt=%u echo=%u pars=%u binerr=%u otherev=%u rxspan_us=%u devspan_us=%u rxrate=%u",
                      st.rx_bin, st.rx_txt, st.tx_echo, st.parse_err, st.bin_err, st.other_ev,
                      rx_span, dev_span, st.rx_rate);
    SEGGER_RTT_printf(0, " txact=%u txsent=%u txok=%u txfail=%u txdone=%u txnext=%u txcount=%u txspan_us=%u inflight=%u pipe=%u txerr=%u txrate=%u stalls=%u",
                      (unsigned)job.active, ftx.sent, ftx.ok, ftx.failed, job.done_ok, job.next_seq,
                      job.count, tx_span, ftx.in_flight, ftx.max_in_flight, job.errors + ftx.proto_errors + ftx.io_errors,
                      st.tx_rate, job.stalls);
    SEGGER_RTT_printf(0, " drop=%u ovr=%u hwovr=%u ringmax=%u binmax=%u txtmax=%u cksum=%u ftxt=%u fbin=%u evdrop=%u unk=%u mism=%u txbytes=%u stream=%d rails=%x stashlost=%u\n",
                      ls.dropped_frames, ls.ring_overrun_bytes, ls.hw_overruns, ls.ring_max_fill,
                      ls.binary_max_fill, ls.text_max_fill, ls.checksum_errors, ls.frames_text,
                      ls.frames_binary, dev.dropped_text_events, bdev.unknown_frames,
                      bdev.size_mismatches, ls.tx_bytes, stream_mode, (unsigned)rails_live, ow_raw_stash_lost_count());
}

static uint32_t arg_u(char** argv, int argc, int i, int base, uint32_t dflt) {
    if (i >= argc) return dflt;
    return (uint32_t)strtoul(argv[i], 0, base);
}

static void dispatch(char* line) {
    char* argv[12];
    int argc = 0;
    char* p = line;
    while (*p && argc < 12) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (argc == 0) return;
    if (strcmp(argv[0], "ID") == 0) {
        SEGGER_RTT_printf(0, "=ID canblast %s link=%d stream=%d\n", APP_VERSION_STR, link_ok, stream_mode);
    } else if (strcmp(argv[0], "RESET") == 0) {
        if (job.active) tx_stop();
        counters_reset();
        SEGGER_RTT_printf(0, "=OK\n");
    } else if (strcmp(argv[0], "STREAM") == 0) {
        ow_status s = stream_set((int)arg_u(argv, argc, 1, 10, 2));
        SEGGER_RTT_printf(0, s == OW_OK ? "=OK stream=%d\n" : "=ERR %d\n", s == OW_OK ? stream_mode : (int)s);
    } else if (strcmp(argv[0], "RX") == 0) {
        uint32_t run = arg_u(argv, argc, 1, 10, 0);
        uint32_t count = arg_u(argv, argc, 2, 10, 0);
        uint32_t arb = arg_u(argv, argc, 3, 16, ARB_RX_DEFAULT);
        chk_reset(arb, run, count);
        SEGGER_RTT_printf(0, "=OK rx run=%u count=%u arb=%x\n", run, count, arb);
    } else if (strcmp(argv[0], "TX") == 0) {
        uint32_t run = arg_u(argv, argc, 1, 10, 0);
        uint32_t count = arg_u(argv, argc, 2, 10, 0);
        uint32_t len = arg_u(argv, argc, 3, 10, 8);
        uint32_t arb = arg_u(argv, argc, 4, 16, ARB_TX_DEFAULT);
        int fd = (int)arg_u(argv, argc, 5, 10, len > 8 ? 1 : 0);
        int xtd = (int)arg_u(argv, argc, 6, 10, 0);
        uint32_t pipe = arg_u(argv, argc, 7, 10, pipe_depth);
        if (job.active) tx_stop();
        tx_start(run, count, len, arb, fd, xtd, pipe, 0);
        SEGGER_RTT_printf(0, "=OK tx run=%u count=%u len=%u arb=%x fd=%d pipe=%u\n", run, count, len, arb, fd, pipe);
    } else if (strcmp(argv[0], "TXABORT") == 0) {
        tx_stop();
        SEGGER_RTT_printf(0, "=OK\n");
    } else if (strcmp(argv[0], "STAT") == 0) {
        reply_stat();
    } else if (strcmp(argv[0], "PERIODIC") == 0) {
        /* PERIODIC slot on period_us len arb_hex */
        uint32_t slot = arg_u(argv, argc, 1, 10, 0);
        uint32_t on = arg_u(argv, argc, 2, 10, 0);
        uint32_t period = arg_u(argv, argc, 3, 10, 0);
        uint32_t len = arg_u(argv, argc, 4, 10, 8);
        uint32_t arb = arg_u(argv, argc, 5, 16, ARB_TX_DEFAULT);
        uint8_t buf[64];
        ow_status s;
        if (len > 64) len = 64;
        build_payload(buf, len, slot, FLOOD_RUN_ID);
        s = ow_io_canfd_write_canfd_periodic(&dev, (int32_t)slot, on ? 1 : 0, (int32_t)period, CAN_CHANNEL,
                                             arb, len > 8 ? 1 : 0, 0, buf, on ? len : 0);
        SEGGER_RTT_printf(0, s == OW_OK ? "=OK periodic slot=%u on=%u\n" : "=ERR %d\n",
                          s == OW_OK ? slot : (uint32_t)s, on);
    } else if (strcmp(argv[0], "CFG") == 0) {
        can_configure((int)arg_u(argv, argc, 1, 10, 0), (int)arg_u(argv, argc, 2, 10, 500000),
                      (int)arg_u(argv, argc, 3, 10, 2000000), (int)arg_u(argv, argc, 4, 10, 0));
        SEGGER_RTT_printf(0, "=OK\n");
    } else if (strcmp(argv[0], "PIPE") == 0) {
        pipe_depth = arg_u(argv, argc, 1, 10, 8);
        if (pipe_depth < 1) pipe_depth = 1;
        if (pipe_depth > OW_FAST_MAX_IN_FLIGHT) pipe_depth = OW_FAST_MAX_IN_FLIGHT;
        ftx.max_in_flight = pipe_depth;
        SEGGER_RTT_printf(0, "=OK pipe=%u\n", pipe_depth);
    } else if (strcmp(argv[0], "RAW") == 0) {
        /* RAW <command words...>: send the command as-is and dump every byte
         * the text stream returns within 700 ms (escaped), then the event
         * queue state. Debug aid for the bridge. */
        static char cmd[256];
        static uint8_t buf[512];
        size_t pos = 0;
        cmd[0] = 0;
        for (int i = 1; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (pos + l + 2 >= sizeof cmd) break;
            if (i > 1) cmd[pos++] = ' ';
            memcpy(cmd + pos, argv[i], l); pos += l; cmd[pos] = 0;
        }
        SEGGER_RTT_printf(0, "=RAW sent %d bytes: %s\n", ow_raw_send(&dev, cmd), cmd);
        {
            absolute_time_t deadline = make_timeout_time_ms(700);
            int total = 0;
            while (!time_reached(deadline)) {
                int n = dev.t.read(dev.t.ctx, buf, sizeof buf, 50);
                if (n <= 0) continue;
                total += n;
                SEGGER_RTT_printf(0, "=RAWRX %d: ", n);
                for (int i = 0; i < n; i++) {
                    if (buf[i] >= 0x20 && buf[i] < 0x7F) SEGGER_RTT_printf(0, "%c", buf[i]);
                    else SEGGER_RTT_printf(0, "<%02x>", buf[i]);
                }
                SEGGER_RTT_printf(0, "\n");
            }
            SEGGER_RTT_printf(0, "=RAWEND total=%d line_len=%u evq=%u\n", total, (unsigned)dev.line_len, (unsigned)dev.evq_count);
        }
    } else if (strcmp(argv[0], "ZONES") == 0) {
        SEGGER_RTT_printf(0, "=ZONES valid=%d rails=%x can=%d\n", rails_valid, (unsigned)rails_live,
                          (int)((rails_live & picpwr_zone_bit(PICPWR_ZONE_CAN)) != 0));
    } else {
        SEGGER_RTT_printf(0, "=ERR unknown command %s\n", argv[0]);
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

/* ── screen ────────────────────────────────────────────────────────────── */
static char shown[10][48];

static void line_at(int idx, int y, int scale, uint16_t fg, const char* text) {
    char padded[48];
    int width = 480 / (6 * scale);
    int n = (int)strlen(text);
    if (width > 47) width = 47;
    if (n > width) n = width;
    memcpy(padded, text, (size_t)n);
    memset(padded + n, ' ', (size_t)(width - n));
    padded[width] = 0;
    if (strcmp(shown[idx], padded) == 0) return;
    strcpy(shown[idx], padded);
    st7796_draw_text(0, y, scale, fg, C_BLACK, padded);
}

static void screen_static(void) {
    memset(shown, 0, sizeof shown);
    st7796_fill_screen(C_BLACK);
    line_at(0, 4, 2, C_WHITE, "CAN BLAST  onewili display link");
    line_at(9, 296, 1, C_GREY, "GREEN flood  BLUE len  RED reset  YELLOW stream   HOLD HOME 5S TO EXIT");
}

static void screen_update(void) {
    char s[64];
    ow_fwgui_stats ls;
    ow_fwgui_get_stats(&ls);
    snprintf(s, sizeof s, "RX %8u  %5u/s", (unsigned)chk.received, (unsigned)st.rx_rate);
    line_at(1, 34, 3, C_GREEN, s);
    snprintf(s, sizeof s, "TX %8u  %5u/s", (unsigned)ftx.ok, (unsigned)st.tx_rate);
    line_at(2, 64, 3, C_CYAN, s);
    snprintf(s, sizeof s, "rx lost %u dup %u bad %u ooo %u", (unsigned)chk_lost(), (unsigned)chk.duplicates,
             (unsigned)chk.integrity_errors, (unsigned)chk.out_of_order);
    line_at(3, 104, 2, chk_lost() || chk.integrity_errors ? C_RED : C_WHITE, s);
    snprintf(s, sizeof s, "tx rej %u err %u inflight %u/%u", (unsigned)ftx.failed,
             (unsigned)(job.errors + ftx.proto_errors + ftx.io_errors), (unsigned)ftx.in_flight,
             (unsigned)ftx.max_in_flight);
    line_at(4, 128, 2, C_WHITE, s);
    snprintf(s, sizeof s, "link drop %u ovr %u ring %u bin %u", (unsigned)ls.dropped_frames,
             (unsigned)(ls.ring_overrun_bytes + ls.hw_overruns), (unsigned)ls.ring_max_fill,
             (unsigned)ls.binary_max_fill);
    line_at(5, 152, 2, ls.dropped_frames || ls.ring_overrun_bytes || ls.hw_overruns ? C_RED : C_WHITE, s);
    snprintf(s, sizeof s, "stream %s  flood %s len %u pipe %u",
             stream_mode == 1 ? "text" : stream_mode == 2 ? "binary" : "both",
             job.flood ? "ON " : "off", (unsigned)flood_len, (unsigned)pipe_depth);
    line_at(6, 176, 2, C_YEL, s);
    snprintf(s, sizeof s, "can rail %s  zones %05x  link %s",
             rails_valid ? ((rails_live & picpwr_zone_bit(PICPWR_ZONE_CAN)) ? "ON " : "OFF") : "?  ",
             (unsigned)rails_live, link_ok ? "up" : "DOWN");
    line_at(7, 200, 2, C_WHITE, s);
    snprintf(s, sizeof s, "bin %u txt %u echo %u other %u", (unsigned)st.rx_bin, (unsigned)st.rx_txt,
             (unsigned)st.tx_echo, (unsigned)st.other_ev);
    line_at(8, 224, 2, C_GREY, s);
}

/* ── buttons ───────────────────────────────────────────────────────────── */
static void buttons_poll(void) {
    uartkbd_event_t ev;
    while (uartkbd_next_event(&ev)) {
        if (!ev.pressed) continue;
        switch (ev.btn) {
        case UARTKBD_BTN_GREEN:
            if (job.active) tx_stop();
            else tx_start(FLOOD_RUN_ID, 0, flood_len, ARB_TX_DEFAULT, flood_len > 8, 0, pipe_depth, 1);
            break;
        case UARTKBD_BTN_BLUE:
            flood_len = flood_len == 8 ? 64 : flood_len == 64 ? 1 : flood_len == 1 ? 16 : 8;
            if (job.flood) { tx_stop(); tx_start(FLOOD_RUN_ID, 0, flood_len, ARB_TX_DEFAULT, flood_len > 8, 0, pipe_depth, 1); }
            break;
        case UARTKBD_BTN_RED:
            counters_reset();
            break;
        case UARTKBD_BTN_YELLOW:
            stream_set(stream_mode == 2 ? 1 : stream_mode == 1 ? 3 : 2);
            break;
        default:
            break;
        }
    }
}

int main(void) {
    uint32_t next_screen, next_second;
    /* Bigger RTT terminal buffers: the bench reads STAT lines and writes
     * commands on channel 0. Configured before the first DIAG. */
    SEGGER_RTT_ConfigUpBuffer(0, "Terminal", rtt_up, sizeof rtt_up, SEGGER_RTT_MODE_NO_BLOCK_SKIP);
    SEGGER_RTT_ConfigDownBuffer(0, "Terminal", rtt_down, sizeof rtt_down, SEGGER_RTT_MODE_NO_BLOCK_SKIP);

    board_init();   /* must precede ow_open_fwgui: uart_init reads clk_peri */
    fw2_app_recovery_init();
    st7796_init();
    fw2_app_about_use_lcd();
    agentio_init();                 /* before the first clear: it owns the shadow framebuffer */
    screen_static();
    board_backlight_set(1);
    line_at(1, 34, 3, C_YEL, "CAN rail...");

    /* The CAN controller sits on power zone 15; a standalone display app owns
     * its power policy. picpwr_task() re-asserts the rail if it drops. */
    picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_CAN));
    for (int i = 0; i < 150; i++) {          /* ~1.5 s for the rail walk */
        fw2_app_recovery_task();
        picpwr_task();
        if (picpwr_rails(&rails_live)) rails_valid = 1;
        sleep_ms(10);
    }
    DIAG("canblast: rails valid=%d mask=%x\n", rails_valid, (unsigned)rails_live);

    line_at(1, 34, 3, C_YEL, "MAIN link...");
    while (fw2_app_recovery_open_onewili(&dev) != OW_OK) {
        fw2_app_recovery_task();
        DIAG("canblast: FwGUI link open failed (is the main CPU running stock fw?), retry in 1 s\n");
        fw2_app_recovery_sleep_ms(1000);
    }
    {
        ow_transport bt = ow_fwgui_binary_transport();
        ow_binary_open(&bdev, &bt);
    }
    ow_fast_tx_init(&ftx, &dev, pipe_depth, on_tx_result, 0);
    if (rails_valid) ow_fwgui_send_power_zones(rails_live);
    can_configure(0, 500000, 2000000, 0);
    {
        ow_status s = stream_set(2);
        link_ok = s == OW_OK;
        DIAG("canblast: stream on -> %d\n", (int)s);
    }
    chk_reset(ARB_RX_DEFAULT, 0, 0);
    memset(shown[1], 0, sizeof shown[1]);
    DIAG("canblast: up, sys=%u kHz\n", (unsigned)(clock_get_hz(clk_sys) / 1000));

    next_screen = time_us_32() + 200000;
    next_second = time_us_32() + 1000000;
    for (;;) {
        uint32_t now;
        fw2_app_recovery_task();
        picpwr_task();
        agentio_task();
        rx_poll();
        tx_step();
        rtt_poll();
        buttons_poll();
        now = time_us_32();
        if ((int32_t)(now - next_second) >= 0) {
            next_second += 1000000;
            st.rx_rate = chk.received - st.rx_prev; st.rx_prev = chk.received;
            st.tx_rate = ftx.ok - st.tx_prev;       st.tx_prev = ftx.ok;
            if (picpwr_rails(&rails_live)) {
                rails_valid = 1;
                ow_fwgui_send_power_zones(rails_live);   /* MAIN may have rebooted: keep it informed */
            }
        }
        if ((int32_t)(now - next_screen) >= 0) {
            next_screen = now + 200000;
            screen_update();
        }
    }
}
