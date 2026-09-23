add_executable(hello_ir main.c)
target_link_libraries(hello_ir freewili2_bsp)
fw2_display_app(hello_ir
    POWER_ZONES DISPLAY SENSORS
    VERSION 003
    DESCRIPTION "Visible infrared transmit, receive, and decode loopback")
