add_executable(hello_charger main.c)
target_link_libraries(hello_charger freewili2_bsp)
fw2_display_app(hello_charger
    POWER_ZONES DISPLAY
    VERSION 003
    DESCRIPTION "Battery charger and USB-C status display")
