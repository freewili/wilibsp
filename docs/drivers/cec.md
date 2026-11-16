# CEC (`bsp/cec`)

CEC on the TV connector. The board joins the TV's CEC bus as a playback
device (and, if the app asks, a second device type). It claims a logical
address, gives the answers every CEC device owes (physical address, name,
vendor ID, power status, CEC version, menu and deck status), hands the TV
remote's keys and the TV's standby to the app, and can wake the TV and switch
it to this input, or put the TV in standby.

Past that device the app can ask the TV and the other devices on the bus:
any frame with an awaited answer, power status, system information, names,
volume, remote keys with hold, routing, the device menu, `<Abort>` and
`<Feature Abort>`, vendor commands and a bus scan. And it can watch the bus:
a capture keeps the frames for this board, or every frame on the cable, with
a timestamp and the acknowledge.

**Hardware status.** The device part (address claim, remote keys, standby
received, wake and input switch sent) was verified on the board 2026-11-14:
`docs/superpowers/findings/2026-11-14-cec-e2e.md`. **Everything else on this
page is built and host-tested, not yet run on a board**: the requests, the
capture, the second device type, the vendor ID setting, the menu, language,
deck and feature-abort events, and the key-repeat, vendor-frame and routing
events with `cec_accept_vendor_frames()`
(`docs/superpowers/findings/2026-11-14-cec-full-e2e.md`,
`docs/superpowers/findings/2026-11-14-tvout-cec-app-gaps.md`). The bit engine and the
device are the stock DISPLAY firmware's CEC code.

## Quick start

```c
#include "fw2.h"

int main(void) {
    board_init();
    fw2_app_recovery_init();
    if (!cec_start("My app"))                 // the name in the TV's input list
        DIAG("cec: %s\n", cec_last_error());
    while (true) {
        fw2_app_recovery_task();
        cec_task();                           // every pass
        cec_event_t ev;
        while (cec_next_event(&ev)) {
            if (ev.type == CEC_EV_KEY_DOWN)
                DIAG("key %s\n", cec_key_name(ev.data));
            else if (ev.type == CEC_EV_STANDBY)
                DIAG("TV went to standby\n");
        }
        ...
    }
}
```

### Asking another device

One request runs at a time. Start it, keep calling `cec_task()`, and take the
outcome when it is there; nothing blocks.

```c
if (cec_request_power_status(CEC_LOGICAL_TV))
    DIAG("asked\n");
...
cec_reply_t r;
if (cec_request_result(&r)) {                 // true once, when it finished
    if (r.result == CEC_RESULT_ANSWERED)
        DIAG("TV is %s\n", cec_power_status_name(r.power_status));
    else
        DIAG("no: %s\n", cec_result_name(r.result));
}
```

Any frame, here `<Give Device Vendor ID>` with the answer the standard pairs
with it:

```c
static const uint8_t give_vendor[] = { 0x8C };          // opcode, then operands
cec_request_raw(CEC_LOGICAL_TV, give_vendor, 1, CEC_ANSWER_STANDARD);
// later: r.answer is the whole <Device Vendor ID> frame, r.vendor_id its value
```

### Watching the bus

```c
cec_capture(CEC_CAPTURE_ALL);                 // every frame on the cable
...
cec_capture_frame_t f;
while (cec_next_frame(&f))
    DIAG("%u %x>%x %s acked=%d\n", f.time_us, f.frame.block[0] >> 4, f.frame.block[0] & 15,
         f.frame.len > 1 ? cec_opcode_name(f.frame.block[1]) : "poll", (f.flags & CEC_FRAME_ACKED) != 0);
```

`apps/hello_cec` is the example app: it starts the device and the colour-bar
picture (`docs/drivers/tvout.md`) so the input it selects shows something;
GREEN wakes the TV (`cec_wake_tv()`), RED puts it in standby, YELLOW asks the
TV's power status (a request and its `cec_request_result()`), the remote's
arrows move a square, and the claimed address, keys, standby and input
changes print on the LCD and over RTT. The bench runs that verified the
driver (every request, the bus scan, capture, roles, key repeats) are
recorded in `docs/superpowers/findings/2026-11-14-cec-e2e.md` and
`2026-11-14-cec-full-e2e.md`. `apps/emberward` uses the device for remote
keys in a full app.

