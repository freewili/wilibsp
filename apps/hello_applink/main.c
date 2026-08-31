/* hello_applink — end-to-end smoke test for the AppLink tunnel.
 *
 * Sends a counter to custom ESP32-C5 firmware once a second and draws whatever
 * comes back on the LCD. MAIN relays both directions without inspecting the
 * bytes, so nothing here needs MAIN's firmware to know about this app.
 *
 *   DISPLAY (this app) --FwGUI event 73--> MAIN --bnose 128--> ESP32
 *   DISPLAY (this app) <--FwGUI cmd 0xF0-- MAIN <--bnose 128-- ESP32
 *
 * THE OTHER HALF IS REQUIRED. The stock ESP32 firmware has no AppLink handler
 * installed, so out of the box this app sends into the void and the RX counter
 * stays at zero — which is the correct behaviour, not a bug. Flash the echo
 * example from bottlenose-firmware/examples/applink_echo/ to close the loop.
 *
 * Uses STANDALONE mode: this app owns UART0 and drives the frame parser itself.
 * An app that also needs OneWili (MAIN GPIO, SD card) must use shared mode
 * instead — see libs/applink/README.md.
 */
#include "fw2.h"
#include "platform/diag.h"
#include "pico/stdlib.h"

#include "applink.h"
#include "applink_protocol.h"

#include <stdio.h>
#include <string.h>

/* App-defined ports. MAIN attaches no meaning to these — they exist so one
 * tunnel can carry several independent streams. Keep this block identical to
 * the ESP32 side. */
#define HELLO_PORT_PING   0

static volatile uint32_t g_rx_frames;
static char              g_last_reply[32];
static volatile bool     g_reply_dirty;

static void on_applink_rx(uint8_t port, const uint8_t *data, uint16_t len, void *ctx)
{
    (void)ctx;
    if (port != HELLO_PORT_PING)
        return;

    g_rx_frames++;

    /* Treat the reply as text for display purposes only; the tunnel itself is
     * binary-clean and carries whatever your protocol defines. */
    uint16_t n = len < sizeof(g_last_reply) - 1 ? len : sizeof(g_last_reply) - 1;
    memcpy(g_last_reply, data, n);
    g_last_reply[n] = '\0';
    g_reply_dirty = true;
}

int main(void)
{
    board_init();          /* must precede applink_init: uart_init reads clk_peri */
    st7796_init();
    st7796_fill_screen(0x0000);
    board_backlight_set(1);

    st7796_draw_text(8, 8, 2, 0xFFFF, 0x0000, "APPLINK TUNNEL");
    st7796_draw_text(8, 40, 1, 0xFFFF, 0x0000, "DISPLAY <-> MAIN <-> ESP32-C5");
    st7796_draw_text(8, 288, 1, 0xFFFF, 0x0000, "NEEDS applink_echo ON THE ESP32");

    applink_init_standalone(on_applink_rx, NULL);
    DIAG("hello_applink: tunnel up, pinging every 1 s\n");

    uint32_t seq = 0;
    absolute_time_t next_ping = get_absolute_time();

    for (;;) {
        applink_pump();   /* drain UART0; must run far more often than we send */

        if (absolute_time_diff_us(get_absolute_time(), next_ping) <= 0) {
            next_ping = delayed_by_ms(get_absolute_time(), 1000);

            char msg[24];
            int  n = snprintf(msg, sizeof(msg), "ping %lu", (unsigned long)seq++);
            applink_send(HELLO_PORT_PING, (const uint8_t *)msg, (uint16_t)n);

            char line[48];
            snprintf(line, sizeof(line), "TX %lu  RX %lu  DROP %lu",
                     (unsigned long)seq,
                     (unsigned long)g_rx_frames,
                     (unsigned long)applink_dropped_frames());
            st7796_fill_rect(8, 80, 460, 16, 0x0000);
            st7796_draw_text(8, 80, 1, 0xFFFF, 0x0000, line);
        }

        if (g_reply_dirty) {
            g_reply_dirty = false;
            st7796_fill_rect(8, 110, 460, 24, 0x0000);
            st7796_draw_text(8, 110, 2,
                             g_rx_frames ? 0x07E0 : 0xFFFF, 0x0000, g_last_reply);
            DIAG("hello_applink: reply \"%s\"\n", g_last_reply);
        }
    }
}
