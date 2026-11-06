add_executable(hello_rfid main.c)
target_link_libraries(hello_rfid freewili2_bsp)
fw2_display_app(hello_rfid
    POWER_ZONES DISPLAY NFC_RFID
    VERSION 001
    DESCRIPTION "125 kHz EM4100 RFID reader smoke test")
