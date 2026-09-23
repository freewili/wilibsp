add_executable(hello_display main.c)
target_link_libraries(hello_display freewili2_bsp)
fw2_display_app(hello_display
    POWER_ZONES DISPLAY RGB_LEDS
    VERSION 003
    DESCRIPTION "LCD, touch, and RGB LED hardware smoke test")
