# AGENTS.md — guide for AI agents and contributors

> **Reading requirement:** read this file through EOF before changing code.
> It is intentionally long. If your tool truncates it, continue from the last
> line in additional chunks until EOF. Do not treat the first chunk as the
> complete contract. External app repositories must link here from their own
> root `AGENTS.md`; see `docs/app-project-setup.md`.

This file orients coding agents (Claude Code, Cursor, Copilot, etc.) and new
human contributors working in `wilibsp`. It is intentionally dense: the goal
is that you do **not** have to rediscover the hard-won facts this project was
built on. Read it before making changes.

## What this is

`wilibsp` is a board-support **monorepo** for the **FreeWili 2** (Raspberry Pi
**RP2350B**, 48 GPIO, 16 MB flash, 8 MB PSRAM). Everything it builds runs on
the **display processor**. (This means you must use OpenOCD interface 0 —
FreeWili 2 exposes multiple debug interfaces.) A display app can now pair
with an app on the **ESP32-C5** through OneWili peer streams, but that half is
built by the ESP32 firmware project, not here — see "The ESP32-C5 and peer
streams" below. It provides:

- `bsp/` — the shared `freewili2_bsp` CMake **STATIC library**: platform
  bring-up, display, touch, and LED drivers, harvested and normalized from the
  owner's proven repos (primarily `subghz`).
- `apps/` — individual CMake executables that link `freewili2_bsp`
  (`template` — starter scaffold; `hello_display` — v1 on-hardware smoke
  test: display renders, touch responds, LEDs light).
- `libs/` — optional static libraries apps can link in addition to the BSP.
  Today: `libs/onewili` — the generated OneWili C command API for driving the
  **main CPU** (GPIO, LEDs, radio, …) over the FwGUI display link (UART0,
  8 Mbaud), plus `ow_sd_*` for reading and writing the **SD card** the main
  CPU owns (SDFS over the same link), and `ow_stream_*` **peer streams**
  to the ESP32-C5, the CM0 and the PC host, routed by the main CPU. The
  submodule ships every OneWili language package; the display-CPU C package
  is `libs/onewili/wilibsp` (see its `README.md`);
  `apps/toggleled`, `apps/hello_sdcard` and `apps/dualcpu` are the worked
  examples.
- `tools/fw.py` (+ `tools/fw` / `tools/fw.cmd` launchers) — a cross-platform
  CLI that drives CMake + OpenOCD identically on Windows and Linux.
- `tests/` — a standalone host CTest tree for pure logic (no Pico SDK, no
  hardware).

The umbrella header is `bsp/fw2.h` — include this from an app to pull in the
board + drivers.

**Status:** the v1 smoke test and every driver increment since have passed on
real hardware — display, touch, LEDs, platform, I2S audio, PDM mics, CC1101
radio, I2C sensors, DVI, and the agentio harness. The per-increment records
live in `docs/superpowers/findings/`, summarized in
`docs/hardware/facts.md` ("Hardware verification status") and tracked per
peripheral in `docs/hardware/catalog.md`. Anything still marked TODO in the
catalog (NFC, PIO-USB) is unverified because its driver has not been
harvested yet.

**Do not assume a doc's description of behavior is a confirmed result.** Where
this repo describes what something does, check whether a findings file backs
it. If none does, it is design intent — say so rather than repeating it as
fact.

## Command vocabulary

All commands run from the repo root and are identical on Windows and Linux
(the CLI is Python; `tools/fw` is the POSIX launcher, `tools/fw.cmd` the
Windows one — both just call `python tools/fw.py "$@"`).

| Command             | What it does                                                                                                                            |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `fw configure`      | Configure `build/` against the pinned Pico SDK + toolchain (`--clean` wipes it first). Rarely needed directly — `fw build` calls it.    |
| `fw build [app]`    | Configure + build `apps/<app>` for the RP2350B target via `cmake --build --preset target --target <app>` (default app: `hello_display`) |
| `fw flash [app]`    | Program `build/apps/<app>/<app>.elf` over the cmsis-dap debug probe via OpenOCD (`tools/openocd/freewili2.cfg`). **Refuses an ELF whose loadable segments sit in QSPI flash** — that would replace the stock DISPLAY firmware. Prefer `fw install-app`; override with `--replace-display-firmware` |
| `fw ramrun [app]`   | Load a `no_flash` (SRAM) app over the probe and start it (`tools/openocd/ramrun.tcl`); `fw flash` cannot start SRAM apps (no flash bank at 0x20000000, and its reset boots the stock firmware) |
| `fw rtt`            | Attach to the target and stream SEGGER RTT diagnostics (OpenOCD RTT server on port 9090)                                                |
| `fw test`           | Configure + build + run the standalone host CTest tree in `tests/` (MinGW GCC + Ninja on Windows; no Pico SDK, no hardware)             |
| `fw new-app <name>` | Scaffold `apps/<name>` by copying `apps/template` and rewriting the CMake target name                                                   |
| `fw install-app <uf2> [<uf2> ...]` | Find MAIN with fwFinder, hand its SD reader to the PC, copy and flush one or more UF2s to `/apps/` (or `--folder path` beneath it), wait for writes to settle, and return the SD to MAIN without a Windows eject/unmount |
| `fw screenshot`     | Capture the screen to a PNG (`--surface lcd|dvi`, `--crop x,y,w,h`, `--scale N`) via the agentio RTT channel (verified on hardware 2026-07-26) |
| `fw press <btn>`    | Inject a button press+release (`fw hold` / `fw release` for a sustained hold) |
| `fw touch <x> <y>`  | Inject a touch tap (`--down` / `--up` for a sustained touch)              |
| `fw type "text"`    | Type text through the fw2kb chord engine                                  |

