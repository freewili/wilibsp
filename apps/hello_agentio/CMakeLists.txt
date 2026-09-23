add_executable(hello_agentio main.c)
target_link_libraries(hello_agentio freewili2_bsp)
fw2_display_app(hello_agentio
    POWER_ZONES DISPLAY
    VERSION 003
    DESCRIPTION "Agent input injection, display capture, and RTT diagnostics smoke test")
