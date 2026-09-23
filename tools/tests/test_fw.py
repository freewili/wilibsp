import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import fw
fw.SD_HANDOFF_SETTLE_SECONDS = 0

class FakeSerial:
    def __init__(self, replies):
        self.replies = iter(replies)
        self.writes = []
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def reset_input_buffer(self): pass
    def write(self, data): self.writes.append(data)
    def readline(self): return next(self.replies, b"")

def app_uf2(address=0x11000000, block_no=0, num_blocks=1):
    import struct
    data = bytearray(512)
    struct.pack_into("<8I", data, 0, 0x0A324655, 0x9E5D5157, 0,
                     address, 256, block_no, num_blocks, 0xE48BFF59)
    struct.pack_into("<I", data, 508, 0x0AB16F30)
    return bytes(data)

def test_new_app_copies_template(tmp_path):
    root = tmp_path
    (root / "apps" / "template").mkdir(parents=True)
    (root / "apps" / "template" / "CMakeLists.txt").write_text(
        'add_executable(template main.c)\n')
    (root / "apps" / "template" / "main.c").write_text("// template\n")

    dest = fw.new_app("blinky", repo_root=root)

    assert dest == root / "apps" / "blinky"
    assert (dest / "main.c").exists()
    # the app target must be renamed from 'template' to the new name
    assert "add_executable(blinky" in (dest / "CMakeLists.txt").read_text()

def test_new_app_rejects_existing(tmp_path):
    root = tmp_path
    (root / "apps" / "blinky").mkdir(parents=True)
    try:
        fw.new_app("blinky", repo_root=root)
        assert False, "expected FileExistsError"
    except FileExistsError:
        pass

def test_install_app_copies_to_apps_and_returns_sd_without_windows_eject(tmp_path, monkeypatch):
    source = tmp_path / "mesh.uf2"
    source.write_bytes(app_uf2())
    volume = tmp_path / "card"
    volume.mkdir()
    calls = []
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setattr(fw, "_mounted_volumes", lambda: {tmp_path / "old"})
    monkeypatch.setattr(fw, "_wait_for_sd", lambda baseline, timeout: volume)
    monkeypatch.setattr(fw, "_set_sd_host", lambda port, pc: calls.append((port, pc)))

    fw.install_app(source, "FW123")

    assert (volume / "apps" / "mesh.uf2").read_bytes() == app_uf2()
    assert calls == [("COM7", True), ("COM7", False)]

def test_install_app_copies_to_nested_apps_folder(tmp_path, monkeypatch):
    source = tmp_path / "mesh.uf2"
    source.write_bytes(app_uf2())
    volume = tmp_path / "card"
    volume.mkdir()
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setattr(fw, "_mounted_volumes", set)
    monkeypatch.setattr(fw, "_wait_for_sd", lambda baseline, timeout: volume)
    monkeypatch.setattr(fw, "_set_sd_host", lambda port, pc: None)
    monkeypatch.setattr(fw, "_eject_volume", lambda path: None)

    fw.install_app(source, folder="beta/radio")

    assert (volume / "apps" / "beta" / "radio" / "mesh.uf2").read_bytes() == app_uf2()

def test_install_app_batches_multiple_uf2s_in_one_handoff(tmp_path, monkeypatch):
    first = tmp_path / "one.uf2"
    second = tmp_path / "two.uf2"
    first.write_bytes(app_uf2())
    second.write_bytes(app_uf2(address=0x20000000))
    volume = tmp_path / "card"
    volume.mkdir()
    calls = []
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setattr(fw, "_mounted_volumes", set)
    monkeypatch.setattr(fw, "_wait_for_sd", lambda baseline, timeout: volume)
    monkeypatch.setattr(fw, "_set_sd_host", lambda port, pc: calls.append(pc))

    fw.install_app([first, second], folder="daver")

    assert (volume / "apps" / "daver" / "one.uf2").read_bytes() == first.read_bytes()
    assert (volume / "apps" / "daver" / "two.uf2").read_bytes() == second.read_bytes()
    assert calls == [True, False]

def test_windows_unmount_does_not_use_shell_eject():
    source = pathlib.Path(fw.__file__).read_text()
    body = source[source.index("def _eject_volume"):source.index("def _app_subfolder")]
    assert "FSCTL_LOCK_VOLUME" in body or "0x00090018" in body
    assert "FSCTL_DISMOUNT_VOLUME" in body or "0x00090020" in body
    assert "InvokeVerb('Eject')" not in body

