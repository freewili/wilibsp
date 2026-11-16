add_executable(emberward main.c ew_game.c ew_input.c ew_scores.c ew_sfx.c ew_mix.c ew_render.c ew_lcd.c)
target_link_libraries(emberward freewili2_bsp onewili_fwgui)
fw2_display_app(emberward
    POWER_ZONES DISPLAY AUDIO SDCARD
    VERSION 001
    DESCRIPTION "Emberward: guard the core on your TV. Block embers, feed glims, TV remote as controller")