Add `--print` to `build`/`flash`/`rtt`/`test` to print the underlying
command(s) instead of running them (useful for an agent to inspect what would
run without touching hardware).

Apps must put app-owned persistent data—preferences, saves, logs, generated
maps, caches, and similar files—under `/appdata/<app-name>/`, creating that
directory before the first write. Do not place app-owned files at the SD root
or directly in `/appdata/`. User-selected exports and deliberately shared or
interoperable files may live elsewhere when the UI or documentation makes
that intent clear. See `docs/app-storage.md`.

`/apps/` is a non-destructive DISPLAY launch surface. App UF2s may target only
SRAM (`0x20000000..0x20070000`) or PSRAM (`0x11000000..0x11800000`), never
QSPI flash (`0x10000000..0x11000000`). `fw install-app` checks every block and
fails before mounting the SD if the file is malformed, mixed-target, or touches
flash. The DISPLAY recovery loader is fused in OTP; a write at flash base
replaces the stock DISPLAY firmware, not the loader.

`fw flash` applies the same rule to the ELF it is about to program, refusing by
default when any loadable segment lands in `0x10000000..0x11000000`. The trap it
exists to catch is `pico_set_binary_type(copy_to_ram)`: that image RUNS from
SRAM but is STORED in flash, so a debugger writes it at flash base and silently
replaces the DISPLAY firmware. Build display apps with `fw2_display_app()` — it
sets `no_flash` — and install them with `fw install-app`. `fw flash` on such an
app writes SRAM only and stays non-destructive, which is why it remains the
normal edit/flash/`fw rtt` loop. Pass `--replace-display-firmware` only when
replacing the firmware is the actual intent.

The UF2 is a required distribution artifact of the app contract. Every
published app release must attach its validated `.uf2` as a downloadable
release artifact in the app's repository; a source tag or ephemeral CI
artifact by itself is insufficient.

If an app's source repository is public, the app contract also requires an
on-device About screen showing the app version and repository link. Holding
PAGE for five seconds is the conventional unobtrusive way to reveal it, though
another discoverable gesture or menu entry is acceptable.

After `fw new-app <name>` you must add
`add_subdirectory(apps/<name>)` to the top-level `CMakeLists.txt` yourself —
the CLI only scaffolds the directory, it does not edit the top-level CMake.

**`libs/onewili` is a git submodule.** A fresh clone or a new git worktree
starts with that directory empty, and the configure then fails with
"does not contain a CMakeLists.txt file". Run
`git submodule update --init libs/onewili` once per checkout.

**You do not need to export `PICO_SDK_PATH`.** The SDK and toolchain versions
are pinned in `tools/fw.py` (`PICO_SDK_VERSION = "2.3.0"`,
`PICO_TOOLCHAIN_VERSION = "14_2_Rel1"`) and passed to CMake explicitly, each
falling back to the newest version installed under `~/.pico-sdk`. `fw build`
configures `build/` when it is missing, and wipes + reconfigures it when it was
configured against a different SDK — so `rm -rf build` is safe and no longer
strands the tree on whatever SDK happens to be in the shell environment.
Bump the version by editing those constants, not by exporting anything.

## Invariants — do NOT relearn these the hard way

Treat these as facts; each cost real debugging time in the source repos this
BSP was harvested from. They are also recorded in `docs/hardware/facts.md`.

1. **RP2350B, not RP2350A.** `bsp/boards/freewili2.h` sets `PICO_RP2350A 0`
   (48 GPIO). The board is selected via `set(PICO_BOARD freewili2)` in the
   top-level `CMakeLists.txt` — **NEVER** pass `-DPICO_BOARD` on the cmake
   command line; it overrides the cached value and reverts to the wrong
   config.
