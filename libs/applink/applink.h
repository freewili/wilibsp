/* applink.h — DISPLAY-app end of the AppLink tunnel.
 *
 * A transparent byte channel between a DISPLAY-CPU app and custom ESP32-C5
 * firmware. MAIN relays the payload between the two links and never inspects
 * it, so the protocol that rides inside is entirely yours: adding a message
 * touches this app and the ESP32 firmware only, never MAIN's firmware.
 *
 * Wire definition, opcode allocation, and size limits: applink_protocol.h
 * (shared verbatim with MAIN and the ESP32 firmware).
 *
 *   DISPLAY app  --FwGUI event 73-->  MAIN  --bnose cmd 128-->  ESP32
 *   DISPLAY app  <--FwGUI cmd 0xF0--  MAIN  <--bnose res 128--  ESP32
 *
 * ── Choosing an integration mode ────────────────────────────────────────────
 *
 * Both modes share one UART0 (Rx=GPIO0, Tx=GPIO1, RTS=GPIO2, CTS=GPIO3,
 * 8 Mbaud, hardware flow control). There is exactly one receiver on that pin,
 * so only one piece of code may drain it.
 *
 *   STANDALONE — the app uses AppLink and nothing else on the display link.
 *       applink_init_standalone(on_rx, ctx);
 *       for (;;) { applink_pump(); ... }
 *
 *   SHARED — the app also uses OneWili (ow_* calls, SD access). OneWili's
 *       transport owns the UART; AppLink must not touch it. Call
 *       applink_init_shared() instead, and route unclaimed command frames in
 *       from that transport — see README.md for the ten-line patch to
 *       onewili_fwgui.c's rx_byte(). Calling applink_pump() in this mode will
 *       steal bytes from OneWili and break both.
 *
 * Sends are safe in either mode: TX on this link is fire-and-forget with no
 * shared state beyond the UART FIFO.
 */
#ifndef FW2_APPLINK_H
#define FW2_APPLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Receives one AppLink payload from the ESP32. `data`/`len` are the app bytes
 * after the port byte; `len` may be 0. The buffer is invalid once the callback
 * returns — copy anything you keep. Runs on whatever context called
 * applink_pump() / applink_feed(), so it is as re-entrant as your main loop. */
typedef void (*applink_rx_fn)(uint8_t port, const uint8_t *data, uint16_t len,
                              void *ctx);

/* STANDALONE mode: brings up UART0 (pins, baud, RTS/CTS) and installs the
 * receive callback. Do not use alongside ow_open_fwgui. */
void applink_init_standalone(applink_rx_fn fn, void *ctx);

/* SHARED mode: installs the receive callback only. Leaves the UART entirely
 * alone — the caller is responsible for feeding bytes or frames in. */
void applink_init_shared(applink_rx_fn fn, void *ctx);

/* STANDALONE mode: drain UART0 and dispatch any complete frames. Call from the
 * app's main loop, often enough that the 32-byte UART FIFO does not overflow at
 * 8 Mbaud. Does nothing useful in shared mode — see the header comment. */
void applink_pump(void);

/* SHARED mode, byte-level entry: feed raw UART0 bytes through AppLink's own
 * frame parser. Use when the sharing peer hands you undecoded bytes. */
void applink_feed(const uint8_t *bytes, size_t n);

/* SHARED mode, frame-level entry: hand in one already-decoded, already
 * checksum-verified command frame. Ignores every command code except
 * APPLINK_FWGUI_COMMAND, so it is safe to call for every frame the peer
 * transport did not claim. This is the entry point the onewili_fwgui.c patch
 * in README.md uses. */
void applink_on_frame(uint8_t command, const uint8_t *payload, uint16_t len);

/* Send one AppLink payload to the ESP32. Returns false if `len` exceeds
 * APPLINK_DATA_MAX; nothing is partially sent.
 *
 * Fire-and-forget — there is no ack anywhere in the relay, and a payload sent
 * while MAIN's ESP32 link is down is dropped silently by MAIN. If your app
 * needs delivery confirmation, build it into your own protocol. */
bool applink_send(uint8_t port, const uint8_t *data, uint16_t len);

/* Frames discarded on receive: checksum mismatch, or a length past
 * APPLINK_FRAME_MAX. Free-running; never reset except by an init call. A
 * climbing count in standalone mode usually means applink_pump() is not being
 * called often enough and the UART FIFO is overflowing mid-frame. */
uint32_t applink_dropped_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* FW2_APPLINK_H */
