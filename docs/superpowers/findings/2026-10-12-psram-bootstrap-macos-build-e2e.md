# PSRAM app built on macOS: bootstrap placement and a hardware run (2026-10-12)

The SRAM bootstrap list matched SDK objects only as `name.c.obj`. CMake uses
that suffix on Windows; on macOS and Linux objects are `name.c.o`, so none of
the SDK entries matched and the clock and PSRAM re-timing code linked into
PSRAM. The list now matches both suffixes, and `hello_psram_exec`'s layout
check covers every function between raising `clk_sys` and re-timing PSRAM.

## Build (macOS, toolchain 15_2_Rel1)

| Image | Layout check |
|---|---|
| master before the libgcc change | fails: `set_sys_clock_pll` in PSRAM |
| this change without the libgcc change | fails only on `__aeabi_uldivmod` |
| this change on current master (with the libgcc change) | passes; `.sram_bootstrap` 29,592 bytes |

## Hardware run

`hello_psram_exec` built from this branch on macOS, written to the SD card as
`/apps/Test/hello_psram_exec.uf2` through MAIN's framed file transfer (readback
matched, 73,216 bytes), launched with `fw run-app Test/hello_psram_exec.uf2`
(MAIN reported `launched`). The compute module was off.

RTT, in order: `psram: bootstrap copied`, board bring-up (`ioexp: init ok`),
`psram: constructors complete`, `psram: main`, `psram: board ready`,
`app: power zones ready=0xA3C3`, `psram: RGB LED rail cycled`,
`psram: lcd ready`, `agentio: up on RTT ch1`, `psram: ui ready`. The screen
(`2026-10-12-psram-bootstrap-macos-build.png`) shows "EXECUTING FROM PSRAM /
NORMAL BSP + SDK RUNTIME / HOLD HOME TO RETURN". After `fw hold home` the
agentio channel stopped answering (the app had exited); the stock firmware's
screen was not captured.

## Not verified

- No before/after comparison on hardware: an image with the re-timing code
  in PSRAM was not run, so this record shows the fixed image works, not that
  the old one fails (the old one is expected to work most of the time).
- Only `hello_psram_exec` was run; one launch.
