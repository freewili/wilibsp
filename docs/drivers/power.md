# Coprocessor power-zone protocol

The board's power coprocessor sequences every switched rail. Rails
outside its boot subset stay off until explicitly requested — notably the
audio codec rail (zone 3) and the CAN rail (zone 15). The display CPU
requests rails over the same UART link that carries the coprocessor's
status/button frames (UART1, 62500 baud, 8N1, both directions). The
display CPU is the only device that drives the coprocessor's receive
line; there is no alternate control path at the UART level. (The default
firmware adds a FwGUI-level demand channel on top — see "Automatic zone
management" below.)

BSP support: `bsp/input/uartkbd.*` owns the link (RX parsing and command
TX); `bsp/input/picpwr_frame.*` is the pure frame/mask logic
(host-tested); `bsp/input/picpwr.*` is the application-facing API.

## Command frame

One shape in both directions:

```
SYNC | ID | payload[N] | CHK
```

- `SYNC` — one byte, `0xB0` for commands and replies.
- `ID` — one byte; **payload length is implied by the id**. There is no
  length field. Emitting the wrong payload size lands the checksum in the
  wrong position and the frame is discarded.
- `CHK` — 8-bit additive sum (wraparound) of every preceding byte,
  including `SYNC` and `ID`. Not a CRC. No escaping — payload bytes may
  legitimately equal a sync value.

The unsolicited status frame differs: sync `0xBD`, id `0x1D`, 20-byte
payload, same additive checksum.

## Per-frame handshake (mandatory)

The coprocessor's receiver is command-gated; skipping this sequence means
the bytes are silently ignored:

1. Assert a UART break for ~2 ms, then release it.
2. Wait for a bare (unframed) activity byte `0xC9`, with a ~500 ms
   fallback timeout — proceed anyway if it never arrives.
3. Settle ~5 ms, transmit the frame, then allow ~50 ms before treating
   the link as free.

`0xC9` may arrive mid-frame from the RX parser's perspective. It must be
intercepted ahead of the parser and any partially collected frame
dropped (implemented inside `uartkbd_cmd_send()`).

## Power-zone command (`ID 0x00`, payload 8 bytes)

| Offset | Size | Field |
|---|---|---|
| 0–2 | 3 | awake mask — rails enabled now |
| 3–5 | 3 | sleep mask — rails kept through sleep |
| 6 | 1 | wake-source field |
| 7 | 1 | wake-source overflow field |

This is a single **atomic write of all four fields** — no partial form,
no read-modify-write on the device, and the sleep/wake fields cannot be
read back. Consequences:

- Always send a **complete awake mask**: any zone bit left clear switches
  that rail off (including the display rail).
- Keep a cached copy of all four fields and send the full set every time.
- Seed the awake mask's rail bits from **live status-frame state** (see
  below), never from a local cache alone — other actors move rails
  without notice.

Worked example — zones 1–17 on, sleep/wake zeroed:

```
B0 00 01 FF FF 00 00 00 00 00 AF
```

(Checksum: the byte sum is 0x2AF; truncated to 8 bits = 0xAF.) Note the
reserved bits above zone 17 are zero — that is the required form, not a
simplification. Suitable as a smoke test only because it zeroes the sleep
configuration.

## Zone mask encoding

24-bit field, most-significant byte first. Zone *N* is bit *N−1*, so
zone 1 is bit 0 of the **last** byte and zone 17 is bit 0 of the first.

- Zones 1–17 are switched rails (zone 9 excepted — a flag, see the map).
- Bits above zone 17 are **RESERVED — MUST BE ZERO.** They reach board
  control lines rather than switched rails, are not safely
  software-controllable on current hardware revisions, and zero matches
  the boot state. They are deliberately **not** exposed by the `picpwr`
  API — it clamps every outgoing mask to zones 1–17. If software control
  of one is ever needed it must be a separate, explicitly documented,
  opt-in call gated on a board revision that supports it.

## Zone map

