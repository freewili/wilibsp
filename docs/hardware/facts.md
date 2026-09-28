# Hardware facts — hard-won invariants

These cost real debugging time in the repos this BSP was harvested from
(primarily `subghz`). Treat them as ground truth; don't relearn them. They're
also summarized in `AGENTS.md`; this file is the fuller record.

## RP2350B, not RP2350A

`bsp/boards/freewili2.h` sets `PICO_RP2350A 0`, giving 48 GPIO (not the 30 of
an "A" variant). The board is selected in the **top-level `CMakeLists.txt`**
via `set(PICO_BOARD freewili2 CACHE STRING "Board type")` plus
`list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/bsp/boards")`.
**Never** pass `-DPICO_BOARD=...` on the cmake command line — a cache
variable set that way overrides the `CMakeLists.txt` value and reverts the
build to the wrong board config (wrong GPIO count, wrong flash size).

## Clock / RAM invariant

`board_init()` in `bsp/platform/board.c`:

1. `vreg_set_voltage(VREG_VOLTAGE_1_25)`
2. `sleep_ms(10)`
3. `set_sys_clock_khz(BOARD_SYS_CLOCK_KHZ /* 250000 */, true)`
4. Re-source `clk_peri` from `clk_sys`:
   `clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, f, f)`

Step 4 is not optional: without re-sourcing `clk_peri` after the sys-clock
change, the hardware SPI peripheral has no valid clock and the LCD shows
nothing. The 1.25 V vreg bump exists because 250 MHz at the default vreg was
marginal during heavy ST7796 bring-up in the source repo.

Every app is built through `fw2_display_app()`, which selects the SDK's
`no_flash` binary type (see
`apps/template/CMakeLists.txt`, `apps/hello_display/CMakeLists.txt`): loadable
code, initialized data, and ordinary bss run from the RP2350's 512 KB SRAM,
not flash XIP. This makes running at 250 MHz safe without flash-timing
concerns, but SRAM remains the tight budget — large buffers (framebuffers,
capture clips, waterfalls) can be explicitly placed in PSRAM
(`PSRAM_BASE 0x11000000`, APS6404L, 8 MB, brought up by the SDK's
`hardware_psram` at boot), not on the stack or in a static SRAM array. Place
them with `__uninitialized_psram("group")`, never by casting `PSRAM_BASE` — see
the PSRAM section at the end of this file.

`board_init_clk()` also re-times PSRAM after the clock change (step 5:
`psram_configure_params()` + `psram_reinitialize()`), for the reasons in that
same section.

## Diagnostics = SEGGER RTT only

`bsp/platform/diag.h`: `#define DIAG(...) SEGGER_RTT_printf(0, __VA_ARGS__)`.
There is **no UART or USB stdio** anywhere in this BSP — don't add `printf`.
`SEGGER_RTT_printf` supports `%d %u %x %s %c` and field widths, but **no
floating point** conversions. View RTT output live with `fw rtt`.

## DMA_IRQ_0 is shared

The ST7796 async flush (`bsp/display/st7796.c`) registers its DMA completion
handler with `irq_add_shared_handler(DMA_IRQ_0, ...)` and checks only its own
DMA channel's status flag before acting. Any future user of `DMA_IRQ_0` (a
radio capture, a second display path, etc.) must do the same — registering
with `irq_set_exclusive_handler(DMA_IRQ_0, ...)` would silently break the
display's IRQ.

## Shared SPI1 / GPIO8 dual-function

`PIN_LCD_DC = 8` (`bsp/platform/board.h`) doubles as `PIN_CC1101_MISO`. It is
an OUTPUT (DC) for the LCD and would need to be muxed to SPI1 RX (input)
around any CC1101 SPI access. `PIN_CC1101_CS = 40` is parked HIGH in
`board_init()` before any LCD traffic runs, so the (not-yet-harvested) CC1101
never drives lines shared with the LCD by accident. `bsp/platform/spi_bus.h`
already documents the intended arbitration API
(`spi_bus_acquire_cc1101()` / `spi_bus_release_cc1101()` /
`spi_bus_cc1101_cs()`) for whenever the radio driver is harvested.

## LED count = 16 (LED discrepancy record)