## API (`bsp/cec/cec.h`)

### The device

| Call | What it does |
|---|---|
| `bool cec_start(const char *osd_name)` | Starts the device on the next `cec_task()`. `osd_name`: up to 14 printable characters, `NULL` for "Freewili 2". False with `cec_last_error()` if TIMER1 is not counting microseconds. |
| `void cec_stop(void)` | Leaves the bus. An active source says `<Inactive Source>` first; the queued frames go out over the next `cec_task()` calls (at most 1.5 s), then the wire is released. A request still running ends `CEC_RESULT_FAILED`. |
| `bool cec_running(void)` | The wire is open. |
| `const char *cec_last_error(void)` | Why the last start was refused, or "". |
| `void cec_set_physical_address(uint16_t)` / `cec_physical_address()` | This device's place in the connection tree, A.B.C.D as `0xABCD`. Set by the app, it is final (see [Physical address](#physical-address) for where it comes from otherwise). A change while running claims the address again. |
| `cec_address_source_t cec_physical_address_source(void)` / `cec_address_source_name()` | Where the address in use came from: `CEC_ADDRESS_DEFAULT`, `_SAVED`, `_LEARNED`, `_FIRMWARE`, `_APP` ("default", "saved", ...). |
| `void cec_use_saved_address(uint16_t)` | The address saved on the SD card; used unless a higher-ranked source gave one. `cec_saved_load()` calls it. |
| `bool cec_take_learned_address(uint16_t *)` | True once per learned address, for saving it; `cec_saved_task()` calls it. |
| `bool cec_set_device_types(uint8_t first, uint8_t second)` | What the board presents itself as: `CEC_TYPE_PLAYBACK`, `_RECORDER`, `_TUNER` or `_AUDIO_SYSTEM`, and a second of those or `CEC_TYPE_NONE`. Each claims its own logical address. Default playback alone. Refused: the same type twice, playback with recorder. A change while running claims again. |
| `void cec_set_vendor_id(uint32_t)` | The 24-bit vendor ID answered and announced after the claim; `CEC_VENDOR_NONE`: none, and `<Give Device Vendor ID>` is refused. Default `CEC_VENDOR_ID_DEFAULT` (`0x001582`). Applies at once. |
| `void cec_task(void)` | Every main-loop pass: carries frames between the wire, the device, the request and the capture. |
| `bool cec_next_event(cec_event_t *)` | The next event, oldest first (`uartkbd_next_event()` style). |
| `void cec_accept_vendor_frames(bool on)` | `true`: `<Vendor Command>`, `<Vendor Remote Button Down/Up>` and `<Vendor Command With ID>` for this board are acknowledged, not answered, and handed over whole as `CEC_EV_VENDOR`. `false` (default): the device refuses the directly addressed ones with `<Feature Abort>`. Applies at once and across starts. |
| `bool cec_wake_tv(void)` | One Touch Play: `<Image View On>` to the TV, then `<Active Source>`. |
| `bool cec_tv_standby(void)` | `<Standby>` to the TV. |
| `uint8_t cec_logical_address(void)` | The address requests leave from; 15 while claiming, with none free, or stopped. |
| `uint16_t cec_logical_addresses(void)` | Bit n: logical address n held (one per device type). |
| `cec_get_stats()` / `cec_clear_stats()` | Frame, error, timing and capture counters. |

Events (`cec_event_t`: `.type`, `.data`, `.from` the other device, `.to` this
board's address it concerns, `.extra[2]`, `.time_ms` when `cec_task()` made
it):

