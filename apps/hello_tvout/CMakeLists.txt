add_executable(hello_tvout main.c)
target_link_libraries(hello_tvout freewili2_bsp)
fw2_display_app(hello_tvout
    POWER_ZONES DISPLAY
    VERSION 004
    DESCRIPTION "TV output: the app's picture on the TV and the LCD, page flip, 2x, plug events, sound")
