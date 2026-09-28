# freewili2_bsp — shared board-support library (STATIC once it has sources).
add_library(freewili2_bsp STATIC
    common/uf2_info.c
    agentio/agentio.c
    agentio/agentio_rle.c
    agentio/agentio_shadow.c
    platform/board.c
    platform/ioexp.c
    platform/power.c
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
    input/app_recovery.c
    input/app_about.c
    input/picpwr_frame.c
    input/picpwr.c
    usbhost/usb_core.c
    usbhost/usb_hcd.c
    usbhost/usb_hub.c
    usbhost/usb_msc.c
    usbhost/usb_parse.c
    usbhost/msc_disk.c
    usbhost/usb_store.c
    pio_usb_host/fwUSBHost.cpp
    pio_usb_host/fwUSBHostHID.cpp
    pio_usb_host/fwUSBHostHIDKeyboard.cpp
    pio_usb_host/fwUSBHostHIDMouse.cpp
    pio_usb_host/fwUSBHostHIDGeneric.cpp
    pio_usb_host/fwUSBHostHIDController.cpp
    pio_usb_host/fwUSBHostCDC.cpp
    pio_usb_host/fwUSBHostMSC.cpp
    pio_usb_host/fwUSBHostXInput.cpp
    pio_usb_host/fw2_pio_usb_host.cpp
    pio_usb_host/tusb_xinput/xinput_host.c
    third_party/fatfs/ff.c
    third_party/fatfs/ffunicode.c
    third_party/segger_rtt/SEGGER_RTT.c
    third_party/segger_rtt/SEGGER_RTT_printf.c)

target_include_directories(freewili2_bsp BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_usb_host
    ${CMAKE_CURRENT_SOURCE_DIR}/pio_usb_host/tusb_xinput
    ${PICO_SDK_PATH}/lib/tinyusb/src
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
    hardware_pwm
    hardware_adc
    hardware_watchdog
    tinyusb_pico_pio_usb)

target_sources(freewili2_bsp PRIVATE
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/common/tusb_fifo.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/usbh.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/host/hub.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/cdc/cdc_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/msc/msc_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/class/vendor/vendor_host.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c)

target_compile_definitions(freewili2_bsp PUBLIC FW2_AGENTIO=$<BOOL:${FW2_AGENTIO}>)
target_compile_definitions(freewili2_bsp PRIVATE CFG_TUSB_MCU=OPT_MCU_RP2040)

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

set(FW2_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../tools" CACHE PATH
    "Directory containing FW2 build validation tools")

