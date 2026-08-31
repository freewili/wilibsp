// applink_protocol.h
//
// AppLink — a transparent byte tunnel between a DISPLAY-CPU app and custom
// ESP32-C5 firmware, relayed by MAIN.
//
// Single source of truth for all three hops. This header is shared verbatim by
// every participant, the same way bottlenose_protocol.h is:
//
//   * DISPLAY app          — wilibsp `libs/applink`
//   * MAIN (relay)         — freewilimain `rmpLib/fwgui_applink.cpp`
//   * ESP32-C5 firmware    — bottlenose-firmware `main/applink.cpp`
//
// It is C/C++ compatible and dependency-free (stdint only) so it can be
// dropped into the Pico SDK build, the ESP-IDF build, or a host test harness.
//
// ----------------------------------------------------------------------------
// TOPOLOGY
// ----------------------------------------------------------------------------
//
//   DISPLAY app                MAIN (relay)                 ESP32-C5 app code
//   ───────────                ────────────                 ─────────────────
//        │                          │                              │
//        │  FwGUI event 0x49        │   bnose cmd 128              │
//        ├─────────────────────────►├─────────────────────────────►│
//        │  (B0 1D frame, UART0)    │   (BE EF frame, PIO-UART)    │
//        │                          │                              │
//        │  FwGUI command 0xF0      │   bnose res 128              │
//        │◄─────────────────────────┤◄─────────────────────────────┤
//        │  (BE BA frame, UART0)    │   (FE ED frame, PIO-UART)    │
//
// MAIN copies the payload between the two links and never inspects it. Adding a
// message to your app protocol therefore touches the DISPLAY app and the ESP32
// firmware only — MAIN does not need a rebuild.
//
// ----------------------------------------------------------------------------
// WHY NEW OPCODES ON EVERY HOP
// ----------------------------------------------------------------------------
//
// The obvious-looking reuse does not work, so it is spelled out here to stop
// the next person rediscovering it:
//
//   * bnose 21 / 16 (BNOSE_CMD/RES_BINARY_STREAM_DATA) are NOT free. On the
//     ESP32, inbound 21 is forwarded to WebSocket clients and the BLE terminal
//     (main.cpp), and outbound 16 carries WebSocket binary frames back
//     (websocketTerminalBridge.cpp). Tunnelling over them would interleave app
//     traffic with the terminal bridge in both directions.
//
//   * FwGUI 0x5D/0x5E/0x5F are the OneWili and SDFS overlays. A DISPLAY app
//     already uses those for ow_* calls and SD access, so AppLink has to be a
//     fourth overlay rather than a passenger on an existing one.
//
// ----------------------------------------------------------------------------
// FRAME PAYLOAD
// ----------------------------------------------------------------------------
//
// Identical on all three hops — each transport supplies its own sync word,
// length and checksum, and AppLink rides inside as the payload:
//
//   +--------+=====================+
//   |  PORT  |     APP BYTES       |
//   +--------+=====================+
//    1 byte    0..APPLINK_DATA_MAX
//
// PORT is an app-defined demultiplexing byte. It exists so one app can run
// several independent streams (control vs. bulk, say), or so two cooperating
// components can share the tunnel without a second opcode allocation. MAIN
// copies it through untouched and attaches no meaning to any value.
//
#ifndef APPLINK_PROTOCOL_H
#define APPLINK_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// Opcode allocation
// ----------------------------------------------------------------------------

// DISPLAY -> MAIN, carried as a FwGUI event frame (sync B0 1D).
//
// Event codes are densely allocated 0..72 in rmpLib/fwGUIEvents.h; 73 (0x49) is
// the first free one. Events are dispatched on the numeric code by fwEventSink,
// so a collision silently parses one event as another — see the renumbering
// note on FWGUI_EVENT_RFID_* in that header for what that costs.
#define APPLINK_FWGUI_EVENT     73

// MAIN -> DISPLAY, carried as a FwGUI command frame (sync BE BA).
//
// The highest real command in rmpLib/fwGUIConstants.h is 0x99
// (FWGUI_API_LCD_REINIT), leaving 0x9A..0xFF free. Deliberately taken from the
// TOP of that range: upstream appends new FWGUI_API_* codes upward from 0x9A,
// so a low pick would eventually collide.
#define APPLINK_FWGUI_COMMAND   0xF0

// MAIN <-> ESP32, carried as bottlenose frames (sync BE EF host->esp,
// FE ED esp->host).
//
// bnose_cmd_t runs 1..29 and bnose_res_t 1..30 in bottlenose_protocol.h, both
// densely packed. 128 leaves the whole 30..127 span for upstream growth.
#define APPLINK_BNOSE_CMD       128
#define APPLINK_BNOSE_RES       128

// ----------------------------------------------------------------------------
// Size limits
// ----------------------------------------------------------------------------

// Total AppLink payload (PORT + app bytes) carried in one frame.
//
// The binding constraints, smallest first:
//   * FwGUI command TX frames into a 256-byte stack buffer and hard-rejects
//     payloads over 249 (fwgui_transport.cpp processHostCommand — a 294-byte
//     payload smashed the stack on the bench 2026-07-31).
//   * FwGUI event RX drops frames whose length exceeds 256
//     (fwgui_transport.cpp drainDecodedFrames).
//   * The ESP32 link is wider than both (frames run to 1022 bytes).
//
// 192 sits comfortably under the real ceiling with room for the transports to
// tighten. Anything larger is the app's job to segment and reassemble.
#define APPLINK_FRAME_MAX       192
#define APPLINK_DATA_MAX        (APPLINK_FRAME_MAX - 1)   // 191

// ----------------------------------------------------------------------------
// Reserved ports
// ----------------------------------------------------------------------------

// Ports are app-defined. These two are conventions, not enforcement — MAIN
// never reads the port byte.
#define APPLINK_PORT_CONTROL    0     // small request/response, app-defined
#define APPLINK_PORT_BULK       1     // streamed data

// ----------------------------------------------------------------------------
// Payload accessors
// ----------------------------------------------------------------------------
//
// Trivial, but they keep the "port is byte 0" convention in one place instead
// of open-coded +1 / -1 arithmetic at every call site on three CPUs.

// Port byte of a received AppLink payload. Caller must have checked len >= 1.
static inline uint8_t applink_port(const uint8_t* payload)
{
    return payload[0];
}

// App bytes of a received AppLink payload.
static inline const uint8_t* applink_data(const uint8_t* payload)
{
    return payload + 1;
}

// App-byte count of a received AppLink payload of total length `len`.
// Returns 0 for a malformed (empty) payload rather than underflowing.
static inline uint32_t applink_data_len(uint32_t len)
{
    return len ? (len - 1u) : 0u;
}

// True if `data_len` app bytes fit in one frame.
static inline int applink_fits(uint32_t data_len)
{
    return data_len <= (uint32_t)APPLINK_DATA_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* APPLINK_PROTOCOL_H */
