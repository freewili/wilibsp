# TV output, stage 1: the colour-bar test picture (2026-11-14)

**Status: verified on a board 2026-11-14 (see "Bench result" at the
end).** The record above the bench result was written before that run and
is left as written:

**Status when written: built and host-tested, not run on a board.** Nothing below is a
hardware result yet. The bench steps at the end say what to run and what a
pass looks like.

## What was built

- `bsp/display/tvout.{c,h}`: start, stop, clock plan, HSTX setup, a three-
  channel looped DMA list with no interrupt, FIFO level read.
- `bsp/display/rpDVIFrame`, `rpDVIList`, `rpDVIPacket`, `rpDVIModes.h`,
  `fwPortability.h`: the stock firmware's scan-out list builder, copied
  unchanged (C++). `tvout_list.{h,cpp}` is the C entry point to it.
- `apps/hello_tvout`: bars on the TV; measured clocks, USB/ADC availability
  and the lowest HSTX FIFO level over RTT and on the LCD; GREEN runs 50
  stop/start cycles.
- Refusals: `rfid_reader_start()` refuses while video runs;
  `tvout_start_test()` refuses while the RFID reader runs;
  `board_die_temp_c()` returns `NAN` while `clk_adc` is stopped.
- Host tests: `tests/test_dvi_frame.cpp` (the firmware's frame/list test,
  ported; 12,775,979 checks, 0 failures on Windows MinGW GCC 16.1) and
  `tests/test_tvout_list.c` (the C entry points). The host tree now builds
  C++20 as well as C11.

## Build evidence

| Check | Result |
|---|---|
| `fw test` | 52/52 passed (two new) |
| `fw build hello_tvout` | OK; `.text` 34,136, `.data` 1,652, `.bss` 14,908 bytes; `s_list` 8,400 bytes |
| Every app (`cmake --build --preset target`) | All 19 apps built; `hello_rfid` links only `tvout_running()` from the module (section garbage collection drops the rest) |
| `check_app_uf2.py hello_tvout.uf2` (BSP and firmware checkers) | 152 SRAM blocks, QSPI flash untouched |

## What rests on what

| Claim | Basis |
|---|---|
| USB PLL at 126 MHz for `clk_hstx`, `clk_usb`/`clk_adc` on the system PLL / 5 at 240 MHz, 50/50 stop/start cycles | Stock firmware bench record (a RAM app, 2026-11-11); not run with this BSP |
| The bars' list, HSTX expander settings and pin polarity | Same code as the stock firmware's TV output, which shows the bars on the bench; host-tested here |
| RFID capture plus TV picture runs the HSTX FIFO short | Stock firmware observation; not measured with this BSP |
| A DMA read or write of PSRAM holds up the HSTX feed | Stock firmware lesson (IR ring in PSRAM emptied the FIFO); not measured here |

**UNVERIFIED HW:**

- The 250 MHz path (the BSP default): `clk_usb` and `clk_adc` stopped while
  video runs and restored after. The firmware only ran 240 MHz, where both
  clocks move to the system PLL instead. Nothing here has run the stopped
  path.
- Whether the RP2350 ADC is in specification at 41.67 or 50 MHz: not read
  from any source. The module stops the ADC rather than running it there.
- The FIFO depth of 8 words used in the readout comes from the firmware's
  records, not from a measurement here.

## Not done

- The firmware's audio-feeder test (its sound-slot timing on the list) was
  left out of the port: it needs the audio core, which belongs to the sound
  stage.
- No run-time guard against `hstx_dvi` and `tvout` in one app; documented
  instead.
- TIMER1 availability in a BSP app was not checked (needed from stage 3).

## Bench steps (owner)

1. `fw install-app hello_tvout.uf2` (or `fw ramrun hello_tvout`), TV or
   monitor on the TV connector, launch, `fw rtt`.
2. Pass: eight bars, left to right white, yellow, cyan, green, magenta, red,
   blue, black, filling the screen.
3. Pass: `clk_sys  250000 kHz`, `clk_hstx 126000 kHz PASS` (within 0.1 %),
   `clk_usb 0  clk_adc 0 kHz`, `native USB available: no`, `ADC available:
   no` at 250 MHz; `FIFO lowest N of 8 PASS` with N above 0, printed every
   second.
4. Press GREEN: the TV blinks 50 times over about 5 s; pass is
   `cycles 50/50 PASS`.
5. Hold HOME five seconds: the stock firmware returns, with its own TV output.

## Bench result, 2026-11-14 (owner, FreeWili 2, TV on the TV connector)

`hello_tvout` at the board default 250 MHz: eight bars on the TV; `FIFO lowest 7 of 8 PASS`;
`cycles 50/50 PASS`; `clk_usb 0 clk_adc 0 kHz`, `ADC available: no` (expected at 250 MHz).
The USB PLL at 126 MHz and the stopped USB/ADC clocks at 250 MHz are now verified on the board.

## Note, 2026-11-16

The bench apps named above were folded into one example per driver: `apps/hello_tvout` is now a short introduction (320x240 page flip on TV and LCD, 2x, plug events, a written tone) with no PASS/FAIL lines, and `hello_tvout_psram`, `hello_tvout_ring` and `hello_tvsound` were removed. The results recorded here were measured with the apps as they were then (in git history before commit "chore(apps): one TV output example app, as for the other drivers").
