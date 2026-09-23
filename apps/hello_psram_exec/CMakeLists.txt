add_executable(hello_psram_exec main.c)
target_link_libraries(hello_psram_exec freewili2_bsp)
fw2_psram_app(hello_psram_exec
    POWER_ZONES DISPLAY RGB_LEDS
    NAME [=[PSRAM Execute]=]
    VERSION 001
    DESCRIPTION [=[BSP application executing from external PSRAM]=])
add_custom_command(TARGET hello_psram_exec POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/verify_layout.py
            $<TARGET_FILE:hello_psram_exec> ${CMAKE_OBJDUMP}
            $<TARGET_FILE_DIR:hello_psram_exec>/hello_psram_exec.uf2
    VERBATIM)