| Type | Data | When |
|---|---|---|
| `CEC_EV_ADDRESS` | Logical address claimed, 15 if none was free; `extra[0]` the device type | Once per device type per start |
| `CEC_EV_KEY_DOWN` | `[UI Command]` (`CEC_KEY_*`); `to` the address it was sent to | A remote key pressed, once per press |
| `CEC_EV_KEY_REPEAT` | `[UI Command]` | The remote's device sent `<User Control Pressed>` again for the held key: the key is held, not tapped. A held key repeats within 450 ms (CEC 13.13.3); a tap gives none |
| `CEC_EV_KEY_UP` | `[UI Command]`; `extra[0]` 0: a release (or another key) ended it, 1: no release came | The key released, or 550 ms after its last press or repeat with no release |
| `CEC_EV_VENDOR` | The opcode: `0x89`, `0x8A`, `0x8B`, `0xA0`; `.frame` the whole frame, header first | Only after `cec_accept_vendor_frames(true)`: a vendor frame for this board, or a broadcast `<Vendor Command With ID>` |
| `CEC_EV_STANDBY` | 0 | `<Standby>` received: the TV is going off |
| `CEC_EV_ACTIVE_SOURCE` | 1 / 0 | The TV switched to this input / to another one |
| `CEC_EV_MENU_REQUEST` | `CEC_MENU_ACTIVATE` / `_DEACTIVATE` / `_QUERY` | `<Menu Request>` received; `<Menu Status>` is already answered |
| `CEC_EV_MENU_LANGUAGE` | `data`, `extra[0]`, `extra[1]`: three letters, ISO 639-2 | The TV announced its menu language |
| `CEC_EV_DECK_PLAY` / `CEC_EV_DECK_CONTROL` | `[Play Mode]` / `[Deck Control Mode]` | `<Play>` / `<Deck Control>` to a playback or recorder address |
| `CEC_EV_FEATURE_ABORT` | The opcode refused; `extra[0]` the reason (`CEC_REASON_*`) | `from` refused a message of this board |
| `CEC_EV_ROUTING` | `0x80` or `0x86`; `extra[0]`, `extra[1]` the physical address the route now leads to, high byte first | A broadcast `<Routing Change>` (its new address) or `<Set Stream Path>` from `from` |

