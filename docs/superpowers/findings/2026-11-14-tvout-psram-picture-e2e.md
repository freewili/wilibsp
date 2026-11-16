# TV output, stage 3: a PSRAM framebuffer through the line ring (2026-11-14)

**Status: verified on a board 2026-11-14 (see "Bench result" at the
end).** The record above the bench result was written before that run and
is left as written:

**Status when written: built and host-tested, not run on a board.** Nothing below is a
hardware result yet. The bench steps at the end say what to run and what a
pass looks like.

## What was built

- `tvout_start(pixels, w, h)` now also takes pixels in PSRAM (cached window
  `0x11000000` or uncached `0x15000000`, 4-byte aligned, width at most
  `TVOUT_RING_WIDTH_MAX`, 480 by default). The compact list is built over
  eight SRAM line buffers and every picture row stays pointed at its line
  buffer (`tvout_ring.c`). A TIMER1 alarm interrupt at the highest priority,
  on the core that started video, keeps the buffers up to six rows ahead of
  the TV: at most three rows a tick, then an 8 µs gap (30 µs when there is
  nothing to fetch). Each row comes from PSRAM through the XIP streaming FIFO
  and a seventh DMA channel paced by the stream DREQ that writes SRAM only.
- `tvout_core1_init()`: on core 1, installs a second TIMER1 alarm interrupt
  at the highest priority; the reader raises it through the force register
  for each row read and core 1 spins in it, off PSRAM, until the row is in.
- `tvout_psram_uncached()`: the uncached alias to draw through.
- `tvout_get_stats()` gains `rows_late`, `row_reads`, `row_read_timeouts`,
  `row_read_us_max`, `reader_us` and `reader_window_us`.
- `tvout_show()` refuses a PSRAM picture.
- Row logic: the stock firmware's TV-mirror row bookkeeping, harvested
  unchanged as `bsp/display/rpDVIMirrorCore.h` (two comment lines renamed to
  BSP files), behind C entry points in `tvout_ring_core.{h,cpp}`, following
  stage 1's `tvout_list` facade. The file keeps its firmware name, as stage 1
  kept `rpDVIFrame`, `rpDVIList` and `rpDVIPacket`.
- `tvout_list_picture_t` gains `pieces` (scan pieces per frame), which the
  row logic needs.
- `apps/hello_tvout_ring`: 480x320 PSRAM framebuffer, redrawn continuously
  by core 0 through the uncached window; YELLOW starts core 1 drawing into
  PSRAM without the hold (the control), GREEN with it.
- Host tests: `tests/test_dvi_mirror.cpp` (the firmware's simulation of the
  row logic on a simulated TV, reader and drawing core, ported, plus eight
  always-on runs of 600 frames each, the PSRAM case) and
  `tests/test_tvout_ring_core.c` (the C entry points).

## The hard rule for app authors

**While a PSRAM picture is on the TV, no DMA may read or write PSRAM**, from
the app or from any driver it uses. A DMA write to PSRAM holds the DMA write
port until the PSRAM completes it, and the HSTX feed's writes wait behind it;
the stock firmware saw every IR edge empty the HSTX FIFO while its IR capture
ring was in PSRAM. That includes `st7796_flush_async()` from a PSRAM buffer:
feed the LCD from SRAM. The rule holds for SRAM pictures too; it bites PSRAM
apps first because their big buffers live there.

Also: draw the picture through `tvout_psram_uncached()`, and call
`tvout_core1_init()` on core 1 before core 1 touches PSRAM if it runs at all.

## Core cost

| Picture | Reader's share of the starting core | Basis |
|---|---|---|
| 480x320 | about 38 % (320 rows × 19.9 µs per 16.7 ms frame), plus a tick overhead of a few µs per three rows | Estimate from the stock firmware's held row read, 19.9 µs for 960 bytes, measured with flash traffic present (board record). The host simulation, rows 19.6–21.5 µs, gives 39.5 %. |
| 320x240 | about 19 % (240 rows × about 13 µs) | Estimate, scaled by row length. |

The design note's "~57 %" for 480x320 multiplied by 480 rows; a 480x320
picture has 320 rows. An SRAM-only app has no flash traffic, so the read may
be faster; not measured. `hello_tvout_ring` prints the measured share each
second (`reader_us` / `reader_window_us`, timed with TIMER1 inside the
interrupt). Core 1, when registered, loses the same time again while held.

