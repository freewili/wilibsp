# App target name MUST be the single token 'hello_applink' — `fw new-app <name>`
# rewrites every 'hello_applink' occurrence to the new app name.
add_executable(hello_applink main.c)
target_link_libraries(hello_applink freewili2_bsp applink)
# No ESP32 power zone exists in fw2_display_app's POWER_ZONES list (zone 5 is
# MAIN's to manage, and the flasher menu command declares it itself). A DISPLAY
# app therefore cannot demand the ESP32 rail; if it is down, AppLink payloads
# are dropped silently by MAIN. See libs/applink/README.md.
fw2_display_app(hello_applink
    POWER_ZONES DISPLAY
    VERSION 001
    DESCRIPTION "AppLink tunnel smoke test: DISPLAY to ESP32-C5 via MAIN")
