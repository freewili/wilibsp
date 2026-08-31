/* applink.c — DISPLAY-app end of the AppLink tunnel. See applink.h.
 *
 * The RX state machine deliberately mirrors the one in the OneWili FwGUI
 * transport (onewili/wilibsp/src/onewili_fwgui.c): same frame shape, same
 * resync behaviour, same "parse then discard" handling of overlong frames. Two
 * parsers on one wire that disagree about resync would be a debugging trap, so
 * when one changes the other should follow. */

#include "applink.h"
#include "applink_protocol.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"

/* Display link, MAIN side of the board: UART0 at 8 Mbaud with hardware flow
 * control. Pins per bsp/platform/board.h and the OneWili transport. */
#define APPLINK_UART        uart0
#define APPLINK_BAUD        8000000
#define APPLINK_PIN_RX      0
#define APPLINK_PIN_TX      1
#define APPLINK_PIN_RTS     2
#define APPLINK_PIN_CTS     3

/* Command frames arrive MAIN->DISPLAY with sync BE BA; event frames go
 * DISPLAY->MAIN with sync B0 1D. Asymmetric on purpose — a receiver can never
 * mistake its own traffic for an inbound frame. */
#define APPLINK_CMD_SYNC0   0xBE
#define APPLINK_CMD_SYNC1   0xBA
#define APPLINK_EVT_SYNC0   0xB0
#define APPLINK_EVT_SYNC1   0x1D

static applink_rx_fn g_handler;
static void         *g_ctx;
static uint32_t      g_dropped;
static bool          g_owns_uart;

/* ── RX ─────────────────────────────────────────────────────────────────── */

enum rx_state {
    RX_SYNC0 = 0, RX_SYNC1, RX_LEN0, RX_LEN1, RX_CMD, RX_PAYLOAD, RX_CK0, RX_CK1
};

static struct {
    enum rx_state st;
    uint16_t      len, got, sum, ck;
    uint8_t       cmd;
    uint8_t       payload[APPLINK_FRAME_MAX];
    int           overlong;   /* parse to the end, then discard: keeps sync */
} g_rx;

void applink_on_frame(uint8_t command, const uint8_t *payload, uint16_t len)
{
    if (command != APPLINK_FWGUI_COMMAND)
        return;                       /* GUI traffic, or another overlay */
    if (len < 1) {                    /* no port byte: malformed */
        g_dropped++;
        return;
    }
    if (!g_handler)
        return;

    g_handler(applink_port(payload), applink_data(payload),
              (uint16_t)applink_data_len(len), g_ctx);
}

static void rx_byte(uint8_t b)
{
    switch (g_rx.st) {
    case RX_SYNC0:
        if (b == APPLINK_CMD_SYNC0) { g_rx.sum = b; g_rx.st = RX_SYNC1; }
        break;
    case RX_SYNC1:
        if (b == APPLINK_CMD_SYNC1) { g_rx.sum += b; g_rx.st = RX_LEN0; }
        /* A repeated sync0 is the start of the real frame, not a failure. */
        else g_rx.st = (b == APPLINK_CMD_SYNC0) ? RX_SYNC1 : RX_SYNC0;
        break;
    case RX_LEN0:
        g_rx.sum += b; g_rx.len = b; g_rx.st = RX_LEN1;
        break;
    case RX_LEN1:
        g_rx.sum += b; g_rx.len |= (uint16_t)(b << 8);
        g_rx.got = 0;
        g_rx.overlong = g_rx.len > APPLINK_FRAME_MAX;
        g_rx.st = RX_CMD;
        break;
    case RX_CMD:
        g_rx.sum += b; g_rx.cmd = b;
        g_rx.st = g_rx.len ? RX_PAYLOAD : RX_CK0;
        break;
    case RX_PAYLOAD:
        g_rx.sum += b;
        if (!g_rx.overlong) g_rx.payload[g_rx.got] = b;
        if (++g_rx.got >= g_rx.len) g_rx.st = RX_CK0;
        break;
    case RX_CK0:
        g_rx.ck = b; g_rx.st = RX_CK1;
        break;
    case RX_CK1:
        g_rx.ck |= (uint16_t)(b << 8);
        if (g_rx.ck != g_rx.sum || g_rx.overlong) {
            /* Only count frames that were addressed to us. A checksum failure
             * on someone else's GUI frame is not this library's problem, but we
             * cannot tell the two apart once the checksum is bad — so count the
             * overlong case (which we can attribute) and stay quiet otherwise. */
            if (g_rx.overlong && g_rx.cmd == APPLINK_FWGUI_COMMAND)
                g_dropped++;
        } else {
            applink_on_frame(g_rx.cmd, g_rx.payload, g_rx.len);
        }
        g_rx.st = RX_SYNC0;
        break;
    }
}

void applink_feed(const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        rx_byte(bytes[i]);
}

void applink_pump(void)
{
    if (!g_owns_uart)
        return;                       /* shared mode: OneWili owns the UART */
    while (uart_is_readable(APPLINK_UART))
        rx_byte((uint8_t)uart_getc(APPLINK_UART));
}

/* ── TX ─────────────────────────────────────────────────────────────────── */

bool applink_send(uint8_t port, const uint8_t *data, uint16_t len)
{
    if (!applink_fits(len))
        return false;
    if (len && !data)
        return false;

    /* B0 1D | len u16le (payload after the event code) | event code | port |
     * data | cksum u16le (additive sum over every preceding byte). */
    uint8_t  f[2 + 2 + 1 + APPLINK_FRAME_MAX + 2];
    uint16_t payload_len = (uint16_t)(1 + len);   /* port + data */
    uint32_t k = 0;

    f[k++] = APPLINK_EVT_SYNC0;
    f[k++] = APPLINK_EVT_SYNC1;
    f[k++] = (uint8_t)(payload_len & 0xFF);
    f[k++] = (uint8_t)(payload_len >> 8);
    f[k++] = APPLINK_FWGUI_EVENT;
    f[k++] = port;
    if (len) { memcpy(&f[k], data, len); k += len; }

    uint16_t sum = 0;
    for (uint32_t i = 0; i < k; i++)
        sum = (uint16_t)(sum + f[i]);
    f[k++] = (uint8_t)(sum & 0xFF);
    f[k++] = (uint8_t)(sum >> 8);

    uart_write_blocking(APPLINK_UART, f, k);
    return true;
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

static void applink_reset(applink_rx_fn fn, void *ctx)
{
    g_handler = fn;
    g_ctx     = ctx;
    g_dropped = 0;
    memset(&g_rx, 0, sizeof(g_rx));
}

void applink_init_shared(applink_rx_fn fn, void *ctx)
{
    applink_reset(fn, ctx);
    g_owns_uart = false;
}

void applink_init_standalone(applink_rx_fn fn, void *ctx)
{
    applink_reset(fn, ctx);
    g_owns_uart = true;

    uart_init(APPLINK_UART, APPLINK_BAUD);
    gpio_set_function(APPLINK_PIN_RX,  GPIO_FUNC_UART);
    gpio_set_function(APPLINK_PIN_TX,  GPIO_FUNC_UART);
    gpio_set_function(APPLINK_PIN_RTS, GPIO_FUNC_UART);
    gpio_set_function(APPLINK_PIN_CTS, GPIO_FUNC_UART);
    uart_set_hw_flow(APPLINK_UART, true, true);
    uart_set_format(APPLINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(APPLINK_UART, true);
}

uint32_t applink_dropped_frames(void)
{
    return g_dropped;
}