| Zone | Powers | On at boot? | Application notes |
|---|---|---|---|
| 1 | Motion/magnetic/humidity/light sensors + an I/O expander | yes | The expander serves more than the sensors — switching off has a wide blast radius |
| 2 | LCD panel + touch controller | yes | Off = blank screen, no touch |
| 3 | Audio codec | no | — |
| 4 | Sub-GHz radio + LoRa module (shared rail) | no | Both radios are usable: the CC1101 via this interface, and the WIO-E5 LoRa bridge over the DISPLAY's own PIO UART on the same rail (standby-booted, revived after rail cycles — see docs/drivers/lora.md). The "held in reset, not usable" note was stale as of the default firmware's LoRa support |
| 5 | Wi-Fi / Bluetooth module | no | Auto-managed in the default firmware on MAIN's declared FwGUI 0x7F demands (persisted wifi/BLE enables, the ESP32 flasher, and each ad-hoc scan's bounded demand window); kept powered by the fail-safe-by-absence rule until MAIN's first declaration |
| 6 | FPGA + its external memory | no | — |
| 7 | microSD card + bridge | yes | Unmount before switching off — corruption risk |
| 8 | USB hub | with USB cable | Managed automatically by the device on cable attach/detach — do not fight it |
| 9 | *Flag, not a rail*: status-LED enable | set | When set, the device's status LED shows heartbeat/activity and the application CPUs mirror it. Clear for LED-quiet operation. Safe either way; no rail moves |
| 10 | Addressable RGB LEDs | no | — |
| 11 | Analog subsystem (DAC, ADC, op-amps) | no | — |
| 12 | No confirmed load | no | Leave in its default state; exposed as a **manual setting** in the default firmware — never inferred or auto-released. Verified not required for the LCD (apps draw with it off) or the video output (signal, sink detection and hotplug all unaffected with it off) |
| 13 | NFC + low-frequency RFID | no | Driven by the default firmware's FwGUI NFC RPC (0x6F–0x71) — see the "Implemented upstream" notes in docs/hardware/catalog.md |
| 14 | USB-serial bridge | with USB cable | Managed automatically on cable attach/detach |
| 15 | CAN controller + transceiver | no | — |
| 16 | On-board debug probe | with USB cable | Off mid-session = you lose your debugger. Managed automatically on cable attach/detach |
| 17 | Compute module + its microSD | no | **Not usable through this interface**: its run/enable line is a reserved control bit (held clear). No safe-shutdown path exists — never cut power to a running module |

Notes on the map:

- **The boot-on set is a property of the device firmware and may change
  between versions. Do not rely on it** — request the zones your
  application needs explicitly, every time.
- **Omitting a zone from the awake mask switches it off.** The pattern
  that prevents blanking your own display on the first attempt: read
  live rail state, OR in the zones you need, send the complete mask
  (`picpwr_keep_awake()` / `picpwr_ensure_awake()` do this for you).
- The display CPU's own rail is not in the mask at all; it cannot be
  reached — and therefore cannot be switched off by accident.
- Nothing in 1–17 is electrically unsafe to switch off. The real risks
  are data loss (zones 7 and 17) and losing the interface you are
  working through (zones 2, 8, 16).
- Whether a zone's peripheral is usable from a display-CPU application
  is a bus-topology question this document does not settle. The
  checkable rule: if this BSP ships a driver for the peripheral, the
  zone is usable from here; if it does not, treat it as not supported
  yet rather than as a hardware limitation.

## Reading rail state back

Rail state is confirmed from the **unsolicited status frame** (the same
one carrying buttons/charger data), not from any cache. In its 20-byte
payload, high = powered:

| Zone | Byte | Mask | | Zone | Byte | Mask |
|---|---|---|---|---|---|---|
| 1 | 3 | 0x08 | | 10 | 5 | 0x08 |
| 2 | 3 | 0x10 | | 11 | 5 | 0x10 |
| 3 | 3 | 0x20 | | 12 | 5 | 0x20 |
| 4 | 3 | 0x40 | | 13 | 6 | 0x01 |
| 5 | 3 | 0x80 | | 14 | 6 | 0x02 |
| 6 | 4 | 0x01 | | 15 | 6 | 0x04 |
| 7 | 4 | 0x02 | | 16 | 6 | 0x10 |
| 8 | 5 | 0x02 | | 17 | 7 | 0x08 |
| 9 | 5 | 0x04 | | | | |