def test_run_app_sends_apps_relative_path(monkeypatch):
    wire = FakeSerial([b"noise\n", b"[a\\r 1]\n", b"[d Ok 1]\n"])
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setitem(sys.modules, "serial", type("SerialModule", (), {
        "Serial": staticmethod(lambda *args, **kwargs: wire)
    }))
    fw.run_app("wilibsp/hello_psram_exec.uf2", "FW123")
    assert wire.writes == [b"\x02a\\r wilibsp/hello_psram_exec.uf2\n"]


def test_run_app_waits_for_deferred_loader_failure(monkeypatch):
    wire = FakeSerial([b"[a\\r 1]\n", b"[d psram stub did not answer 0]\n"])
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setitem(sys.modules, "serial", type("SerialModule", (), {
        "Serial": staticmethod(lambda *args, **kwargs: wire)
    }))
    try:
        fw.run_app("team/bad.uf2", "FW123")
        assert False, "expected deferred loader failure"
    except RuntimeError as exc:
        assert "psram stub did not answer" in str(exc)


def test_sd_host_waits_for_complete_slow_mount_response(monkeypatch):
    wire = FakeSerial([b"[h\\x\\k ABC 9 ", b"main 1]\r\n"])
    monkeypatch.setitem(sys.modules, "serial", type("SerialModule", (), {
        "Serial": staticmethod(lambda *args, **kwargs: wire)
    }))
    fw._set_sd_host("COM7", False)
    assert wire.writes == [b"\x02h\\x\\k 0\n", b"\x02"]

def test_run_app_rejects_unsafe_paths_before_hardware(monkeypatch):
    monkeypatch.setattr(fw, "_fwfinder_main_port",
                        lambda serial: (_ for _ in ()).throw(AssertionError("hardware touched")))
    for path in ("", "../bad.uf2", "/bad.uf2", "team//bad.uf2", "team" + chr(92) + "bad.uf2", "bad.bin"):
        try:
            fw.run_app(path)
            assert False, "expected ValueError"
        except ValueError:
            pass

def test_install_app_rejects_escaping_or_ambiguous_subfolders_before_hardware(tmp_path, monkeypatch):
    source = tmp_path / "mesh.uf2"
    source.write_bytes(app_uf2())
    monkeypatch.setattr(fw, "_fwfinder_main_port",
                        lambda serial: (_ for _ in ()).throw(AssertionError("hardware touched")))
    for folder in ("../outside", "/outside", "team//app", "team\\app"):
        try:
            fw.install_app(source, folder=folder)
            assert False, "expected ValueError"
        except ValueError:
            pass

def test_install_app_returns_sd_when_no_volume_ever_mounted(tmp_path, monkeypatch):
    source = tmp_path / "mesh.uf2"
    source.write_bytes(app_uf2())
    calls = []
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setattr(fw, "_mounted_volumes", set)
    monkeypatch.setattr(fw, "_wait_for_sd", lambda baseline, timeout: (_ for _ in ()).throw(RuntimeError("no card")))
    monkeypatch.setattr(fw, "_set_sd_host", lambda port, pc: calls.append(pc))

    try:
        fw.install_app(source)
        assert False, "expected RuntimeError"
    except RuntimeError as exc:
        assert "no card" in str(exc)
    assert calls == [True, False]

def test_install_app_returns_sd_after_copy_failure(tmp_path, monkeypatch):
    source = tmp_path / "mesh.uf2"
    source.write_bytes(app_uf2())
    volume = tmp_path / "card"
    volume.mkdir()
    calls = []
    monkeypatch.setattr(fw, "_fwfinder_main_port", lambda serial: "COM7")
    monkeypatch.setattr(fw, "_mounted_volumes", set)
    monkeypatch.setattr(fw, "_wait_for_sd", lambda baseline, timeout: volume)
    monkeypatch.setattr(fw, "_set_sd_host", lambda port, pc: calls.append(("host", pc)))
    monkeypatch.setattr(fw.shutil, "copyfile", lambda *args: (_ for _ in ()).throw(OSError("copy failed")))

    try:
        fw.install_app(source)
        assert False, "expected OSError"
    except OSError as exc:
        assert "copy failed" in str(exc)
    assert calls == [("host", True), ("host", False)]

def test_install_app_rejects_flash_before_touching_hardware(tmp_path, monkeypatch):
    source = tmp_path / "bad.uf2"
    source.write_bytes(app_uf2(0x10000000))
    monkeypatch.setattr(fw, "_fwfinder_main_port",
                        lambda serial: (_ for _ in ()).throw(AssertionError("hardware touched")))
    try:
        fw.install_app(source)
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "QSPI flash" in str(exc)

