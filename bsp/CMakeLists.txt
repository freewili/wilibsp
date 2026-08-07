# freewili2_bsp — shared board-support library (STATIC once it has sources).

# App target configuration + enforcement. Included first so the board guard runs
# before anything is built, and so consumers get fw2_finalize_app() simply by
# doing add_subdirectory(wilibsp/bsp) -- no extra wiring, and it versions with
# the submodule instead of being copied into each project's docs.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/fw2_app.cmake)

add_library(freewili2_bsp STATIC
    agentio/agentio.c
    agentio/agentio_rle.c
    agentio/agentio_shadow.c
    platform/board.c
    platform/ioexp.c
    platform/psram.c
    platform/spi_bus.c
    display/st7796.c
    display/font5x7.c
    display/dvi_osd.c
    display/hstx_dvi.c
    input/ft6336.c
    input/ft6336_map.c
    leds/led_color.c
    leds/ws2812_driver.c
    leds/led_ui.c
    audio/tone_gen.c
    audio/vu_meter.c
    audio/codec_nau88c10.c
    audio/audio_i2s_duplex.c
    audio/audio_capture.c
    radio/cc1101_regs.c
    radio/cc1101.c
    radio/gdo_capture.c
    radio/monitor_engine.c
    radio/ook_tx.c
    radio/scan_engine.c
    radio/capture_store.c
    pdm/pdm_capture.c
    dsp/cic.c
    dsp/dcblock.c
    sensors/sht40.c
    sensors/opt4001.c
    sensors/bmi323.c
    sensors/bmm350.c
    sensors/bmm350_comp.c
    ir/ir_protocols.c
    ir/ir_decode.c
    ir/ir_encode.c
    ir/ir_frame.c
    ir/ir_tx_pack.c
    ir/ir_file.c
    ir/ir_resolve.c
    ir/db_sort.c
    ir/ir_capture.c
    ir/ir_tx.c
    gfx/palette.c
    keyboard/fw2kb.c
    keyboard/fw2kb_hidmap.c
    input/uartkbd_parse.c
    input/uartkbd.c
    input/picpwr_frame.c
    input/picpwr.c
    usbhost/usb_core.c
    usbhost/usb_hcd.c
    usbhost/usb_hub.c
    usbhost/usb_msc.c
    usbhost/usb_parse.c
    usbhost/msc_disk.c
    usbhost/usb_store.c
    third_party/fatfs/ff.c
    third_party/fatfs/ffunicode.c
    third_party/segger_rtt/SEGGER_RTT.c
    third_party/segger_rtt/SEGGER_RTT_printf.c)

target_include_directories(freewili2_bsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/segger_rtt
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fatfs)

target_link_libraries(freewili2_bsp PUBLIC
    pico_stdlib
    hardware_i2c
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_psram
    hardware_pwm)

target_compile_definitions(freewili2_bsp PUBLIC FW2_AGENTIO=$<BOOL:${FW2_AGENTIO}>)

# GDO0 = GPIO32; PIO must use the GPIO base window 16..47 to reach it.
target_compile_definitions(freewili2_bsp PUBLIC PICO_PIO_USE_GPIO_BASE=1)

# Generate the WS2812 PIO header into the build tree for ws2812_driver.c.
pico_generate_pio_header(freewili2_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/leds/ws2812.pio)

# Generate the I2S full-duplex PIO header into the build tree for audio_i2s_duplex.c.
pico_generate_pio_header(freewili2_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/i2s_duplex.pio)

# Generate the GDO0 capture PIO header into the build tree for gdo_capture.c.
pico_generate_pio_header(freewili2_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/radio/gdo_capture.pio)

# Generate the PDM capture PIO header into the build tree for pdm_capture.c.
pico_generate_pio_header(freewili2_bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/pdm/pdm_capture.pio)

# Generate the IR PIO headers into the build tree for ir_capture.c and ir_tx.c.
pico_generate_pio_header(freewili2_bsp ${CMAKE_CURRENT_SOURCE_DIR}/ir/ir_capture.pio)
pico_generate_pio_header(freewili2_bsp ${CMAKE_CURRENT_SOURCE_DIR}/ir/ir_tx.pio)