Byte indices are into the 20-byte payload (frame bytes 2–21). The status
frame also carries indications for the reserved control lines (byte 6
`0x08`, byte 7 `0x01`, byte 5 `0x80`). **Exclude them — and everything
above zone 17 — from any requested-vs-actual comparison**: one of those
pins is set high by the coprocessor's own boot code but never written by
the rail walk, so a command correctly sending 0 reads back 1 forever. A
comparison that includes it mismatches permanently and, if wired to a
debounced re-assert, thrashes the link with rail walks. Compare zones
1–17 only.

## Timing and serialization

Applying a mask runs a **staggered rail walk with roughly one second of
inrush delay per pass, during which the coprocessor does not parse the
link**. Rules:

- Never send frames back-to-back; allow well over one second between
  commands (`picpwr_send()` enforces 1.5 s) and never stack retries.
- A mask is applied unconditionally even when nothing changed — every
  send costs a walk.
- Do not judge success until the walk completes; then confirm via the
  status frame.

## Autonomous rail changes

The coprocessor changes rails on its own; caches go stale:

- USB detach/attach clears/restores the USB-bridge and debug-probe rails.
- Sleep applies the sleep mask; wake re-applies the coprocessor's cached
  awake mask.
- Low-battery, long-button-hold and ship-mode shutdowns drop everything;
  a coprocessor watchdog reset returns to the boot subset.

There is no keepalive and no idle timeout. Applications that depend on a
rail should watch the status frame and re-assert (debounced — never
during a walk) if their rail reads off. `picpwr_keep_awake()` +
`picpwr_task()` implement exactly this.

## Automatic zone management (default firmware)

The FreeWili 2 default firmware's display image runs a **zone manager on the DISPLAY CPU** on top of the raw protocol
above: the zone-manager family (all host-tested). A standalone BSP app flashing its own
DISPLAY firmware owns its own power policy and keeps using `picpwr_*`; a BSP
app running **against the stock firmware** should expect the manager to be
moving rails underneath it. Constants named here are zone-manager
values in the default firmware — code facts, not hardware-verified in this
BSP, and they may change. Key facts:

- **Batched emission.** Any change is deferred `RP_ZONE_SETTLE_MS` (750 ms)
  and two PZCONFIG frames are never closer than `RP_ZONE_FRAME_FLOOR_MS`
  (3000 ms) — one frame per settle window, never per command. A zone released
  and re-wanted inside one window matches live state again before the
  deadline and emits **nothing**.
- **Declared acquire, inferred release.** Commands declare an
  `iRequiredZoneMask`; the manager acquires what a command needs (with an
  `RP_ZONE_ACQUIRE_FLOOR_MS` 1000 ms floor) and releases what nothing needs,
  via pure per-zone predicates over a snapshot. Requests carry a TTL
  (`RP_ZONE_REQUEST_TTL_MS` 5000 ms); commanded-but-unconfirmed zones are
  graced (`RP_ZONE_GRACE_MS` 3000 ms).
- **Managed zones: 1, 3, 4, 5, 8, 10, 11, 13, 14, 15, 16.** Zones 2, 6, 7,
  9, 12 and 17 are never moved by the manager (owner rulings: display sleep
  owns 2, the FPGA rail owns 6, the SD card owns 7, board-LED flag 9 is not
  a rail, 12 is manual-only, the CM0 17 needs explicit shutdown).
- **Zones 8, 14 and 16 are VBUS-gated** from the coprocessor's `0xBD`
  status frame — the one input that depends on no switched rail. The
  manager drives them off on detach and re-asserts on attach.
- **Escape hatch.** A setting reverts to the old refuse-with-`EPOWERZONE`
  behaviour; bench work and bisection rely on it.
- **Fail-safe by absence.** MAIN's declared demands are treated as all-set
  until the first declaration arrives and again whenever the newest is older
  than `RP_ZONE_MAIN_DEMAND_TTL_MS` (5000 ms) — a rebooting or hung MAIN
  keeps its rails powered.

### What raises each managed zone

The manager never asks "is this hardware in use?". It evaluates a fixed
predicate per zone over a snapshot, and three different sources feed that
snapshot. Which source applies decides how an app keeps a rail up.

