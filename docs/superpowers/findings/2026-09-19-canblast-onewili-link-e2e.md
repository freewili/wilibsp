# canblast: CAN FD at high rate through the OneWili display link (2026-09-19)

## Setup

- FreeWili 2 FX0103 (MAIN FW2 v07 built 2026-09-19 from the firmware working
  tree, DISPLAY running `apps/canblast` v001 loaded over the on-board debug
  probe with `fw ramrun canblast`).
- Bus partner: Intrepid ValueCAN 4-2 driven by VSpy AI through its MCP server
  (the `canfdvalidation` harness, `python -m canfdval.display_bench`). CAN FD
  500 kbit/s arbitration, 2 Mbit/s data, unlimited TX retry.
- Every frame carries a self-checking payload (sequence, run id, pattern,
  XOR), so loss, duplication, reordering and corruption are measured, not
  inferred. The app scores what it receives itself and reports counters over
  SEGGER RTT channel 0; what it sends is scored from the ValueCAN capture.
- The bench talks to the app over RTT (OpenOCD, port 9090) and drives VSpy AI
  over MCP. Reports: `canfdvalidation/reports/display-full-2.md` and the
  `disp-*.md` single runs.

## What was verified on hardware

| Direction | Frames | Rate seen | Delivery |
|---|---|---|---|
| ValueCAN -> display app, 64 B FD, back to back | 2000 | 2913/s | 100% |
| ValueCAN -> display app, 8 B classic, back to back | 2000 | 4237/s | 100% |
| ValueCAN -> display app, 64 B FD, 1 ms pace | 2000 | 998/s | 100% |
| DLC sweep 0..64 B and classic 0/1/8 B, 200 each | 23 runs | 2500-4500/s | 21 of 23 at 100% (see residual) |
| display app -> ValueCAN, 8 B classic, synchronous writes | 500 | 1435/s | 100% |
| display app -> ValueCAN, 64 B FD, synchronous writes | 500 | 770/s | 100% |
| display app -> ValueCAN, periodic slot flood 64 B, 5 s | 14393 | 2879/s | 100% |
| display app -> ValueCAN, periodic slot flood 8 B, 5 s | 20400 | 4080/s | 100% |
| both directions at once, 500 + 500 x 64 B | 1000 | - | 100% |

Screen capture during a 64 B flood in both directions:
`2026-09-19-canblast-under-load.png` (RX counter climbing at ~2800/s; the TX
counter shows one-shot writes only, so a periodic-slot flood reads 0 there).

For comparison, the same firmware driven from a PC over the USB console
(OneWili Python, `canfdvalidation` reports of 2026-09-18) sustains ~400
one-shot writes/s at 8 B and 170-240/s at 64 B: the display link is 3.5x
faster for transmit because commands do not cross USB CDC.

## What had to change to get there

Display side (`libs/onewili`, all tagged `LOCAL ADDITION`, and `apps/canblast`):

1. **Interrupt-driven receive.** The generated transport drained UART0 only
   from inside `ow_poll_*`/`ow_binary_poll`. The FIFO is 32 bytes = 40 us at
   8 Mbaud, so any LCD draw stalled MAIN on CTS, and a stalled MAIN drops CAN
   frames. Now UART0_IRQ moves bytes into a 32 KB ring; the frame parser and
   the per-stream FIFOs (text 8 KB, binary 32 KB) run from the app's context.
   `ow_fwgui_get_stats()` exposes ring/FIFO high-water marks and overruns.
2. **Pipelined transmit** (`onewili_fast.h`): several `write_canfd` commands
   in flight, matched to responses by order, with a wire-byte budget (MAIN's
   display RX ring is 2 KB). Measured: depth 4 gives ~1580/s at 8 B versus
   ~1435/s synchronous, at the cost of reordering when MAIN refuses a frame
   (its one-shot FIFO holds 16 and it refuses rather than queues). The app
   defaults to synchronous.
3. **Response stash.** `ow_poll_text_line` drops any response frame it
   completes (no synchronous call is waiting). With pipelined commands that
   ate the responses; the library now keeps them for `ow_raw_next_response`.
4. **Power.** A standalone display app owns power policy: the app holds zone
   15 (CAN) with `picpwr_keep_awake` and tells MAIN the live rail mask with
   `ow_fwgui_send_power_zones` every second. MAIN retries its CAN controller
   init once a second until the rail is up.

MAIN firmware (stock firmware source, changes uncommitted at the time of writing):

5. Binary `canRxReport` frames are mirrored onto the display link when the
   OneWili bridge is active (they were only sent to the FTDI host port).
6. The display UART transmits from its DMA double buffer instead of a
   blocking FIFO write (~130 us per mirrored CAN frame); the transfer starts
   as soon as data is queued. Before this, 64 B frames above ~2500/s and 8 B
   frames above ~3000/s were lost on MAIN.
7. The display->MAIN frame accumulator rejects a header claiming more than
   256 bytes at once. It used to wait for up to 32 KB, so a frame cut off
   when the display CPU was halted for a reload swallowed every command the
   fresh app sent (the app then reported `link=0`).
8. The console prints nothing to USB while a command is captured for the
   display bridge. Colour escapes and `printOut` leaked, and with a host
   that had opened the USB console and stopped reading (the flash tool, a
   probe script) every leak blocked for the 20 ms stdio timeout: synchronous
   writes fell from ~1500/s to 46/s.

Tooling (`tools/fw.py`, `tools/openocd/ramrun.tcl`):

9. `fw flash` cannot start a `no_flash` app (OpenOCD has no flash bank at
   0x20000000 and its reset boots the stock firmware). `fw ramrun <app>`
   boots the stock firmware, halts both cores, parks core 1 in a spin loop
   with interrupts masked, quiesces the watchdog / NVIC / SysTick / DMA,
   loads the ELF, jumps through its vector table and clears TIMER0's
   debug-pause bits. A cold start from the bootrom panics instead (the app
   expects the loader's QMI/PSRAM state), and a core left in debug halt
   freezes TIMER0 for the running core.

## Residual and gaps

- Short frames at the very top offered rate still lose a few frames on MAIN
  (in the final matrix: 3 of 200 at DLC 12, 7 of 200 classic DLC 0; earlier
  runs lost at DLC 0/2/5/7). No display-link drops were recorded in any run
  (`drop=0 ovr=0 hwovr=0`), so this is the MAIN main-loop residual already
  known from the USB-path validation. ISR-driven CAN receive on MAIN is the
  next step there.
- `fw screenshot` during traffic loses frames: the agentio capture holds the
  app's main loop for seconds, the 32 KB ring overflows (`ringmax=32768`).
  Capture between runs, or make the capture incremental.
- A fresh launch occasionally records one link checksum error: the stock
  firmware's frame cut off by the halt. Harmless, counted, not repeated.
- The bench must not leave an OpenOCD behind: a stray instance holds the
  probe and serves an RTT session bound to the previous image, which looks
  like a silent app. `canfdval.wili_display` now kills strays and re-attaches.
- After a MAIN reflash the on-board probe re-enumerates for a few seconds.
- The `libs/onewili` changes live in the submodule working tree and are
  tagged `LOCAL ADDITION`; the package is generated by menutool and must
  learn them at the source.