def test_check_app_uf2_rejects_incomplete_declared_block_set(tmp_path):
    source = tmp_path / "truncated.uf2"
    source.write_bytes(app_uf2(num_blocks=2))
    try:
        fw.check_app_uf2(source)
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "incomplete" in str(exc)

def test_check_app_uf2_rejects_duplicate_block_number(tmp_path):
    source = tmp_path / "duplicate.uf2"
    source.write_bytes(app_uf2(block_no=0, num_blocks=2) * 2)
    try:
        fw.check_app_uf2(source)
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "duplicates" in str(exc)

def test_check_app_uf2_rejects_inconsistent_block_totals(tmp_path):
    source = tmp_path / "inconsistent.uf2"
    source.write_bytes(app_uf2(block_no=0, num_blocks=2) +
                       app_uf2(address=0x11000100, block_no=1, num_blocks=3))
    try:
        fw.check_app_uf2(source)
        assert False, "expected ValueError"
    except ValueError as exc:
        assert "inconsistent" in str(exc)

def test_build_command_uses_target_preset():
    cmd = fw.build_command("hello_display")
    assert cmd[:3] == ["cmake", "--build", "--preset"]
    assert "target" in cmd

def test_test_command_configures_and_runs_ctest():
    cmds = fw.test_command()
    # Standalone tests/ tree: configured directly into build-tests/, no
    # Pico-SDK-coupled preset involved (fw test works without the SDK).
    configure = cmds[0]
    assert configure[:2] == ["cmake", "-S"]
    assert str(fw.REPO_ROOT / "tests") in configure
    assert "-B" in configure
    assert str(fw.REPO_ROOT / "build-tests") in configure[configure.index("-B") + 1]

    build = cmds[1]
    assert build[:2] == ["cmake", "--build"]
    assert str(fw.REPO_ROOT / "build-tests") in build

    ctest = cmds[-1]
    assert ctest[0] == "ctest"
    assert "--test-dir" in ctest
    assert str(fw.REPO_ROOT / "build-tests") in ctest[ctest.index("--test-dir") + 1]

def test_flash_command_programs_correct_elf():
    cmd = fw.flash_command("hello_display")
    # cmd[0] is a discovered openocd exe (pico-sdk install path or bare "openocd")
    assert "openocd" in cmd[0].lower()
    assert "-f" in cmd and fw.OPENOCD_CFG in cmd
    program_arg = cmd[-1]
    assert "build/apps/hello_display/hello_display.elf" in program_arg
    assert "verify" in program_arg
    assert "reset" in program_arg

def test_rtt_command_sets_up_and_serves_rtt():
    cmd = fw.rtt_command()
    assert "openocd" in cmd[0].lower()
    assert "-f" in cmd and fw.OPENOCD_CFG in cmd
    assert any("rtt setup" in c for c in cmd)
    assert any(f"rtt server start {fw.RTT_PORT}" in c for c in cmd)

def test_main_print_dispatches_build_command(capsys):
    fw.main(["build", "--print"])
    captured = capsys.readouterr()
    assert "cmake --build --preset target --target hello_display" in captured.out

def test_configure_command_pins_sdk_and_uses_preset():
    cmd = fw.configure_command()
    assert cmd[:3] == ["cmake", "--preset", "target"]
    sdk = [c for c in cmd if c.startswith("-DPICO_SDK_PATH=")]
    # Only asserted when the pinned SDK is actually installed on this machine.
    if fw._pico_sdk_dir("sdk", fw.PICO_SDK_VERSION) is not None:
        assert sdk and sdk[0].endswith(fw.PICO_SDK_VERSION)

def test_configure_command_never_overrides_pico_board():
    # AGENTS.md invariant 1/8: -DPICO_BOARD on the command line overrides the
    # cached value from the top-level CMakeLists and reverts the board config.
    assert not any("PICO_BOARD" in c for c in fw.configure_command())

def test_pico_sdk_dir_falls_back_to_newest_installed(tmp_path, monkeypatch):
    root = tmp_path / ".pico-sdk" / "sdk"
    (root / "2.1.0").mkdir(parents=True)
    (root / "2.4.0").mkdir(parents=True)
    monkeypatch.setattr(fw.pathlib.Path, "home", staticmethod(lambda: tmp_path))
    assert fw._pico_sdk_dir("sdk", "2.1.0").name == "2.1.0"   # pinned wins when present
    assert fw._pico_sdk_dir("sdk", "9.9.9").name == "2.4.0"   # else newest installed

