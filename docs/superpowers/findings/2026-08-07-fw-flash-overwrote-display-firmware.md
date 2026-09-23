# `fw flash` replaced the stock DISPLAY firmware — 2026-08-07

**Result: INCIDENT, then a guard.** An app built from the pre-app-contract
template was flashed over the debug probe repeatedly during a development
session. Every one of those flashes overwrote the stock DISPLAY firmware. The
board was never bricked — the recovery loader is fused in OTP — but the firmware
was gone and nothing in the workflow said so.

## What happened

`fw flash` is `openocd -c 'program <elf> verify reset exit'`, which writes each
loadable segment at its **physical** address. For the app in question those were

```
LOAD  VirtAddr 0x20000110   PhysAddr 0x10000210
LOAD  VirtAddr 0x20008c08   PhysAddr 0x10008d04
```

— running addresses in SRAM, **stored** addresses at flash base. That is what
`pico_set_binary_type(<app> copy_to_ram)` produces: the image is copied into
SRAM by the C runtime at boot, but it lives in QSPI flash, which is where the
stock DISPLAY firmware lives too.

Nothing in the path objected. `fw install-app` has validated UF2 payloads
against the same memory map for some time and fails closed before it even mounts
the SD card; `fw flash` trusted the ELF completely.

## Why the app was built that way

`apps/template` used `pico_set_binary_type(copy_to_ram)` before the app contract
landed. Apps in this tree have since moved to `fw2_display_app()`, which sets
`no_flash` and produces an SRAM image — so every app here is already safe, and
`fw flash` on any of them writes SRAM only. The exposure is external app
repositories and any app predating or ignoring the contract, which is precisely
the population least likely to know the hazard exists.

## The guard

`fw flash` now parses the ELF it is about to program and refuses by default if
any loadable segment intersects `0x10000000..0x11000000`:

```
fw flash: refusing to program myapp
build/apps/myapp/myapp.elf stores 2 loadable segment(s) in QSPI flash
(0x10000000+512, 0x10000210+35572).
Programming it would REPLACE the stock DISPLAY firmware.
Build the app with fw2_display_app() so it targets SRAM or PSRAM, then
install it non-destructively with `fw install-app <app>.uf2`.
If replacing the DISPLAY firmware is genuinely what you want, re-run
with `fw flash --replace-display-firmware`.
```

The memory-window constants are now shared with `check_app_uf2()`, so the ELF
path and the UF2 path cannot drift apart.

`fw flash` is deliberately **not** removed or deprecated. On a
`fw2_display_app()` target it loads SRAM over the probe and is non-destructive,
which is the fast edit/flash/`fw rtt`/`fw screenshot` loop the whole agentio
workflow depends on — and it works with no SD card, no MAIN firmware, and no
`pyfwfinder`/`pyserial`. What it now refuses is the one thing that was never
meant to be routine.

## Verified

- Refuses the actual ELF from the incident: four segments reported at
  `0x10000000+528, 0x10000210+35572, 0x10008d04+13540, 0x1000c1e8+20`, exit
  status 2, no traceback.
- Accepts `hello_display` built via `fw2_display_app()`: one load segment at
  `0x20000000+28628`, command emitted unchanged.
- `--replace-display-firmware` emits the original command.
- A missing build produces the OpenOCD "no such file" error as before, rather
  than a confusing ELF-parse failure.
- `fw test` 45/45, including nine new cases in `tools/tests/test_fw.py` covering
  physical-vs-virtual addressing, NOLOAD segments, a segment that only partially
  overlaps the window, both override paths, and the not-yet-built case.

## Worth knowing

- **`copy_to_ram` does not mean "loaded into RAM".** It means "copied to RAM at
  boot". The image is flash-resident, and a debugger writes where the image is
  stored. Read `PhysAddr`, not `VirtAddr`, when reasoning about what a flash
  command will touch.
- **A screenshot cannot tell you the firmware is gone.** Captures kept working
  throughout, because `fw screenshot` walks the agentio PSRAM shadow — what the
  driver was told to draw. The first hard evidence was `ioexp: init NAK` and
  `ft6336: init NO-ACK` at boot.

## Not covered

Restoring the stock DISPLAY firmware is a separate maintenance workflow and is
not automated here; this change only stops the destructive write from happening
by accident. A guard at configure time — warning when a target links
`freewili2_bsp` without going through `fw2_display_app()` — would catch the same
mistake one step earlier and is left as a follow-up.
