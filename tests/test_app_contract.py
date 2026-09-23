from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "apps"


def app_dirs():
    return sorted(path.parent for path in APPS.glob("*/CMakeLists.txt")
                  if "add_executable(" in path.read_text(encoding="utf-8"))


def test_every_app_uses_contract_wrapper():
    missing = []
    for app in app_dirs():
        cmake = (app / "CMakeLists.txt").read_text(encoding="utf-8")
        if "fw2_display_app(" not in cmake and "fw2_psram_app(" not in cmake:
            missing.append(app.name)
    assert not missing, "apps missing a FW2 app wrapper: " + ", ".join(missing)


def test_metadata_generator_creates_its_clean_build_directory():
    cmake = (ROOT / "bsp/CMakeLists.txt").read_text(encoding="utf-8")
    directory = cmake.index('set(_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_uf2_info")')
    command = cmake.index("add_custom_command(", directory)
    assert 'file(MAKE_DIRECTORY "${_dir}")' in cmake[directory:command]


def test_every_app_initializes_and_services_recovery():
    missing = []
    for app in app_dirs():
        main = app / "main.c"
        if not main.exists():
            continue
        source = main.read_text(encoding="utf-8")
        required = ("fw2_app_recovery_init()", "fw2_app_recovery_task()")
        absent = [call for call in required if call not in source]
        if absent:
            missing.append("%s: %s" % (app.name, ", ".join(absent)))
    assert not missing, "apps missing recovery calls: " + "; ".join(missing)


def _matching_brace(source, opening):
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset
    raise AssertionError("unclosed loop body")


def test_every_terminal_loop_services_recovery():
    """A token elsewhere in main.c must not make an unserviceable loop pass."""
    missing = []
    terminal = re.compile(r"(?:while\s*\(\s*(?:true|1)\s*\)|for\s*\(\s*;\s*;\s*\))")
    for app in app_dirs():
        main = app / "main.c"
        if not main.exists():
            continue
        source = main.read_text(encoding="utf-8")
        for match in terminal.finditer(source):
            opening = source.find("{", match.end())
            semicolon = source.find(";", match.end())
            if opening < 0 or (semicolon >= 0 and semicolon < opening):
                missing.append(f"{app.name}:{source.count(chr(10), 0, match.start()) + 1}")
                continue
            body = source[opening + 1:_matching_brace(source, opening)]
            if "fw2_app_recovery_task()" not in body:
                missing.append(f"{app.name}:{source.count(chr(10), 0, match.start()) + 1}")
    assert not missing, "terminal loops missing recovery service: " + ", ".join(missing)


def test_apps_do_not_use_unserviceable_sleep():
    offenders = []
    direct_sleep = re.compile(r"(?<!fw2_app_recovery_)sleep_ms\s*\(")
    for app in app_dirs():
        main = app / "main.c"
        if main.exists() and direct_sleep.search(main.read_text(encoding="utf-8")):
            offenders.append(app.name)
    assert not offenders, (
        "apps must use fw2_app_recovery_sleep_ms(): " + ", ".join(offenders)
    )


def test_apps_do_not_use_blocking_pdm_capture():
    offenders = []
    for app in app_dirs():
        main = app / "main.c"
        if main.exists() and "pdm_capture_block(" in main.read_text(encoding="utf-8"):
            offenders.append(app.name)
    assert not offenders, "apps must service recovery while capturing PDM: " + ", ".join(offenders)


def test_onewili_apps_wrap_synchronous_transports():
    offenders = []
    for source_path in APPS.glob("*/*.c"):
        source = source_path.read_text(encoding="utf-8")
        main_at = source.find("int main(")
        if main_at < 0:
            continue
        body = source[main_at:]
        sync_calls = [at for token in ("ow_io_", "ow_sd_")
                      if (at := body.find(token)) >= 0]
        wrap_at = body.find("fw2_app_recovery_open_onewili(")
        if "onewili.h" in source and sync_calls and (wrap_at < 0 or
                                                      wrap_at > min(sync_calls)):
            offenders.append(str(source_path.relative_to(APPS)))
        first_sd = body.find("ow_sd_")
        wrap_sd = body.find("fw2_app_recovery_wrap_sd(")
        if first_sd >= 0 and (wrap_sd < 0 or wrap_sd > first_sd):
            offenders.append(str(source_path.relative_to(APPS)) + " (SD)")
    assert not offenders, "OneWili calls can starve HOME recovery: " + ", ".join(offenders)


def test_lcd_apps_establish_surface_before_backlight():
    offenders = []
    for source_path in APPS.glob("*/*.c"):
        source = source_path.read_text(encoding="utf-8")
        init_at = source.find("st7796_init()")
        if init_at < 0:
            continue
        clear_at = source.find("st7796_fill_screen(", init_at)
        light_at = source.find("board_backlight_set(1)", init_at)
        if clear_at < 0 or light_at < 0 or clear_at > light_at:
            offenders.append(str(source_path.relative_to(APPS)))
    assert not offenders, "LCD apps expose inherited panel pixels: " + ", ".join(offenders)


def test_board_startup_clears_persistent_leds_without_power_request():
    board = (ROOT / "bsp/platform/board.c").read_text(encoding="utf-8")
    driver = (ROOT / "bsp/leds/ws2812_driver.c").read_text(encoding="utf-8")
    assert "ws2812_clear_once(pio1, PIN_LED_DATA)" in board
    helper = driver[driver.index("void ws2812_clear_once("):]
    assert helper.count("temporary_zero_frame(pio, sm)") >= 2
    assert "pio_remove_program(" in helper
    assert "pio_sm_unclaim(" in helper
    assert "picpwr" not in helper


