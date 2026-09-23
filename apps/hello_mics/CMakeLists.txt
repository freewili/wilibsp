add_executable(hello_mics main.c)
target_link_libraries(hello_mics freewili2_bsp)
fw2_display_app(hello_mics
    POWER_ZONES DISPLAY SENSORS
    VERSION 003
    DESCRIPTION "PDM microphone capture and level display")
