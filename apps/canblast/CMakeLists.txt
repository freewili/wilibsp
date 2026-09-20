add_executable(canblast main.c)
target_link_libraries(canblast freewili2_bsp onewili_fwgui)
fw2_display_app(canblast
    VERSION 001
    DESCRIPTION "CAN FD blaster: high-rate transmit and receive through the OneWili display link"
    REPOSITORY "https://github.com/freewili/wilibsp")
