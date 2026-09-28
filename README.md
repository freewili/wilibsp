# wilibsp — FreeWili2 Board Support Package

Board-support monorepo for the **FreeWili 2** (Raspberry Pi **RP2350B**, 48
GPIO, 16 MB flash, 8 MB PSRAM): a shared `freewili2_bsp` CMake static
library, a set of apps, and a cross-platform `fw` CLI to build/flash/test
them.

Driven today: the 480x320 ST7796-class touch LCD, FT6336U touch, 16 WS2812
RGB LEDs, full-duplex I2S audio (NAU88C10), the CC1101 sub-GHz radio, the
4-mic PDM array, four I2C sensors (OPT4001 light, SHT40 humidity/temp,
BMI323 IMU, BMM350 magnetometer), IR receive/decode/encode/transmit (with a
Flipper-`.ir` parser/writer), and a polled native-USB host MSC stack
(thumb drives, no TinyUSB) with FatFs. Apps also reach the rest of the
board through `libs/onewili`: the main CPU's OneWili command API over the
display link, the SD card the main CPU owns, and **peer streams to the
ESP32-C5 (Bottlenose)**, so a display app and an ESP32 app can run as the two
halves of one program (see "Two CPUs" below). **Implemented upstream in the
default FreeWili 2 firmware** (not yet harvested into this BSP): the LoRa
(WIO-E5) bridge and NFC (ST25R3916B), plus the CM0 Linux module. Still
`TODO` in this BSP (not yet harvested): NFC, LoRa, and Pico-PIO-USB. See
[`docs/hardware/catalog.md`](./docs/hardware/catalog.md) for the full
peripheral → driver → provenance table, and `docs/drivers/` for per-driver
usage docs. Each driver ships with an `apps/hello_*` on-hardware smoke
test; the pure-logic layers (DSP, palettes, IR protocol codecs, `.ir`
parsing, sensor compensation) are host-unit-tested with no hardware or
Pico SDK needed.

**Agents:** read [`AGENTS.md`](./AGENTS.md) first — it's the dense
orientation doc (command table, hardware invariants, how to add a driver).
[`CLAUDE.md`](./CLAUDE.md) just points there.

## Quick start

Prerequisites: Pico SDK 2.3.0 + ARM GCC toolchain (`~/.pico-sdk`), CMake +
Ninja, a cmsis-dap debug probe (e.g. Raspberry Pi Debug Probe) + OpenOCD for
flashing/RTT, Python 3 for the `fw` CLI, and `pytest` for `fw test`
(`python -m pip install pytest`). Works the same on Windows
(PowerShell) and Linux.

The SDK and toolchain versions are pinned in `tools/fw.py`
(`PICO_SDK_VERSION` / `PICO_TOOLCHAIN_VERSION`) and passed to CMake explicitly,
so builds do not depend on `PICO_SDK_PATH` being exported in your shell. Each
falls back to the newest version installed under `~/.pico-sdk`.

```bash
fw build            # configure + build apps/hello_display for the RP2350B target
fw flash            # program it over the debug probe (OpenOCD); refuses an
                    # image stored in flash, which would replace the
                    # stock DISPLAY firmware
fw ramrun canblast  # load an SRAM app over the probe and start it (fw flash cannot start SRAM apps)
fw rtt              # stream live SEGGER RTT diagnostics
fw install-app app.uf2  # copy a loadable app to SD:/apps and return the card to MAIN
fw install-app app.uf2 --folder beta/radio  # install to SD:/apps/beta/radio
fw run-app beta/radio/app.uf2  # launch the installed app without navigating on-device
```

(`tools/fw` is the POSIX launcher, `tools/fw.cmd` the Windows one; both just
invoke `python tools/fw.py`. Run them from the repo root, or put `tools/` on
your `PATH`.)

### Checking your work on the board

This is an embedded BSP: most bugs that matter here are invisible to the
compiler and to the host tests. The `agentio` harness lets you — or an AI
agent — drive the board and see the panel without anyone sitting at the
hardware:

```bash
fw screenshot -o shot.png   # capture the LCD as a PNG, then look at it
fw press green              # inject a button press
fw touch 240 160            # inject a touch
fw type "hello"             # type through the chord keyboard
```

Verify changes this way rather than stopping at "it builds", and record what
you ran in `docs/superpowers/findings/`. Full surface and limitations:
[`docs/drivers/agentio.md`](./docs/drivers/agentio.md).

No hardware handy? Run the host-only unit tests instead (no Pico SDK, no
debug probe):

```bash
fw test
```

Scaffold a new app from the template:

```bash
fw new-app my_app
# then add `add_subdirectory(apps/my_app)` to the top-level CMakeLists.txt
```

