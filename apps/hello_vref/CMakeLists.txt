# App target name MUST be the single token 'hello_vref' — `fw new-app <name>`
# rewrites every 'hello_vref' occurrence to the new app name.
add_executable(hello_vref main.c)
target_link_libraries(hello_vref freewili2_bsp onewili_fwgui hardware_adc)
fw2_display_app(hello_vref
    POWER_ZONES DISPLAY SENSORS
    VERSION 003
    DESCRIPTION "Visible external GPIO VIO selection and toggle test")
