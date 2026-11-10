add_executable(hello_wilinfc main.c ow_sd_io.c)
target_link_libraries(hello_wilinfc freewili2_bsp onewili_fwgui)
fw2_display_app(hello_wilinfc
    POWER_ZONES DISPLAY NFC_RFID SDCARD
    VERSION 001
    DESCRIPTION "13.56 MHz NFC: read a card, save it and emulate its UID")
