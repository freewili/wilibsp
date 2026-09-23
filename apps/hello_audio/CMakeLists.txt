add_executable(hello_audio main.c)
target_link_libraries(hello_audio freewili2_bsp)
fw2_display_app(hello_audio
    POWER_ZONES DISPLAY SENSORS AUDIO
    VERSION 002
    DESCRIPTION "Speaker and audio-codec playback test with volume controls")
