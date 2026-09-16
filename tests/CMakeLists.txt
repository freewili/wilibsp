cmake_minimum_required(VERSION 3.13)
project(wilibsp_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import pytest"
    RESULT_VARIABLE FW2_PYTEST_RESULT
    OUTPUT_QUIET ERROR_QUIET)
if(NOT FW2_PYTEST_RESULT EQUAL 0)
    message(FATAL_ERROR "Host tests require pytest: ${Python3_EXECUTABLE} -m pip install pytest")
endif()

add_test(NAME python_app_contract
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
            ${CMAKE_CURRENT_SOURCE_DIR}/test_app_contract.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_uf2_info.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/test_make_app_uf2.py)

# Standalone tree: no Pico SDK, no pico_sdk_init(). Pure-logic modules from
# ../bsp are compiled directly against the host C compiler so tests run
# natively on Windows/Linux with no hardware.

add_executable(test_led_color
    test_led_color.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/leds/led_color.c)
target_include_directories(test_led_color PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME led_color COMMAND test_led_color)

add_executable(test_tone_gen
    test_tone_gen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/audio/tone_gen.c)
target_include_directories(test_tone_gen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_tone_gen m)
add_test(NAME tone_gen COMMAND test_tone_gen)

add_executable(test_vu_meter
    test_vu_meter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/audio/vu_meter.c)
target_include_directories(test_vu_meter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_vu_meter m)
add_test(NAME vu_meter COMMAND test_vu_meter)

add_executable(test_cc1101_regs
    test_cc1101_regs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/radio/cc1101_regs.c)
target_include_directories(test_cc1101_regs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cc1101_regs COMMAND test_cc1101_regs)

add_executable(test_monitor_engine
    test_monitor_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/radio/monitor_engine.c)
target_include_directories(test_monitor_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME monitor_engine COMMAND test_monitor_engine)

add_executable(test_capture_store
    test_capture_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/radio/capture_store.c)
target_include_directories(test_capture_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME capture_store COMMAND test_capture_store)

add_executable(test_ook_tx
    test_ook_tx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/radio/ook_tx.c)
target_include_directories(test_ook_tx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_ook_tx PRIVATE HOST_TEST)
add_test(NAME ook_tx COMMAND test_ook_tx)

add_executable(test_scan_engine
    test_scan_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/radio/scan_engine.c)
target_include_directories(test_scan_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_scan_engine PRIVATE HOST_TEST)
add_test(NAME scan_engine COMMAND test_scan_engine)

add_executable(test_cic
    test_cic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/dsp/cic.c)
target_include_directories(test_cic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cic COMMAND test_cic)

add_executable(test_dcblock
    test_dcblock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/dsp/dcblock.c)
target_include_directories(test_dcblock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dcblock COMMAND test_dcblock)

add_executable(test_sht40
    test_sht40.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/sensors/sht40.c)
target_include_directories(test_sht40 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sht40 COMMAND test_sht40)

add_executable(test_opt4001
    test_opt4001.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/sensors/opt4001.c)
target_include_directories(test_opt4001 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME opt4001 COMMAND test_opt4001)

add_executable(test_bmi323
    test_bmi323.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/sensors/bmi323.c)
target_include_directories(test_bmi323 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bmi323 COMMAND test_bmi323)

add_executable(test_bmm350_comp
    test_bmm350_comp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/sensors/bmm350_comp.c)
target_include_directories(test_bmm350_comp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bmm350_comp COMMAND test_bmm350_comp)

add_executable(test_palette
    test_palette.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/gfx/palette.c)
target_include_directories(test_palette PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME palette COMMAND test_palette)

# --- bsp/ir pure-logic group (harvested from WiliIR; see docs/drivers/ir.md) ---
set(BSPIR ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/ir)
add_executable(test_ir_nec test_ir_nec.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_nec PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_nec COMMAND test_ir_nec)

add_executable(test_ir_pd test_ir_pd.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_pd PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_pd COMMAND test_ir_pd)

add_executable(test_ir_manchester test_ir_manchester.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_manchester PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_manchester COMMAND test_ir_manchester)

add_executable(test_ir_kaseikyo test_ir_kaseikyo.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_kaseikyo PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_kaseikyo COMMAND test_ir_kaseikyo)

add_executable(test_ir_strict_length test_ir_strict_length.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_strict_length PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_strict_length COMMAND test_ir_strict_length)

add_executable(test_ir_dispatch test_ir_dispatch.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_dispatch PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_dispatch COMMAND test_ir_dispatch)

add_executable(test_ir_frame test_ir_frame.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_protocols.c ${BSPIR}/ir_frame.c)
target_include_directories(test_ir_frame PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_frame COMMAND test_ir_frame)

add_executable(test_ir_tx_pack test_ir_tx_pack.c ${BSPIR}/ir_tx_pack.c)
target_include_directories(test_ir_tx_pack PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_tx_pack COMMAND test_ir_tx_pack)

add_executable(test_ir_file test_ir_file.c ${BSPIR}/ir_file.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_file PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_file COMMAND test_ir_file)

add_executable(test_db_sort test_db_sort.c ${BSPIR}/db_sort.c)
target_include_directories(test_db_sort PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME db_sort COMMAND test_db_sort)

add_executable(test_ir_golden test_ir_golden.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_golden PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_golden COMMAND test_ir_golden)

