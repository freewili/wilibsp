# fw2_app.cmake — app target configuration and enforcement for the FreeWili 2
# display CPU. Included by bsp/CMakeLists.txt, so it runs inside the consumer's
# configure step and applies to consumer targets.
#
# WHY THIS FILE EXISTS
#
# Most of what a consumer project used to have to know about building a display
# app was prose in AGENTS.md, re-read and re-applied by hand in every project.
# That does not hold. One downstream project reasoned its way past the "/apps/
# may never target flash" rule and overwrote the stock DISPLAY firmware; another
# rediscovered the PSRAM-app boot requirements from scratch over two sessions.
# Both were documented. Neither was enforced.
#
# So: a rule that can be enforced is not written down here, it is executed. The
# entry point is
#
#     fw2_finalize_app(<target> <SRAM|PSRAM|FLASH>)
#
# which picks the binary type, applies whatever that window needs, emits the
# UF2, and then verifies the artifact actually matches the window it claims.
# The consumer declares intent once; the rules follow from it.

set(FW2_APP_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "fw2_app.cmake dir")
set(FW2_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tools" CACHE INTERNAL "wilibsp tools dir")

# --- Board guard (AGENTS.md invariants 1 and 8) -----------------------------
#
# `-DPICO_BOARD=...` on the command line overrides the cached value the parent
# project sets and silently reverts the build to the RP2350A config: 30 GPIO
# instead of 48, so every pin above 29 is wrong and the failure shows up as
# dead peripherals, not as a build error. Checking the value catches it however
# it got there.
if(NOT PICO_BOARD STREQUAL "freewili2")
    message(FATAL_ERROR
        "PICO_BOARD is '${PICO_BOARD}', must be 'freewili2'.\n"
        "  The FreeWili 2 is an RP2350B (48 GPIO). Any other board header selects "
        "the RP2350A pin count and every GPIO above 29 silently becomes wrong.\n"
        "  Set it in your top-level CMakeLists.txt:\n"
        "      set(PICO_BOARD freewili2 CACHE STRING \"Board type\")\n"
        "  and do NOT pass -DPICO_BOARD on the cmake command line -- that "
        "overrides the cached value.")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# --- fw2_finalize_app -------------------------------------------------------
#
#   fw2_finalize_app(<target> SRAM)    staged into SRAM by the fused bootloader
#   fw2_finalize_app(<target> PSRAM)   staged into the 8 MB window by FW2PsramStub
#   fw2_finalize_app(<target> FLASH)   written to flash; REPLACES FW2Display
#
# SRAM and PSRAM are the two non-destructive `/apps/` launch surfaces. FLASH is
# for `fw flash` over SWD during BSP development and is deliberately awkward to
# reach by accident.
function(fw2_finalize_app TARGET WINDOW)
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "fw2_finalize_app: '${TARGET}' is not a target")
    endif()

    if(WINDOW STREQUAL "PSRAM")
        # A PSRAM app is an ordinary XIP image whose execute-in-place window is
        # the APS6404L on QMI CS1 at 0x11000000 rather than the flash on CS0.
        # NOT no_flash, NOT copy_to_ram -- the default binary type, relocated.
        pico_add_linker_script_override_path(${TARGET}
            ${FW2_APP_CMAKE_DIR}/psram_app_ld
            FILES pico_flash_region.ld pico_psram_region.ld)

        # The app enters with PSRAM already brought up by the stub and EXECUTES
        # from that window. The board header defines PICO_PSRAM_CS_PIN, which
        # arms the SDK's runtime_init_setup_psram -- that would re-time the
        # memory the next instruction is fetched from, before main(). The PSRAM
        # app contract forbids exactly this. (AGENTS.md invariant 12.)
        target_compile_definitions(${TARGET} PRIVATE
            FW2_PSRAM_APP=1
            PICO_RUNTIME_SKIP_INIT_PSRAM=1)

        # Added to the APP, not the BSP archive: this overrides a weak SDK
        # symbol, and a definition sitting in a static library is not guaranteed
        # to displace one the linker has already resolved.
        target_sources(${TARGET} PRIVATE
            ${FW2_APP_CMAKE_DIR}/../platform/psram_app_runtime.c)

        # picotool refuses the PSRAM window ("entry point is not in mapped part
        # of file") because 0x11000000 is not a device range it knows, so
        # pico_add_extra_outputs is unusable here. The .bin is fine; wrap it.
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}>
                    $<TARGET_FILE_DIR:${TARGET}>/${TARGET}.bin
            COMMAND ${Python3_EXECUTABLE} ${FW2_TOOLS_DIR}/bin2uf2.py
                    $<TARGET_FILE_DIR:${TARGET}>/${TARGET}.bin
                    $<TARGET_FILE_DIR:${TARGET}>/${TARGET}.uf2
                    --base 0x11000000 --family rp2350-arm-s
            VERBATIM)
        set(_win psram)

    elseif(WINDOW STREQUAL "SRAM")
        pico_set_binary_type(${TARGET} no_flash)
        pico_add_extra_outputs(${TARGET})
        set(_win sram)

    elseif(WINDOW STREQUAL "FLASH")
        pico_set_binary_type(${TARGET} copy_to_ram)
        pico_add_extra_outputs(${TARGET})
        set(_win flash)
        message(WARNING
            "${TARGET} is a FLASH app: installing it REPLACES the stock DISPLAY "
            "firmware (FW2Display).\n"
            "  This is correct for `fw flash` over SWD. It must NOT be copied to "
            "/apps/ on the SD card -- that surface is SRAM/PSRAM only.\n"
            "  If you wanted a loadable app, use SRAM or PSRAM instead.")

    else()
        message(FATAL_ERROR
            "fw2_finalize_app(${TARGET} ${WINDOW}): window must be one of "
            "SRAM, PSRAM, FLASH")
    endif()

    # Verify the artifact against the window it claims, every build. Catches a
    # binary type that does not match the declared intent, an image that is not
    # based at the window, and a vector table that would not launch.
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${FW2_TOOLS_DIR}/uf2check.py
                $<TARGET_FILE_DIR:${TARGET}>/${TARGET}.uf2 --window ${_win}
        VERBATIM)
endfunction()
