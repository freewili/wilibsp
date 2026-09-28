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
(thumb drives, no TinyUSB) with FatFs. **Implemented upstream in the
default FreeWili 2 firmware** (not yet harvested into this BSP): the LoRa
(WIO-E5) bridge and NFC (ST25R3916B), plus the MAIN-side ESP32-C5
(Bottlenose) link and the CM0 Linux module. Still `TODO` in this BSP (not yet harvested): NFC, LoRa, and Pico-PIO-USB. See
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
`hello_usbdrive`'s thumb-drive mount, 2026-07-06). The host test tree is at
26 green binaries. `docs/hardware/facts.md` records the hard-won invariants
— shared SPI1 arbitration, shared DMA_IRQ_0 ownership, pio2 cohabitation
(radio GDO capture + IR, radio inits first), the power-gated rails on the
PCAL6524 I/O expander — and keeps claims scoped to what a bench session
actually demonstrated.

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
  tools/                      fw CLI (fw.py) + POSIX/Windows launchers + its own pytest
  tests/                      standalone host CTest tree (no Pico SDK, no hardware)
  docs/
    hardware/                 pinmap.md, facts.md, catalog.md
    drivers/                  per-driver usage docs (platform ... ir, usbhost, lora)
    superpowers/plans/        the full implementation plan / spec
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
