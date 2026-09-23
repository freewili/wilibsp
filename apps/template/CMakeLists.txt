# App target name MUST be the single token 'template' — `fw new-app <name>`
# rewrites every 'template' occurrence to the new app name.
add_executable(template main.c)
target_link_libraries(template freewili2_bsp)
fw2_display_app(template
    POWER_ZONES DISPLAY
    VERSION 003
    DESCRIPTION "Minimal starting point for a FreeWili 2 RAM application")
