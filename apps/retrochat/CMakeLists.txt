add_executable(retrochat
    main.c
    audio_glue.c
    protocol.c
    ui.c
    compose.c
    modem/crc16.c
    modem/frame.c
    modem/msgq.c
    modem/afsk_mod.c
    modem/afsk_demod.c)
target_include_directories(retrochat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(retrochat freewili2_bsp pico_multicore pico_unique_id)
fw2_display_app(retrochat
    POWER_ZONES DISPLAY AUDIO
    VERSION 003
    DESCRIPTION "Acoustic text chat using the speaker and microphones")