**`FwDisplayVibe.md`** (repo root, the original hardware description) says:

> There are 7 ws serial LEDs connected to GPIO 21.

**The verified board header and driver disagree and are authoritative:**
`bsp/leds/ws2812_driver.h` defines `WS2812_NUM_PIXELS 16` and
`FW2_LED_COUNT` as an alias for it — "single source of truth = 16, verified
board" per the header's own comment. `bsp/leds/ws2812_driver.c` drives 16
pixels on `pio1` via `PIN_LED_DATA` (GPIO 21, matching `FwDisplayVibe.md` on
the GPIO, just not the count).

**Resolution: 16 is authoritative.** Do not use the `FwDisplayVibe.md` count
of 7 anywhere in code, docs, or tests. If you ever see `7` in a new context
describing this board's LEDs, treat it as the same stale figure and correct
it to 16.

## CC1101 chip-select GPIO discrepancy

**`FwDisplayVibe.md` says:**

> The LCD SPI bus is shared with CC1101 radio. Its chip select is on GPIO 23.

**`bsp/platform/board.h` and `bsp/platform/board.c` disagree and are
authoritative:** `#define PIN_CC1101_CS 40`, and `board_init()` actively
drives GPIO 40 (not 23) HIGH at boot to park the CC1101 off the shared SPI1
bus before any LCD traffic. GDO0 (GPIO32) and GDO2 (GPIO37) match between the
two sources, so this is specifically a chip-select-pin discrepancy, not a
wholesale renumbering.

**Resolution: GPIO 40 is authoritative** for `PIN_CC1101_CS`. When the CC1101
driver is eventually harvested (see `docs/hardware/catalog.md`), use
`board.h`'s `PIN_CC1101_CS` macro rather than hard-coding either number.
Note: 'authoritative for `PIN_CC1101_CS`' means the pin the macro names —
which `board_init()` parks HIGH. In the default firmware that same pin is
the **WIO-E5 UART TX**, not a dedicated CC1101 chip-select output driven
directly — the CSn is reachable only through the IC113 mux under the
sub-GHz arbiter (see the GPIO40 section below).

**GPIO 23 is, in fact, the WIO-E5 LoRa UART RX** (display-side PIO UART at
115200 baud, TX = GPIO 40; see `docs/drivers/lora.md`). Verified against the firmware and the board schematics: the DISPLAY's
LoRa PIO UART uses GPIO 23 as its RX, and the schematic's
`LoRA_SPI_CS` net runs from DISPLAY GPIO 23 to the WIO module's PB6 (USART1 TX). The `FwDisplayVibe.md` 'GPIO 23' figure is **consistent
with** this WIO RX pin — a real signal, just not the CC1101 chip-select.

## GPIO 40: WIO-E5 UART TX, shared with the CC1101 CS via the IC113 mux

