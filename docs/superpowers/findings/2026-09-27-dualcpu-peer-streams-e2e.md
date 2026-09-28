# dualcpu: DISPLAY and ESP32 apps over OneWili peer streams (2026-09-27)

## Setup

- FreeWili 2 FX0103. MAIN running a development build with OneWili peer
  streams (router on MAIN, FwGUI command/event 0xF1 to the DISPLAY, Bottlenose
  id 103 to the ESP32, text commands `h\a\w` / `h\a\p` / `h\a\c` for the PC)
  and ESP32 binary events (Bottlenose id 102), installed through the
  `FW2Main FBL` loader. `ESP32 Mode` set to OneWili API.
- That build was the pre-release development tree, in which `w\e` and
  `h\a\w/p/c` had stable ids 614-617. The work has since been committed as
  firmware branch `feat/esp32-onewili-streams`, rebased onto
  `release/v08-preview.3`, where ISO-TP holds 614-623 and these commands
  are 624-627. The DISPLAY app calls them by text path and is unaffected,
  but the results below were not rerun on the rebased images.
- DISPLAY: `apps/dualcpu` v001, started with `fw ramrun dualcpu`.
- ESP32-C5: the Bottlenose firmware built with the DUALCPU BSP app (the ESP32
  half of this demo; overlay `freewilibottlenose/sdkconfig.bsp-dualcpu` on the
  firmware branch), flashed over the C5's own USB Serial/JTAG with esptool.
- Observed through RTT channel 0 (`stats:` lines and `=` command replies), an
  agentio screenshot, and the ESP32's USB Serial/JTAG console.

## What was verified on hardware

| Check | Result |
|---|---|
| Stream link comes up (HELLO, then CREDIT) | `MAIN stream link confirmed`, ESP32 link live within a few seconds of launch |
| PING DISPLAY -> ESP32, PONG back, 1 per second | 78 of 78 answered in one 80 s window, 0 lost; round trip 6.05-6.18 ms (MAIN's pump cadence; sub-ms samples when MAIN pumps back to back) |
| ESP32 TELEMETRY -> DISPLAY, 1 per second | every datagram received, no sequence gaps; uptime, temperature, heaps, event counts, LED state and scan results decoded |
| Buttons -> ESP32 (`fw press red`, `fw press ok`) | ESP32 LED set to solid red (64,0,0), confirmed in its telemetry; Wi-Fi scan run, 17 APs, top 4 shown |
| Burst of 150 PINGs back to back (`burst 150`) | first run: exactly 69 accepted (69 x 11 B = 759 of the 768 B credit window, payload-only accounting), 81 refused locally and counted; 1 datagram lost across two bursts (138 accepted). Retest with framing counted (19 B per PING): exactly 40 accepted, 110 refused, per burst; 1 of 80 dropped by MAIN and counted; DISPLAY receive FIFO peak 164 of 2048 bytes; no ring overrun on either end |
| PC -> DISPLAY datagrams through the text route | 25 of 25 delivered |
| `ESP32 Mode` switched to Default and back | ESP32 cut off at MAIN (telemetry stops, link stale within 3 s); live again within seconds of OneWili API |
| Same, held off ~40 s while the ESP32 kept streaming (retest) | afterwards 50 of 50 PINGs answered and 50 more telemetry datagrams with the ESP32's refused count flat: its credit window fully recovered. Over the whole run every PING is accounted for: 417 sent = 375 answered + 42 dropped at the closed gate |
| ESP32 reset into its ROM loader repeatedly (MAIN's `w\a` loader tests) | apps recover by themselves once the ESP32 app restarts |

Screenshot of the running app (retest, after LED green and a scan): `2026-09-27-dualcpu.png`.

The retest ran MAIN, the ESP32 and this app rebuilt with the fixes from a final
review of the stream code (credit accounting now counts link framing, the
DISPLAY's datagram buffer is a 2 KB byte FIFO instead of 16 slots, losses are
judged on the newest CREDIT, gated frames count as consumed, CTS-hold
detection no longer joins separate holds). Details in
`docs/onewili-streams-phase3-4.md` on firmware branch
`feat/esp32-onewili-streams`.

## Found and fixed along the way

- **`fw ramrun` could start an app inside a stock-firmware interrupt handler.**
  `tools/openocd/ramrun.tcl` halts the stock DISPLAY firmware at an arbitrary
  moment and rewrites PC/SP; a halt inside a handler left core 0 in Handler
  mode with that handler's NVIC active bit set, so no interrupt of equal
  priority could ever run. Every app then hung at its first interrupt-driven
  wait: a silent stop after `board: leds` (the `sleep_us()` in
  `ws2812_clear_once()`), with TIMER0_IRQ_3 pending, xPSR IPSR = 27
  (DMA_IRQ_1), PC in `__wfe`. `toggleled` did the same, so it was not app
  specific. ramrun now resumes and re-halts until core 0 is in Thread mode.
  After the fix 11 of 12 back-to-back launches reached the main loop; the one
  miss was a different failure (the app never started, empty RTT buffer).
  ramrun also clears PRIMASK and BASEPRI before the jump, in case the halt
  landed inside a critical section.
- **A stale RTT view is easy to mistake for a hang.** Twice `fw rtt` showed
  only the first lines while the app was in fact running; dumping the up
  buffer from memory (`_SEGGER_RTT` -> aUp[0].pBuffer, WrOff) settles it.

## Hazards noted

- A plain `openocd -f target/rp2350.cfg -c "init; halt ..."` on the DISPLAY
  interface disturbed a running app; use `tools/openocd/freewili2.cfg` and
  read memory without halting where possible.
- A SWD core reset of MAIN while the `FW2Main FBL` loader was installing a
  UF2 wedged MAIN (no USB, flash reads stalled, core would not halt) until an
  RP2350 rescue reset (`target/rp2350-rescue.cfg`) followed by `reset run`.
  Wait for MAIN to re-enumerate as the application before touching it.
- Rebooting MAIN reboots the DISPLAY into its stock firmware, so a
  `ramrun`-launched app has to be relaunched after any MAIN restart.
- RTT channel 0's buffers cannot be resized at run time
  (`SEGGER_RTT_Config*Buffer` only changes flags for channel 0); the app now
  keeps the BSP's compiled-in 1 KB up / 16 B down buffers.
