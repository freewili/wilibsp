add_executable(toggleled main.c)
target_link_libraries(toggleled freewili2_bsp onewili_fwgui)
fw2_display_app(toggleled
    POWER_ZONES DISPLAY SENSORS
    VERSION 003
    DESCRIPTION "Visible external header GPIO toggle through OneWili")
