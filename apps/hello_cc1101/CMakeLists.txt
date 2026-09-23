add_executable(hello_cc1101 main.c)
target_link_libraries(hello_cc1101 freewili2_bsp)
fw2_display_app(hello_cc1101
    POWER_ZONES DISPLAY SENSORS SUBGHZ
    VERSION 003
    DESCRIPTION "CC1101 radio probe, RSSI scan, and GDO capture test")