**Observed on the DISPLAY CPU.** Use the hardware through its normal driver and
the rail follows:

| Zone | Raised while |
|---|---|
| 1 | a sensor stream is enabled, the VREF mux is connected, or anything is holding zone 4 — the DISPLAY I/O expander on this rail is the only writer of the sub-GHz mux bits, so a zone-4 holder must hold zone 1 too |
| 3 | audio playback is active |
| 4 | the sub-GHz arbiter has granted the CC1101, the LoRa RX-control shadow is set, or a LoRa transmit is in flight |
| 10 | any LED in the pixel buffer is lit |
| 13 | the NFC reader shadow is set — the reader's own enable/disable, not chip presence |

**Declared by MAIN over `0x7F`.** Re-sampled from MAIN-side state and sent on
change or at least once a second. The demand is that specific state, *not*
whether the peripheral is being touched:

| Zone | Bit | Raised while |
|---|---|---|
| 5 | `0x01` wifi, `0x02` BLE | station or AP enabled, BLE enabled, the ESP32 flasher running, or a wifi/BLE scan's bounded window open |
| 11 | `0x08` analog | a DAC wave is running or a DC level is commanded, either ADC stream rate is non-zero, the logic player or analyzer owns the ADC, or the Analog panel is selected |
| 15 | `0x10` CAN | the CAN stream or API is enabled, a periodic TX slot is armed, the CAN log is open, or the Neptune panel is selected |

**VBUS-gated.** Zones 8, 14 and 16, from the coprocessor's `0xBD` status frame.
Not app-controllable.

### Rules for an app running against the stock firmware

- **Express demand; do not drive a managed rail.** Switching a managed zone on
  from outside the manager — `0x6C` or `0x6D` — holds only until the next
  evaluation, which sees nothing demanding that zone and switches it back off.
  The app gets a rail cycle instead of a powered rail. Where the rail's devices
  share a bus with others this is felt well beyond the app: zone 1 carries the
  DISPLAY I/O expander alongside the touch controller and the sensors, so
  cycling it disturbs the whole `i2c1` bus.
- **A managed zone nothing demands is driven off, not left alone.** It is
  switched off the first time the manager sees a live status frame, so a rail
  that happened to be on at boot will not stay on for an app that never
  declares a need for it.
- **Bypassing MAIN's state bypasses the demand.** Driving zone 5, 11 or 15
  hardware without going through the state in the table above means the rail
  reads as unwanted and is dropped mid-operation, typically within a couple of
  heartbeats.
- **Zones 2, 6, 7, 9, 12 and 17 are the app's.** The manager never moves them;
  nothing raises them for the app either.
- If an app's hardware genuinely cannot be expressed as demand, the escape
  hatch above reverts the device to manual zone control.

### The FwGUI power channel (host → DISPLAY, i.e. MAIN → DISPLAY)

The raw coprocessor link is still single-writer (only the DISPLAY drives the
coprocessor UART), but the default firmware adds a higher-level channel over
the FwGUI link that BSP apps can use to reach the zone manager. All four
commands below are `host → display` (MAIN → DISPLAY); the display never
initiates any of them:

- `0x6B` power telemetry (2 bytes, `rateMs` u16le) — start/stop
  the DISPLAY power sampler; replies with event 47 (power data).
- `0x6C` power-zone set (2 bytes: `zone` u8 1-based, `on` u8) —
  ask the DISPLAY to switch one zone; the handler seeds untouched zones from
  live state so it cannot clobber zones it does not name. The result is
  observed in the next status frame / event 48 (power zones); a
  queue-full drop is silent.
- `0x6D` set power zones (4 bytes, `zoneMask` u32le, bits 19:0) —
  whole 20-bit awake mask forwarded to the coprocessor as one PZCONFIG (the
  `picpwr` power-zone frame). Used
  by MAIN's Power Management menu; the Linux-CPU toggle rides this (S17 +
  CM0_RUNPG in one frame so the ascending walk powers the rail first).
