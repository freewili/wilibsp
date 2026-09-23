# PSRAM bootstrap interrupt masking — hardware findings

## Scope

Investigated PSRAM-targeted apps that launched to a black screen and did not
respond to the HOME recovery hold.

## Failure found

The SRAM bootstrap enabled interrupts before changing the system clocks and
re-timing QMI/PSRAM. The app vector table and ordinary interrupt handlers are
in PSRAM, so an interrupt in that interval could fetch code from the bus while
it was being reconfigured and lock the display CPU before `main()`.

The bootstrap now preserves the loader's interrupt-disabled state through
`board_init_psram()` and enables interrupts only after QMI re-timing completes.
`board_init()` also recognizes that bootstrap initialization already occurred
and performs only inherited-peripheral initialization when the app calls it.

## Observed result

After installing and launching the rebuilt `hello_psram_exec` image, an RTT
capture reported:

    psram: bootstrap copied
    board: spi
    board: radio cs
    board: backlight
    board: i2c
    board: ioexp
    board: peripherals ready

This proves that the PSRAM image passed the previously dead startup interval
and reached the SRAM BSP bootstrap and peripheral initialization on hardware.

## Follow-up: inherited SDK alarm locks

The first bootstrap fix exposed a later hang at `board: leds`. A debugger halt
placed the CPU in `spin_lock_unsafe_blocking()` below `sleep_until()`. The UART
second-stage loader is itself an SDK application, so a warm jump can inherit a
hardware spin lock from its alarm pool while the new app starts with cleared
software bookkeeping. The bootstrap now resets hardware spin locks and creates
a fresh default alarm pool before board initialization.

After that change, freshly copied and hash-checked GuitarMan and Orca Field
Notes images both launched through the MAIN loader. GuitarMan reported its M3
boot milestone, a valid FT6336 touch-controller ID, and USB-host initialization.
Orca reported its panel boot banner, valid touch-controller ID, first LVGL
flush, audio low-power setup, and repeated main-loop heartbeats. This verifies
that both applications pass PSRAM startup and their display/touch bring-up.

Physical five-second HOME and About-screen behavior still require observation
because these external applications do not initialize the AgentIO command
channel, so the BSP cannot inject their button holds or capture their screens.
