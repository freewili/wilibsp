add_executable(hello_cec main.c)
target_link_libraries(hello_cec freewili2_bsp)
fw2_display_app(hello_cec
    POWER_ZONES DISPLAY
    VERSION 007
    DESCRIPTION "CEC on the TV connector: wake the TV, standby, remote keys, a power-status request")