**Vendor frames.** Some TVs send remote keys a device does not take as
remote keys as `<Vendor Remote Button Down>` / `<Vendor Remote Button Up>`
(the vendor's own code in the operands), and vendors exchange `<Vendor
Command With ID>`. The stock device behaviour answers a directly addressed one
with `<Feature Abort>` [Unrecognized opcode], which may make a TV stop
sending them. The core's behaviour switches only turn every refusal off at
once, so the BSP handles this itself: with `cec_accept_vendor_frames(true)`
those four opcodes never reach the device and arrive as `CEC_EV_VENDOR` with
the frame and its `time_ms`, so a learner records them without the capture.
The app then owns the answer: nothing, or its own `cec_request_raw()`.
Whether a given TV stops sending after a refusal has not been checked with
this BSP.

**Held keys.** A game that repeats a held arrow steps on `CEC_EV_KEY_DOWN`
and on each `CEC_EV_KEY_REPEAT`, at the remote's own rate, or runs its own
repeat timer from the first `CEC_EV_KEY_REPEAT` until `CEC_EV_KEY_UP`; a tap
never produces a repeat, so nothing waits for the 550 ms time-out. A learner
gets the press, each repeat and the release with their `time_ms` and needs no
capture. Some TVs send transport keys as `<Play>` or `<Deck Control>`
(`CEC_EV_DECK_*`) instead of remote keys: handle both for PLAY and PAUSE.

TVs commonly send remote keys only to the device they have switched to, and only when their
CEC setting (each brand names it differently) allows it: switch to the board's input
first (`cec_wake_tv()` does) before expecting keys.

### Requests to other devices

Each `cec_request_*()` call starts one request and returns true, or returns
false and starts nothing: CEC not running, a request still running, the send
queue short of room, or an argument out of range. `cec_task()` sends its
frames and waits for the answers: up to 4 s for a frame to get onto the wire,
1 s for each answer (CEC 9.2). `cec_request_result(&reply)` is true once,
when it has finished; `cec_request_busy()` says one runs.

`reply.result` is that of the request's first frame:

| `cec_result_t` | Meaning |
|---|---|
| `CEC_RESULT_ANSWERED` | The answer came; `reply.answer` is the whole frame, header first |
| `CEC_RESULT_SENT` | Acknowledged; no answer asked for (a broadcast: nobody rejected it) |
| `CEC_RESULT_REFUSED` | The destination answered `<Feature Abort>`; `reply.abort_reason` |
| `CEC_RESULT_NO_ANSWER` | Acknowledged, and no answer within a second |
| `CEC_RESULT_NOT_ACKED` | Nobody acknowledged it (nothing at that address); a broadcast: a device rejected it |
| `CEC_RESULT_FAILED` | It did not get onto the wire, or CEC stopped |

| Call | Sends | Fills in `cec_reply_t` | What the TV (or device) needs |
|---|---|---|---|
| `cec_request_raw(destination, data, len, answer)` | Any frame: `data` = opcode + up to 14 operands, `len` 0 polls `destination` (0-15). `answer`: `CEC_ANSWER_NONE`, `CEC_ANSWER_STANDARD` or an opcode | `answer`, and the field for a known answer | Whatever the message needs |
| `cec_request_power_status(destination)` | `<Give Device Power Status>` | `power_status` (`CEC_POWER_STATUS_*`) | Required of every device, in standby too |
| `cec_request_system_info(destination)` | `<Give Physical Address>`, then if acknowledged `<Get CEC Version>`, `<Give Device Vendor ID>`, and to the TV `<Get Menu Language>` | `phys_addr`, `device_type`, `cec_version`, `vendor_id`, `language` | Physical address and CEC version are required of every device; vendor ID and menu language most TVs give |
| `cec_request_osd_name(destination)` | `<Give OSD Name>` | `name` | Optional: many TVs refuse it |
| `cec_request_set_osd_name(name)` | Sets this board's name now and for later starts, then `<Set OSD Name>` to the TV | `name` (in use); `SENT` or `REFUSED` | A TV that ignores it asks for the name when it next lists its inputs |
| `cec_request_osd_string(display_control, text)` | `<Set OSD String>`, 1-13 characters (`CEC_OSD_SHOW_DEFAULT`, `_UNTIL_CLEARED`), or `CEC_OSD_CLEAR` | `SENT` or `REFUSED` | Optional: most TVs refuse it |
| `cec_request_volume(steps, destination)` | `steps` 1-20 volume up, -1..-20 down, 0 mute; each press `<User Control Pressed>` + `<User Control Released>`, 300 ms apart | `sent`: presses acknowledged | The TV's remote pass-through setting; a TV routes volume to an audio system when one runs system audio. TV-dependent: on the bench (2026-11-16) one TV changed its volume; a Vizio acknowledged every press and ignored it |
| `cec_request_key(destination, ui_command, hold_ms)` | `<User Control Pressed>`, again every 400 ms for `hold_ms` (0-10000), then `<User Control Released>` | | Remote control pass-through at the destination |
| `cec_request_active_source()` | `<Request Active Source>` to all | `source`, `phys_addr` | A device that is the active source (none answers while this board is it) |
| `cec_request_stream_path(phys_addr)` | `<Set Stream Path>` to all | `source`, `phys_addr` | The TV switches there; this board answers its own address itself, so that one is `NO_ANSWER` |
| `cec_request_menu(destination, menu_request)` | `<Menu Request>` (`CEC_MENU_*`) | `menu_state` (`CEC_MENU_OPEN` / `_CLOSED`) | Optional: TVs commonly refuse it |
| `cec_request_abort(destination)` | `<Abort>` | `ANSWERED` with `abort_reason` | Every device must refuse it with `<Feature Abort>` |
| `cec_request_feature_abort(destination, opcode, reason)` | `<Feature Abort>` naming `opcode` | `SENT` | |
| `cec_request_vendor_command(destination, data, len)` | `<Vendor Command>`, 0-14 bytes, `destination` 0-14 | `SENT` | Meaningful only to a device of that vendor |
| `cec_request_vendor_command_with_id(destination, vendor_id, data, len)` | `<Vendor Command With ID>`: the 24-bit `vendor_id` + 0-11 bytes, `destination` 0-15 | `SENT` | As above |
| `cec_request_bus_scan()` | Polls 0-14 but this board's own; asks each device that acknowledges for its physical address, vendor ID, name, power status and CEC version | `found`, `found_mask`; each device in `cec_scan_entry(address, &entry)` | Nothing; a question nobody answers costs a second |

The fields a request does not fill keep their "not answered" values:
`CEC_PHYS_ADDR_NONE`, `CEC_TYPE_NONE`, `CEC_VENDOR_NONE`,
`CEC_POWER_STATUS_NONE`, 0xFF, 15, 0 or "".

### Capture

| Call | What it does |
|---|---|
| `void cec_capture(cec_capture_mode_t mode)` | `CEC_CAPTURE_OFF` (default), `CEC_CAPTURE_OURS` (frames for this board, broadcasts, and the frames it sends), `CEC_CAPTURE_ALL` (every frame on the cable, polls included, and the frames it sends). What is waiting is dropped. |
| `bool cec_next_frame(cec_capture_frame_t *)` | The oldest frame kept: `time_us` (`time_us_32()` when `cec_task()` took it in, so within one loop pass of the frame's end), `flags` (`CEC_FRAME_ACKED`, `CEC_FRAME_FOR_US`, `CEC_FRAME_SENT`), `frame`. |

The capture keeps the newest 32 frames; older ones not taken are dropped and
counted in `capture_dropped`. It runs beside the device: in `ALL` the board
still acknowledges only its own addresses, so it watches without taking part.

### Names

`cec_key_name()`, `cec_opcode_name()`, `cec_result_name()`,
`cec_device_type_name()`, `cec_power_status_name()`,
`cec_abort_reason_name()`: single-word names ("give_osd_name", "standby",
...), "unknown" past the tables.

## A second device type

`cec_set_device_types(CEC_TYPE_PLAYBACK, CEC_TYPE_AUDIO_SYSTEM)` makes the
board claim playback 1-3 and the audio system address, 5. Keys sent to 5 come
as `CEC_EV_KEY_DOWN` with `to == 5`. The device does not implement System
Audio Control: `<Give Audio Status>`, `<System Audio Mode Request>` and the
rest are refused with `<Feature Abort>`. Whether a TV sends its volume keys to
an audio system that answers that way is not known; an app can announce
system audio itself with `cec_request_raw()` (`<Set System Audio Mode>`,
opcode `0x72`, operand 1, to all). A recorder answers `<Give Deck Status>`,
`<Play>` and `<Deck Control>` like the playback device; a tuner claims an
address and answers the mandatory messages only.

## Physical address

The physical address is the TV input the board is plugged into (`4.0.0.0`
for input 4). The TV's EDID holds it, and only the main CPU reads the EDID.
A wrong one still claims a logical address, but the TV forwards remote keys
only to the input it shows, and `cec_wake_tv()` names the wrong input in
`<Active Source>`, so the TV switches away from the board's picture.

Where the address comes from, highest rank first. A source replaces the
address in use only when it ranks the same or higher;
`cec_physical_address_source()` says which one is in use:

1. **App** (`CEC_ADDRESS_APP`): `cec_set_physical_address()`. Final.
2. **Firmware** (`CEC_ADDRESS_FIRMWARE`): an app launched by the firmware
   finds the address the firmware uses, from the TV's EDID
   (or the TV input set in the firmware's CEC settings), in its app-info
   record: watchdog scratch 0 holds `CEC_PLACE_INFO_MAGIC` (`0xCEC0A101`, low
   byte the layout version), scratch 1 the address with its complement in the
   upper half (`cec_place.h`). The first `cec_start()` reads it and clears
   scratch 0. Two launch paths write the record just before the jump:
   - the DISPLAY's own launcher (an app started from the DISPLAY's menu);
   - MAIN's loader, for an app started from the FreeWili app list: MAIN
     sends the address to the DISPLAY-side stub, which writes the record,
     and the launch is a jump with no reset. This needs FreeWili firmware
     with the matching loader; older firmware writes no record and the app
     falls back to the sources below.

   A record with no address (no TV reported yet), another version, or a
   wrong complement counts as none.
3. **Learned** (`CEC_ADDRESS_LEARNED`): below the firmware's, the board
   learns its input from the TV. When the TV broadcasts `<Routing Change>` or
   `<Set Stream Path>` to an address that is not the board's, and within 2 s
   (`CEC_PLACE_ANSWER_MS`) no device answers `<Active Source>` or `<Routing
   Information>` for that address or one below it, the TV is showing an input
   no other CEC device holds: the board takes that address, claims again and
   says `<Active Source>`. Switching the TV to the board's input with the
   TV's remote is enough. An input with a device that does not speak CEC
   looks the same, so a switch there moves the address too; the next switch
   to the board's input moves it back.
4. **Saved** (`CEC_ADDRESS_SAVED`): `/appdata/wilibsp/cec_address.txt` on
   the SD card, one line `A.B.C.D`, shared by every app and writable from a
   PC. `bsp/cec/cec_saved_onewili.h` (for apps that link `libs/onewili`)
   reads it with `cec_saved_load()`, saves a learned address with
   `cec_saved_task()` every loop pass, and saves any address with
   `cec_saved_store()`.
5. **Default** (`CEC_ADDRESS_DEFAULT`): `CEC_PHYS_ADDR_DEFAULT`, `1.0.0.0`.

`CEC_EV_ROUTING` still reports every route the TV announces.
`cec_request_system_info(CEC_LOGICAL_TV)` and the bus scan show the other
devices' addresses, which can tell which inputs are taken. `hello_cec`
shows the address and its source when the claim completes.

## Timing and lateness

CEC bits are timed from interrupts: the edge handler and the alarm handler may
each run about 160 µs after their cause before a bit leaves the standard's
limits. The engine checks its own lateness: past 200 µs it abandons a send
and sends it again (`late_aborts`), or drops a frame being received
(`rx_dropped`) instead of putting a wrong bit on the wire. `alarms`,
`late_100us`, `late_300us` and `late_max_us` count how late the alarm ran.

**Pass is `late_aborts == 0`.** Anything that holds interrupts off for more
than about 150 µs, or another interrupt at the highest priority that runs that
long, eats into this margin. RAM apps run every handler from SRAM; in a
PSRAM-executing app the handlers and the engine calls they make are placed in
SRAM (`__not_in_flash_func`). Requests and the capture run in `cec_task()`
only, never in a handler.

## Resources

| Resource | Use |
|---|---|
| GPIO31 (`PIN_CEC`) | The CEC wire, open drain: output value 0, the output enable pulls it low. Left an input with no pull by `board_init()` and after `cec_stop()`. |
| TIMER1 | One alarm, claimed at the first `cec_start()` and kept. |
| `IO_IRQ_BANK0` | A raw handler for GPIO31's edges. The vector's priority is raised to `PICO_HIGHEST_IRQ_PRIORITY` for every pin of the core. Another GPIO interrupt user must add a raw or shared handler and leave GPIO31 alone. |
| TIMER1 alarm interrupt | Exclusive handler, `PICO_HIGHEST_IRQ_PRIORITY`. |
| SRAM | About 2.8 KB of state (engine, device, 16-frame send queue, 16-event queue, the request with its bus scan, the 32-frame capture); about 19 KB of code and tables. |
| DMA, PIO, PSRAM | None. |

Both handlers stay installed after `cec_stop()` but are inert: GPIO31's edge
enables and the alarm interrupt are off.

The NFC board's `NFC_RFQ` line is the same net as the CEC wire on father board
rev 2. The stock firmware runs CEC with the NFC board mated; that has not been
tried with this BSP.

## Source layout

`cec.c` (C) holds the pin, the alarm, both handlers, the send and event queues
and the task. `cec_place.{h,c}` (pure, no SDK) reads the firmware's app-info
record, the saved text form, and learns the input from routes; host test
`tests/test_cec_place.c`. `cec_watch.{h,c}` (pure, no SDK) makes the events that come
from received frames the device does not report (`CEC_EV_ROUTING`,
`CEC_EV_KEY_REPEAT`) and takes vendor frames from the device when asked
(`CEC_EV_VENDOR`); host test
`tests/test_cec_watch.c`. `cec_control.{h,cpp}` (pure, no SDK) holds the requests to
other devices, chained exchanges with their answers and time limits, and the
capture ring. The bit engine (`fwCECWire`) and the device and message core
(`fwCECCore`) are C++ harvested unchanged from the stock firmware's CEC,
behind the C entry points in `cec_engine.{h,cpp}`. Host tests:
`tests/test_cec_*.cpp` (the firmware's message, device, controller,
receive-queue and held-key tests, and the bit engine on a simulated wire next
to a scripted sender, checked against the standard's limits with handler
delays up to 160 µs), `tests/test_cec_engine.c` (the C entry points, two
device types, vendor ID and name changes, the device events) and
`tests/test_cec_control.c` (every request, answered, refused, unanswered,
not acknowledged and timed out, the bus scan, the capture modes and overflow).

## Not yet

The TV remote's keys mapped onto the front-panel buttons; System Audio Control as an audio system (the
device refuses it); a capture with no device on the bus (the board always
claims an address while CEC runs).