2. **Clock/RAM invariant.** `board_init()` (`bsp/platform/board.c`) does:
   `vreg_set_voltage(VREG_VOLTAGE_1_25)` → `sleep_ms(10)` →
   `set_sys_clock_khz(250000, true)` → **re-source `clk_peri` from
   `clk_sys`** via
   `clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, f, f)`
   → **re-time PSRAM** (`psram_configure_params()` + `psram_reinitialize()`;
   both are required — the first only stores values, the second writes the QMI
   register). Without the `clk_peri` re-source the SPI peripheral has no clock
   and the LCD is dead. Every app binary is
   `fw2_display_app()` selects the SDK's `no_flash` binary type: UF2 payloads,
   code, initialized data, and ordinary bss live in 512 KB SRAM, so watch the RAM budget —
   large buffers (framebuffers, capture clips) can be explicitly placed in
   PSRAM (`PSRAM_BASE 0x11000000`, APS6404L, 8 MB, brought up by the
   SDK's `hardware_psram` at boot from `bsp/boards/freewili2.h`). Allocate them
   with `__uninitialized_psram("group")`, **never** by casting `PSRAM_BASE` —
   the linker's PSRAM region starts at that same address, so a raw pointer
   aliases whatever the linker placed there. Note `arm-none-eabi-size` folds
   PSRAM into `bss`; use `size -A` to read the real SRAM figure.
   **Caveat for RAM apps (`no_flash`):** `__uninitialized_psram` emits a
   `.psram_noload` section, and ld gives it a load address by continuing from
   the previous section's — which in a `no_flash` build is SRAM, because the
   SDK aliases `PSRAM_STORE` to `RAM`. The resulting phantom load range runs
   off the end of SRAM and **picotool refuses to emit the UF2**
   (`Memory segment ... is outside of valid address range`), even though the
   segment carries zero file bytes. A flash build hides this by putting the
   phantom range in flash. Until picotool stops range-checking zero-length
   segments, a `no_flash` app cannot use `__uninitialized_psram` at all; either
   reserve a fixed window (see `bsp/agentio/agentio.c`, which does this with a
   bounds check against `__psram_end__`) or strip `.psram_noload` from the ELF
   before the UF2 step.
   250 MHz is the DEFAULT (audio-optimal: NAU88C10 MCLK = clk_sys/61 = 4.0984 MHz
   ~ 16 kHz fs). An app may bring the board up at another even-MHz clock via
   board_init_clk(khz) — the DVI demo uses 252 MHz for an exact 25.2 MHz pixel
   clock, which shifts audio pitch ~0.8% (only relevant if that app also plays
   audio). board_init() == board_init_clk(250000). See docs/drivers/dvi.md.
3. **Diagnostics = SEGGER RTT only.** `DIAG(...)` (`bsp/platform/diag.h`) →
   `SEGGER_RTT_printf(0, ...)` on channel 0. There is no UART/USB stdio.
   `SEGGER_RTT_printf` supports `%d %u %x %s %c` and field widths — **no
   floats**. View with `fw rtt`.
4. **DMA_IRQ_0 is shared.** The ST7796 async flush registers with
   `irq_add_shared_handler(DMA_IRQ_0, ...)` and acts only on its own DMA
   channel's status. Any new DMA user on this line must do the same — never
   `irq_set_exclusive_handler(DMA_IRQ_0, ...)`.
5. **Shared SPI1 / GPIO8 dual-function.** `PIN_LCD_DC = 8` doubles as
   `PIN_CC1101_MISO`; `PIN_CC1101_CS = 40` is parked HIGH in `board_init()`
   before any LCD traffic. (The CC1101 radio driver is harvested — see
   `docs/hardware/catalog.md` — and the pin sharing and parking are live in
   `board.c`.) GPIO 40 is also the
   **WIO-E5 LoRa UART TX** (and GPIO 23 its UART RX, display-side PIO UART
   at 115200 baud) in the default firmware — the same line the BSP parks
   HIGH as the CC1101 CS; the firmware reaches the CC1101's CSn (the shared
   LCD/`SCREEN_CS1` line) through the IC113 mux, and the sub-GHz arbiter
   owns the handover. See `docs/drivers/lora.md`.
6. **LED count = 16.** `FW2_LED_COUNT` / `WS2812_NUM_PIXELS`
   (`bsp/leds/ws2812_driver.h`) = 16, on `pio1` via `PIN_LED_DATA` (GPIO 21).
   `FwDisplayVibe.md` (repo root, the original hardware description) says 7
   and is **WRONG** — the verified board header wins. `FwDisplayVibe.md` also
   disagrees with `board.h` on the CC1101 chip-select pin (says GPIO 23;
   `board.h`/`board.c` say GPIO 40 and actively drive it). See
   `docs/hardware/facts.md` for both discrepancy records.
7. **No LCD reset GPIO.** The panel relies on SWRESET only; RESX is
   hardware/ioexp-handled (`bsp/platform/ioexp.c` releases it as part of
   `ioexp_init()`). Don't look for a `PIN_LCD_RESET`-style define — there
   isn't one.
