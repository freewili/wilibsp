# App target name MUST be the single token 'template' — `fw new-app <name>`
# rewrites every 'template' occurrence to the new app name.
add_executable(template main.c)
target_link_libraries(template freewili2_bsp)

# Where this app runs. `fw new-app <name> --window <w>` rewrites the token
# below; everything that follows from the choice is handled for you.
#
#   FLASH  written to the display's QSPI flash and run from there
#          (copy_to_ram). Programmed over SWD with `fw flash`. This REPLACES
#          the stock display firmware, FW2Display -- fine for BSP development,
#          never for the SD card.
#   SRAM   staged into SRAM by the fused bootloader. Loadable from the SD
#          card's /apps/, launched with `fw run-app`, flash untouched.
#   PSRAM  staged into the 8 MB window at 0x11000000 by FW2PsramStub. Same
#          non-destructive launch surface as SRAM, with far more room. The
#          PSRAM-specific boot requirements -- moving the XIP window, skipping
#          the SDK's PSRAM re-init, sparing the bank-0 pad that carries XIP
#          CS1, and emitting a UF2 picotool refuses to make -- are all applied
#          by fw2_finalize_app(); do not hand-roll them.
#
# Whichever you pick, the built UF2 is verified against it on every build, so
# an app cannot silently end up targeting a window it did not declare.
fw2_finalize_app(template FLASH)
