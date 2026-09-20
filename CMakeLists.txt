cmake_minimum_required(VERSION 3.13)

# RP2350B board header (PICO_RP2350A=0 -> 48 GPIO, 16MB flash).
# Do NOT pass -DPICO_BOARD on the command line — that overrides this.
set(PICO_BOARD freewili2 CACHE STRING "Board type")
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/bsp/boards")

# agentio: remote input injection + screen capture for agent E2E verification.
# OFF strips it entirely — no PSRAM shadow, no RTT buffers, no draw-path cost.
option(FW2_AGENTIO "Build the agentio remote-control/capture harness" ON)

include(pico_sdk_import.cmake)
project(wilibsp C CXX ASM)
set(CMAKE_C_STANDARD 11)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
pico_sdk_init()

add_subdirectory(bsp)

# Reusable non-BSP libraries.
add_subdirectory(libs/onewili/wilibsp)   # the submodule ships every OneWili language package; wilibsp/ is the display-CPU C one

# Each app opts in; add new apps here.
add_subdirectory(apps/hello_audio)
add_subdirectory(apps/hello_cc1101)
add_subdirectory(apps/hello_display)
add_subdirectory(apps/hello_ir)
add_subdirectory(apps/hello_mics)
add_subdirectory(apps/hello_sdcard)
add_subdirectory(apps/hello_sensors)
add_subdirectory(apps/hello_usbdrive)
add_subdirectory(apps/hello_dvi)
add_subdirectory(apps/toggleled)
add_subdirectory(apps/retrochat)
add_subdirectory(apps/hello_keyboard)
add_subdirectory(apps/hello_charger)
add_subdirectory(apps/hello_agentio)
add_subdirectory(apps/hello_vref)
add_subdirectory(apps/hello_psram_exec)
add_subdirectory(apps/canblast)
