# Consuming wilibsp from another project

Read this if your project pulls `wilibsp` in as a submodule and builds its own
display app. If you are working **on** wilibsp itself — adding a driver,
editing the BSP — read [AGENTS.md](../AGENTS.md) instead; it is the deep
reference and this is not a summary of it.

This file is deliberately short. Most of what used to be documented here is now
enforced by the build and the CLI, because prose that has to be remembered and
re-applied by every project does not survive contact with a deadline. What
remains is the part no check can make for you.

## Wiring it in

```cmake
set(PICO_BOARD freewili2 CACHE STRING "Board type")     # never -DPICO_BOARD
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/wilibsp/bsp/boards")

include(pico_sdk_import.cmake)
project(myproject C CXX ASM)
pico_sdk_init()

add_subdirectory(wilibsp/bsp)          # also provides fw2_finalize_app()
add_subdirectory(wilibsp/libs/onewili) # optional: main-CPU + SD access
add_subdirectory(src)
```

Then, for your app target:

```cmake
add_executable(myapp main.c)
target_link_libraries(myapp freewili2_bsp)
fw2_finalize_app(myapp PSRAM)          # SRAM | PSRAM | FLASH
```

**That one call is the whole contract.** Declaring the window applies the
binary type, any linker overrides, the runtime-init overrides, and the right
UF2 emission — then verifies on every build that the artifact actually targets
what you declared. Do not hand-roll `pico_set_binary_type`, linker script
overrides, or PSRAM boot workarounds; if you find yourself needing to, that is
a bug in `fw2_finalize_app()` worth reporting rather than working around.

### Which window

| | Where it runs | Deployed by | Touches stock firmware? |
|---|---|---|---|
| `SRAM` | staged into SRAM by the fused bootloader | SD card `/apps/` | no |
| `PSRAM` | 8 MB window at `0x11000000`, staged by FW2PsramStub | SD card `/apps/` | no |
| `FLASH` | the display's QSPI flash | `fw flash` over SWD | **yes — replaces FW2Display** |

`SRAM` and `PSRAM` are the non-destructive launch surfaces. Prefer them unless
you are developing the BSP itself. `FLASH` warns at configure time and is
rejected by `fw install-app`; restoring the stock firmware afterwards means
reflashing `FW2Display.uf2`.

## Workflow

```sh
fw install-app build/src/myapp.uf2   # copy to the card's /apps/ (refuses flash images)
fw run-app myapp.uf2                 # reset the display, launch, report the real result
fw list-apps                         # what is actually on the card

fw rtt                               # DIAG output (RTT is the only diagnostic channel)
fw screenshot -o shot.png            # capture the panel
fw touch <x> <y> / fw press <btn>    # drive the UI
fw alive                             # is it executing? (does not perturb it)
fw peek <addr>                       # read memory from a running target
```

`fw run-app` exists because a bare launch is unreliable in two non-obvious
ways: the display must be reset first or the fused bootloader will not answer,
and the real outcome arrives in a deferred frame long after the immediate ack.
Both are handled for you — use it rather than driving the console yourself.

## The parts nothing can check for you

Everything above is enforced. These are judgment calls:

1. **"GPIO 12" is ambiguous on this board — ask which one.** Display GPIO, main
   GPIO, and the user header are different pin spaces. Guessing produces code
   that compiles, runs, and drives the wrong pin. See `docs/hardware/pinmap.md`.

2. **Request power rails before touching a peripheral.** Most rails boot OFF,
   and the boot-on set is firmware-defined and changes between versions. A
   driver that reads garbage or returns silence is more often an unpowered rail
   than a bug. `picpwr_keep_awake(...)`, then wait ~1 s, then init. Full zone
   map in `docs/drivers/power.md`.

3. **Do not assume a doc describes verified behaviour.** Where this repo says
   what something does, check whether a file under `docs/superpowers/findings/`
   backs it. If none does, it is design intent — say so rather than repeating
   it as fact. This applies to the file you are reading.

4. **Observation is not free, and the debugger is not neutral.** A screenshot
   RLE-encodes and streams 307,200 bytes from your app's own main loop. Inside
   a timed region that cost lands *between* your measurements, so your own
   microsecond accounting looks fine while wall-clock throughput collapses — a
   downstream benchmark published figures 5x and 15x low exactly this way.
   Wrap timed regions in `agentio_measure_begin()` / `agentio_measure_end()`
   and captures will be refused rather than silently corrupting the result.

   The same caution applies to SWD. `fw alive` and `fw peek` are safe by
   construction; anything that halts a core is not, because the RP2350 pauses
   TIMER0 while a core is debug-halted and your app's whole timebase stops with
   it. If a board ever looks hung with a frozen screen but a responsive
   agentio, that is the signature — `fw thaw`.

5. **Large buffers go in PSRAM via the linker, never by casting `PSRAM_BASE`.**
   The linker's PSRAM region starts at that same address, so a raw pointer
   there silently aliases whatever it allocated. Use
   `__uninitialized_psram("group")`.

## When something does not work

Before believing that a launch failed, check in this order:

1. `fw list-apps` — is the file even on the card?
2. `fw alive` — is the display CPU executing at all?
3. `fw rtt` — did the app reach its own DIAG output?

The expensive failure mode in this ecosystem is not a broken board, it is a
correct-looking observation taken with a tool that changed what it measured.
Two separate multi-session investigations here reached confident, wrong
conclusions that way. If a result is surprising, suspect the instrument before
the hardware.
