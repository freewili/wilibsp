# TV output, stage 2: the app's framebuffer, page flip, plug detection (2026-11-14)

**Status: verified on a board 2026-11-14 (see "Bench result" at the
end).** The record above the bench result was written before that run and
is left as written:

**Status when written: built and host-tested, not run on a board.** Nothing below is a
hardware result yet. The bench steps at the end say what to run and what a
pass looks like.

## What was built

- `tvout_start(pixels, w, h)`: the app's RGB565 framebuffer (high byte
  first), read in place from SRAM through the compact list (six DMA channels:
  pixel, list, rewind, join, row, restart), centred in 640x480 with black
  around. Refuses a buffer not wholly in SRAM, an odd width, or a picture
  larger than 640x480.
- `tvout_show(pixels)`: page flip. Reads the scan position from the DMA
  channels (the stock firmware's scan-position reading, ported to C) and,
  with interrupts off, rewrites every row's read address while the scan is
  between the end of the last row and three scan pieces before the first.
  A rewrite whose closing scan position is outside that window counts in
  `tvout_get_stats()`'s `flips_late`. Times out after 50 ms.
- `tvout_task()`, `tvout_tv_connected()`, `tvout_set_auto()`: the
  coprocessor's hot-plug flag behind the stock firmware's settle times (300 ms
  connected, 1000 ms removed); settled changes returned as events; automatic
  start off by default.
- The static list is one 8,576-byte block shared by the bars and a picture
  (the largest picture list is 8,568 bytes; the bars take 8,400).
- `apps/hello_tvout` v002: 480x320 one-buffer mode, 320x240 page-flip mode,
  colour bars (BLUE steps); PASS/FAIL lines in the picture and over RTT;
  GREEN 50 stop/start cycles of the running picture; YELLOW automatic start.
- `apps/hello_tvout_psram`: the same source built with `fw2_psram_app()`,
  the decision-6 test.
- Host tests: `tests/test_tvout_list.c` (every picture size from 1x1 to
  642x481 sized or refused correctly and fitting the block; row read-address
  words; window bounds), `tests/test_tvout_plug.c` (settle times, short drops
  and blips, timer wrap), and a page-flip check in `tests/test_dvi_frame.cpp`:
  on the DMA model, at every scan position read in one step that opens the
  window, the frame being sent has read every row's address or none, for all
  nine DVI picture sizes in that test.

## Build evidence

| Check | Result |
|---|---|
| `fw test` | 53/53 passed (one new test executable; two extended) |
| `fw build hello_tvout` | OK; `.text` 42,724, `.rodata` 3,508, `.data` 1,712, `.bss` 326,588 bytes (framebuffer 307,200, list block 8,576); ends below `0x20070000` |
| `fw build hello_tvout_psram` | OK; `main` and the drawing code at `0x1100xxxx`, `s_fb`, `s_list` and every `tvout_*` function in SRAM; `.sram_bootstrap` 38,080, `.psram_text` 7,200; `hello_psram_exec`'s layout check passes |
| Every app (`cmake --build --preset target`) | All 21 apps built |
| `check_app_uf2.py` (BSP and firmware checkers) | `hello_tvout`: 188 SRAM blocks; `hello_tvout_psram`: 191 PSRAM blocks; QSPI flash untouched |

## What rests on what

| Claim | Basis |
|---|---|
| A 480x320 SRAM picture through the compact list with the FIFO never below 6 of 8 | Stock firmware bench record (two minutes); not run with this BSP |
| The compact list's channel set-up, control words and scan-position reading | Same as the stock firmware's TV output; host-tested here on the DMA model |
| Page flip timing: the rewrite (one store per row, interrupts off) finishes inside three scan pieces | Estimate; only the window rule is host-tested |
| 300 ms / 1000 ms settle times | The stock firmware's values |

**UNVERIFIED HW:**

- Hot-plug polarity (1 = TV connected). It rests on a firmware comment and
  a 2026-11-12 bench note in the stock firmware's records; the BSP's own
  keyboard doc calls the flag polarity unverified.
- Which coprocessor firmware boards carry. A connected TV reads correctly
  only with the firmware that sets the hot-plug input to TTL levels; older
  firmware reads 0 with a TV connected, so `hello_tvout` never prints "TV
  connected". The plug test tells the two apart.
- Whether code fetched from PSRAM by a PSRAM-executing app disturbs the HSTX
  feed (decision 6). `hello_tvout_psram` is the test.
- Whether the LCD's SPI flush by DMA from SRAM, running beside the six video
  channels, keeps the FIFO at 6 or more. The firmware record above did not
  cover this combination.
- Whether `tvout_show()`'s rewrite finishes inside the margin: `flips_late`
  counts the ones that did not.

## Not done

- PSRAM framebuffers (stage 3), a tear-free mode for one buffer, sound,
  CEC.
- `tvout_show()` waits for the window by polling; there is no interrupt-driven
  flip.

## Bench steps (owner)

1. Install `hello_tvout.uf2` and `hello_tvout_psram.uf2`, TV on the TV
   connector, launch `hello_tvout`, `fw rtt`.
2. Pass: the LCD and the TV show the same picture: text lines, four colour
   ramps (red, green, blue, grey) and a moving block; after 10:00 RTT prints
   `10 minutes 480x320 ONE BUFFER: PASS`.
3. BLUE: the 320x240 page-flip mode. Pass: after 10:00
   `10 minutes 320x240 PAGE FLIP: PASS`, and the picture reads
   `FLIP LATE 0 MISS 0 PASS`. BLUE again: the colour bars.
4. Unplug and replug the TV cable. Pass: `TV removed` then `TV connected`,
   each within about 1 s. Never `TV connected` with a TV plugged in: the
   coprocessor firmware is the older one.
5. GREEN: `CYCLES 50/50 PASS`.
6. Steps 1-3 with `hello_tvout_psram` (line 2 reads `PSRAM APP`): the same
   pass lines mean a PSRAM-executing app can show a full-size SRAM picture.

## Bench result, 2026-11-14 (owner, FreeWili 2, TV on the TV connector)

All steps passed: the 480x320 one-buffer picture on LCD and TV, the 320x240 page flip
(`FLIP LATE 0 MISS 0 PASS`), `TV removed` / `TV connected` on unplug and replug (so this board
has the TTL-input coprocessor firmware), and `hello_tvout_psram` showing the same picture: a
PSRAM-executing app with its framebuffer in SRAM works next to the TV output.

## Note, 2026-11-16

The bench apps named above were folded into one example per driver: `apps/hello_tvout` is now a short introduction (320x240 page flip on TV and LCD, 2x, plug events, a written tone) with no PASS/FAIL lines, and `hello_tvout_psram`, `hello_tvout_ring` and `hello_tvsound` were removed. The results recorded here were measured with the apps as they were then (in git history before commit "chore(apps): one TV output example app, as for the other drivers").
