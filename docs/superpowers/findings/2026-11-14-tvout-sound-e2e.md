# TV output, stage 4b: sound over audio framing (2026-11-14)

**Status: built and host-tested, not run on a board.** Nothing below is a
hardware result yet. The bench steps at the end say what to run and what a
pass looks like.

## What was built

- `tvout_set_audio_framing(on)`: Audio framing (data islands, AVI and
  audio InfoFrames, audio clock regeneration at 48 kHz) for the next
  `tvout_start_test()` or `tvout_start()`; off by default. The 5,240-byte
  island pool is static in `tvout_audio.c` and linked only into apps that call
  this. With audio framing a picture's compact list is shorter (lines in the
  pool go out as one entry): `tvout_list_picture_bytes()` and the list layout
  follow the framing.
- `tvout_audio_start()` / `tvout_audio_stop()`: the feeder. One TIMER1 alarm,
  claimed once and kept, its interrupt at the highest priority, runs the
  stock firmware's tick: read the scan position, make the next lines'
  packets, write each into its slot once the TV has finished the line that
  used it before. Stop puts every slot back to a null packet, a slot at a
  time, then disarms. `tvout_stop()` stops the feeder first.
- `tvout_audio_tone()`: the built-in 1 kHz tone (-12 dBFS) left, right or
  both, or the written samples.
- `tvout_audio_write()`: the app's own 48 kHz stereo samples into the
  feeder's 128-frame buffer.
- `tvout_audio_get_stats()`: packets written, underruns, late lines, repeats,
  lost frames.
- The scan position for the feeder (`tvout_scan_entry()`): the bars' list
  channel read address, or the compact list's reading stage 2 already uses for
  page flips. Both run from SRAM.
- Harvested unchanged (C++): `rpDVIAudioCore.h`. C entry point:
  `tvout_audio_core.{h,cpp}`. Hardware glue in C: `tvout_audio.c`. Small
  additions to `tvout.c` (framing pool, scan entry, the stop order) and
  `tvout_list.cpp` (framing flag, list size with framing, frame accessors).
- `apps/hello_tvsound`: a 480x320 SRAM framebuffer on the LCD and, with
  audio framing, the TV. GREY/YELLOW/GREEN the tone left/right/both, BLUE a
  500 Hz tone written with `tvout_audio_write()`, RED restarts the picture
  with plain DVI framing or back. The results are text lines in the picture
  (so the TV shows them too) and over RTT, with a summary line every 10 s and
  a 10-minute verdict.

## Host tests

