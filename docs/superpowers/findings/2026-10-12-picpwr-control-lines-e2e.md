# picpwr: rail changes from an app leave a running CM0 alone (2026-10-12)

Before this fix every `picpwr` frame sent the control lines above zone 17 as
zero. Zone 19 is `CM0_RUNPG`, the compute module's run line, which MAIN raises
when it starts Linux; the device drives it to the frame's bit on every apply.
So any rail change from a display app reset a running CM0 (and drove PIC
`RG3`, zone 18, low). `picpwr_send()` now repeats both lines' levels from
the last two status frames (high in either stays high; `CM0_RUNPG` only while
zone 17 stays on) and sends nothing before a status frame has arrived.

## Setup

- FreeWili 2 (board revision not determined) with a FW2 v09 MAIN build,
  CM0 running Linux (powered and started from MAIN before each run).
- DISPLAY: throwaway RAM apps built against this BSP, started with
  `fw ramrun`, observed over RTT. The CM0 was checked from the PC through its
  USB gadget console (`/proc/uptime`); MAIN through `h\p\g` and `h\p\n`.

## Before: the old `picpwr` resets the CM0

An app that called `picpwr_keep_awake()` for zones the CM0 path already had on
and then `picpwr_release_unused()` launched with the audio rail inherited on.
The release sent one frame (audio off, control lines zero). From then on the
CM0's USB gadget was gone and MAIN's `l\u status` got no reply; it stayed that
way after the app exited, until the board was power cycled. An earlier app
with the same two calls lost the CM0 the same way. The same app with both
calls removed (no frame sent) ran with the CM0 up throughout.

## After: off, on, off with the fix

A test app switched the audio rail off (`picpwr_release`), on
(`picpwr_keep_awake`) and off again, 6 s apart, logging every change in the
status frame's rails and control lines. It ran twice: first with the lines
taken from the latest status frame alone, then with the final two-frame rule
(`picpwr_lines_carry()`). Both runs logged the same sequence.

| Check | Result |
|---|---|
| Frames sent | 3 (two `picpwr: released rails 00004`, one keep-awake) |
| Audio rail in the status frame | `1A3E7` -> `1A3E3` -> `1A3E7` -> `1A3E3`, as commanded |
| `CM0_RUNPG` in every status frame | 1 (released) before, during and after |
| `WIO_RST` in every status frame | 0, unchanged (the level it had before launch) |
| CM0 uptime across the run | 1060.9 -> 1118.6 s, then 2782.3 -> 2819.5 s: no reset |
| CM0 USB gadget | present throughout |
| MAIN `h\p\n` (WIO_RST, CM0_RUNPG, MAIN_PWR_RST levels) after | `0 1 1`, unchanged |

Host tests: `test_picpwr_frame` covers the frame placement of the lines
(checksum included), their decode, the two-frame and zone-17 rules of
`picpwr_lines_carry()`, and that zone 20 and everything above it never reach
the wire.

## Not verified

- `WIO_RST` was low for the whole run, so only "a low line stays low" was
  exercised for it, not "a released line stays released". Whether the old
  code affected the LoRa module was not tested; zone 18 reaches its reset
  only on rev 1 boards (`docs/drivers/lora.md`).
- Whether the `CM0_RUNPG` readback is the PIC's latch or the shared
  run/power-good net was not established, and no CM0 reboot was performed
  while an app ran (a dip longer than two status frames would be carried).
- `picpwr_cycle()` was not run on hardware; it sends through the same
  `picpwr_send()`.
- MAIN's own zone readout (`h\p\g`) still showed the audio rail on after the
  app had switched it off: under a BSP app the stock firmware's zone event
  that keeps MAIN's copy current is gone, so MAIN's view goes stale. The
  status frame read by the app is the source of truth.