def test_needs_configure_detects_missing_and_stale_trees(tmp_path, monkeypatch):
    monkeypatch.setattr(fw, "BUILD_DIR", tmp_path / "build")
    assert fw.needs_configure() is True                       # no build tree at all

    sdk = fw._pico_sdk_dir("sdk", fw.PICO_SDK_VERSION)
    if sdk is None:
        return                                                # no SDK installed here
    (tmp_path / "build").mkdir()
    cache = tmp_path / "build" / "CMakeCache.txt"
    cache.write_text(f"PICO_SDK_PATH:PATH={sdk}\n")
    # a cache without a generator file means the configure died part-way
    assert fw.needs_configure() is True
    (tmp_path / "build" / "build.ninja").write_text("")
    assert fw.needs_configure() is False                      # already on the pinned SDK
    cache.write_text("PICO_SDK_PATH:PATH=/somewhere/sdk/1.0.0\n")
    assert fw.needs_configure() is True                       # configured against another SDK

def test_packbits_decode_handles_literal_and_repeat_runs():
    # literal run of two units, then a repeat run of three
    data = bytes([1, 0x12, 0x34, 0xAB, 0xCD, 0xFE, 0x07, 0xE0])
    assert fw.packbits_decode(data, 5) == [0x1234, 0xABCD, 0x07E0, 0x07E0, 0x07E0]

def test_packbits_decode_rejects_truncated_input():
    try:
        fw.packbits_decode(bytes([1, 0x12]), 2)
        assert False, "expected ValueError"
    except ValueError:
        pass

def test_png_write_produces_a_valid_signature_and_size(tmp_path):
    out = tmp_path / "x.png"
    # 2x1 image: one red pixel, one blue
    fw.png_write(str(out), 2, 1, [0xF800, 0x001F])
    blob = out.read_bytes()
    assert blob[:8] == b"\x89PNG\r\n\x1a\n"
    assert b"IHDR" in blob and b"IDAT" in blob and b"IEND" in blob
    # IHDR width/height are big-endian at a fixed offset
    import struct
    w, h = struct.unpack(">II", blob[16:24])
    assert (w, h) == (2, 1)

def test_rtt_command_serves_both_channels():
    cmd = fw.rtt_command()
    assert any(f"rtt server start {fw.RTT_PORT} 0" in c for c in cmd)
    assert any(f"rtt server start {fw.AGENTIO_PORT} 1" in c for c in cmd)

def test_main_print_dispatches_screenshot():
    # --print must not touch hardware
    fw.main(["screenshot", "--print"])

def test_agentio_enter_cleans_up_leaked_process_on_timeout(monkeypatch):
    # Regression: if OpenOCD never opens the RTT port within the deadline,
    # __enter__ must still terminate the process it spawned. Python skips
    # __exit__ when __enter__ raises, so without explicit cleanup inside
    # __enter__ this would leak an OpenOCD process holding the debug probe.
    monkeypatch.setattr(fw, "_port_open", lambda port: False)

    class FakeProc:
        def __init__(self):
            self.terminated = False
            self.killed = False
        def poll(self):
            return None  # still "running" the whole time
        def terminate(self):
            self.terminated = True
        def wait(self, timeout=None):
            return 0
        def kill(self):
            self.killed = True

    fake = FakeProc()
    monkeypatch.setattr(fw.subprocess, "Popen", lambda *a, **k: fake)

    # A fake clock that advances past the 10s deadline in a few calls, and a
    # no-op sleep, so the test does not actually wait 10 seconds.
    class FakeClock:
        def __init__(self):
            self.t = 0
        def time(self):
            self.t += 3
            return self.t

    monkeypatch.setattr(fw.time, "time", FakeClock().time)
    monkeypatch.setattr(fw.time, "sleep", lambda s: None)

    try:
        with fw._Agentio():
            assert False, "expected RuntimeError"
    except RuntimeError:
        pass

    assert fake.terminated is True

def test_agentio_capture_surfaces_err_reply_instead_of_hanging(monkeypatch):
    # Regression: every CAP error reply ("ERR rect\n" = 9 bytes, "ERR
    # surface\n" = 12 bytes) is shorter than AGENTIO_HEADER_LEN (18). The old
    # agentio_capture() did an unconditional recv_exact(AGENTIO_HEADER_LEN)
    # first, which can never return for these replies — the CLI would block
    # until the raw socket timeout instead of raising the server's message.
    monkeypatch.setattr(fw, "_port_open", lambda port: True)  # skip spawning OpenOCD

    class FakeSocket:
        """Just enough of the socket API for _Agentio: recv() hands back
        bytes from a fixed buffer, one chunk at a time, like a real socket."""
        def __init__(self, data):
            self.data = data
            self.pos = 0
        def settimeout(self, t):
            pass
        def sendall(self, b):
            pass
        def recv(self, n):
            chunk = self.data[self.pos:self.pos + n]
            self.pos += len(chunk)
            return chunk
        def close(self):
            pass

    fake = FakeSocket(b"ERR rect\n")
    monkeypatch.setattr(fw.socket, "create_connection", lambda *a, **k: fake)

    try:
        fw.agentio_capture("lcd", (0, 0, 0, 0), 1, "unused.png")
        assert False, "expected RuntimeError"
    except RuntimeError as e:
        assert "rect" in str(e)

