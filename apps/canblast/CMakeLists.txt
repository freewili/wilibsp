add_executable(canblast main.c)
target_link_libraries(canblast freewili2_bsp onewili_fwgui)
# Power-comparison baseline (see main.c): never idle, keep inherited rails.
option(CANBLAST_POWER_BASELINE "canblast: build the power-comparison baseline" OFF)
if(CANBLAST_POWER_BASELINE)
    target_compile_definitions(canblast PRIVATE CANBLAST_POWER_BASELINE=1)
endif()
fw2_display_app(canblast
    VERSION 002
    DESCRIPTION "CAN FD blaster: high-rate transmit and receive through the OneWili display link"
    REPOSITORY "https://github.com/freewili/wilibsp")
