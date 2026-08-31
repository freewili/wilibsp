# AppLink — DISPLAY-app <-> ESP32 byte tunnel, relayed by MAIN.
#
# applink_protocol.h is a COPY. Source of truth:
# freewili-firmware/shared/applink_protocol.h, shared verbatim with MAIN's relay
# (freewilimain/rmpLib/fwgui_applink.cpp) and the ESP32 firmware
# (bottlenose-firmware/main/applink.cpp). Re-copy it when the wire changes; the
# three copies drifting apart is the failure mode this note exists to prevent.
cmake_minimum_required(VERSION 3.20)

add_library(applink STATIC
    applink.c
)
target_include_directories(applink PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(applink PUBLIC pico_stdlib hardware_uart hardware_gpio)