def test_declared_power_zones_are_runtime_enforced():
    """Target metadata, not handwritten app loops, owns rail maintenance."""
    cmake = (ROOT / "bsp/CMakeLists.txt").read_text(encoding="utf-8")
    generator = (ROOT / "tools/gen_uf2_info.py").read_text(encoding="utf-8")
    recovery = (ROOT / "bsp/input/app_recovery.c").read_text(encoding="utf-8")
    assert '"POWER_ZONES"' in cmake
    assert "--power-zones" in cmake
    assert "fw2app_power_zones" in generator
    assert "picpwr_keep_awake(fw2app_power_zones)" in recovery
    assert "(rails & fw2app_power_zones) != fw2app_power_zones" in recovery
    assert "app: power zones ready" in recovery
    assert "picpwr_task();" in recovery


def test_lcd_apps_declare_display_power_zone():
    offenders = []
    for source_path in APPS.glob("*/main.c"):
        source = source_path.read_text(encoding="utf-8")
        if "st7796_init()" not in source:
            continue
        cmake = (source_path.parent / "CMakeLists.txt").read_text(encoding="utf-8")
        if "POWER_ZONES" not in cmake or "DISPLAY" not in cmake:
            offenders.append(source_path.parent.name)
    assert not offenders, "LCD apps missing DISPLAY power declaration: " + ", ".join(offenders)


def test_native_and_pio_usb_hosts_do_not_export_the_same_hcd_init():
    """TinyUSB owns hcd_init; the BSP-native MSC host must stay namespaced."""
    header = (ROOT / "bsp/usbhost/usb_hcd.h").read_text(encoding="utf-8")
    source = (ROOT / "bsp/usbhost/usb_hcd.c").read_text(encoding="utf-8")
    assert "fw2_native_usb_hcd_init" in header
    assert "fw2_native_usb_hcd_init" in source
    assert "void        hcd_init(void)" not in header
    assert "void hcd_init(void)" not in source


def test_every_app_registers_an_about_surface():
    offenders = []
    for app in app_dirs():
        main = app / "main.c"
        if not main.exists():
            continue
        source = main.read_text(encoding="utf-8")
        if ("fw2_app_about_use_lcd()" not in source and
                "fw2_app_about_set_renderer(" not in source):
            offenders.append(app.name)
    assert not offenders, "apps missing PAGE-hold About screen: " + ", ".join(offenders)


def test_about_uses_embedded_version_and_repository_metadata():
    about = (ROOT / "bsp/input/app_about.c").read_text(encoding="utf-8")
    generator = (ROOT / "tools/gen_uf2_info.py").read_text(encoding="utf-8")
    assert "info->app_version" in about
    assert "fw2app_repository_url" in about
    assert "--repository" in generator


def test_about_release_restores_the_registered_surface():
    about = (ROOT / "bsp/input/app_about.c").read_text(encoding="utf-8")
    released = about[about.index("if (!down)"):about.index("if (!s_tracking)")]
    assert "s_shown && s_restore" in released
    assert "s_restore();" in released
    assert "memcpy(s_lcd_backup" in about
    assert "st7796_blit_rect(" in about
    assert "fw2_app_about_use_lcd_restore" in about
    assert "s_shown && s_lcd_modal" in released


def test_lcd_about_is_modal_without_blocking_the_app_loop():
    about = (ROOT / "bsp/input/app_about.c").read_text(encoding="utf-8")
    display = (ROOT / "bsp/display/st7796.c").read_text(encoding="utf-8")
    assert "st7796_set_write_suppressed(true)" in about
    assert "st7796_set_write_suppressed(false)" in about
    modal = about[about.index("s_shown = true;"):about.index("const fw2app_uf2_info_t")]
    assert modal.index("st7796_set_write_suppressed(true)") < modal.index("st7796_flush_wait()")
    assert modal.index("st7796_flush_wait()") < modal.index("memcpy(s_lcd_backup")
    for function in ("st7796_fill_screen", "st7796_fill_rect",
                     "st7796_blit_rect", "st7796_draw_text",
                     "st7796_flush_async"):
        body = display[display.index("void " + function):]
        assert "s_write_suppressed" in body[:500]


def test_psram_bootstrap_keeps_interrupts_off_during_qmi_retime():
    bootstrap = (ROOT / "bsp/app/psram_bootstrap.c").read_text(encoding="utf-8")
    assert bootstrap.index("board_init_psram();") < bootstrap.index('"cpsie i"')
    assert bootstrap.index("spin_locks_reset();") < bootstrap.index("runtime_init_default_alarm_pool();")
    assert bootstrap.index("spin_locks_reset();") < bootstrap.index("runtime_init_mutex();")
    assert bootstrap.index("runtime_init_mutex();") < bootstrap.index("runtime_init_default_alarm_pool();")
    assert bootstrap.index("runtime_init_default_alarm_pool();") < bootstrap.index("board_init_psram();")


def test_psram_bootstrap_runs_c_runtime_initializers_before_main():
    bootstrap = (ROOT / "bsp/app/psram_bootstrap.c").read_text(encoding="utf-8")
    init = "run_init_array(__init_array_start, __init_array_end);"
    assert init in bootstrap
    assert "run_init_array(__preinit_array_start" not in bootstrap
    assert bootstrap.index("board_init_psram();") < bootstrap.index(init) < bootstrap.index("(void)main();")


def test_psram_bootstrap_makes_app_board_init_skip_qmi_retime():
    board = (ROOT / "bsp/platform/board.c").read_text(encoding="utf-8")
    assert "if (s_psram_bootstrapped)" in board
    assert "board_init_inherited();" in board
    assert board.index("s_psram_bootstrapped = true;") > board.index("psram_reinitialize();")