`PIN_CC1101_CS = 40` (wilibsp `board.h`) is a **shared line**: the BSP names
it the CC1101 chip-select and parks it HIGH at boot, and the default
FreeWili 2 firmware drives the same pin as the **WIO-E5 LoRa bridge's UART
TX** (display-side PIO UART, 115200 baud; RX = GPIO 23). The board schematics show why both roles are real: GPIO 40's
only connection beyond the RP2350 pad is the IC113 mux (`SN74LVC1G3157`
SPDT) — `A` = GPIO 40 via R325, `B1` = `SCREEN_CS1` (shared with the LCD chip-select and
the **CC1101's CSn, IC60 pin 7**), `B2` = `LoRA_PB7` (the WIO
module's PB7 UART RX), `S` = `LoRA_1101_SEL = NOR(V1_1, V2_1)` — so the pin
is a UART TX when the arbiter selects B2 and the CC1101/LCD chip-select
when it selects B1. The default firmware drives the same pin as the CC1101's CS through the
IC113 demux ("→ IC113 demux → SCREEN_CS1"), and the sub-GHz arbiter
owns the mux and sequences the handover (a live UART
would clock bit patterns onto the chip-select). `board_init()`'s HIGH
parking is compatible with both roles (UART TX idles high). Any BSP driver
that would actively toggle GPIO 40 as a CS line must go through the sub-GHz
arbiter and the antenna mux, exactly as the sub-GHz arbiter does. See
`docs/drivers/lora.md` and `docs/drivers/radio.md`.

## CC1101: bound every pin/register wait (a dark rail wedges core1)

A CC1101 register/status wait with no timeout can hang forever when the
rail is dark: the sub-GHz rail (zone 4) is switched, and on a board where
it is off, polling a CC1101 pin or register never reaches its terminal
state. The default firmware bounds **every** CC1101 pin/register wait with a
timeout (a default-firmware hardening commit), so a dark rail costs a
bounded delay instead of a wedged core. Any harvested CC1101 driver must
carry the same bound.

## LoRa bridge: standby boot + revive after rail cycles

The WIO-E5 bridge does not assume its rail (zone 4) is up:
it boots into **standby**, and after a zone-4 rail cycle it is **revived**
by re-issuing the init sequence (SET_DIO → CONFIGURE → RX_START) once the
rail returns. A send commanded
while the rail is still coming up is held until the UART answers. See
`docs/drivers/lora.md`.

## Display boot order (default firmware)

The default DISPLAY firmware settled a few boot facts worth
preserving in any display-core port:

- **Black the panel before switching it on**, then show the logo — the
  backlight/panel-on must not precede a configured panel.
- **Drive the LCD from the core that owns `spi1`** — cross-core SPI access
  to the panel wedges it.
- **Keep MAIN's IO expander configured and off a bus it wedges** during cold boot.
- The boot path was instrumented to cut ~⅓ off boot time;
  measure before claiming an ordering change is free.

## I/O expander: verify direction writes

Direction writes to the PCAL6524 can fail to land (bus glitch, expander
not ready); the default firmware's expander driver **reports whether the direction write actually landed** rather
than assuming success. `ioexp_init()`-style bring-up should check the write
result and retry/recover, because a silently-missed direction write leaves
pins in the wrong direction.

## No LCD reset GPIO

The ST7796-class panel has no dedicated reset GPIO in this design. The
comment at the top of `bsp/platform/board.h` states it plainly:

> There is NO LCD reset GPIO on this board — the panel RESX is
> hardware-handled, so the driver relies on SWRESET only.

`bsp/platform/ioexp.c`'s `ioexp_init()` releases the LCD's hardware reset
(`SCREEN_NRST`) via the PCAL6524 I/O expander as part of board bring-up; the
`st7796_init()` driver itself only ever issues a software reset (SWRESET)
command over SPI. Don't add a `PIN_LCD_RESET`/`gpio_init` reset sequence —
there's no pin for it, and `FwDisplayVibe.md`'s claim that "reset is shared
with the display [touch controller]" refers to this same hardware-only
reset line, not a GPIO the firmware toggles.

## gfx/palette carry (complete)

`bsp/leds/led_ui.c`'s `led_spectrum_map()` depends on `bsp/gfx/palette.h`
(`inferno_rgb565()`). Both the header and the implementation
(`bsp/gfx/palette.c`, harvested verbatim from `subghz/src/gfx/palette.c`) are
now present and compiled into `freewili2_bsp` via `bsp/CMakeLists.txt`'s
source list. `led_ui.c` / `led_spectrum_map()` link cleanly against the real
`inferno_rgb565` implementation — no further harvesting needed.

## WS2812 first-frame latch (refresh the LEDs)

The **first** `ws2812_show()` transmission after the `pio1` state machine starts
does not reliably latch all 16 LEDs — in practice only pixel 0 (the data-in end
of the chain) lights and the other 15 stay dark. A **second** `ws2812_show()`
(any subsequent frame) latches the full strip. Verified on hardware 2026-07-01.

This is **not** a driver defect: `bsp/leds/ws2812_driver.c` and `ws2812.pio` are
functionally identical to the proven `sensorview` and `wilidispval`
implementations (confirmed by diff — only whitespace/comment/include differ). It
is a WS2812 timing/startup quirk on this board.

**How to handle it:** refresh the LED state periodically instead of calling
`ws2812_show()` exactly once. Real apps redraw every frame and never notice.
`apps/hello_display/main.c` re-issues `ws2812_show()` every ~250 ms from its main
loop; the symptom was originally seen because an earlier version showed the LEDs
once during setup and then sat in a touch-only loop that never refreshed them.

WS2812 pixels also retain their last latched colors when the display CPU resets
into another RAM app. `board_init()` therefore sends two all-off frames during
startup. Its one-shot helper then disables and unclaims the temporary `pio1`
state machine and removes the PIO program; it does not request the RGB power
zone. Apps remain free to use `pio1` for the LEDs or PDM microphones afterward.

## Host tests are a standalone CMake project

`tests/` is configured and built as its **own** CMake project (no Pico SDK,
no cross-compiler) via `fw test` → `tools/fw.py`'s `test_command()`, which
runs `cmake -S tests -B build-tests`, `cmake --build build-tests`, then
`ctest --test-dir build-tests`. There is **no `host-test` CMake preset** in
`CMakePresets.json` — an earlier draft of the plan proposed one, but it was
removed once the standalone-project approach was chosen. Don't tell an agent
to run `cmake --preset host-test`; it doesn't exist. The only preset defined
today is `target` (for on-device builds).

## Audio: an undrained RX FIFO silences the DAC (duplex SM coupling)

`i2s_duplex.pio` runs **one** state machine for both directions — it interleaves
`out pins,1` (DAC) with `in pins,1` (ADC) and side-sets BCLK/LRCK. So RX and TX
are not independent paths that happen to share pins: they share the *instruction
stream*.

With RX autopush enabled and nothing draining the RX FIFO, the `in` stalls the
state machine as soon as that 4-deep FIFO fills — 4 frames, ~250 µs at 16 kHz.
A stalled SM stops side-setting BCLK/LRCK and stops consuming TX words, so the
**DAC dies with it**, while every codec register still reads correct.

**Recognise it by ear: a CLICK per note instead of a tone.** Each note plays
only its first ~250 µs (a fraction of one cycle) before the SM wedges. It is not
permanent silence — anything that calls `audio_i2s_duplex_play_stop()` between
notes clears the FIFOs and briefly unwedges the SM — so a chime degrades into
pitchless clicks.

Found 2026-07-25 building wilidoro's audio: it skipped `audio_capture_start()`
(reasonably — it never wants mic input, and skipping it avoids a `DMA_IRQ_0`
handler) and playback broke.

Confirmed two ways on RP2350 rev 3. **Registers, no reflash:** clear
`PIO0->FDEBUG` (`0x50200008`), hand-feed 8 words to `TXF0` (`0x50200010`), then
re-read — `RXSTALL` (bits 3:0, sticky) latches, `FSTAT` shows RXFULL, and
`FLEVEL` shows RX0=4 with words still stranded in TX; with the fix all 8 words
are consumed and nothing stalls. **A/B on the live device:** toggling just the
AUTOPUSH bit (`SM0_SHIFTCTRL` `0x502000d0` bit 16) on one unchanged firmware
image flipped the onboard speaker between clean tones and clicks — mic-measured
tonal magnitude at the intended note ~1100 with autopush off vs <4 with it on,
a ~300x difference, and the dominant frequency stopped tracking the note.

**Rule:** `audio_i2s_duplex_init()` leaves autopush **off** so playback works
standalone. Only `audio_i2s_duplex_rx_enable()` turns it on, and only
`audio_capture_start()` should call it — never enable RX without a consumer
draining it every frame.

## Audio: lock LRCK to MCLK/256 (DAC sample-slip tick)

The NAU88C10 runs **MCLK-direct** (reg 0x06 = 0), so its DAC/ADC sample rate is set
by MCLK, not by LRCK. MCLK is generated by an **integer** PWM divide of `clk_sys`, and
250 MHz does **not** divide to an exact 4.096 MHz: `250e6 / 61 = 4.0984 MHz`, so the
codec actually runs `fs = 4.0984e6 / 256 = 16009 Hz`. If the I2S frame rate (LRCK) is
set to the nominal 16000 Hz, the DAC consumes ~9 samples/s faster than data arrives and
**slips one sample ~9 times/second → an audible ~9 Hz periodic tick** (a blown/loud
speaker masks it; a clean headphone/line-out exposes it). Verified on 2026-07-04 as
±9.2 Hz sidebands on the 1 kHz carrier (−17 dB), gone (−44 dB) after the fix.

250 MHz is the board DEFAULT partly for this reason — it divides near-exactly to
the codec MCLK. Apps needing a different clock call board_init_clk(khz); the DVI
demo runs 252 MHz for an exact 25.2 MHz pixel clock, which moves the codec MCLK to
252e6/61 = 4.131 MHz (~0.8% audio pitch shift) — only relevant if that app also
plays audio.

**Rule:** derive the I2S PIO clkdiv from the **same integer MCLK divider**, not from
the nominal sample rate, so `LRCK == MCLK/256` exactly and data-in == data-out. In
`bsp/audio/audio_i2s_duplex.c`: `float div = (float)(8u * ticks) / 3.0f;` (where
`ticks` is the MCLK PWM wrap). Net pitch is +0.06 % (16009 vs 16000 Hz), inaudible.
Do **not** "simplify" this back to `clk_sys / (96*fs)` — that reintroduces the tick.

## Audio: RP2350-E5 requires dma_channel_cleanup(), not dma_channel_abort(), to stop the TX chain

`audio_i2s_duplex_play_loop()` and `audio_i2s_duplex_play_stream_loop()` configure the two
TX DMA channels to chain to each other (`channel_config_set_chain_to(&c, s_tx_dma[i ^ 1])`
on both), so each channel's completion re-triggers the other and playback loops with zero
CPU involvement. RP2350 **errata RP2350-E5** (see the RP2350 datasheet) says that aborting a
channel which is another channel's `CHAIN_TO` target can leave that other channel
re-triggerable with stale configuration, even though it was never itself aborted — a bare
`dma_channel_abort()` on one of a chained pair is not sufficient to fully stop playback. The
Pico SDK's `hardware/dma.h` documents this directly in the doc comment above
`dma_channel_abort()` and ships an errata-aware helper, `dma_channel_cleanup()`
(`hardware/dma.h`, implemented in `hardware_dma`'s `dma.c`), which clears the channel's
`CHAIN_TO` (to itself) and `EN` bits, disables its IRQs, *then* aborts it, and finally clears
its IRQ status — the IRQ-disable-first step also suppresses the spurious completion IRQ
`dma_channel_abort()` can raise mid-abort, which matters for `play_stream_loop()` since it
enables `DMA_IRQ_0` on both TX channels.

**Rule:** `audio_i2s_duplex_play_stop()` (`bsp/audio/audio_i2s_duplex.c`) calls
`dma_channel_cleanup()` on both TX channels, not `dma_channel_abort()`. This is safe to call
repeatedly across playback cycles: `play_loop()`/`play_stream_loop()` rewrite the channel's
full CTRL word (`dma_channel_configure()` → `dma_channel_set_config()`) on every re-arm, so
the cleared `EN`/`CHAIN_TO` state from the previous stop is not sticky.

## Hardware verification status

The `hello_display` on-hardware smoke test **passed on 2026-07-01** (RP2350 rev 3,
programmed + verified over the cmsis-dap probe). Confirmed live on the board:

- **Display (ST7796 + DMA):** panel lights, backlight on, "TOUCH THE SCREEN"
  text renders.
- **Touch (FT6336U):** chip detected over I2C (`ft6336: init ok id=0x64` via RTT);
  taps register and draw dots at the correct coordinates.
- **LEDs (WS2812 ×16):** all 16 light green (after applying the periodic-refresh
  fix — see "WS2812 first-frame latch" above).
- **Platform:** boots at 250 MHz (`hello_display up: sys=250000 kHz` via RTT),
  ioexp init OK, RTT diagnostics working (control block found at `0x200045d0`).

The **I2S full-duplex audio** driver (`hello_audio`) **passed on 2026-07-04** (RP2350
rev 3): codec bring-up OK (`rev=0x01A`, `pm2=0x015`), speaker↔jack routing switches at
the register level, RX capture never starves (shared `DMA_IRQ_0` coexists with the
ST7796 flush), and — after the MCLK/LRCK-lock fix (see "Audio: lock LRCK to MCLK/256"
above) — the 1 kHz tone plays clean on both the onboard speaker and the 3.5 mm jack
(±9.2 Hz slip sidebands −17 dB → −44 dB). Full record:
`docs/superpowers/findings/2026-07-04-i2s-audio-e2e.md`.

Peripherals still marked TODO in `docs/hardware/catalog.md` remain
unverified — their driver harvest is future work. Note that the **default
FreeWili 2 firmware** now implements the full
subsystem set that this BSP tracks as TODO or out-of-scope: the LoRa
(WIO-E5) bridge, NFC (ST25R3916B), the ESP32-C5 Bottlenose link, the CM0
Linux module, and the automatic power-zone manager. Where a peripheral is
implemented upstream, the default firmware is the authoritative reference — see
`docs/hardware/catalog.md` "Implemented upstream". The ESP32-C5 is the one a
display app can now talk to: OneWili peer streams routed by the main CPU,
verified on hardware with `apps/dualcpu` on 2026-09-27 and 2026-09-28
(`docs/superpowers/findings/2026-09-27-dualcpu-peer-streams-e2e.md`,
`2026-09-28-dualcpu-renumbered-ids-rerun.md`): about 6 ms round trip, no
loss at 1 PING/s, a 768-byte credit window, and the ESP32 held up by the app
on power zone 5.
PDM mics are now DONE (see "PDM microphones" below). The four I2C sensors
(OPT4001, SHT40, BMI323, BMM350) are also now DONE (see "I2C sensors"
below) — hardware-verified 2026-07-04 via `apps/hello_sensors` (4/4 chip-ids,
gravity vector PASS; human stimulus tests pending — see
`docs/superpowers/findings/2026-07-04-i2c-sensors-e2e.md`).

The **GPIO reference voltage (VIO) select** (`ioexp_vref()`) **passed on
2026-07-26** via `apps/hello_vref`: all five selections drive distinct PCAL6524
port-2 bits, `VREF_3V3` takes VIO from ~25 mV to ~3.27 V (measured on the
display CPU's own rail monitor, GPIO 45 = ADC input 5, 2:1 divider), and
main-CPU GPIO 25 toggles 0/1 at 2 s under that rail. The header pin's own
voltage was **not** measured, and `VREF_EXT_PIN` read ~4.81 V with nothing
connected to the external pin — unexplained. Full record:
`docs/superpowers/findings/2026-07-26-gpio-vref-e2e.md`.

## Header GPIO is silently dead without a VIO rail

The user GPIO header is level shifted; the shifters need a reference voltage
that the display I/O expander gates (`ioexp_vref()`, see AGENTS.md invariant 11).
The trap is that **nothing reports the failure**: with no VIO the main-CPU GPIO
still toggles internally, `ow_io_gpio_read_all()` reports the new state, and
every OneWili status is `OW_OK` — the header pin just never moves. `ioexp_init()`
defaults to `VREF_EXT_PIN` (as the stock firmware does), which supplies nothing
on a bare board, so apps driving the header call `ioexp_vref(VREF_3V3)`
themselves.

## GPIO VIO rail monitors: GPIO 41 and GPIO 45 (display-side ADC)

The display RP2350 can measure the two GPIO-header supply rails itself, so VREF
work does not need a scope. From `rpADC::initFW2Display()` +
`fwAboutPanelVREF.cpp` in the stock FreeWili 2 firmware:

- **GPIO 41 = ADC input 1** — programmable Vout monitor ("Vout")
- **GPIO 45 = ADC input 5** — GPIO header VIO monitor ("Vio")

Both sit behind a 2:1 divider, so millivolts = `adc_read() * 6600 / 4095` at the
12-bit default. Neither pin is claimed by `bsp/platform/board.h`; there is no
`PIN_*` define for them yet. Confirmed on hardware 2026-07-26 (the VIO tap
tracked every `ioexp_vref()` selection).

## Radio: GDO0 capture runs on PIO2, not PIO0

`bsp/radio/gdo_capture.c` sets `s_pio = pio2` and calls `pio_set_gpio_base(pio2, 16)`
so it can reach GDO0 = GPIO32 (the PIO GPIO-base window must move to 16..47).
`pio_set_gpio_base` shifts the base for the WHOLE PIO block, so this cannot run on
`pio0` (audio I2S, GPIO 4–7) or `pio1` (WS2812 LEDs, GPIO 21) without breaking their
low-GPIO access. The BSP is built with
`PICO_PIO_USE_GPIO_BASE=1` (a PUBLIC compile def on `freewili2_bsp`) — required for any
GPIO≥32 PIO access. The subghz source used `pio0`; the `pio0`→`pio2` change is the only
functional edit made during the harvest. GDO2 (GPIO37) remains unused. The capture DMA
is ENDLESS and polled (`write_addr`), so it registers no `DMA_IRQ_0` handler.

### pio2 cohabitation: radio + IR (harvest, 2026-07-06)

`pio2` is no longer radio-exclusive: the harvested `bsp/ir/` driver also runs
on it — `ir_capture_init()` claims SM0 (`ir_capture.pio`), `ir_tx_init()`
claims SM1 (`ir_tx.pio`). Across all three programs that can live on pio2 —
`gdo_capture` (11 instructions), `ir_capture` (11 instructions), `ir_tx` (9
instructions; confirmed via the generated `build/bsp/ir_tx.pio.h`'s
`.length = 9`, not the 8 an earlier estimate assumed) — pio2's 32-instruction
memory holds **31 of 32 slots**, 1 free.

**Init-order hazard:** `gdo_capture_init()` calls `pio_set_gpio_base(pio2, 16)`
to reach GDO0 = GPIO32, but does not check that call's return value. The Pico
SDK's `pio_set_gpio_base()` fails once any program already occupies pio2's
instruction memory — the base can only be (re)set before the PIO block holds
program data. So if an app calls `ir_capture_init()`/`ir_tx_init()` (which
load `ir_capture`/`ir_tx` into pio2) **before** `gdo_capture_init()`, the
later `pio_set_gpio_base(pio2, 16)` call silently fails, the gpio_base stays
at 0, and `gdo_capture` silently ends up capturing the wrong pin instead of
GDO0 = GPIO32 — no crash, no diagnostic, just wrong data. Radio-first
ordering (`gdo_capture_init()` before `ir_capture_init()`/`ir_tx_init()`)
avoids this because IR's pins (GPIO20/24) remain reachable from the
gpio_base=16 window the radio path establishes. **Any app that combines
radio and IR must call `gdo_capture_init()` first.** No current app in this
repo combines the two (see `docs/drivers/ir.md` § Dependencies).

## PDM microphones (increment 2, 2026-07-04)

- **PDM clock is 1.024 MHz, not the datasheet-typical 3.072 MHz.** The FW2
  MEMS mics did not output data at 3.072 MHz on this board (measured in the
  `microphonearray` repo; matches the known-working movieplayer mic). 1.024 MHz
  × CIC decimate 64 → 16 kHz PCM. `PDM_CLK_HZ` lives in `bsp/platform/board.h`.
- **RP2350 pad-isolation trap:** input pads power up with the input buffer
  disabled AND the ISO latch (PADS bit 8) engaged — the PIO reads stuck-0 and
  the PDM stream decimates to pure DC. `pdm_capture.pio`'s init explicitly
  enables the input buffers and clears ISO on GPIO 29/30.
- **Mic power is PCAL6524 P1 bit 7 (`MIC_PWR`, active-high)** — off at
  power-on and after `ioexp_init()`; `pdm_capture_init()` drives it on via
  `ioexp_mic_pwr(true)` and waits 50 ms.
- **Physical left-to-right mic order is D, B, A, C** (bench-measured phase
  ramp in the `microphonearray` repo), not A, B, C, D. Channel indices MIC_A..D
  are line/phase order: SIG1-high, SIG1-low, SIG2-high, SIG2-low.
- **Overrun semantics:** the capture DMA free-runs into a 32 KiB ring
  (~64 ms of slack). A consumer stalled longer than that silently loses/tears
  the stalled span — there is no overrun flag. Single consumer only (shared
  CIC state).

## I2C sensors (increment 4, 2026-07-04)

- **Guard convention:** `bsp/sensors/*` splits pure logic from hardware with
  `#ifdef PICO_BUILD` — the Pico SDK defines `PICO_BUILD=1` for every target
  build (sdk 2.3.0 `src/rp2350/pico_platform/CMakeLists.txt:11`) and the host
  CTest tree does not, so the guards work with zero configuration. This
  coexists with the repo's older `#ifndef HOST_TEST` convention (ook_tx,
  scan_engine): HOST_TEST requires the test target to define it; PICO_BUILD
  requires nothing. Both are valid; new harvests should keep whichever the
  source repo uses.
- **BMI323 and BMM350 I2C reads return 2 leading dummy bytes** (confirmed on
  hardware in the source repo). The drivers account for it; anyone writing
  raw I2C to these parts must too.
- **OPT4001 lux factor is package-dependent** (437.5e-6 in the driver, the
  datasheet value for the common package). Absolute lux should be calibrated
  against a known light level; relative changes are trustworthy regardless.
- **BMM350 init takes ~130 ms of mandatory settles** (soft reset 24 ms, OTP
  download, magnetic reset 14+18 ms, PMU steps, normal mode 40 ms). Do not
  "optimize" the sleeps — the sequence is ported from the hardware-validated
  fwcom protocol notes.
- All four sensors are blocking polled I2C1 (400 kHz) — no DMA/IRQ/PIO. The
  SHT40 high-precision measure blocks ~10 ms per read.

## PSRAM moved to the SDK's hardware_psram (2026-07-26, SDK 2.3.0)

The 130-line hand-rolled APS6404L bring-up (harvested from `evaderkrub/usbcamfw`)
is gone; `bsp/platform/psram.c` is now a shim over the SDK. Four facts that cost
real reading of the SDK source to establish:

- **Boot-time PSRAM timing goes stale when you overclock.** `runtime_init` brings
  PSRAM up before `main()`, at the boot `clk_sys`, and `psram_configure_params()`
  derives divisor/rxdelay/select/deselect from `clock_get_hz(clk_sys)` at call
  time. Nothing in `hardware_clocks` re-runs it on a clock change, so
  `board_init_clk()` must re-time PSRAM after `set_sys_clock_khz()`.
- **`psram_configure_params()` alone does nothing to the hardware.** It ends in
  `psram_set_params()`, which only stores the values in file statics. The QMI
  `m[1].timing` register is written by `psram_initialize_internal()`, reached only
  via `psram_reinitialize()`. Re-timing therefore needs BOTH calls — this is the
  easy way to "fix" the timing and have nothing change.
- **`psram_reinitialize()` is documented unsafe while executing from flash or
  PSRAM**, or with IRQ handlers/vector table in flash or PSRAM. It is safe in
  `board_init_clk()` only because every app executes from RAM (invariant 2) and
  core1 has not started yet. Keep that in mind if a future app is ever linked
  to run from flash.
- **The linker's PSRAM region ORIGIN is `0x11000000` — the same address as
  `PSRAM_BASE`.** Setting `PICO_PSRAM_SIZE_BYTES` sizes that region, so any
  `__uninitialized_psram` variable is placed from `0x11000000` upward and a raw
  `(uint16_t *)PSRAM_BASE` pointer aliases it. Allocate PSRAM buffers with
  `__uninitialized_psram("group")`, never by casting `PSRAM_BASE`.
  `psram_selftest()` now tests above `__psram_end__` for the same reason.

Timing values changed with this migration: the old driver ran the APS6404L at
`clk_sys/3` (83.3 MHz at 250 MHz) with rxdelay 1, min_deselect 13 and
SELECT_HOLD 3; the SDK defaults give `clk_sys/2` (125 MHz) with rxdelay 3,
min_deselect 4 and no SELECT_HOLD. The SDK path is Raspberry Pi's APS6404
reference (same chip and CS pin as `adafruit_fruit_jam`), but it is **faster than
anything previously run on this board** — `psram_selftest()` on hardware is the
gate before trusting it. Note `psram_set_params()` cannot express SELECT_HOLD at
all; the SDK's register write leaves it 0.

**Reporting artifact:** `.psram_noload` is NOBITS, so flat `arm-none-eabi-size`
folds PSRAM into `bss` (`hello_keyboard` shows 311 KB bss for 4 KB of real SRAM
bss). Use `size -A` when checking the 512 KB SRAM budget.
