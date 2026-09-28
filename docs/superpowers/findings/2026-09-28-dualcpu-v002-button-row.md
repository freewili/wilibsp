# dualcpu v002: labels directly above the five front-panel buttons (2026-09-28)

v001 drew six label-sized boxes in the order RED GREEN BLUE RAINBOW OFF SCAN,
so no label sat over its physical button. v002 draws five fixed boxes, one
above each button in the button's colour — OFF (grey), RAINBOW (yellow),
GREEN, BLUE, RED at `x = i * 96`, width 93 — and moves SCAN, which has no
button in that row, to a touch button in the title bar.

## Setup

FreeWili 2 FX0103, with the MAIN and ESP32 images of
`2026-09-28-dualcpu-renumbered-ids-rerun.md`. DISPLAY: `apps/dualcpu` v002,
started with `fw ramrun dualcpu`. Each input was injected with agentio, and
the ESP32's LED state was read back from its telemetry (`led=` in the RTT
`stats` line).

Screenshot: `2026-09-28-dualcpu-v002.png`.

## Results

| Input | Expected | Telemetry after |
|---|---|---|
| `fw press grey` / `fw touch 46 296` | LED off | `led=0,0,0,0` / same |
| `fw press yellow` / `fw touch 142 296` | rainbow | mode 2 / same |
| `fw press green` / `fw touch 238 296` | green | `led=1,0,64,0` / same |
| `fw press blue` / `fw touch 334 296` | blue | `led=1,0,0,64` / same |
| `fw press red` / `fw touch 430 296` | red | `led=1,64,0,0` / same |
| `fw touch 94 296` (gap between grey and yellow) | nothing | unchanged |
| `fw touch 436 14` (SCAN) | new scan | access-point count changed with the next telemetry |

The touches were issued in a different order from the presses, so each box
was matched to its action independently.
