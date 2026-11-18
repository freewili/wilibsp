add_executable(hello_wilirfid main.c)
target_link_libraries(hello_wilirfid freewili2_bsp onewili_fwgui)
fw2_display_app(hello_wilirfid
    POWER_ZONES DISPLAY NFC_RFID SDCARD
    VERSION 001
    DESCRIPTION "125 kHz RFID: read any card, clone, emulate, save and load")
