# canblast v003: app-contract fixes on hardware (2026-10-12)

v003 declares `POWER_ZONES DISPLAY CAN` (the CAN rail was raised by hand
before), clears the whole panel in `main()` before the backlight (the clear
was inside `screen_static()`, which the contract check cannot see), and uses
`fw2_app_recovery_sleep_ms()` in its startup loop. With these, the host tree's
`python_app_contract` passes (45/45).

## Run

`fw ramrun canblast` with the compute module off and no CAN bus attached.

- RTT: `app: required power zones=0x4002`, `app: power zones ready=0xE3C7`,
  `picpwr: released rails 00204` (audio and RGB LEDs),
  `canblast: CAN1 mode 0 rate 500000 fd 2000000 ... -> 0 0 0 0`,
  `canblast: stream on -> 0`, `canblast: up, sys=250000 kHz`.
- Screen (`2026-10-12-canblast-v003.png`): counters at zero, `can rail ON`,
  `zones 0e1c3`, `link up`, the four button labels.
- After `fw hold home` the agentio channel stopped answering (the app exited).

## Not verified

No CAN traffic: no bus or peer was attached, so transmit and receive were not
exercised in this run (they are unchanged from v002).