8. **Board selection is CMake-only.** `set(PICO_BOARD freewili2)` lives in
   the top-level `CMakeLists.txt`; `list(APPEND PICO_BOARD_HEADER_DIRS ...)`
   points at `bsp/boards`. Never override on the command line (repeats
   invariant 1 — it's the single most common way to break a fresh build).
9. **No need to calibrate touch screen**. The touch screen is pre calibrated at the factory.
10. **Audio Speaker** The speaker for first production FreeWili 2 is 0.5 Watt. Please enforce this limit when using the speaker. Also, make sure to disable the audio driver when not in use.
11. **Header GPIO needs a VIO rail — `ioexp_vref()`.** The user GPIO header is
    level shifted and the shifters are dead without a reference voltage, which
    the **display** I/O expander gates (PCAL6524 port 2: `P3V3_VREF` bit6,
    `P5V_VREF` bit5, `EXT_VREF` bit3, `INT_VREF` bit4, mutually exclusive).
    **This failure is silent.** A main-CPU GPIO with no VIO still toggles
    internally, `ow_io_gpio_read_all()` still reports the new state, every
    OneWili call still returns `OW_OK` — and the header pin never moves. There
    is no error and no log line. If you are debugging "the pin does nothing",
    check VIO before anything else. `ioexp_init()` defaults to `VREF_EXT_PIN`
    (matching the stock firmware, `fw2VREFConnection::vVIO`), which is whatever
    external circuitry supplies — i.e. nothing on a bare board. **Any app that
    drives the header at a known logic level must call `ioexp_vref(VREF_3V3)`
    (or `VREF_5V0`) itself**; `apps/toggleled` and `apps/hello_vref` do.
    `ioexp_vref_get()` reports the current selection. Verified on hardware
    2026-07-26, caveats included:
    `docs/superpowers/findings/2026-07-26-gpio-vref-e2e.md`.

12. **PSRAM apps need an SRAM bootstrap, not BOOTRAM.** A loadable app that
    executes from `0x11000000..0x11800000` inherits live QMI/PSRAM setup from
    the DISPLAY loader. Its C/C++ runtime entry and every clock/QMI-sensitive
    boot routine must execute from SRAM; do not rerun cold-boot PSRAM setup or
    reset the bus carrying the executing image. Keep the vector table first in
    PSRAM, with its initial stack in SRAM, then transfer into the SRAM bootstrap.
    RP2350 BOOTRAM is ROM-owned special memory and is not the app bootstrap
    region. Verify symbol addresses plus every UF2 target block, then verify an
    observable runtime milestone on hardware. See `docs/app-storage.md`.


## The FW2App contract

Firmware built by this BSP must be identifiable, versioned, self-describing
and recoverable without a human touching the board.

**1. Every app declares `VERSION` and `DESCRIPTION`.**

```cmake
fw2_display_app(bench_display
    VERSION 001
    DESCRIPTION "Bench console for the display drivers: charger, RTC, ...")
```

Declare every switched rail the app uses with `POWER_ZONES`; valid names are
`SENSORS`, `DISPLAY`, `AUDIO`, `SUBGHZ`, `SDCARD`, `USB_HUB`, `RGB_LEDS`,
`ANALOG`, `NFC_RFID`, and `CAN`. The declaration is emitted into the app's
linked metadata. `fw2_app_recovery_init()` requests the declared mask and
`fw2_app_recovery_task()` maintains it, so apps must not duplicate that
lifecycle by hand. For example:

```cmake
fw2_display_app(radio_ui
    VERSION 001
    DESCRIPTION "Sub-GHz monitor"
    POWER_ZONES SENSORS DISPLAY SUBGHZ RGB_LEDS)
```

`VERSION` is exactly three digits, bumped by hand. `DESCRIPTION` is required
and has no default — it is what the App Explorer shows a human choosing what
to flash. `NAME` defaults to the CMake target.
Missing or malformed is a configure error.

**2. Holding HOME for five seconds must leave the RAM app.**

Call `fw2_app_recovery_init()` immediately after `board_init()`, then call
`fw2_app_recovery_task()` on every main-loop path, including retry and fatal
error loops. A five-second HOME hold performs a normal watchdog reboot so the
DISPLAY recovery loader can resume its flash application. Do not call
`reset_usb_boot()`; entering BOOTSEL defeats unattended recovery.

Synchronous OneWili calls can otherwise hide the keyboard link for their full
timeout. Apps using `ow_open_fwgui()` must include
`input/app_recovery_onewili.h` and open the link with
`fw2_app_recovery_open_onewili(&dev)`. Apps using
`ow_sd_*` must also call `fw2_app_recovery_wrap_sd()`. The wrappers split
transport waits into short polls and service HOME between them. Physical HOME
state also expires when fresh keyboard status frames stop arriving; explicit
AgentIO holds remain active until released.

**3. Every image carries a `fw2app_uf2_info_t` record.**
`bsp/common/uf2_info.h` defines a 216-byte record with the 8-byte magic
`FW2AINFO`, name, description, version, optional build identity, and CRC32.
`tools/check_app_uf2.py` validates the final UF2 POST_BUILD and fails the
build if the record is missing, duplicated, or wrong.

Three things a reader needs to know about it:

- **Every FW2 app contains exactly one record.** This BSP targets the DISPLAY
  CPU; FW2 RAM apps do not embed a second processor's image. An app with an
  ESP32 half ships that half as a separate image for the ESP32 firmware
  project (see "The ESP32-C5 and peer streams"); the UF2 stays DISPLAY-only.
- **Build identity is optional.** The current examples leave `build` and
  `build_ts` empty; consumers must accept that representation.
- **Nothing in the firmware references the record**, so it is held by
  `-Wl,--undefined=fw2app_uf2_info`. `__attribute__((retain))` is ignored by
  this toolchain, and the SDK's KEEP'd `.binary_info.keep.*` section is wrong
  here — picotool walks that region as an array of pointers.


The wrapper proves that metadata was declared and survived the linker. It
cannot prove `fw2_app_recovery_task()` is reached on every runtime path;
review and hardware verification must cover that part.

**4. LCD apps establish the whole surface before enabling the backlight.**

A loadable app inherits the panel RAM left by the previous firmware. After
`st7796_init()`, clear or fully render all `480x320` pixels before calling
`board_backlight_set(1)` or making partial draws. The normal pattern is
`st7796_fill_screen(background)`. If AgentIO capture is enabled, call
`agentio_init()` first so the clear also initializes its shadow framebuffer.
Headless and DVI-only apps are unaffected.

## Peripheral power zones — request rails BEFORE touching hardware

The board's power sequencer boots with most peripheral rails **OFF**
(audio codec, CAN, radios, RGB LEDs, the analog subsystem, ...). A driver
that reads garbage, NAKs, or produces silence is very often an unpowered
rail, not a code bug — check power before debugging the driver. The
boot-on set is sequencer-firmware-defined and can change between firmware
versions: **never rely on it; request what you need, every time.**

The pattern for any app using a peripheral (see `docs/drivers/power.md`
for the full zone map, per-zone cautions, and the protocol):

```c
fw2_app_recovery_init();                              // keyboard link + HOME recovery
picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_AUDIO)); // or _CAN, _RGB_LEDS, ...
picpwr_release_unused();   // and drop the inherited rails this app does NOT use
// rails take ~1 s to apply; THEN init the peripheral
...
while (true) {
    fw2_app_recovery_task();
    picpwr_task();     // re-asserts your rails if the sequencer drops them,
                       // and carries out the queued release
    ...
}
```

**An app that runs hot is a rail that was never released.** An app inherits
the rails of whatever ran before it — not the boot-on set — and nothing but
the app will ever switch them off; the audio rail in particular is sometimes
inherited on, with the codec left mid-playback. `picpwr_release_unused()`
drops the app-owned rails (audio, sub-GHz/LoRa, RGB LEDs, NFC) the app did
not ask for. The CPU is not the problem: sleeping the 250 MHz core 97% of the
time moved the die ~1 C on the bench, so do not reach for WFI loops.
`docs/drivers/power.md` ("Releasing rails"); `board_die_temp_c()`
(`bsp/platform/power.h`) for before/after checks.

A key qualification for anyone running **against the stock firmware**
instead of a standalone BSP app: the default DISPLAY image runs an
**automatic zone manager** (the zone-manager family) that acquires and
releases managed zones (1, 3, 4, 5, 8, 10, 11, 13, 14, 15, 16) on its own,
batched into one PZCONFIG per settle window, with an escape-hatch setting
that reverts to `EPOWERZONE` refusal. A standalone app that flashes its own
DISPLAY firmware owns its power policy and uses `picpwr_*` exactly as
above. See `docs/drivers/power.md` for the full picture.

Rules that cost real bench time:

- Request rails **before** initializing peripherals on them; a rail
  rising mid-session can glitch a shared I2C bus (run bus recovery after
  a rail apply if the bus was live during it).
- Read the zone map before switching anything **off** — some zones blank
  the display, drop your debug probe, or risk filesystem corruption.
- Never set mask bits above zone 17; the API strips them (they are
  reserved — `docs/drivers/power.md`).

## "GPIO" is ambiguous on this board — ASK which one

**When a request mentions GPIO, stop and ask the user which kind before writing
any code.** There are two completely different sets of pins on a FreeWili 2 and
they share a numbering space, so a request like "toggle GPIO 25" has two valid
readings that produce entirely different code. Guessing wastes a build/flash
cycle at best, and at worst silently does the wrong thing on hardware.

| | **External** (user GPIO header) | **Internal** (display-CPU pins) |
| --- | --- | --- |
| Owned by | the **main** CPU | the **display** CPU (the one this BSP runs on) |
| Driven via | `libs/onewili` over the FwGUI link — `ow_io_gpio_set_io_high/low/toggle()`, `ow_io_gpio_read_all()` | the Pico SDK directly — `gpio_init()` / `gpio_put()` |
| Pin numbers from | the FreeWili 2 header / product docs | `bsp/platform/board.h` (**authoritative**) |
| Needs `ioexp_vref()` | **yes** — level shifted, dead without VIO (invariant 11) | no |
| Requires | main CPU running the stock firmware (OneWili bridge) | nothing extra |
| Example | `apps/toggleled`, `apps/hello_vref` | `PIN_LED_DATA`, `PIN_IR_TX`, `board_backlight_set()` |

**GPIO 25 is the trap that makes this concrete.** On the header it is a
main-CPU user pin (what `apps/toggleled` toggles). In `bsp/platform/board.h` it
is `PIN_LCD_BL`, the display CPU's backlight enable. Same number, different
chip, different code, and neither one errors if you pick wrong.

Good clarifying questions: *"Do you mean the external GPIO on the header
(main CPU, over OneWili) or a display-CPU pin from `board.h`?"* and, once it is
the header, *"which VIO rail — 3.3 V, 5 V, or whatever the external Trig_IN/VREF
pin supplies?"* Only skip the question when the request already names one
unambiguously (e.g. it cites a `PIN_*` define, or says "over OneWili").

There is a third set nobody here can drive: the **ESP32-C5's own pins**. No
OneWili command reaches them. If an app needs the ESP32 to do something with
its GPIO, Wi-Fi or LED, the ESP32 half of the app does it and the display
half asks over a peer stream, in a message format the two halves define
(`apps/dualcpu`'s `SET_LED` and `SCAN_NOW` are the pattern).

## The five front-panel buttons — label each one directly above it

Five physical buttons sit in a row along the bottom edge of the LCD, equally
spaced, left to right: **grey, yellow, green, blue, red**. Apps read them as
`UARTKBD_BTN_GREY` … `UARTKBD_BTN_RED` (`bsp/input/uartkbd_parse.h`, values
0-4 in that left-to-right order; `fw2kb` calls them `FW2KB_BTN_GRAY` …
`FW2KB_BTN_RED`), and `fw press grey|yellow|green|blue|red` injects them. The
nav pad, OK, CANCEL, PAGE and HOME are not part of this row.

An on-screen label for one of these buttons must sit directly above it, so the
user can see which button does what:

- **Geometry:** five boxes along the bottom of the 480×320 screen; box *i*
  (0 = grey) at `x = i * 96`, width `(ST7796_W - 12) / 5` = 93, so 3 px gaps.
  That is the stock firmware's own menu bar. The height is the app's choice.
- **Keep the physical order and the fixed fifths.** Never size boxes to their
  labels, reorder them to suit the text, or add a sixth box to the row. An
  action with no coloured button (one bound to OK, say) gets its touch target
  somewhere else on the screen. A button that does nothing in the app gets no
  box; leave its slot empty rather than moving the others.
- **Colour:** fill each box with its button's colour and write what the button
  does. In the wire order `st7796_fill_rect()` takes: grey `0x9AD6`, yellow
  `0x06FF`, green `0x0012`, blue `0xF800`, red `0x0780`. The stock bar writes
  white on all five; black reads better on the light grey and yellow boxes,
  which is what `apps/dualcpu` does.
- **Touch:** if the labels are also touch targets, hit-test the same
  rectangles, so touching a label and pressing the button under it do the same
  thing.

`apps/dualcpu` is the example whose labels are also touch targets;
`apps/canblast` labels four buttons and leaves grey's slot empty;
`apps/hello_keyboard` and `apps/retrochat` draw the same bar, display-only, for
the chord keyboard.

## The ESP32-C5 and peer streams

The FreeWili 2 has four processors an app may care about. MAIN (RP2350) runs
the stock firmware and owns the OneWili menu, the power sequencer, the SD card
and every link. DISPLAY (RP2350B) runs the wilibsp app. The ESP32-C5
(Bottlenose) runs the firmware's Wi-Fi/Bluetooth image, which can carry a
**BSP app** of its own. The CM0 Linux module and the PC host are the other two
OneWili clients. A wilibsp app builds for DISPLAY only; it reaches MAIN with
generated commands and the ESP32 with **peer streams**, and nothing else.

**Peer streams** (`onewili_stream.h`, `libs/onewili/wilibsp`; wire contract in
`ow_stream_wire.h`) are best-effort datagrams of 1-`OW_STREAM_MTU` (128) bytes
between MAIN's clients (`OW_PEER_DISPLAY`, `OW_PEER_ESP32`, `OW_PEER_CM0`,
`OW_PEER_HOST`; `OW_PEER_MAIN` is reserved and dropped). `ow_open_fwgui()`
binds them to the display link, so a datagram is one link frame and never a
command round trip. The same API with the same semantics runs on the ESP32
(over its own link to MAIN) and on the PC and CM0 (over three text commands),
so a message format agreed between two halves works unchanged from any of
them. Delivered whole or not at all; no ordering across senders, no retry, no
acknowledgement. Layer reliability on top if you need it.

Rules for a display app that talks to the ESP32. Each one cost bench time:

1. **The ESP32 is power zone 5 and you must hold it up yourself.** Under the
   stock DISPLAY firmware MAIN's own demand keeps the rail on; a BSP app
   discards that demand, so nothing re-asserts it after a sleep, USB attach or
   watchdog. Zone 5 has no `POWER_ZONES` name: call
   `picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_WIFI_BT))` before
   `picpwr_release_unused()` and before opening the link. The ESP32's RGB LED
   is on zone 10, so declare `RGB_LEDS` too if the ESP32 half drives it.
   Whether the stock DISPLAY firmware raises zone 5 from MAIN's ESP32 Mode
   demand alone is unconfirmed (two bench sessions disagreed); an app that
   requests the rail itself does not depend on it.
2. **Set MAIN's ESP32 Mode to OneWili API**, once per launch:
   `ow_wireless_e_sp32_mode(&dev, 1)`. MAIN answers the ESP32's OneWili
   traffic only in that mode. In Default mode datagrams to and from the ESP32
   are dropped and counted at MAIN, and the link reads stale within 3 s. The
   setting is saved on MAIN and survives reboots, but set it anyway; it is
   also a cheap first command that proves MAIN is listening.
3. **Call `ow_stream_poll()` on every loop pass**, even if you only send. It
   drains the 2 KB receive FIFO (each datagram costs its size plus 2 bytes)
   and sends the once-a-second keepalive; after 3 s of silence MAIN closes
   the link and drops everything addressed to this CPU. A synchronous OneWili
   call that blocks longer than that (a long `ow_sd_*` transfer, a stalled
   command) has the same effect. Use the `fw2_app_recovery_*` wrappers as for
   any app.
4. **Writes are refused, never queued.** `ow_stream_write()` returns
   `OW_ERR_BUFFER` until MAIN's first CREDIT arrives (one round trip after
   open) and whenever the write would put more than `OW_STREAM_WINDOW` (768)
   bytes in flight, counting each datagram as its size plus 10 bytes of
   framing: about 5 full-size or 40 nine-byte datagrams. A refused write is
   counted in `ow_stream_drops()`. Resend from your own state; do not spin on
   a refusal.