add_executable(test_ir_resolve test_ir_resolve.c ${BSPIR}/ir_resolve.c ${BSPIR}/ir_encode.c ${BSPIR}/ir_decode.c ${BSPIR}/ir_protocols.c)
target_include_directories(test_ir_resolve PRIVATE ${BSPIR} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_resolve COMMAND test_ir_resolve)

add_executable(test_dvi_osd
    test_dvi_osd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/display/dvi_osd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/display/font5x7.c)
target_include_directories(test_dvi_osd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dvi_osd COMMAND test_dvi_osd)

# --- apps/retrochat acoustic-modem pure logic (see docs/superpowers in outer repo) ---
set(RCMODEM ${CMAKE_CURRENT_SOURCE_DIR}/../apps/retrochat/modem)
add_executable(test_rc_crc16 test_rc_crc16.c ${RCMODEM}/crc16.c)
target_include_directories(test_rc_crc16 PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rc_crc16 COMMAND test_rc_crc16)

add_executable(test_rc_frame test_rc_frame.c ${RCMODEM}/frame.c ${RCMODEM}/crc16.c)
target_include_directories(test_rc_frame PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rc_frame COMMAND test_rc_frame)

add_executable(test_rc_msgq test_rc_msgq.c ${RCMODEM}/msgq.c)
target_include_directories(test_rc_msgq PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rc_msgq COMMAND test_rc_msgq)

add_executable(test_rc_afsk_mod test_rc_afsk_mod.c ${RCMODEM}/afsk_mod.c)
target_include_directories(test_rc_afsk_mod PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rc_afsk_mod m)
add_test(NAME rc_afsk_mod COMMAND test_rc_afsk_mod)

add_executable(test_rc_afsk_demod test_rc_afsk_demod.c
    ${RCMODEM}/afsk_demod.c ${RCMODEM}/afsk_mod.c ${RCMODEM}/frame.c ${RCMODEM}/crc16.c)
target_include_directories(test_rc_afsk_demod PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rc_afsk_demod m)
add_test(NAME rc_afsk_demod COMMAND test_rc_afsk_demod)

add_executable(test_rc_afsk_loopback test_rc_afsk_loopback.c
    ${RCMODEM}/afsk_demod.c ${RCMODEM}/afsk_mod.c ${RCMODEM}/frame.c ${RCMODEM}/crc16.c)
target_include_directories(test_rc_afsk_loopback PRIVATE ${RCMODEM} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_rc_afsk_loopback m)
add_test(NAME rc_afsk_loopback COMMAND test_rc_afsk_loopback)

# --- bsp/keyboard fw2kb chord engine (harvested from ../wilikeyboard) ---
set(BSPKBD ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/keyboard)
add_executable(test_fw2kb test_fw2kb.c ${BSPKBD}/fw2kb.c)
target_include_directories(test_fw2kb PRIVATE ${BSPKBD} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME fw2kb COMMAND test_fw2kb)

add_executable(test_fw2kb_hid test_fw2kb_hid.c ${BSPKBD}/fw2kb.c ${BSPKBD}/fw2kb_hidmap.c)
target_include_directories(test_fw2kb_hid PRIVATE ${BSPKBD} ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME fw2kb_hid COMMAND test_fw2kb_hid)

add_executable(test_fw2kb_chord
    test_fw2kb_chord.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/keyboard/fw2kb.c)
target_include_directories(test_fw2kb_chord PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME fw2kb_chord COMMAND test_fw2kb_chord)

# --- bsp/input uartkbd_parse pure-logic parser (no UART hardware) ---
add_executable(test_uartkbd_parse
    test_uartkbd_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input/uartkbd_parse.c)
target_include_directories(test_uartkbd_parse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_uartkbd_parse m)
add_test(NAME uartkbd_parse COMMAND test_uartkbd_parse)

add_executable(test_uartkbd_inject
    test_uartkbd_inject.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input/uartkbd_parse.c)
target_include_directories(test_uartkbd_inject PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_uartkbd_inject m)
add_test(NAME uartkbd_inject COMMAND test_uartkbd_inject)

add_executable(test_app_recovery
    test_app_recovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input/app_recovery.c)
target_include_directories(test_app_recovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_app_recovery PRIVATE HOST_TEST)
add_test(NAME app_recovery COMMAND test_app_recovery)

add_executable(test_uf2_info
    test_uf2_info.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/common/uf2_info.c)
target_include_directories(test_uf2_info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME uf2_info COMMAND test_uf2_info)

add_executable(test_agentio_rle
    test_agentio_rle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/agentio/agentio_rle.c)
target_include_directories(test_agentio_rle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME agentio_rle COMMAND test_agentio_rle)

add_executable(test_agentio_shadow
    test_agentio_shadow.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/agentio/agentio_shadow.c)
target_include_directories(test_agentio_shadow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME agentio_shadow COMMAND test_agentio_shadow)

add_executable(test_picpwr_frame
    test_picpwr_frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/input/picpwr_frame.c)
target_include_directories(test_picpwr_frame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME picpwr_frame COMMAND test_picpwr_frame)

add_executable(test_rc_compose
    test_rc_compose.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/retrochat/compose.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/keyboard/fw2kb.c)
target_include_directories(test_rc_compose PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../apps/retrochat
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME rc_compose COMMAND test_rc_compose)

add_executable(test_usb_parse
    test_usb_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp/usbhost/usb_parse.c)
target_include_directories(test_usb_parse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../bsp
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME usb_parse COMMAND test_usb_parse)
