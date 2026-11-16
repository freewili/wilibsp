# CEC pin release: expander P1_6 and GPIO31 left as inputs (2026-11-14)

**Status: built and host-tested, not run on a board.** Nothing below is a
hardware result yet. The bench steps at the end say what to run and what a
pass looks like.

## Change

On father board rev 2 the TV connector's CEC wire (`GPIO31_DISP`) reaches
DISPLAY GPIO31 and the DISPLAY I/O expander's P1_6, with a board pull-up.
Before this change `ioexp_init()` configured port 1 as all outputs and wrote
P1_6 high, so every BSP app drove the CEC wire high push-pull.

- `bsp/platform/ioexp.c`: port 1 output base 0x24 (was 0x64); port 1
  direction byte 0x40 (P1_6 an input; was 0x00). `ioexp_init()` reads the
  three direction bytes back and prints them.
- `bsp/platform/board.c`: `board_init()` runs `gpio_init(31)` and
  `gpio_disable_pulls(31)` after the expander, and prints the pin's
  direction, pulls and level.
- `bsp/platform/board.h`: `PIN_HDMI_CEC` 31.

## What rests on what

| Claim | Basis |
|---|---|
| P1_6 is `GPIO31_DISP` (the CEC wire) on father board rev 2 | Schematic notes, confirmed by the board owner 2026-11-11 |
| A push-pull high on P1_6 stops other devices pulling the wire low | Follows from CEC being open drain; not observed with a BSP app |
| P1_6 is `I2C_PULL` (header I2C0 pull-up enable) on rev 1 | Schematic notes; the BSP no longer drives it on either revision |
| The GPIO31 pad resets with its pull-down on | RP2350 reset default; the stock firmware's comment says the same |

**UNVERIFIED HW:** none of the above was measured with this BSP.

## Not done

- No host test: the BSP has no host-test pattern for `ioexp.c` (it is
  I2C-bound) and the change is three constants.
- GPIO31's edge-interrupt enables are not cleared at start-up. An app
  launched after the stock firmware ran CEC could inherit them; nothing has
  been observed, so nothing was added.

## Bench steps (owner)

1. Launch `hello_display` built from this branch; run `fw rtt -s 5`.
   Pass: `ioexp: direction read-back ok P0 0x0 P1 0x40 P2 0x4 (P1_6 CEC wire
   input: yes)` and `board: cec wire GPIO31 input=1 pull-up=0 pull-down=0
   level=1` (level 1 = the board pull-up holds the idle wire high).
2. TV connected, its CEC device list open: with an older BSP app running, a
   CEC device on the same TV cannot be controlled (expected); with this build
   it can.
3. Scope on the TV connector's CEC pin while the TV powers on: lows reach 0 V
   with this build.