## Build evidence

| Check | Result |
|---|---|
| `fw test` | 55/55 passed (two new test executables, one extended) |
| `test_dvi_mirror` | 1,491 checks, 0 failures; the firmware's twelve mirror runs unchanged; always-on quiet runs: 0 stale, 0 part-filled, 0 refilled, 0 late, reading rows 39.5 % of the time |
| `fw build hello_tvout_ring` | OK; `.text` 41,404, `.data` 1,676, `.bss` 23,064 bytes; line buffers 7,680 bytes; `s_fb` 307,200 bytes at `0x1104B000` (PSRAM, after the About screen's backup) |
| Every app (`cmake --build --preset target -j 2`) | All 22 apps built, no warning from the new files |
| `check_app_uf2.py hello_tvout_ring.uf2` (BSP and firmware checkers) | 183 SRAM blocks; QSPI flash untouched; the PSRAM framebuffer is NOLOAD |

## What rests on what

| Claim | Basis |
|---|---|
| The row logic: no line buffer refilled while the TV reads it, rows fetched before the TV reaches them when the reader is served within ~70 µs | Host simulation (firmware's, ported, plus always-on runs) |
| Reading a PSRAM row through the streaming FIFO into SRAM with one DMA channel, the 300 µs read timeout, the core-1 hold | The stock firmware's TV mirror, which used them during LCD redraws on the bench; this BSP runs them all the time, which the firmware never did |
| Held read 19.9 µs; unheld reads averaging 35.7 µs and peaking at 163 µs against a 31.8 µs line | Stock firmware board record |
| Drawing through the uncached window | The stock firmware writes its PSRAM copy that way |

**UNVERIFIED HW:**

- Everything about the always-on ring on a board: rows late, FIFO minimum,
  the reader's real share of core 0, and whether the TV keeps the picture.
- Whether the XIP streaming FIFO sees writes still held in the XIP cache.
  The module tells apps to draw uncached and `hello_tvout_ring` does; drawing
  through the cache was not tried.
- Whether TIMER1 counts microseconds in a BSP app; the start checks it
  (200 µs busy wait) and refuses otherwise.
- The core-1 hold depends on core 1 taking the interrupt within 4 µs; with
  core 1's interrupts off the read goes ahead unheld.

## Not done

- In the always-on simulation under a busy interrupt load the late count
  drifts up to 0.6 % from the wrong lines it stands for (firmware's mirror
  runs: within 0.5 %); the test allows 1 %. It is a health signal, not a
  tally.
- No page flip and no tear-free mode for PSRAM pictures.
- No check that the LCD flush is not running from PSRAM: the rule is
  documented, not enforced.

## Bench steps (owner)

1. `fw install-app hello_tvout_ring.uf2`, TV on the TV connector, launch,
   `fw rtt`.
2. The TV shows colour ramps with a white block moving and blue bands
   drifting, centred, black around. The LCD shows the verdict lines.
3. CORE 1 OFF, 10 minutes: pass is `LATE 0 PASS`, `TIMEOUTS 0 PASS`,
   `FIFO MIN n/8 PASS` with n at least 6. Write down `READER n.n % OF CORE 0`
   and `ROW READ MAX`.
4. Press YELLOW (core 1 draws magenta stripes in the bottom 64 rows, not
   held): `LATE` is expected to grow and the TV may show broken lines. This
   is the control for step 5.
5. Press GREEN (core 1 held), 10 minutes: pass as in step 3; `LATE` restarts
   at 0 for the phase.
6. Hold HOME five seconds: the stock firmware returns.

## Bench result, 2026-11-14 (owner, FreeWili 2, TV on the TV connector)

`hello_tvout_ring` passed: the PSRAM picture (ramps, moving block, drifting bands) on the TV
with `LATE 0 PASS` and FIFO minimum at least 6/8; the YELLOW control (core 1 drawing without
the hold) broke lines as expected; GREEN (core 1 held) was clean again with `LATE` back at 0.

## Note, 2026-11-16

The bench apps named above were folded into one example per driver: `apps/hello_tvout` is now a short introduction (320x240 page flip on TV and LCD, 2x, plug events, a written tone) with no PASS/FAIL lines, and `hello_tvout_psram`, `hello_tvout_ring` and `hello_tvsound` were removed. The results recorded here were measured with the apps as they were then (in git history before commit "chore(apps): one TV output example app, as for the other drivers").
