# Launch a no_flash (SRAM) FreeWili 2 DISPLAY app over the debug probe.
#
# Cold-starting the image from the bootrom panics (the app expects the
# QMI/PSRAM state the DISPLAY loader leaves behind), so: reset, let the stock
# DISPLAY firmware boot, halt both cores, park core 1 in a branch-to-self loop
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
targets rp2350.cm1
halt
mww 0xE000E180 0xFFFFFFFF
mww 0xE000E184 0xFFFFFFFF
mww 0xE000E280 0xFFFFFFFF
mww 0xE000E284 0xFFFFFFFF
mww 0xE000E010 0
mwh 0x20080ffc 0xe7fe
reg sp 0x20080ff0
reg pc 0x20080ffc
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
resume
# TIMER0 pauses while EITHER core sits in debug halt (DBGPAUSE, reset value
# 0x7), which freezes every sleep/alarm in the app if a later probe session
# leaves a core halted. Clear it once the app's runtime init has run.
sleep 300
mww 0x400b002c 0
shutdown
