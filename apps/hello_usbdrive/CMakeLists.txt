add_executable(hello_usbdrive main.c)
target_link_libraries(hello_usbdrive freewili2_bsp)
fw2_display_app(hello_usbdrive
    POWER_ZONES DISPLAY SENSORS USB_HUB
    VERSION 003
    DESCRIPTION "Mount and inspect a USB mass-storage device")
