add_executable(hello_sdcard main.c)
target_link_libraries(hello_sdcard freewili2_bsp onewili_fwgui)
fw2_display_app(hello_sdcard
    POWER_ZONES DISPLAY SDCARD
    VERSION 003
    DESCRIPTION "Read and write the main CPU SD card through OneWili")
