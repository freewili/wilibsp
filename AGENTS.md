# AGENTS.md — guide for AI agents and contributors

> **Consuming wilibsp from another project?** Read
> **[docs/consuming.md](docs/consuming.md)** instead — it is short, and it is
> the whole of what a parent project needs. This file is the deep reference for
> working **on** wilibsp, and about 60 % of it is internal to that job.
>
> Do not copy rules out of either file into your own project's docs. Anything
> copied is pinned to the submodule commit you copied it at and rots silently
> when you bump it; the enforceable rules already ship as build checks and CLI
> behaviour that version with the code.

This file orients coding agents (Claude Code, Cursor, Copilot, etc.) and new
human contributors working in `wilibsp`. It is intentionally dense: the goal
is that you do **not** have to rediscover the hard-won facts this project was
built on. Read it before making changes.

## What this is

`wilibsp` is a board-support **monorepo** for the **FreeWili 2** (Raspberry Pi
**RP2350B**, 48 GPIO, 16 MB flash, 8 MB PSRAM). Importantly, today it covers the display processor only. (This means you must use OpenOCD interface 0 — FreeWili 2 exposes multiple debug interfaces.) It provides:

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
  CPU owns (SDFS over the same link). See `libs/onewili/README.md`;
  `apps/toggleled` and `apps/hello_sdcard` are the worked examples.
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
catalog (NFC, buttons, PIO-USB) is unverified because its driver has not been
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
| `fw flash [app]`    | Program `build/apps/<app>/<app>.elf` over the cmsis-dap debug probe via OpenOCD (`tools/openocd/freewili2.cfg`)                         |
| `fw rtt`            | Attach to the target and stream SEGGER RTT diagnostics (OpenOCD RTT server on port 9090)                                                |
| `fw test`           | Configure + build + run the standalone host CTest tree in `tests/` (MinGW GCC + Ninja on Windows; no Pico SDK, no hardware)             |
| `fw new-app <name>` | Scaffold `apps/<name>` by copying `apps/template` and rewriting the CMake target name                                                   |
| `fw install-app <uf2>` | Find MAIN with fwFinder, mount its SD reader on the PC, copy the UF2 to `/apps/`, safely unmount, and return the SD to MAIN       |
| `fw screenshot`     | Capture the screen to a PNG (`--surface lcd|dvi`, `--crop x,y,w,h`, `--scale N`) via the agentio RTT channel (verified on hardware 2026-07-26) |
| `fw press <btn>`    | Inject a button press+release (`fw hold` / `fw release` for a sustained hold) |
| `fw touch <x> <y>`  | Inject a touch tap (`--down` / `--up` for a sustained touch)              |
| `fw type "text"`    | Type text through the fw2kb chord engine                                  |

Add `--print` to `build`/`flash`/`rtt`/`test` to print the underlying
command(s) instead of running them (useful for an agent to inspect what would
run without touching hardware).

Apps should put app-owned maps, logs, preferences, and similar data under
`/appdata/<app-name>/` by default. This keeps the SD root tidy and makes file
ownership obvious; it is a convenience convention, not a hard rule. User
needs win when a root-level or otherwise shared path is appropriate. See
`docs/app-storage.md`.

`/apps/` is a non-destructive DISPLAY launch surface. App UF2s may target only
SRAM (`0x20000000..0x20070000`) or PSRAM (`0x11000000..0x11800000`), never
QSPI flash (`0x10000000..0x11000000`). `fw install-app` checks every block and
fails before mounting the SD if the file is malformed, mixed-target, or touches
flash. The DISPLAY recovery loader is fused in OTP; a write at flash base
replaces the stock DISPLAY firmware, not the loader.

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
   `pico_set_binary_type(<app> copy_to_ram)`: all code+data+bss live in 512 KB
   SRAM, so watch the RAM budget — large buffers (framebuffers, capture clips)
   belong in PSRAM (`PSRAM_BASE 0x11000000`, APS6404L, 8 MB, brought up by the
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
uartkbd_init();                                       // link to the sequencer
picpwr_keep_awake(picpwr_zone_bit(PICPWR_ZONE_AUDIO)); // or _CAN, _RGB_LEDS, ...
// rails take ~1 s to apply; THEN init the peripheral
...
while (true) {
    uartkbd_task();
    picpwr_task();     // re-asserts your rails if the sequencer drops them
    ...
}
```

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

    fw build <app> && fw flash <app>
    fw screenshot -o shot.png     # then actually LOOK at the PNG
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

**Gotcha:** back-to-back one-shot commands can fail with `openocd did not open
port 9091 within 10s` because the previous OpenOCD has not released the probe.
Leave a couple of seconds between them, or keep a `fw rtt` running — it holds
the probe once and every one-shot verb reuses it.

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
- **Main-CPU control (OneWili over the FwGUI link)**: `libs/onewili/README.md`.
- **Related default-firmware subsystems** (implemented upstream in the default
  FreeWili 2 firmware, not in this BSP): LoRa WIO-E5 bridge
  (`docs/drivers/lora.md`), NFC ST25R3916B, ESP32-C5 Bottlenose
  (MAIN-side), CM0 Linux bridge, and the automatic power-zone manager
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
- **Don't report a driver as working because it builds.** Flash it and check
  it with `fw screenshot` / `fw rtt`, and record the result in
  `docs/superpowers/findings/`. If no probe is attached, say the work is
  unverified rather than describing intended behavior as an outcome. See
  "Verify on hardware" above.