- `0x7F` zone demands (1 byte) — MAIN's declared power-zone
  demands: `0x01` wifi (zone 5), `0x02` BLE (zone 5), `0x04` websocket
  (zone 5, reserved — always 0), `0x08` analog (zone 11), `0x10` CAN
  (zone 15). Sent on change and on a heartbeat of at most 1 s; zone 5's bits
  come from the persisted wifi/BLE enables, the ESP32 flasher, and each
  ad-hoc scan's bounded demand window — a scan against a dark ESP32 raises
  the rail itself and is held pending until the ESP32 answers.

## Releasing rails — an app inherits more than the boot-on set

A standalone app does **not** start from the "On at boot?" column above. It
starts from whatever the firmware before it left switched on: `fw ramrun`
halts the stock DISPLAY firmware 3.5 s into its boot, and the loader hands
over in whatever state it was in. Which rails that is varies from launch to
launch — the same app has come up with the audio rail on (`rails=E3C7`) and
off (`rails=E3C3`). And once the app is running nothing will ever switch an
inherited rail off: the zone manager that releases idle rails is part of the
firmware the app replaced, and `picpwr_ensure_awake()` / `picpwr_keep_awake()`
are strictly additive by design.

The audio rail (zone 3) is the one that matters. Inherited on, the codec is
exactly as the stock firmware left it mid-playback — power and output
drivers on (R1=0x15D, R3=0xED), speaker amp and 5 V boost enabled, DAC
**unmuted** (R0A=0x000) — with no I2S clocks, for the life of the app.

```c
picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_CAN));  // what this app uses
picpwr_release_unused();                               // drop what it does not
...
for (;;) { fw2_app_recovery_task(); picpwr_task(); ... }
```

- `picpwr_release_unused()` queues the release of every **app-owned** rail
  that has not been requested with `picpwr_keep_awake()`:
  `PICPWR_ZONE_MASK_APP_OWNED` = audio (3), sub-GHz/LoRa (4), RGB LEDs (10),
  NFC/RFID (13) — the rails whose only user is the display CPU, i.e. the app
  itself. Rails that serve the main CPU (Wi-Fi/BT 5, FPGA 6, analog 11,
  CAN 15) are never released on inference: an app cannot see MAIN's needs.
- `picpwr_release(zone_bits)` releases specific rails and drops them from the
  keep-awake set (e.g. after the app is done with the codec — mute it first
  with `codec_nau88c10_speaker_low_power()`).
- Both are non-blocking. `picpwr_task()` sends the frame once two consecutive
  status frames agree on the rail state and the send rate limit allows it
  (2–3 s after the call), logging `picpwr: released rails <mask>`.
- A released rail is remembered: the additive helpers rebuild their masks
  from live rail state, where a just-released rail still reads as powered, and
  would otherwise switch it straight back on. Asking for the rail again with
  `picpwr_keep_awake()` clears that.

The CPU is not where the heat goes: on the bench, sleeping the 250 MHz core
97% of the time lowered the die by ~1 C. `board_die_temp_c()`
(`bsp/platform/power.h`) reads the die sensor for before/after comparisons;
canblast's `TEMP`, `KEEP z`, `DROP z` and `CODEC` RTT commands are the
bench for this.

## Usage guidance

- Request rails **before** initializing the peripherals that sit on
  them: a rail transition can disturb devices already operating on a
  shared bus, and a peripheral initialized before its rail is stable
  reads back garbage.
- After a rail walk completes, re-initialize shared-bus state (e.g. run
  I2C bus recovery) before first use if devices on that bus were live
  during the walk.
- Status frames may be absent for several seconds after the display CPU
  resets; tolerate that rather than treating it as a dead link.
- Never seed an awake mask from a single status frame: use two agreeing
  frames (as `picpwr_ensure_awake()` does) or send a superset. A stale
  snapshot echoed back switches off every rail it failed to report.

## Out-of-band bytes

Bare, unframed single bytes appear between and even inside frames; the
parser tolerates them anywhere:

| Byte | Meaning |
|---|---|
| `0xC9` | RX active (handshake gate) |
| `0xE5` | frame parsed, checksum OK |
| `0xE4` | checksum mismatch |
| `0xE2` | unrecognized command id |
| `0xE1` | first byte was not the command sync |

`0xE5` acknowledges the parse only — rails have not necessarily finished
moving. There is no dedicated reply to the power-zone command; confirm
via the status frame.
