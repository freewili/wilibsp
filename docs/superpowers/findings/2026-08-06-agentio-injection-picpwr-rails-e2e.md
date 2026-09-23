# Injected button presses vs. picpwr rail state — 2026-08-06

**Result: BUG FOUND AND FIXED.** A single `fw press` took an entire FreeWili 2
off USB — LCD, USB hub, USB-serial bridge and the on-board debug probe all
unpowered — and left it that way across resets and re-flashes. Reproduced
twice, root-caused, fixed in `bsp/input/uartkbd.c`, and the fix verified on the
same board.

**Setup:** FreeWili 2 (RP2350B) over a FreeWili Multiprobe debug probe
(CMSIS-DAP, `VID:PID=0x2e8a:0x000c`), SDK 2.3.0, toolchain 14_2_Rel1,
`FW2_AGENTIO=ON`. The application under test was a display-CPU app that drives
the panel, the touch controller, the WS2812 strip and the audio codec, and that
holds the rails it needs with `picpwr_keep_awake()` + `picpwr_task()`.

## Symptom

After `fw press green`, every FreeWili USB interface disappeared: `lsusb`
reported no `2e8a:*` device at all, so the debug probe was gone along with
everything else. The board did not come back on its own; re-plugging the USB
cable restored the VBUS-gated zones (8, 14, 16) but **not** zones 1 and 2, and a
later session showed the app booting with

```
ioexp: init NAK
ft6336: init NO-ACK id=0x00
codec: i2c write reg 0x00 failed (-2)
```

— every I2C device silent, because the rails under them were off.

## Cause

`synth_frame()` fabricates a well-formed uartkbd frame to carry an injected
button edge. It deliberately preserves the charger bytes and the
connection-detect flags across that frame, because a synthetic frame carries no
information about either. It did **not** preserve the 20-byte status payload —
and that payload is where `picpwr_rails_decode()` reads live rail state. Bytes
6..9 of the synthetic frame are zero, so after any injected press
`picpwr_rails()` reported **every rail off**.

An application holding rails then behaves exactly as designed, on bad data:

1. `picpwr_task()` sees its kept rails missing.
2. It debounces, requiring two consecutive frames to agree. `fw press` sends a
   `TAP` — press *and* release — so two synthetic frames arrive back to back and
   agree perfectly.
3. It re-asserts, building an awake mask from that snapshot.

The awake mask is a **complete** mask: every zone absent from it is switched
off. The mask that went out therefore contained only the zones the app had
explicitly requested, and the coprocessor switched off everything else — the
sensors/IO-expander rail (1), the LCD and touch rail (2), the USB hub (8), the
USB-serial bridge (14) and the on-board debug probe (16).

Because `picpwr`'s re-assert is additive (`cached | live | desired`), a rail
that is off and was never requested can never come back. That is why the state
survived resets: nothing in the system had any reason to raise zones 1 and 2
again.

There is a quieter instance of the same defect at boot. `uartkbd_init()` emits
one synthetic frame to prime the parser, which set `status_valid` with an
all-zero payload — so `picpwr_rails()` reported all rails off from boot until
the first real frame arrived.

## Fix

Save and restore `status_raw` / `status_valid` around the synthetic frame feed,
exactly as the charger and flag fields already were. Both instances above are
covered: at boot the restore leaves `status_valid` false, so `picpwr_rails()`
correctly reports "no data yet" rather than "all rails off".

## Why no app hit this before

The two halves had never been combined in this tree. `apps/hello_audio` uses
`picpwr_keep_awake()` but not agentio injection; `apps/hello_agentio` uses
injection but requests no rails. An application that does both is the first to
meet the bug, and every agent-driven E2E session on such an app would meet it
immediately.

## Verification

Fix flashed to the same board that had been bricked into the bad rail state.
The app requests zones 1, 2, 3 and 10, so the first boot after the fix both
recovered the board and demonstrated the repair:

```
ioexp: init NAK                          <- board_init(), before the request
app: rails=0xA2C7 wanted=0x207 up
ioexp: init ok
ft6336: init ok id=0x64
app: codec probe ok, fs=16000
```

The rail mask went from `0xA2C4` (zones 1 and 2 down) to `0xA2C7`, with no
power cycle. `fw press green` was then issued repeatedly — dozens of injected
presses across several sessions — with `lsusb` checked after each: the board
stayed on USB, and the live rail mask read by the app never changed.

## Notes for anyone doing this again

- **A capture is not proof the panel is lit.** Through the whole bad-rail
  period, `fw screenshot` kept returning plausible-looking images, because a
  capture walks the agentio PSRAM shadow — what the driver was *told* to draw.
  It cannot see that the LCD rail is off. `docs/drivers/agentio.md` says this;
  it is worth believing.
- **Print the rail mask.** An app that logs `picpwr_rails()` periodically turns
  this class of failure from a guessing game into one line of output. A dark
  panel, a silent speaker or an unlit LED strip is far more often an unpowered
  zone than a driver fault.
- **Request every zone you depend on, including the boot-on ones.** An app that
  only asks for the zones that are off at boot has no protection against a rail
  it uses being switched off by anything else, and additive re-asserts mean it
  can never recover one. Asking for zones 1 and 2 is what repaired this board.

## Not covered

The fix is in `bsp/input/uartkbd.c`, which has hardware includes and is not
host-testable, so there is no direct regression test for `synth_frame()` itself.
`tests/test_uartkbd_inject.c` gained a case pinning the parse-layer behaviour
the fix depends on — that the status payload is latched from every
checksum-valid frame regardless of origin — which is the property that makes the
save/restore necessary. Moving the synthetic-frame construction behind a pure
helper in `uartkbd_parse.c` would make the invariant itself testable, and is
left as a follow-up.