5. **Nothing waits for the destination, so count.** `ow_stream_drops()` is
   the sum of everything lost anywhere: refused here, lost on the link,
   dropped at MAIN in either direction, or dropped here for lack of room. A
   silent loss shows up only there and in your own sequence numbers.
   `ow_fwgui_get_stats()` has the `stream_*` link counters.
6. **One task per device.** Push links are single-threaded: call
   `ow_stream_*` for a device from one core and do not interleave them with
   other `ow_*` calls from another.
7. **A MAIN reboot restarts DISPLAY into its stock firmware**, so a
   `fw ramrun` app is gone after any MAIN restart; relaunch it. The ESP32 half
   keeps running and both halves reconnect on their own, including after the
   ESP32 is reset into its ROM loader by the `w\a` flasher queries.

**Building and flashing the ESP32 half.** The ESP32 half is not a wilibsp
target. It is a menuconfig choice in the FreeWili 2 ESP32 firmware project,
"BSP app on the ESP32" (none by default; the OneWili demo, which exercises the
text, binary-event and stream channels; and the dual-CPU app's ESP32 half),
selected by an `sdkconfig.bsp-*` overlay, built with ESP-IDF in a build
directory of its own and flashed over the ESP32-C5's own USB Serial/JTAG port
with esptool. Its log is that same console. MAIN must run a firmware build
that carries peer streams; older MAIN firmware answers nothing on the stream
path and the link never confirms. The generated `ow_wireless_esp32_flasher_*`
bindings (`w\a`) can reflash the ESP32 from a folder on the SD card through
MAIN; that route has been driven from a PC, not yet from a display app.

**Worked example: `apps/dualcpu`.** ESP32 to DISPLAY: TELEMETRY about once a
second (uptime, chip temperature, heaps, stream drops, event counts, LED state,
the strongest Wi-Fi access points) and PONG. DISPLAY to ESP32: PING, SET_LED
(off, solid, rainbow) and SCAN_NOW. Every datagram starts with a type byte,
fields are little-endian, unknown types are ignored and short ones counted as
malformed. The display half labels the five buttons for the LED, times each
round trip, and takes bench commands over the RTT down buffer (`stats`,
`ping`, `burst <n>`, `led`, `scan`, `espmode`). Verified on hardware
2026-09-27 and again 2026-09-28 on the released command ids: 80/80 PINGs at
about 6 ms, bursts of 150 accept exactly 40, ESP32 Mode gate and ROM-loader
resets recovered, PC-to-DISPLAY and PC-to-ESP32 text-route echoes clean
(`docs/superpowers/findings/2026-09-27-dualcpu-peer-streams-e2e.md`,
`2026-09-28-dualcpu-renumbered-ids-rerun.md`). Not exercised there: the
ESP32's GPIO-report and text-event mirrors, and the green button under load
in `canblast`.

**Not there yet:** one build target that produces both halves; text or
binary events pushed to the CM0; anything that lets a display app run code on
the ESP32 without an ESP32 app already flashed there.

## How to add a driver

The BSP grows by harvesting a proven driver from one of the owner's other
repos (see `docs/hardware/catalog.md` for which repo owns which peripheral),
not by writing one from scratch:

1. **Copy** the `.c`/`.h` (and any `.pio`) files verbatim into
   `bsp/<domain>/` (e.g. `bsp/radio/`, `bsp/nfc/`), matching the directory
   layout the source repo uses under its `src/` (`platform/`, `display/`,
   `input/`, `leds/`, ...) so its existing `#include "domain/x.h"`-style
   includes resolve unchanged against the `bsp/` include root.
2. **Wire it into `bsp/CMakeLists.txt`**: add the new `.c` files to the
   `add_library(freewili2_bsp STATIC ...)` source list, and add any new
   `target_link_libraries` (pico_sdk component) or
   `pico_generate_pio_header` calls it needs.
3. **Activate the include in `bsp/fw2.h`**: add
   `#include "domain/x.h" // (Task N)` to the umbrella header so apps get it
   for free via `#include "fw2.h"`.
4. **Update `docs/hardware/catalog.md`**: flip the peripheral's row from
   `TODO` to `DONE`.
5. If the driver has pure/host-testable logic, add a `tests/test_*.c` +
   `tests/CMakeLists.txt` entry so `fw test` covers it (see the
   `subghz`-repo pattern of splitting pure decision logic from hardware
   binding behind `#ifndef HOST_TEST`).
6. **Verify it on the board and write up what happened** — see "Verify on
   hardware" below. A harvest is not done when it compiles; every driver in
   this BSP has a findings file behind it, and yours should too.

The `skills/freewili2-add-driver/SKILL.md` skill in this repo walks an agent
through exactly this procedure.

## Verify on hardware — you can do this yourself now

**"It builds" and "the host tests pass" are not evidence that a driver works.**
This is an embedded BSP: nearly every bug that has cost real time here —
the WS2812 first-frame latch, the audio LRCK slip, the PSRAM re-timing after
the overclock, the active-low keyboard bits — was invisible to the compiler
and to `fw test`. They were all found by running the code on the board.

Historically an agent could not do that, so claims stopped at "builds clean".
Since `agentio` (verified 2026-07-26) an agent can drive the board and see the
panel directly, with no human present. **Use it.** With a CMSIS-DAP probe
attached:

    fw build <app> && fw ramrun <app>    # SRAM apps; fw flash is for flash images only
    fw screenshot -o shot.png     # then actually LOOK at the PNG (not during high-rate traffic: the capture stalls the app)
    fw press green                # inject a button
    fw touch 240 160              # inject a touch
    fw type "hello"               # type through the chord engine
    fw rtt -s 5                   # capture DIAG() output for 5 s

See `docs/drivers/agentio.md` for the full surface and its limitations.

**What good verification looks like:**

- **Look at the screenshot.** Do not just check that the PNG was written —
  read it and compare against what the code says it drew. A capture that
  succeeds and shows the wrong thing is the failure mode worth catching.
- **Drive the input path**, don't just render. If a change affects buttons,
  touch, or text entry, inject and re-capture to prove the event reached the
  app.
- **Read the RTT log** (`fw rtt -s <seconds>`) alongside the screenshot —
  `DIAG()` output catches what the panel does not show.
- **Write down what happened**, including anything that failed or that you
  could not test, in `docs/superpowers/findings/YYYY-MM-DD-<topic>-e2e.md`.
  Follow the existing files. A findings doc that only records successes is
  worth much less than one that is honest about gaps.
- **Then update the status docs** — `docs/hardware/catalog.md` and the
  "Hardware verification status" section of `docs/hardware/facts.md`.

**If no probe is attached**, say so plainly and report the work as
unverified — do not describe expected behavior in a way that reads like a
result. `fw flash`/`fw rtt`/`fw screenshot` all need the probe; a board in
BOOTSEL mass-storage mode can take a UF2 but gives you no RTT channel, so
none of the agentio verbs work against it.

**Gotcha:** a leftover OpenOCD (from a crashed script) keeps the probe and
serves an RTT session bound to the image that was loaded when it started, so
a freshly `fw ramrun` app looks silent. Kill strays before relaunching. After
a MAIN reflash the on-board probe re-enumerates for a few seconds. Never leave
a core in debug halt: TIMER0 pauses for both cores (DBGPAUSE) and every sleep
in the app freezes; `ramrun.tcl` clears those bits after the jump.

**Gotcha:** back-to-back one-shot commands can fail with `openocd did not open
port 9091 within 10s` because the previous OpenOCD has not released the probe.
Leave a couple of seconds between them, or keep a `fw rtt` running — it holds
the probe once and every one-shot verb reuses it.

**Gotcha:** the agentio verbs see only the DISPLAY. An ESP32 half logs to the
ESP32-C5's USB Serial/JTAG console, and a `fw screenshot` during high-rate
stream traffic stalls the app long enough to lose the keepalive; read the
counters over RTT instead while a link is busy.

## Where things live

- **Pin map**: `docs/hardware/pinmap.md` (generated from and cross-checked
  against `bsp/platform/board.h`, the **authoritative** pin source).
- **Hardware facts / invariants**: `docs/hardware/facts.md`.
- **Peripheral status (done vs. TODO)**: `docs/hardware/catalog.md`.
- **Per-driver usage docs**: `docs/drivers/*.md` (platform, display, touch,
  leds, audio, pdm, radio, sensors, ir, usbhost, dvi, agentio).
- **On-hardware verification records**: `docs/superpowers/findings/*-e2e.md` —
  what was actually run on the board and what came back. Check here before
  claiming any behavior is confirmed.
- **Main-CPU control (OneWili over the FwGUI link)**: `libs/onewili/wilibsp/README.md`.
- **The ESP32-C5 from a display app**: peer streams — the "Peer streams"
  section of `libs/onewili/wilibsp/README.md`, `onewili_stream.h` and
  `ow_stream_wire.h` beside it, "The ESP32-C5 and peer streams" above,
  `apps/dualcpu`, and the two dualcpu findings files.
- **Related default-firmware subsystems** (implemented upstream in the default
  FreeWili 2 firmware, not in this BSP): LoRa WIO-E5 bridge
  (`docs/drivers/lora.md`), NFC ST25R3916B, the MAIN-side ESP32-C5 link and
  its `w\a` flasher, CM0 Linux bridge, and the automatic power-zone manager
  (`docs/drivers/power.md`).
  That covers the SD card too — the display CPU has no direct card path, so
  `ow_sd_*` is the only route (`apps/hello_sdcard`).
- **Original hardware description**: `FwDisplayVibe.md` (repo root) — a
  secondary source, useful for the broader peripheral inventory (radio, NFC,
  IR, DVI, audio, mics, buttons, PIO-USB, sensors) not yet in `board.h`, but
  known to contain at least one error (LED count — see above). When it
  conflicts with `bsp/platform/board.h` or `bsp/leds/ws2812_driver.h`, the
  verified board header/driver code wins.
- **Full implementation plan / spec**:
  `docs/superpowers/plans/2026-07-01-freewili2-bsp.md`.
- **Agent E2E harness (input injection + screen capture)**:
  `docs/drivers/agentio.md`.

## Naming note

Harvested drivers keep their proven names from the source repos:
`st7796_*` (display), `ft6336_*` (touch), `ws2812_*` (LEDs), `board_*` /
`ioexp_*` / `psram_*` (platform). There was **no** `fw2_`-prefix rename — the
spec proposed one, but forcing it onto an already-consistent, harvested
codebase would be pure churn. `fw2.h` is the umbrella include; the `fw2_`
prefix convention (if ever used) applies only to new BSP-level convenience
code written from scratch in this repo, not to harvested drivers.

## Conventions

- **Conventional Commits**: `feat:`, `fix:`, `docs:`, `refactor:`, `test:`
  with optional scope; imperative subject.
- **Diagnostics via `DIAG()`** — never `printf`, never USB/UART stdio.
- `build/`, `build-tests/`, `*.uf2`, `*.elf`, `*.bin`, `__pycache__/`,
  `.venv/` are git-ignored — don't commit them.

## Documentation maintenance

Documentation must stand on its own using observable behavior, supported APIs,
and hardware names. Do not cite another repository, a local checkout path,
implementation-only class or symbol names, or commit history. Translate source
investigation into product behavior that can be understood and verified from
this repository.

When importing or refreshing information from the stock firmware:

1. Run `git fetch --prune` and report whether this branch is behind before
   calling the BSP current. Do not pull across unrelated local changes.
2. Update the relevant driver or hardware page without leaving a source-tree
   breadcrumb.
3. Run `python -m pytest tests/test_no_private_refs.py` and `git diff --check`.
4. Preserve honest verification language: a behavior without a local findings
   record is expected or documented, not hardware-verified by this BSP.

`tests/test_no_private_refs.py` scans the Markdown shipped in this repository for
private upstream repository and path references. Add a regression pattern when
a new kind of private breadcrumb is discovered.

## Gotchas for automated edits

- Don't add USB/UART `printf` stdio — use `DIAG()` (invariant 3).
- Never pass `-DPICO_BOARD` on a cmake command line (invariant 1/8).
- Register any new DMA_IRQ_0 user as a shared handler, guarded on its own
  channel (invariant 4).
- If you touch GPIO8, remember it is dual-purpose (LCD_DC / CC1101 MISO)
  (invariant 5).
- Trust `bsp/platform/board.h` over `FwDisplayVibe.md` for any pin or LED
  count discrepancy (invariant 6).
- **Ask which GPIO the user means** — external header pin (main CPU, OneWili,
  needs VIO) or internal display-CPU pin (`board.h`, plain SDK calls)? They
  share a numbering space; GPIO 25 is valid as both. See the section above.
- If your code drives a **header GPIO**, call `ioexp_vref()` — without a VIO
  rail the pin is silently dead while every status code says OK (invariant 11).
- Allocate PSRAM buffers with `__uninitialized_psram("group")`, never by
  casting `PSRAM_BASE` (invariant 2) — the linker's PSRAM region starts at
  that same address, so a raw pointer silently aliases whatever the linker
  placed there.
- **Talking to the ESP32?** Hold zone 5 up yourself
  (`PICPWR_ZONE_WIFI_BT`), set ESP32 Mode to OneWili API, call
  `ow_stream_poll()` every loop pass, and treat a refused write as normal.
  See "The ESP32-C5 and peer streams".
- **Don't report a driver as working because it builds.** Flash it and check
  it with `fw screenshot` / `fw rtt`, and record the result in
  `docs/superpowers/findings/`. If no probe is attached, say the work is
  unverified rather than describing intended behavior as an outcome. See
  "Verify on hardware" above.
