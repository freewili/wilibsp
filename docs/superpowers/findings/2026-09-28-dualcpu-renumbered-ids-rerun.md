# dualcpu: rerun on the rebased images with the renumbered ids (2026-09-28)

The 2026-09-27 run (`2026-09-27-dualcpu-peer-streams-e2e.md`) used a
development tree in which `w\e` and `h\a\w/p/c` had stable ids 614-617. This
repeats it on the committed firmware branch `feat/esp32-onewili-streams`
(rebased onto `release/v08-preview.3`, where ISO-TP holds 614-623 and these
commands are 624-627).

## Setup

- FreeWili 2 FX0103.
- MAIN: `FW2Main.uf2` from `feat/esp32-onewili-streams`, installed through the
  `FW2Main FBL` loader. Before the install MAIN did not answer the ISO-TP
  status command `i\c\t\i`; afterwards it did. An SWD readback of the
  application region matched the UF2 payload byte for byte.
- ESP32-C5: the Bottlenose firmware from the same branch with overlay
  `sdkconfig.bsp-dualcpu`, flashed over the C5's own USB Serial/JTAG; esptool
  verified all four regions, and the boot banner's ELF hash matched the build.
- DISPLAY: `apps/dualcpu` v001, started with `fw ramrun dualcpu`.
- Observed through RTT channel 0 and the ESP32's USB Serial/JTAG console. A
  second pass re-derived every number below from the raw logs.

## Results

| Check | 2026-09-27 | 2026-09-28 |
|---|---|---|
| Stream link comes up | confirmed, ESP32 live within seconds | confirmed; PINGs flowing about 5 s after launch |
| PING DISPLAY -> ESP32, 1 per second, 80 s | 78/78, 6.05-6.18 ms | 80/80, 0 lost, 6.03-6.36 ms |
| ESP32 TELEMETRY -> DISPLAY | no sequence gaps | 80/80, no gaps |
| `fw press red`, `fw press ok` | LED (64,0,0); scan, 17-21 APs | LED (64,0,0) in telemetry; scan, 16 APs |
| `burst 150`, twice | 40 accepted, 110 refused | 40 / 110 each; 1 of 80 dropped by MAIN and counted on both ends; no overruns |
| PC -> DISPLAY through the text route | 25/25 | 25/25 |
| PC <-> ESP32 echo through the text route | 20/20 | 20/20 |
| ESP32 Mode to Default for ~40 s, then back | stale within 3 s; 50/50 after | stale after 3.2 s; live 1.0 s after reopening; 56/56 after; ESP32 refused count flat |
| PING accounting over the whole run | 417 = 375 answered + 42 dropped at the gate | 534 = 492 answered + 42 lost (41 at the gate, 1 in a burst) |
| ESP32 reset into its ROM loader by read-only `w\a` queries | apps recover | both recover on their own (DISPLAY stale 4 s, 6 PINGs lost), all answered afterwards |

The four commands work by text path, including a PC loopback through
`h\a\w` / `h\a\p`. No host command calls MAIN by numeric id, so the ids were
checked in the built image instead: its command table maps 614-623 to ISO-TP
and 624-627 to `w\e`, `h\a\w`, `h\a\p`, `h\a\c`.

## Not covered, and differences

- The ESP32's GPIO-report and text-event mirror was not exercised: MAIN's IO
  and event streams were off after its reboot, so both counts stayed 0.
- No camera was attached; the LED colour was read from telemetry only.
- The PING round trip sat at about 6.0, 6.2 or 11.6 ms, moving between levels
  after some commands and once on its own. No PING was lost to it; the 11.6 ms
  level was not seen on 2026-09-27.
- With the stock DISPLAY firmware still running, the ESP32 appeared to power up
  about 1 s after ESP32 Mode was set to OneWili API, which the 2026-09-27 run
  did not see. Not yet confirmed by a deliberate test.
