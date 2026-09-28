# Launch a no_flash (SRAM) FreeWili 2 DISPLAY app over the debug probe.
#
# Cold-starting the image from the bootrom panics (the app expects the
# QMI/PSRAM state the DISPLAY loader leaves behind), so: reset, let the stock
# DISPLAY firmware boot, halt both cores, park core 1 in a wfi loop
# with interrupts masked (leaving it in debug halt would pause TIMER0 through
# DBGPAUSE, and it keeps running stock code in SRAM otherwise), quiesce the
# watchdog / NVIC / SysTick / DMA the stock firmware left armed, load the ELF
# into SRAM, verify, and jump core 0 through the image's own vector table
# (word 0 = initial SP, word 1 = reset handler). BSP apps are single-core.
#
# Usage: openocd ... -f freewili2.cfg -c "set ELF path/to/app.elf" -f ramrun.tcl
init
targets rp2350.cm0
reset run
sleep 3500
halt
rp2350.cm0 cortex_m smp off
rp2350.cm1 cortex_m smp off
# Only take over core 0 in Thread mode. Rewriting PC does not leave an
# exception: a halt that lands in one of the stock firmware's interrupt
# handlers would start the app *inside* that handler, with its NVIC active bit
# still set, so no interrupt of equal or lower priority can ever preempt it.
# The app then hangs at its first interrupt-driven wait -- board_init's
# sleep_us() in ws2812_clear_once(), seen as a silent stop after "board: leds"
# with TIMER0_IRQ_3 pending and IPSR = 27 (DMA_IRQ_1).
targets rp2350.cm0
for {set i 0} {$i < 100} {incr i} {
    if {([lindex [get_reg xpsr] 1] & 0x1ff) == 0} break
    resume
    sleep 1
    halt
}
if {([lindex [get_reg xpsr] 1] & 0x1ff) != 0} {
    echo "ramrun: core 0 did not halt in Thread mode; the app may hang at its first interrupt wait"
}
targets rp2350.cm1
halt
mww 0xE000E180 0xFFFFFFFF
mww 0xE000E184 0xFFFFFFFF
mww 0xE000E280 0xFFFFFFFF
mww 0xE000E284 0xFFFFFFFF
mww 0xE000E010 0
# Park loop: `wfi` / `b .-2`. With every NVIC enable cleared above, SysTick off
# and PRIMASK set, nothing wakes core 1, so it sleeps with its clock gated. A
# bare branch-to-self here kept a whole core executing at the app's 250 MHz for
# as long as the app ran - a share of the heat a ramrun-launched app produced.
mwh 0x20080ff8 0xbf30
mwh 0x20080ffa 0xe7fd
reg sp 0x20080ff0
reg pc 0x20080ff8
reg primask 1
resume
targets rp2350.cm0
mww 0x400d8000 0
mww 0xE000E180 0xFFFFFFFF
mww 0xE000E184 0xFFFFFFFF
mww 0xE000E280 0xFFFFFFFF
mww 0xE000E284 0xFFFFFFFF
mww 0xE000E010 0
mww 0x50000404 0
mww 0x50000414 0
mww 0x50000424 0
mww 0x50000434 0
mww 0x50000464 0xFFFF
sleep 20
load_image $ELF
verify_image $ELF
set vt [read_memory 0x20000000 32 2]
echo [format "ramrun: sp=0x%08x pc=0x%08x" [lindex $vt 0] [lindex $vt 1]]
reg sp [lindex $vt 0]
reg pc [lindex $vt 1]
# A reset handler assumes reset state: interrupts unmasked. The stock
# firmware may have been halted inside a critical section.
reg primask 0
reg basepri 0
resume
# TIMER0 pauses while EITHER core sits in debug halt (DBGPAUSE, reset value
# 0x7), which freezes every sleep/alarm in the app if a later probe session
# leaves a core halted. Clear it once the app's runtime init has run.
sleep 300
mww 0x400b002c 0
shutdown
