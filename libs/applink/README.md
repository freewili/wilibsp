# AppLink — DISPLAY app ↔ ESP32 byte tunnel

A transparent byte channel between a DISPLAY-CPU app and custom ESP32-C5
firmware. MAIN relays the payload and never inspects it, so **the protocol
inside is yours**: adding a message touches your app and your ESP32 firmware
only, and MAIN does not need a rebuild or a release.

```
DISPLAY app  --FwGUI event 73-->  MAIN  --bnose cmd 128-->  ESP32
DISPLAY app  <--FwGUI cmd 0xF0--  MAIN  <--bnose res 128--  ESP32
```

## The three pieces

| Where | What | Repo |
| --- | --- | --- |
| Wire definition | `applink_protocol.h` | `freewili-firmware/shared/` (canonical) |
| MAIN relay | `rmpLib/fwgui_applink.cpp` | `freewili-firmware/freewilimain/` |
| DISPLAY client | `libs/applink/` | this repo |
| ESP32 client | `main/applink.cpp` | `bottlenose-firmware/` |

`applink_protocol.h` is copied into all three trees, following the same
convention `bottlenose_protocol.h` already uses. **Re-copy it when the wire
changes** — three copies drifting apart is the failure mode worth guarding
against.

## Quick start

```c
#include "applink.h"

static void on_rx(uint8_t port, const uint8_t *data, uint16_t len, void *ctx) {
    /* your protocol */
}

int main(void) {
    board_init();                          /* before applink: uart needs clk_peri */
    applink_init_standalone(on_rx, NULL);
    for (;;) {
        applink_pump();                    /* drain UART0 every loop */
        applink_send(0, (const uint8_t *)"hi", 2);
    }
}
```

`apps/hello_applink` is the worked example; `bottlenose-firmware/examples/applink_echo/`
is its ESP32 counterpart. The stock ESP32 firmware installs no handler, so
without that example flashed the tunnel is a one-way trip into the void — which
is correct behaviour, not a bug.

## Standalone vs. shared mode

There is one UART0 on the display link and exactly one receiver may drain it.

**Standalone** (`applink_init_standalone`) — this library brings up UART0 and
owns the RX pump. Correct when AppLink is the only thing your app uses on the
display link.

**Shared** (`applink_init_shared`) — for an app that *also* uses OneWili
(`ow_*` calls, SD card access). OneWili's transport owns the UART; AppLink must
never call `applink_pump()`. Instead, route unclaimed command frames in from
that transport. In `libs/onewili/src/onewili_fwgui.c`, `rx_byte()`'s `RX_CK1`
arm currently reads:

```c
if (g_rx.cmd == OWFW_CMD_RESPONSE) fifo_push_frame(&g_text, g_rx.payload, g_rx.len);
else if (g_rx.cmd == OWFW_CMD_BINARY) fifo_push_frame(&g_binary, g_rx.payload, g_rx.len);
else if (g_rx.cmd == OWFW_CMD_SDFS) sdfs_push(g_rx.payload, g_rx.len);
/* every other command code (GUI traffic) is discarded */
```

Add a fourth arm:

```c
else applink_on_frame(g_rx.cmd, g_rx.payload, g_rx.len);
```

`applink_on_frame` ignores every command code except `0xF0`, so it is safe as a
catch-all. Note that `libs/onewili` is a **submodule** — that edit lives in the
`onewili` repo, tagged the way its `CMakeLists.txt` tags other local additions,
not as a stray change in this tree.

## Limits and behaviour

- **MTU is 192 bytes** total (`APPLINK_FRAME_MAX`), so 191 app bytes after the
  port byte. Set by the FwGUI command path, which frames into a 256-byte stack
  buffer and hard-rejects payloads over 249. Segment above this layer.
- **Fire-and-forget.** No ack exists anywhere in the relay. A payload sent while
  MAIN's ESP32 link is down is dropped silently by MAIN. Build delivery
  confirmation into your own protocol if you need it.
- **The port byte is yours.** MAIN copies it through and attaches no meaning to
  any value. Use it to run several streams over one tunnel, or to let two
  cooperating components share it without a second opcode allocation.
- **You cannot demand the ESP32 rail.** `fw2_display_app`'s `POWER_ZONES` has
  no ESP32 entry — zone 5 belongs to MAIN, whose flasher command declares it
  itself. If that rail is down, MAIN drops AppLink payloads silently, which
  looks exactly like an app that isn't answering.
- **Pump often.** At 8 Mbaud the UART FIFO is 32 bytes. A climbing
  `applink_dropped_frames()` in standalone mode usually means `applink_pump()`
  is not being called often enough.

## Why new opcodes on every hop

Worth recording, so the next person does not rediscover it:

- **bnose 21/16 are not free.** On the ESP32, inbound `BNOSE_CMD_BINARY_STREAM_DATA`
  is forwarded to WebSocket clients and the BLE terminal (`main.cpp`), and
  outbound `BNOSE_RES_BINARY_STREAM_DATA` carries WebSocket binary frames back
  (`websocketTerminalBridge.cpp`). Tunnelling over them would interleave app
  traffic with terminal traffic in both directions.
- **FwGUI 0x5D/0x5E/0x5F are taken** by the OneWili and SDFS overlays, which a
  DISPLAY app is likely already using. AppLink is a fourth overlay, not a
  passenger on an existing one.
- **`0xF0`, not `0x9A`.** The highest real FwGUI command is `0x99`, leaving
  `0x9A–0xFF` free. Upstream appends upward from `0x9A`, so AppLink takes from
  the top of the range. Event `73` is the first free event code; `fwEventSink`
  dispatches on the numeric code, and `fwGUIEvents.h` records a merge-time
  renumbering caused by exactly that kind of collision.