function(fw2_display_app target)
    cmake_parse_arguments(PARSE_ARGV 1 APP "" "NAME;VERSION;DESCRIPTION;REPOSITORY" "POWER_ZONES")
    if(NOT APP_NAME)
        set(APP_NAME "${target}")
    endif()
    if(NOT APP_VERSION MATCHES "^[0-9][0-9][0-9]$")
        message(FATAL_ERROR
            "${target}: VERSION must be exactly three digits (000-999)")
    endif()
    if(NOT APP_DESCRIPTION)
        message(FATAL_ERROR
            "${target}: DESCRIPTION is required for the App Explorer")
    endif()
    if(NOT APP_REPOSITORY)
        set(APP_REPOSITORY "https://github.com/freewili/wilibsp")
    endif()
    if(APP_NAME MATCHES "[^ -~]" OR APP_DESCRIPTION MATCHES "[^ -~]")
        message(FATAL_ERROR
            "${target}: NAME and DESCRIPTION must contain printable ASCII only")
    endif()

    string(LENGTH "${APP_NAME}" _name_len)
    string(LENGTH "${APP_DESCRIPTION}" _description_len)
    if(_name_len GREATER 31)
        message(FATAL_ERROR "${target}: NAME exceeds 31 ASCII bytes")
    endif()
    if(_description_len GREATER 127)
        message(FATAL_ERROR "${target}: DESCRIPTION exceeds 127 ASCII bytes")
    endif()

    string(REGEX REPLACE "^0+([0-9])" "\\1" _version_number
           "${APP_VERSION}")
    set(_power_zone_mask 0)
    foreach(_zone IN LISTS APP_POWER_ZONES)
        if(_zone STREQUAL "SENSORS")
            set(_zone_number 1)
        elseif(_zone STREQUAL "DISPLAY")
            set(_zone_number 2)
        elseif(_zone STREQUAL "AUDIO")
            set(_zone_number 3)
        elseif(_zone STREQUAL "SUBGHZ")
            set(_zone_number 4)
        elseif(_zone STREQUAL "SDCARD")
            set(_zone_number 7)
        elseif(_zone STREQUAL "USB_HUB")
            set(_zone_number 8)
        elseif(_zone STREQUAL "RGB_LEDS")
            set(_zone_number 10)
        elseif(_zone STREQUAL "ANALOG")
            set(_zone_number 11)
        elseif(_zone STREQUAL "NFC_RFID")
            set(_zone_number 13)
        elseif(_zone STREQUAL "CAN")
            set(_zone_number 15)
        else()
            message(FATAL_ERROR "${target}: unknown POWER_ZONES entry '${_zone}'")
        endif()
        math(EXPR _power_zone_mask
             "${_power_zone_mask} | (1 << (${_zone_number} - 1))")
    endforeach()
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_uf2_info")
    file(MAKE_DIRECTORY "${_dir}")
    add_custom_command(
        OUTPUT "${_dir}/${target}_uf2_info.c"
        COMMAND ${Python3_EXECUTABLE} "${FW2_TOOLS_DIR}/gen_uf2_info.py"
                --out "${_dir}/${target}_uf2_info.c"
                --version "${_version_number}"
                --name "${APP_NAME}"
                --description "${APP_DESCRIPTION}"
                --repository "${APP_REPOSITORY}"
                --power-zones "${_power_zone_mask}"
        DEPENDS "${FW2_TOOLS_DIR}/gen_uf2_info.py"
        COMMENT "Generating FW2 app metadata for ${target}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_dir}/${target}_uf2_info.c")
    target_link_options(${target} PRIVATE
        -Wl,--undefined=fw2app_uf2_info)

    # /apps images execute only from SRAM or PSRAM; never target QSPI flash.
    pico_set_binary_type(${target} no_flash)

    # The SDK's no_flash script aliases PSRAM_STORE to SRAM because a debugger
    # normally loads the whole ELF before PSRAM exists. DISPLAY's app loader
    # already owns PSRAM bring-up and loads UF2 blocks directly, so keeping
    # that alias would give every .psram_uninitialised section an artificial
    # SRAM physical address and can push the ELF/UF2 beyond SRAM. Override only
    # the alias include so NOLOAD sections no longer consume an SRAM
    # load-address range. Ordinary fw2_display_app images must not contain
    # initialized PSRAM data: the app installer deliberately rejects a UF2
    # that mixes SRAM and PSRAM payload blocks.
    set(_link_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_app_link")
    file(MAKE_DIRECTORY "${_link_dir}")
    file(WRITE "${_link_dir}/memory_aliases_no_flash.incl"
        "REGION_ALIAS(\"TEXT_STORE\", RAM);\n"
        "REGION_ALIAS(\"RAM_STORE\", RAM);\n"
        "REGION_ALIAS(\"PSRAM_STORE\", PSRAM);\n"
        "REGION_ALIAS(\"XIP_RAM_STORE\", XIP_RAM);\n"
        "REGION_ALIAS(\"SCRATCH_X_STORE\", SCRATCH_X);\n"
        "REGION_ALIAS(\"SCRATCH_Y_STORE\", SCRATCH_Y);\n")
    pico_add_linker_script_override_path(${target} "${_link_dir}"
        FILES memory_aliases_no_flash.incl)
    # picotool rejects external-PSRAM ELF segments even when they are NOLOAD.
    # Generate the conventional diagnostics directly, then create UF2 from
    # loadable ELF bytes with the app-aware converter.
    pico_add_dis_output(${target})
    pico_add_hex_output(${target})
    pico_add_bin_output(${target})
    pico_add_map_output(${target})
    target_compile_definitions(${target} PRIVATE PICO_TARGET_NAME="${target}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${FW2_TOOLS_DIR}/make_app_uf2.py"
        "${FW2_TOOLS_DIR}/check_app_uf2.py"
        "${FW2_TOOLS_DIR}/check_uf2_info.py"
        "${FW2_TOOLS_DIR}/fw.py")
    add_custom_command(TARGET ${target} POST_BUILD
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${target}.uf2"
        COMMAND ${Python3_EXECUTABLE} "${FW2_TOOLS_DIR}/make_app_uf2.py"
                "$<TARGET_FILE:${target}>"
                "$<TARGET_FILE_DIR:${target}>/${target}.uf2"
        VERBATIM)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${FW2_TOOLS_DIR}/check_app_uf2.py"
                "$<TARGET_FILE_DIR:${target}>/${target}.uf2"
        VERBATIM)
endfunction()

function(fw2_psram_app target)
    fw2_display_app(${target} ${ARGN})
    set(_link_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_app_link")
    file(WRITE ${_link_dir}/memory_aliases_no_flash.incl [=[
REGION_ALIAS("TEXT_STORE", PSRAM);
REGION_ALIAS("RAM_STORE", PSRAM);
REGION_ALIAS("PSRAM_STORE", PSRAM);
REGION_ALIAS("XIP_RAM_STORE", XIP_RAM);
REGION_ALIAS("SCRATCH_X_STORE", SCRATCH_X);
REGION_ALIAS("SCRATCH_Y_STORE", SCRATCH_Y);
]=])
    target_sources(${target} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/app/psram_startup.S
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/app/psram_bootstrap.c)
    target_compile_definitions(${target} PRIVATE
        PICO_CRT0_NO_RESET_SECTION=1)
    pico_add_linker_script_override_path(${target}
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/app/psram_link
        FILES memory_aliases_no_flash.incl sections_no_flash.incl section_no_flash_text.incl
              section_no_flash_rodata.incl section_no_flash_data.incl)
endfunction()
