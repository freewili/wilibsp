add_executable(dualcpu main.c)
target_link_libraries(dualcpu freewili2_bsp onewili_fwgui)
# RGB_LEDS is zone 10, the ESP32's LED 5 V rail: picpwr_release_unused() would
# otherwise switch it off. The ESP32's own rail (zone 5) has no POWER_ZONES
# name and is requested in main.c.
fw2_display_app(dualcpu
    POWER_ZONES DISPLAY RGB_LEDS
    VERSION 002
    DESCRIPTION "Dual-CPU demo: DISPLAY and ESP32 apps exchange ping, telemetry and LED control over OneWili peer streams")