| Test | Covers | Result |
|---|---|---|
| `test_dvi_audio_framing` (firmware's, ported unchanged) | Audio framing and packet coding checked by an independent decoder | 310,019 checks, 0 failures |
| `test_dvi_audio` (firmware's, ported unchanged) | The feeder on a simulated TV and interrupt with random delays: no torn, wrong-line, stale or repeated packet; counts | 40,689 checks, 0 failures; tick max 23.2 us in the model |
| `test_dvi_frame` feeder section (restored from the firmware's test; stage 1 had left it out) | The feeder on the DMA model of each picture's compact list | 12,776,155 checks in the file, 0 failures |
| `test_tvout_audio` (new) | The pool covers the bars (990 words) and every picture size (1,310 at most); every picture list with framing fits the block (4,264 bytes at most); a DVI frame and a short pool are refused; the bars with the tone for 10 frames: about 200 packets a frame, no late, lost or repeated line, every slot null after stop; a 480x320 picture with written samples kept topped up: no underrun; left to run dry: underruns counted | 0 failures |
| `fw test` | Whole host tree | 56/56 passed |

## Build evidence

| Check | Result |
|---|---|
| `fw build hello_tvsound` | OK, no warning from the new files; `.text` 43,384, `.data` 3,812, `.bss` 328,656 bytes (framebuffer 307,200, list block 8,576, pool 5,240, feeder state 312 + line table and sample buffer 1,040) |
| RAM placement | `tick_irq`, `tvout_audio_core_tick` (1,828 bytes, the core inlined), `tvout_scan_entry`, `scan_piece` in SRAM (`0x2000xxxx`) |
| Every app (`cmake --build --preset target -j 2`) | All 22 apps built; `hello_tvout` (no sound) does not link the framing pool |
| `check_app_uf2.py` (firmware checker) on every app UF2 | All pass; `hello_tvsound`: 199 SRAM blocks, QSPI flash untouched |

## What rests on what

**Correction (2026-11-14, later the same day):** the first row of the table
below says no bench record of the stock firmware's TV sound was found. There
is one: the stock firmware's 1 kHz test tone played on a TV with its screen
mirror running, with no flicker and no restart (stock firmware TV-output
bench record, 2026-11-13). The row is left as first written. That record is
for the firmware, not this BSP: nothing here has run on a board.

| Claim | Basis |
|---|---|
| The framing, slot map, packet coding and tick timing play on a TV | The same core as the stock firmware's TV sound (its built-in tone), host-tested there and here. No bench record of that tone on a TV was found in the project notes read for this stage; treat it as unproven on hardware |
| The feeder keeps up at 250 MHz | Tick budget is set for 240 MHz (about 23 us); 250 MHz is faster. Not measured |
| `tvout_audio_write()` delivers what it is given | Host-tested only. The firmware's equivalent call had no caller, so this path has never run on any board |

**UNVERIFIED HW:**

- TIMER1 counts microseconds in a BSP app. `tvout_audio_start()` checks it
  over 200 us and refuses otherwise; the refusal message says so.
- Sound over a picture from `tvout_start()` (the compact list with audio
  framing). Host-tested on the DMA model only; the BSP's picture path has not
  run on a board at all yet (stage 2 is unverified too).
- Any sound at all on a TV from this core: see the first row above.
- The feeder next to stage 2's page flip: `tvout_show()` turns interrupts off
  for its rewrite, holding the tick off for that time. Not measured.
- A plain DVI monitor with audio framing on: may go blank. Restarting with
  RED (plain DVI) should bring the picture back.
- In a PSRAM-executing app, the core's rarely taken paths (missed lines,
  stopping) and `tvout_list_scan_piece()` are in the image's code region,
  not forced into SRAM. No PSRAM sound build exists.

## Not done

- The stock firmware's second source, the TV playing what the codec plays by
  reading the codec's own DMA buffers, was not carried over. The firmware's
  reader expects two mono buffers of a power-of-two length played in turn;
  the BSP's I2S driver loops one stereo buffer (`[L16|R16]` frames) whose
  streamed length is a multiple of 8192 frames, at 16 kHz. Carrying it over
  means a new reader in the harvested core and a hook from `bsp/audio` into
  `bsp/display`; an app gets the same sound by writing it to both.
- No automatic framing from the display's EDID: the BSP has no EDID.

## Bench steps (owner)

1. A TV that plays sound from the TV connector. `fw install-app
   hello_tvsound.uf2` (or `fw ramrun hello_tvsound`), launch, `fw rtt`.
2. Pass: the TV shows the same picture as the LCD; "PICTURE AUDIO FRAMING
   PASS", "SOUND FEEDER PASS", "PACKETS n PASS" with n rising every second.
3. GREY: a 1 kHz tone from the TV's left speaker only; YELLOW: right only;
   GREEN: both; press the lit button again: silence. BLUE: a lower (500 Hz)
   tone on both; "UNDERRUN" should stay at or near 0.
4. Pass over 10 minutes: "LATE 0 LOST 0 REPEAT 0 PASS", "FIFO MIN n/8 PASS"
   with n at least 6; RTT prints "hello_tvsound: 10 minutes: PASS".
5. RED: the picture restarts with plain DVI framing ("SOUND OFF: NO AUDIO
   FRAMING"); with a DVI-only monitor attached the picture shows. RED again:
   audio framing back.
6. Hold HOME five seconds: the stock firmware returns.

## Note, 2026-11-16

The bench apps named above were folded into one example per driver: `apps/hello_tvout` is now a short introduction (320x240 page flip on TV and LCD, 2x, plug events, a written tone) with no PASS/FAIL lines, and `hello_tvout_psram`, `hello_tvout_ring` and `hello_tvsound` were removed. The results recorded here were measured with the apps as they were then (in git history before commit "chore(apps): one TV output example app, as for the other drivers").
