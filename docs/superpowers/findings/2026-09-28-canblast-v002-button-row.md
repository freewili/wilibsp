# canblast v002: labels directly above the front-panel buttons (2026-09-28)

v001 named its buttons in one grey text line along the bottom ("GREEN flood
BLUE len RED reset YELLOW stream"), not over the buttons and not in their
order. That line was meant to end with "HOLD HOME 5S TO EXIT", but a
small-text line is clipped at 47 characters, so the exit hint never showed.
v002 draws a box above each button it uses, in that button's colour, at
`x = i * 96`, width 93: STREAM (yellow), FLOOD (green), LEN (blue), RESET
(red). Grey does nothing in this app, so its slot is left empty. The exit hint
now has its own line above the bar.

## Setup

FreeWili 2 FX0103, MAIN on firmware branch `feat/esp32-onewili-streams`.
DISPLAY: `apps/canblast` v002, started with `fw ramrun canblast`. Buttons were
injected with agentio; the status line was read from screenshots.

Screenshots: `2026-09-28-canblast-v002.png` (whole screen) and
`2026-09-28-canblast-v002-buttons.png` (the status line before and after each
press).

## Results

| Input | Expected | Status line after |
|---|---|---|
| (start) | | `stream binary  flood off len 8` |
| `fw press yellow` | next stream mode | `stream text` |
| `fw press yellow` | next stream mode | `stream both` |
| `fw press blue` | next flood length | `len 64` |
| `fw press grey` | nothing | unchanged |
| `fw press red` | counters reset | unchanged; the counters were already 0, so a reset cannot show |

GREEN (start/stop a transmit flood) was not pressed: it transmits on CAN 0, and
nothing on the bench was known to be safe to flood. Its handler is unchanged
from v001.