For a standalone app repository, also follow
[`docs/app-project-setup.md`](./docs/app-project-setup.md): pin this repository
at `wilibsp/` and expose its complete `AGENTS.md` contract from the app root.

Published app repositories must attach their validated `.uf2` to each release
as a downloadable release artifact; see [`docs/app-storage.md`](./docs/app-storage.md).
Apps with public source repositories must also expose an on-device About screen
with the app version and repository link; holding PAGE for five seconds is the
recommended convention.

**Status:** every harvested driver group has passed its `hello_*` smoke
test on a physical board (most recently `hello_ir`'s TX→RX loopback and
`hello_usbdrive`'s thumb-drive mount, 2026-07-06), and the two-CPU demo
`apps/dualcpu` has run against its ESP32 half over peer streams
(2026-09-28). The host test tree is at 26 green binaries. `docs/hardware/facts.md` records the hard-won invariants
— shared SPI1 arbitration, shared DMA_IRQ_0 ownership, pio2 cohabitation
(radio GDO capture + IR, radio inits first), the power-gated rails on the
PCAL6524 I/O expander — and keeps claims scoped to what a bench session
actually demonstrated.

## Two CPUs: a display app and an ESP32 app

Everything this repo builds runs on the **display CPU**, the RP2350B. The
FreeWili 2 has two more processors an app can work with, and both are reached
through the **main CPU**, which runs the stock firmware and routes between
its clients:

| CPU | What runs there | How a wilibsp app reaches it |
|---|---|---|
| MAIN (RP2350) | the stock FreeWili 2 firmware: the OneWili menu, power sequencing, the SD card, every link | generated OneWili calls over the display link (`libs/onewili/wilibsp`) |
| DISPLAY (RP2350B) | **your wilibsp app**, started with `fw ramrun` or installed under `/apps/` | the Pico SDK and `bsp/` directly |
| ESP32-C5 (Bottlenose) | the firmware's Wi-Fi/Bluetooth image, optionally carrying an **ESP32 BSP app** | OneWili **peer streams** routed by MAIN, and nothing else |

**Peer streams** (`onewili_stream.h` in `libs/onewili/wilibsp`) carry
best-effort datagrams of 1-128 bytes between MAIN's clients: this display
CPU, the ESP32, the CM0 and the PC host. `ow_stream_write()` never waits for
the destination, `ow_stream_poll()` never blocks, and everything lost anywhere
along the way is counted by `ow_stream_drops()`. The ESP32 side has the same
API with the same meaning, so the two halves of an app share nothing but the
datagram format they agree on. Measured on hardware: about 6 ms display to
ESP32 and back, no loss at one ping per second, and a 768-byte credit window
that refuses a burst rather than queueing it.

```c
#include "onewili_stream.h"

ow_stream_write(&dev, OW_PEER_ESP32, msg, len);      /* OW_OK once it has left */

uint8_t buf[OW_STREAM_MTU]; ow_peer from; int n;
while ((n = ow_stream_poll(&dev, &from, buf, sizeof buf)) > 0) { /* one datagram */ }
```

What a display app must do to talk to the ESP32:

- **Keep the ESP32 powered.** It sits on power zone 5, which has no
  `POWER_ZONES` name, so request it in code before opening the link:
  `picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_WIFI_BT))`. Declare
  `RGB_LEDS` (zone 10) as well if the ESP32's LED is used.
- **Set MAIN's ESP32 Mode to OneWili API** once per launch, with
  `ow_wireless_e_sp32_mode(&dev, 1)`. In Default mode MAIN drops the
  ESP32's OneWili traffic and the stream link goes stale within 3 s.
- **Poll on every loop pass.** `ow_stream_poll()` drains the 2 KB receive
  FIFO and sends the keepalive; after 3 s of silence MAIN stops routing
  datagrams to this CPU.
- **Expect the first write to be refused** until MAIN has confirmed the link
  (one round trip), and read `ow_stream_drops()` instead of assuming delivery.

The ESP32 half is **not built by this repo**. It is a menuconfig choice
("BSP app on the ESP32") inside the FreeWili 2 ESP32 firmware project, built
with ESP-IDF against an `sdkconfig.bsp-*` overlay and flashed over the
ESP32-C5's own USB Serial/JTAG port with esptool. MAIN must run a firmware
build that carries peer streams. There is no single build target that
produces both halves yet.

`apps/dualcpu` is the worked example. Its ESP32 half sends telemetry once a
second (uptime, chip temperature, heaps, the strongest Wi-Fi access points)
and answers PINGs; the display half shows the telemetry, times the round trip,
and drives the ESP32's RGB LED and Wi-Fi scan from the front-panel buttons.
The hardware records are
[`docs/superpowers/findings/2026-09-27-dualcpu-peer-streams-e2e.md`](./docs/superpowers/findings/2026-09-27-dualcpu-peer-streams-e2e.md)
and
[`2026-09-28-dualcpu-renumbered-ids-rerun.md`](./docs/superpowers/findings/2026-09-28-dualcpu-renumbered-ids-rerun.md).

## Repo map

```
wilibsp/
  CMakeLists.txt              top-level: PICO_BOARD, pico_sdk_init, add bsp + apps
  CMakePresets.json           the "target" configure/build preset
  bsp/                        shared freewili2_bsp STATIC library
    fw2.h                     umbrella include — pull this into an app
    boards/freewili2.h        SDK board header (RP2350B, 48 GPIO, 16 MB flash)
    platform/                 clocks/vreg, pin map (board.h), I/O expander, PSRAM,
                               SPI1 bus arbitration, RTT diag
    display/                  ST7796 480x320 LCD driver + 5x7 font
    input/                    FT6336U capacitive touch driver
    leds/                     WS2812 x16 driver + led_color/led_ui helpers
    gfx/                      color palettes (host-tested)
    audio/                    NAU88C10 I2S full-duplex + capture/tone/VU helpers
    radio/                    CC1101 sub-GHz: regs, GDO capture, OOK TX, engines
    pdm/ dsp/                 4-mic PDM array + integer CIC/DC-block filters
    sensors/                  OPT4001, SHT40, BMI323, BMM350(+compensation)
    ir/                       IR capture/TX (pio2) + protocol codecs + .ir files
    usbhost/                  polled native-USB host MSC (no TinyUSB) + FatFs glue
    third_party/segger_rtt/   SEGGER RTT (vendored)
    third_party/fatfs/        FatFs R0.15b (vendored)
  apps/
    template/                 starter app — `fw new-app` copies this
    hello_display/            display + touch + LEDs smoke test
    hello_audio/ hello_cc1101/ hello_mics/ hello_sensors/
    hello_ir/                 NEC TX->RX loopback + live decode
    hello_usbdrive/           thumb-drive mount + root listing
    hello_sdcard/             SD card read/write over OneWili (main CPU owns the card)
    canblast/                 CAN FD blaster: high-rate TX/RX through the OneWili display link
    dualcpu/                  DISPLAY half of a DISPLAY + ESP32 app over OneWili peer streams
  libs/
    onewili/                  git submodule: the OneWili API (main-CPU commands, SD card,
                               peer streams); the display-CPU C package is libs/onewili/wilibsp
  tools/                      fw CLI (fw.py) + POSIX/Windows launchers + its own pytest
  tests/                      standalone host CTest tree (no Pico SDK, no hardware)
  docs/
    hardware/                 pinmap.md, facts.md, catalog.md
    drivers/                  per-driver usage docs (platform ... ir, usbhost, lora)
    superpowers/plans/        the full implementation plan / spec
    superpowers/findings/     what was actually run on the board, and what came back
  skills/
    freewili2-new-app/        Claude Code skill: scaffold a new app
    freewili2-add-driver/     Claude Code skill: harvest a new driver
  AGENTS.md                   dense agent orientation (read this first)
  CLAUDE.md                   thin pointer to AGENTS.md
  FwDisplayVibe.md            original hardware description (secondary source —
                               known to have at least one error; see facts.md)
```

## Further reading

- [`AGENTS.md`](./AGENTS.md) — command vocabulary, hardware invariants, how
  to add a driver, naming conventions.
- [`docs/hardware/pinmap.md`](./docs/hardware/pinmap.md) — full pin table.
- [`docs/hardware/facts.md`](./docs/hardware/facts.md) — hard-won invariants
  and the LED-count discrepancy record.
- [`docs/hardware/catalog.md`](./docs/hardware/catalog.md) — peripheral →
  driver status → harvest source (incl. the "Implemented upstream" table).
- [`libs/onewili/wilibsp/README.md`](./libs/onewili/wilibsp/README.md) —
  OneWili from a display app: main-CPU commands, the SD card, fast CAN
  transmit, and peer streams to the ESP32.
- [`docs/drivers/lora.md`](./docs/drivers/lora.md) — WIO-E5 LoRa bridge:
  implemented in the default firmware, documented for the future harvest.
- [`docs/app-storage.md`](./docs/app-storage.md) — `/apps/` installation and
  the recommended `/appdata/<app-name>/` convention for app-owned data.
- [`docs/superpowers/plans/2026-07-01-freewili2-bsp.md`](./docs/superpowers/plans/2026-07-01-freewili2-bsp.md)
  — the full implementation plan this repo was built from.

## License

MIT — see [LICENSE](LICENSE). Vendored third-party components (SEGGER RTT,
FatFs) keep their own permissive licenses, listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and in the vendored file
headers. Harvested drivers carry the MIT/BSD-3-Clause
terms of their source repos where noted.