def elf(segments, phentsize=32):
    """Minimal 32-bit LE ELF carrying the given (paddr, filesz) LOAD segments."""
    import struct
    phoff = 52
    header = bytearray(52)
    header[0:4] = b"\x7fELF"
    header[4:6] = b"\x01\x01"
    struct.pack_into("<I", header, 28, phoff)
    struct.pack_into("<HH", header, 42, phentsize, len(segments))
    table = bytearray()
    body = bytearray()
    for paddr, filesz in segments:
        offset = phoff + phentsize * len(segments) + len(body)
        table += struct.pack("<8I", 1, offset, paddr, paddr, filesz, filesz, 5, 0x1000)
        body += bytes(filesz)
    return bytes(header) + bytes(table) + bytes(body)


def test_elf_load_segments_reports_physical_addresses():
    # A copy_to_ram image RUNS from SRAM but is STORED in flash; the physical
    # address is the one a debugger writes.
    blob = elf([(0x10000000, 256)])
    assert fw.elf_load_segments(blob) == [(0x10000000, 256)]


def test_elf_load_segments_ignores_nobits():
    import struct
    blob = bytearray(elf([(0x20000000, 256)]))
    struct.pack_into("<I", blob, 52 + 16, 0)          # filesz = 0 -> NOLOAD
    assert fw.elf_load_segments(bytes(blob)) == []


def test_elf_load_segments_rejects_non_elf():
    import pytest
    with pytest.raises(ValueError):
        fw.elf_load_segments(b"not an elf at all")


def test_flash_refuses_an_elf_stored_in_qspi_flash(tmp_path):
    import pytest
    path = tmp_path / "app.elf"
    path.write_bytes(elf([(0x10000000, 512), (0x10000210, 4096)]))
    with pytest.raises(ValueError) as excinfo:
        fw.check_flash_elf(path)
    message = str(excinfo.value)
    assert "REPLACE the stock DISPLAY firmware" in message
    assert "fw install-app" in message
    assert "0x10000000" in message


def test_flash_accepts_sram_and_psram_targets(tmp_path):
    for address in (0x20000000, 0x11000000):
        path = tmp_path / f"app_{address:08x}.elf"
        path.write_bytes(elf([(address, 4096)]))
        fw.check_flash_elf(path)                       # must not raise


def test_flash_catches_a_segment_that_only_overlaps_flash(tmp_path):
    import pytest
    # Ends inside the window rather than starting in it.
    path = tmp_path / "app.elf"
    path.write_bytes(elf([(0x0FFFFF00, 0x200)]))
    with pytest.raises(ValueError):
        fw.check_flash_elf(path)


def test_flash_command_is_guarded_by_default(tmp_path, monkeypatch):
    import pytest
    root = tmp_path
    (root / "build" / "apps" / "boom").mkdir(parents=True)
    (root / "build" / "apps" / "boom" / "boom.elf").write_bytes(
        elf([(0x10000000, 512)]))
    monkeypatch.setattr(fw, "REPO_ROOT", root)
    with pytest.raises(ValueError):
        fw.flash_command("boom")


def test_flash_command_override_allows_firmware_replacement(tmp_path, monkeypatch):
    root = tmp_path
    (root / "build" / "apps" / "boom").mkdir(parents=True)
    (root / "build" / "apps" / "boom" / "boom.elf").write_bytes(
        elf([(0x10000000, 512)]))
    monkeypatch.setattr(fw, "REPO_ROOT", root)
    command = fw.flash_command("boom", replace_display_firmware=True)
    assert "program build/apps/boom/boom.elf verify reset exit" in command


def test_flash_command_without_a_build_still_produces_a_command(tmp_path, monkeypatch):
    # Nothing built yet: let OpenOCD report the missing file rather than
    # failing here with a confusing ELF-parse error.
    monkeypatch.setattr(fw, "REPO_ROOT", tmp_path)
    command = fw.flash_command("nothing_here")
    assert any("nothing_here.elf" in part for part in command)
