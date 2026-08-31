#!/usr/bin/env python3
"""fw — FreeWili2 BSP task runner (cross-platform).

Commands:
  fw configure       configure build/ against the pinned Pico SDK (--clean wipes first)
  fw build [app]     configure+build an app for the RP2350B target (default hello_display)
  fw flash [app]     program the app over the cmsis-dap debug probe via OpenOCD
  fw rtt             stream SEGGER RTT diagnostics
  fw test            build+run host unit tests (CTest, no hardware)
  fw new-app <name>  scaffold apps/<name> from apps/template
  fw install-app UF2 copy an app to the device SD card's /apps directory
  fw run-app PATH     launch an installed /apps/PATH UF2 on DISPLAY
Add --print to any build/flash/test command to print the command(s) instead of running.
"""
import argparse, json, os, pathlib, shutil, socket, stat, struct, subprocess, sys, time, zlib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD_DIR = REPO_ROOT / "build"
DEFAULT_APP = "hello_display"
OPENOCD_CFG = str(REPO_ROOT / "tools" / "openocd" / "freewili2.cfg")
RTT_PORT = 9090
# RP2350 SRAM is 0x20000000..0x20082000; scan the whole range for the RTT block.
RTT_SETUP = 'rtt setup 0x20000000 0x82000 "SEGGER RTT"'
AGENTIO_PORT = 9091          # RTT channel 1: agentio commands + pixels
AGENTIO_CHANNEL = 1
AGENTIO_MAGIC = b"FW2C"
AGENTIO_HEADER_LEN = 18
SD_HANDOFF_SETTLE_SECONDS = 2
SURFACES = {"lcd": 0, "dvi": 1}
# Button indices must match uartkbd_btn_t in bsp/input/uartkbd_parse.h.
BUTTONS = ["grey", "yellow", "green", "blue", "red", "nav_center", "nav_up",
           "nav_down", "nav_left", "nav_right", "home", "ok", "cancel", "page"]

SD_HOST_COMMAND = r"h\x\k"
# ESP32 flasher, MAIN text-menu paths. "w\a\w <folder>" starts a background
# flash from an idf.py build folder on the SD card; "w\a\s" reports
# "flashing progress partition_index partition_count". Both need power zone 5.
ESP_FLASH_FOLDER_COMMAND = r"w\a\w"
ESP_FLASH_STATUS_COMMAND = r"w\a\s"
# MAIN's manifest parser limits, from the Flash From Folder help text in
# freewilimain/MenuX/fwMenuESP32FlasherConfig.h. Checked on the PC so a bad
# bundle fails before the SD has been handed around.
ESP_MANIFEST_MAX_BYTES = 4096
ESP_MANIFEST_MAX_PARTITIONS = 6
RUN_APP_COMMAND = r"a\r"
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)

# DISPLAY memory map. The flash window holds the stock DISPLAY firmware, so a
# loadable app must never target it; `fw install-app` and `fw flash` enforce the
# same rule from one definition.
QSPI_FLASH = (0x10000000, 0x11000000)
APP_WINDOWS = (("SRAM", 0x20000000, 0x20070000),
               ("PSRAM", 0x11000000, 0x11800000))
PT_LOAD = 1

def check_app_uf2(path):
    """Fail closed unless every UF2 payload targets DISPLAY SRAM or PSRAM."""
    data = pathlib.Path(path).read_bytes()
    if not data or len(data) % 512:
        raise ValueError("app UF2 must contain complete 512-byte blocks")
    target = None
    count = 0
    declared_blocks = None
    seen_blocks = set()
    for index in range(len(data) // 512):
        block = data[index * 512:(index + 1) * 512]
        m0, m1, flags, address, size, block_no, num_blocks, _family = struct.unpack_from("<8I", block)
        end, = struct.unpack_from("<I", block, 508)
        if (m0, m1, end) != UF2_MAGIC:
            raise ValueError(f"UF2 block {index} has invalid magic")
        if num_blocks == 0 or block_no >= num_blocks:
            raise ValueError(f"UF2 block {index} has invalid block numbering")
        if declared_blocks is None:
            declared_blocks = num_blocks
        elif num_blocks != declared_blocks:
            raise ValueError(f"UF2 block {index} has inconsistent total block count")
        if block_no in seen_blocks:
            raise ValueError(f"UF2 block {index} duplicates block number {block_no}")
        seen_blocks.add(block_no)
        if flags & 1 or size == 0:
            continue
        if QSPI_FLASH[0] <= address < QSPI_FLASH[1]:
            raise ValueError(f"UF2 block {index} targets QSPI flash at 0x{address:08x}")
        here = next((name for name, start, stop in APP_WINDOWS
                     if size <= 476 and start <= address and address + size <= stop), None)
        if here is None or (target is not None and here != target):
            raise ValueError(f"UF2 block {index} is outside or mixes app-memory windows")
        target = here
        count += 1
    if not count:
        raise ValueError("UF2 has no loadable app payload")
    if len(seen_blocks) != declared_blocks or seen_blocks != set(range(declared_blocks)):
        raise ValueError("UF2 is incomplete: declared block set is not present")
    return target

def _fwfinder_main_port(serial_number=None):
    """Return MAIN's serial port using pyfwfinder (loaded only for this command)."""
    try:
        import pyfwfinder
    except ImportError as exc:
        raise RuntimeError("install pyfwfinder before using 'fw install-app'") from exc
    devices = pyfwfinder.find_all()
    if serial_number:
        devices = [d for d in devices if str(getattr(d, "serial", "")) == serial_number]
    if not devices:
        suffix = f" with serial {serial_number!r}" if serial_number else ""
        raise RuntimeError("no FreeWili device found" + suffix)
    if len(devices) != 1:
        raise RuntimeError(f"{len(devices)} FreeWili devices found; pass --device SERIAL")
    ports = [u for u in devices[0].usb_devices
             if "serial" in str(getattr(u, "kind", "")).lower()]
    if not ports:
        raise RuntimeError("fwFinder found the device but not its serial interface")
    port = next((u for u in ports if "main" in str(getattr(u, "name", "")).lower()), ports[0])
    for attr in ("port", "path", "port_name", "location"):
        if getattr(port, attr, None):
            return str(getattr(port, attr))
    raise RuntimeError("fwFinder did not report a path for MAIN's serial interface")

def _set_sd_host(port, to_pc, timeout=8):
    """Select MAIN (0) or the PC USB reader (1), checking the framed reply."""
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("install pyserial before using 'fw install-app'") from exc
    command = f"{SD_HOST_COMMAND} {1 if to_pc else 0}"
    deadline = time.monotonic() + timeout
    with serial.Serial(port, 1_000_000, timeout=0.2) as wire:
        wire.reset_input_buffer()
        wire.write(b"\x02" + command.encode("ascii") + b"\n")
        pending = ""
        while time.monotonic() < deadline:
            pending += wire.readline().decode("utf-8", "replace")
            if "]" not in pending:
                continue
            line, pending = pending.split("]", 1)
            line = line.strip() + "]"
            if "[" in line:
                line = line[line.rfind("["):]
            if not line.startswith("[" + SD_HOST_COMMAND + " "):
                continue
            # The state token may be "none" after returning the mux to MAIN
            # when its immediate remount has not detected the card yet.  The
            # final success flag reports whether the ownership change itself
            # reached the hardware, which is the operation requested here.
            if line.endswith(" 1]"):
                wire.write(b"\x02")       # leave firmware navigation at the root
                return
            raise RuntimeError(f"device rejected {command!r}: {line}")
    raise RuntimeError(f"timeout waiting for MAIN to acknowledge {command!r}")

def _app_path(path):
    if not path or chr(92) in path or path.startswith("/"):
        raise ValueError("app path must be relative to /apps and use '/' separators")
    parts = path.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("app path must not contain empty, '.' or '..' components")
    if not parts[-1].lower().endswith(".uf2"):
        raise ValueError("app path must name a .uf2 file")
    return "/".join(parts)

def run_app(path, serial_number=None, timeout=120, port=None):
    path = _app_path(path)
    port = port or _fwfinder_main_port(serial_number)
    command = f"{RUN_APP_COMMAND} {path}"
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("install pyserial before using 'fw run-app'") from exc
    deadline = time.monotonic() + timeout
    with serial.Serial(port, 1_000_000, timeout=0.2) as wire:
        wire.reset_input_buffer()
        wire.write(b"\x02" + command.encode("ascii") + b"\n")
        queued = False
        pending = ""
        while time.monotonic() < deadline:
            pending += wire.readline().decode("utf-8", "replace")
            if "]" not in pending:
                continue
            line, pending = pending.split("]", 1)
            line = line.strip() + "]"
            if "[" in line:
                line = line[line.rfind("["):]
            if line.startswith("[" + RUN_APP_COMMAND + " "):
                if not line.endswith(" 1]"):
                    raise RuntimeError(f"device rejected {command!r}: {line}")
                queued = True
                continue
            # The menu command only queues the blocking load.  MAIN reports
            # its actual result later under response key "d" after the UART
            # stub/transfer/launch sequence has completed.
            if queued and line.startswith("[d "):
                if not line.endswith(" 1]"):
                    raise RuntimeError(f"device failed to launch {command!r}: {line}")
                print(f"launched /apps/{path}")
                return
    raise RuntimeError(f"timeout waiting for MAIN to launch {command!r}")

def _mounted_volumes():
    """Mounted removable-volume roots. Kept small and dependency-free."""
    if sys.platform == "win32":
        import ctypes
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        mounted = set()
        for i in range(26):
            root = pathlib.Path(f"{chr(65 + i)}:/")
            if not (mask & (1 << i)) or ctypes.windll.kernel32.GetDriveTypeW(f"{chr(65 + i)}:\\") != 2:
                continue
            try:
                next(root.iterdir(), None)  # excludes a reader whose media is absent
                mounted.add(root)
            except OSError:
                pass
        return mounted
    if sys.platform == "darwin":
        root = pathlib.Path("/Volumes")
        return set(root.iterdir()) if root.is_dir() else set()
    mounts = set()
    try:
        for line in pathlib.Path("/proc/mounts").read_text().splitlines():
            _dev, mount, *_rest = line.split()
            if mount.startswith(("/media/", "/run/media/")):
                mounts.add(pathlib.Path(mount.replace("\\040", " ")))
    except OSError:
        pass
    return mounts

def _wait_for_sd(baseline, timeout=25, poll=0.25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        added = _mounted_volumes() - set(baseline)
        if len(added) == 1:
            return added.pop()
        if len(added) > 1:
            raise RuntimeError("more than one removable volume appeared; cannot safely choose the SD card")
        time.sleep(poll)
    raise RuntimeError("timed out waiting for the SD card USB reader to enumerate")

def _eject_volume(volume):
    volume = pathlib.Path(volume)
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = wintypes.HANDLE
        drive = str(volume).rstrip(chr(92) + "/")
        volume_device = chr(92) * 2 + "." + chr(92) + drive
        handle = kernel32.CreateFileW(volume_device, 0xC0000000, 3, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        returned = wintypes.DWORD()
        try:
            for control in (0x00090018, 0x00090020):
                if control == 0x00090020 and not kernel32.FlushFileBuffers(handle):
                    raise ctypes.WinError(ctypes.get_last_error())
                if not kernel32.DeviceIoControl(handle, control, None, 0, None, 0,
                                                ctypes.byref(returned), None):
                    raise ctypes.WinError(ctypes.get_last_error())
        finally:
            kernel32.CloseHandle(handle)
    elif sys.platform == "darwin":
        subprocess.run(["diskutil", "unmount", str(volume)], check=True)
    else:
        subprocess.run(["udisksctl", "unmount", "-b",
                        subprocess.check_output(["findmnt", "-n", "-o", "SOURCE", str(volume)], text=True).strip()],
                       check=True)

def _app_subfolder(folder):
    """Return a safe relative /apps subfolder as POSIX path components."""
    if folder in (None, "", "."):
        return ()
    if "\\" in folder or folder.startswith("/"):
        raise ValueError("app folder must be relative to /apps and use '/' separators")
    parts = folder.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("app folder must not contain empty, '.' or '..' components")
    return tuple(parts)


def install_app(uf2, serial_number=None, timeout=25, port=None, folder=None):
    """Hand the SD to the PC once, copy + flush one or more UF2s, then return it."""
    requested = list(uf2) if isinstance(uf2, (list, tuple)) else [uf2]
    if not requested:
        raise ValueError("at least one UF2 is required")
    sources = [pathlib.Path(item).resolve() for item in requested]
    for source in sources:
        if source.suffix.lower() != ".uf2" or not source.is_file():
            raise ValueError(f"expected an existing .uf2 file, got {str(source)!r}")
    names = [source.name.lower() for source in sources]
    if len(names) != len(set(names)):
        raise ValueError("UF2 inputs must have distinct destination filenames")
    targets = [check_app_uf2(source) for source in sources]
    folder_parts = _app_subfolder(folder)
    for source, target in zip(sources, targets):
        print(f"verified {source.name}: {target} app, no QSPI-flash payloads")
    port = port or _fwfinder_main_port(serial_number)
    baseline = _mounted_volumes()
    pc_selected = False
    volume = None
    try:
        _set_sd_host(port, True)
        pc_selected = True
        time.sleep(SD_HANDOFF_SETTLE_SECONDS)
        volume = _wait_for_sd(baseline, timeout)
        apps = volume / "apps"
        for part in folder_parts:
            apps /= part
        apps.mkdir(parents=True, exist_ok=True)
        destinations = []
        for source in sources:
            temporary = apps / (source.name + ".tmp")
            shutil.copyfile(source, temporary)
            # Windows' CRT rejects fsync() on a read-only descriptor. Open for
            # update without changing the already-copied contents.
            with temporary.open("r+b") as copied:
                os.fsync(copied.fileno())
            destination = apps / source.name
            os.replace(temporary, destination)
            destinations.append(destination)
        # fsync above drains the file; allow the removable-volume stack to
        # finish its bookkeeping before moving the hardware mux.  Do not ask
        # Windows to eject/unmount this reader: ownership is controlled by the
        # FreeWili h\x\k command, and Windows eject races that handoff.
        time.sleep(SD_HANDOFF_SETTLE_SECONDS)
    finally:
        if pc_selected:
            _set_sd_host(port, False)
    if volume is not None:
        for source, destination in zip(sources, destinations):
            print(f"installed {source.name} to {destination}")

def _console_send(wire, command, timeout=8):
    """Send one MAIN console command on an open port; return its reply tokens.

    Replies are framed as "[<path> <payload...> <0|1>]", the trailing flag being
    the success bit -- the same handshake _set_sd_host uses, factored out so the
    ESP32 flasher commands can share it and a poll loop can reuse one open port
    instead of reopening per request."""
    path = command.split(" ", 1)[0]
    prefix = "[" + path + " "
    wire.reset_input_buffer()
    wire.write(b"\x02" + command.encode("ascii") + b"\n")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = wire.readline().decode("utf-8", "replace").strip()
        if not line.startswith(prefix) or not line.endswith("]"):
            continue
        tokens = line[len(prefix):-1].split()
        wire.write(b"\x02")          # leave firmware navigation at the root
        if not tokens or tokens[-1] != "1":
            raise RuntimeError("device rejected {!r}: {}".format(command, line))
        return tokens[:-1]
    raise RuntimeError("timeout waiting for MAIN to acknowledge {!r}".format(command))

def _open_main_console(port):
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("install pyserial before using 'fw install-bundle'") from exc
    return serial.Serial(port, 1_000_000, timeout=0.2)

def check_esp_build(folder):
    """Validate an idf.py build folder the way MAIN's manifest parser will.

    Returns (manifest_path, [(offset, relative_path, source_path), ...]). Fails
    closed on the PC, so a malformed bundle never reaches the point where the SD
    card has already been handed over."""
    root = pathlib.Path(folder).resolve()
    manifest = root / "flasher_args.json"
    if not manifest.is_file():
        raise ValueError("no flasher_args.json in {} -- point at an idf.py build "
                         "folder (for example build.esp32c5)".format(root))
    size = manifest.stat().st_size
    if size > ESP_MANIFEST_MAX_BYTES:
        raise ValueError("flasher_args.json is {} bytes; MAIN rejects anything over "
                         "{}".format(size, ESP_MANIFEST_MAX_BYTES))
    spec = json.loads(manifest.read_text(encoding="utf-8"))
    files = spec.get("flash_files") or {}
    if not files:
        raise ValueError("flasher_args.json has no flash_files entries")
    if len(files) > ESP_MANIFEST_MAX_PARTITIONS:
        raise ValueError("{} partitions in flash_files; MAIN rejects more than "
                         "{}".format(len(files), ESP_MANIFEST_MAX_PARTITIONS))
    resolved = []
    for offset, relative in sorted(files.items(), key=lambda kv: int(kv[0], 16)):
        source = root / relative
        if not source.is_file():
            raise ValueError("{} (referenced at {}) is missing from the build "
                             "folder".format(relative, offset))
        resolved.append((offset, relative, source))
    return manifest, resolved

def _copy_esp_build(manifest, entries, destination):
    """Copy only the manifest and the binaries it references, preserving the
    relative layout the manifest points at. An idf.py build folder is hundreds
    of megabytes of object files; the device needs perhaps two."""
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(manifest, destination / manifest.name)
    total = manifest.stat().st_size
    for _offset, relative, source in entries:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        with target.open("r+b") as copied:
            os.fsync(copied.fileno())
        total += source.stat().st_size
    return total

def flash_esp_from_device(port, device_folder, timeout=180):
    """Ask MAIN to flash the ESP32 from a folder already on the SD card, then
    poll until it finishes. Flashing runs in MAIN's background, so the start
    command returning success only means it began."""
    with _open_main_console(port) as wire:
        _console_send(wire, "{} {}".format(ESP_FLASH_FOLDER_COMMAND, device_folder))
        print("ESP32 flash started from {}".format(device_folder))
        deadline = time.monotonic() + timeout
        last = None
        # The first poll can legitimately report not-flashing if it lands before
        # MAIN's background task starts, so require one observed busy sample
        # before treating "idle" as done.
        seen_busy = False
        while time.monotonic() < deadline:
            tokens = _console_send(wire, ESP_FLASH_STATUS_COMMAND)
            if len(tokens) < 4:
                raise RuntimeError("unexpected flash status reply: {!r}".format(tokens))
            flashing, progress, index, count = (int(t) for t in tokens[:4])
            if flashing:
                seen_busy = True
                line = "  partition {}/{}  {}%".format(index + 1, count, progress)
                if line != last:
                    print(line)
                    last = line
            elif seen_busy:
                print("ESP32 flash complete")
                return
            time.sleep(0.5)
        raise RuntimeError("ESP32 flash did not finish within {} s".format(timeout))

def install_bundle(uf2, esp_build, name=None, serial_number=None, timeout=25,
                   port=None, flash_esp=True, folder=None):
    """Install a two-processor app: the DISPLAY UF2 and its ESP32 firmware.

    One SD handoff places both artifacts, then MAIN is asked to flash the ESP32
    from the copy on the card. The DISPLAY half is launched the usual way (on
    device, or 'fw run-app'). Mirrors install_app's handoff discipline: no
    Windows eject, settle either side of the mux move."""
    source = pathlib.Path(uf2).resolve()
    if source.suffix.lower() != ".uf2" or not source.is_file():
        raise ValueError("expected an existing .uf2 file, got {!r}".format(uf2))
    target = check_app_uf2(source)
    print("verified {}: {} app, no QSPI-flash payloads".format(source.name, target))

    manifest, entries = check_esp_build(esp_build)
    print("verified ESP32 build: {} partition(s), manifest {} B".format(
        len(entries), manifest.stat().st_size))

    app_name = name or source.stem
    folder_parts = _app_subfolder(folder)
    port = port or _fwfinder_main_port(serial_number)
    # MAIN addresses the card as drive 1:. The ESP image lives under the app's
    # own /appdata/<app>/ tree, per docs/app-storage.md.
    device_folder = "1:/appdata/{}/esp32/".format(app_name)

    baseline = _mounted_volumes()
    pc_selected = False
    volume = None
    destination = None
    copied_bytes = 0
    try:
        _set_sd_host(port, True)
        pc_selected = True
        time.sleep(SD_HANDOFF_SETTLE_SECONDS)
        volume = _wait_for_sd(baseline, timeout)

        apps = volume / "apps"
        for part in folder_parts:
            apps /= part
        apps.mkdir(parents=True, exist_ok=True)
        temporary = apps / (source.name + ".tmp")
        shutil.copyfile(source, temporary)
        with temporary.open("r+b") as copied:
            os.fsync(copied.fileno())
        destination = apps / source.name
        os.replace(temporary, destination)

        esp_destination = volume / "appdata" / app_name / "esp32"
        copied_bytes = _copy_esp_build(manifest, entries, esp_destination)

        time.sleep(SD_HANDOFF_SETTLE_SECONDS)
    finally:
        if pc_selected:
            _set_sd_host(port, False)

    if volume is None:
        return
    print("installed {} to {}".format(source.name, destination))
    print("installed ESP32 image ({} KiB) to {}".format(copied_bytes // 1024, esp_destination))

    if not flash_esp:
        print("skipped ESP32 flash; run this on the device console when ready:")
        print("  {} {}".format(ESP_FLASH_FOLDER_COMMAND, device_folder))
        return

    flash_esp_from_device(port, device_folder)

def packbits_decode(data, units):
    """Decode PackBits-16 (see bsp/agentio/agentio_proto.h) into a list of
    RGB565 values. `units` is the expected count; raises ValueError on
    truncated or malformed input."""
    out, i = [], 0
    while i < len(data) and len(out) < units:
        ctrl = data[i] - 256 if data[i] > 127 else data[i]
        i += 1
        if ctrl >= 0:
            count = ctrl + 1
            if i + count * 2 > len(data):
                raise ValueError("truncated literal run")
            for _ in range(count):
                out.append((data[i] << 8) | data[i + 1])
                i += 2
        elif ctrl != -128:
            count = 1 - ctrl
            if i + 2 > len(data):
                raise ValueError("truncated repeat run")
            v = (data[i] << 8) | data[i + 1]
            i += 2
            out.extend([v] * count)
        else:
            raise ValueError("reserved control byte")
    if len(out) < units:
        raise ValueError(f"short payload: {len(out)} of {units} units")
    return out[:units]

def png_write(path, w, h, pixels):
    """Write RGB565 `pixels` (row-major, w*h values) as an 8-bit RGB PNG.
    Stdlib only — no Pillow."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter type 0 (None) per scanline
        for x in range(w):
            v = pixels[y * w + x]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            # scale 5/6-bit channels to 8-bit so full-scale maps to 255
            raw += bytes(((r * 255 + 15) // 31,
                          (g * 255 + 31) // 63,
                          (b * 255 + 15) // 31))

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))

# Pinned toolchain versions under ~/.pico-sdk. The SDK path used to come from an
# ambient PICO_SDK_PATH (VS Code injects one) plus whatever landed in the CMake
# cache, so `rm -rf build` silently changed SDK versions. Pinning here makes the
# configure reproducible; each falls back to the newest installed version.
PICO_SDK_VERSION       = "2.3.0"      # 2.3.0 adds hardware_psram (official PSRAM support)
PICO_TOOLCHAIN_VERSION = "14_2_Rel1"  # the version every hardware-verified build used

def _pico_sdk_dir(kind, pinned):
    """~/.pico-sdk/<kind>/<pinned>, else the newest installed version, else None."""
    root = pathlib.Path.home() / ".pico-sdk" / kind
    if not root.is_dir():
        return None
    exact = root / pinned
    if exact.is_dir():
        return exact
    versions = sorted((d for d in root.iterdir() if d.is_dir()), reverse=True)
    return versions[0] if versions else None

def _ninja():
    """The Ninja bundled with the Pico SDK VS Code extension, if installed."""
    root = pathlib.Path.home() / ".pico-sdk" / "ninja"
    exe = "ninja.exe" if sys.platform == "win32" else "ninja"
    if root.is_dir():
        found = sorted(root.glob(f"*/{exe}"), reverse=True)
        if found:
            return found[0]
    return pathlib.Path(shutil.which("ninja")) if shutil.which("ninja") else None

def configure_command():
    """`cmake --preset target` with the SDK/toolchain pinned explicitly, so the
    configure does not depend on PICO_SDK_PATH being exported in the shell.
    NEVER add -DPICO_BOARD here — the top-level CMakeLists owns it (AGENTS.md
    invariant 1); overriding it on the command line reverts the board config."""
    cmd = ["cmake", "--preset", "target"]
    sdk = _pico_sdk_dir("sdk", PICO_SDK_VERSION)
    if sdk:
        cmd.append(f"-DPICO_SDK_PATH={sdk.as_posix()}")
    tc = _pico_sdk_dir("toolchain", PICO_TOOLCHAIN_VERSION)
    if tc:
        cmd.append(f"-DPICO_TOOLCHAIN_PATH={tc.as_posix()}")
    # The SDK's Findpicotool only finds a prebuilt picotool via picotool_DIR;
    # without it every fresh configure rebuilds picotool from source (~2 min).
    pt = _pico_sdk_dir("picotool", PICO_SDK_VERSION)
    if pt and (pt / "picotool" / "picotoolConfig.cmake").exists():
        cmd.append(f"-Dpicotool_DIR={(pt / 'picotool').as_posix()}")
    ninja = _ninja()
    if ninja:
        cmd.append(f"-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}")
    return cmd

def _cached_sdk_path():
    """PICO_SDK_PATH recorded in build/CMakeCache.txt, or None if unconfigured."""
    cache = BUILD_DIR / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("PICO_SDK_PATH:"):
            return line.split("=", 1)[1].strip()
    return None

def needs_configure():
    """True when build/ is missing or was configured against a different SDK.
    Changing PICO_SDK_PATH in place leaves stale SDK-derived cache entries, so a
    version change is handled by wiping build/ and configuring fresh."""
    cached = _cached_sdk_path()
    if cached is None:
        return True
    # A configure that failed part-way (missing submodule, bad path) leaves a
    # CMakeCache.txt behind but no generator file. Without this check the cache
    # looks valid, the configure is skipped, and the build dies on a missing
    # build.ninja instead of just re-configuring.
    if not (BUILD_DIR / "build.ninja").exists():
        return True
    sdk = _pico_sdk_dir("sdk", PICO_SDK_VERSION)
    return sdk is not None and pathlib.Path(cached) != sdk

def force_rmtree(path):
    """shutil.rmtree that survives read-only files. On Windows the git pack
    files under build/_deps/picotool-src are read-only, and a plain rmtree dies
    on them with PermissionError."""
    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=on_error)
    else:
        shutil.rmtree(path, onerror=lambda f, p, e: on_error(f, p, e))

def run_configure(clean=False):
    if clean and BUILD_DIR.exists():
        # flush: this print would otherwise buffer past the cmake output below
        print(f"removing {BUILD_DIR} (stale SDK configuration)", flush=True)
        force_rmtree(BUILD_DIR)
    subprocess.run(configure_command(), cwd=REPO_ROOT, check=True)

def build_command(app):
    return ["cmake", "--build", "--preset", "target", "--target", app]

def _openocd():
    """(exe, scripts_dir) for the Pico-SDK OpenOCD. Uses the ~/.pico-sdk install
    (newest version) when present — matching how subghz flashes — otherwise falls
    back to `openocd` on PATH with its built-in scripts (scripts_dir = None)."""
    root = pathlib.Path.home() / ".pico-sdk" / "openocd"
    if root.is_dir():
        exe_name = "openocd.exe" if sys.platform == "win32" else "openocd"
        for ver in sorted(root.iterdir(), reverse=True):
            exe, scripts = ver / exe_name, ver / "scripts"
            if exe.exists():
                return str(exe), (str(scripts) if scripts.is_dir() else None)
    return "openocd", None

def _openocd_base():
    exe, scripts = _openocd()
    cmd = [exe]
    if scripts:
        cmd += ["-s", scripts]
    return cmd + ["-f", OPENOCD_CFG]

def elf_load_segments(blob):
    """Loadable (physical address, size) pairs from a 32-bit LE ELF.

    Physical, not virtual: a `copy_to_ram` binary runs from SRAM but is STORED
    in flash, and it is the stored address a debugger writes.
    """
    if blob[:4] != b"\x7fELF" or blob[4:6] != b"\x01\x01":
        raise ValueError("expected a 32-bit little-endian ELF")
    phoff = struct.unpack_from("<I", blob, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", blob, 42)
    if phentsize < 32 or phoff + phentsize * phnum > len(blob):
        raise ValueError("ELF program-header table is truncated")
    segments = []
    for index in range(phnum):
        kind, _off, _vaddr, paddr, filesz, _memsz, _flags, _align = \
            struct.unpack_from("<8I", blob, phoff + index * phentsize)
        if kind == PT_LOAD and filesz:
            segments.append((paddr, filesz))
    return sorted(segments)


def flash_segments_in_qspi(blob):
    """The loadable segments that would land in the DISPLAY firmware region."""
    start, stop = QSPI_FLASH
    return [(addr, size) for addr, size in elf_load_segments(blob)
            if addr < stop and addr + size > start]


def check_flash_elf(path):
    """Fail closed unless every loadable segment stays out of QSPI flash.

    Writing an ELF at flash base replaces the stock DISPLAY firmware. The
    recovery loader is fused in OTP so the board still boots, but restoring the
    firmware is a separate maintenance workflow — not something a build/flash
    loop should do silently. `pico_set_binary_type(copy_to_ram)` is the usual
    way to trip this: it runs from SRAM but is stored in flash.
    """
    offenders = flash_segments_in_qspi(pathlib.Path(path).read_bytes())
    if not offenders:
        return
    where = ", ".join(f"0x{addr:08x}+{size}" for addr, size in offenders[:4])
    raise ValueError(
        f"{path} stores {len(offenders)} loadable segment(s) in QSPI flash "
        f"({where}).\n"
        "Programming it would REPLACE the stock DISPLAY firmware.\n"
        "Build the app with fw2_display_app() so it targets SRAM or PSRAM, then\n"
        "install it non-destructively with `fw install-app <app>.uf2`.\n"
        "If replacing the DISPLAY firmware is genuinely what you want, re-run\n"
        "with `fw flash --replace-display-firmware`.")


def flash_command(app, replace_display_firmware=False):
    elf = f"build/apps/{app}/{app}.elf"
    if not replace_display_firmware:
        path = REPO_ROOT / elf
        if path.exists():
            check_flash_elf(path)
    return _openocd_base() + ["-c", f"program {elf} verify reset exit"]

def rtt_command():
    """OpenOCD serving BOTH RTT channels: 0 (DIAG) and 1 (agentio). Only one
    process can own the debug probe, so a running `fw rtt` doubles as the
    session that `fw screenshot` / `fw press` reuse."""
    return _openocd_base() + [
        "-c", "init", "-c", RTT_SETUP, "-c", "rtt start",
        "-c", f"rtt server start {RTT_PORT} 0",
        "-c", f"rtt server start {AGENTIO_PORT} {AGENTIO_CHANNEL}"]

def _host_toolchain_args():
    """Extra `cmake` configure args that pin a host C compiler + Ninja for the
    standalone tests/ tree (no Pico SDK, no cross-compiler). Returns [] entries
    that are simply omitted when a tool can't be found, so CMake falls back to
    its own defaults (e.g. system cc/gcc, non-Ninja generator).
    """
    args = []
    if sys.platform == "win32":
        # Mirrors the subghz repo's proven host-test toolchain: MSYS2 MinGW
        # GCC + the Ninja bundled with the Pico SDK VS Code extension.
        gcc = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")
        if gcc.exists():
            args += [f"-DCMAKE_C_COMPILER={gcc}"]
        ninja_root = pathlib.Path.home() / ".pico-sdk" / "ninja"
        ninja = next(iter(sorted(ninja_root.glob("*/ninja.exe"), reverse=True)), None) \
            if ninja_root.is_dir() else None
        if ninja is not None:
            args += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    else:
        # Non-Windows: trust the default host cc/gcc; use Ninja if it's on
        # PATH, otherwise let CMake pick its default generator (e.g. Make).
        if shutil.which("ninja"):
            args += ["-G", "Ninja"]
    return args

def test_command():
    tests_dir = REPO_ROOT / "tests"
    build_dir = REPO_ROOT / "build-tests"
    configure = ["cmake", "-S", str(tests_dir), "-B", str(build_dir)]
    configure += _host_toolchain_args()
    return [
        configure,
        ["cmake", "--build", str(build_dir)],
        ["ctest", "--test-dir", str(build_dir), "--output-on-failure"],
    ]

def new_app(name, repo_root=REPO_ROOT):
    src = pathlib.Path(repo_root) / "apps" / "template"
    dest = pathlib.Path(repo_root) / "apps" / name
    if dest.exists():
        raise FileExistsError(dest)
    shutil.copytree(src, dest)
    cml = dest / "CMakeLists.txt"
    cml.write_text(cml.read_text().replace("template", name))
    return dest

def _run(cmds, do_print):
    if isinstance(cmds[0], str):
        cmds = [cmds]
    for c in cmds:
        if do_print:
            print(" ".join(c))
        else:
            subprocess.run(c, cwd=REPO_ROOT, check=True)

def run_rtt(seconds=0):
    """Start OpenOCD's RTT server (attached, no flash) and stream channel 0 to
    stdout. seconds=0 runs until Ctrl+C; seconds>0 exits after that window
    (for scripted checks). Diagnostics on the FreeWili2 are RTT-only."""
    proc = subprocess.Popen(rtt_command(), cwd=REPO_ROOT,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)  # let OpenOCD attach and locate the RTT control block
        if proc.poll() is not None:
            print("openocd exited early — is the debug probe connected?", file=sys.stderr)
            return 1
        try:
            sock = socket.create_connection(("127.0.0.1", RTT_PORT), timeout=5)
        except OSError as e:
            print(f"could not connect to RTT server on {RTT_PORT}: {e}", file=sys.stderr)
            return 1
        sock.settimeout(0.5)
        deadline = time.time() + seconds if seconds > 0 else None
        print(f"--- RTT connected (port {RTT_PORT}); Ctrl+C to stop ---", file=sys.stderr)
        while deadline is None or time.time() < deadline:
            try:
                data = sock.recv(4096)
                if not data:
                    break
                # Keep RTT diagnostics printable even on Windows legacy
                # consoles, where U+FFFD from Python's "replace" handler is
                # not representable in the active cp1252 stream encoding.
                sys.stdout.write(data.decode("ascii", "replace").replace("\ufffd", "?"))
                sys.stdout.flush()
            except socket.timeout:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        try:
            sock.close()
        except NameError:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0

def _port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
        return True
    except OSError:
        return False

class _Agentio:
    """Connection to the agentio RTT channel. Reuses an OpenOCD already serving
    AGENTIO_PORT (e.g. a running `fw rtt`); otherwise spawns one and tears it
    down on exit."""
    def __init__(self):
        self.proc = None
        self.sock = None

    def _cleanup(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None

    def __enter__(self):
        try:
            if not _port_open(AGENTIO_PORT):
                self.proc = subprocess.Popen(rtt_command(), cwd=REPO_ROOT,
                                             stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL)
                deadline = time.time() + 10
                while time.time() < deadline and not _port_open(AGENTIO_PORT):
                    if self.proc.poll() is not None:
                        raise RuntimeError("openocd exited — is the probe connected?")
                    time.sleep(0.2)
                if not _port_open(AGENTIO_PORT):
                    raise RuntimeError(
                        f"openocd did not open port {AGENTIO_PORT} within 10s")
            self.sock = socket.create_connection(("127.0.0.1", AGENTIO_PORT), timeout=10)
            self.sock.settimeout(30)
        except BaseException:
            # __exit__ is NOT called when __enter__ raises, so a spawned OpenOCD
            # would leak and keep holding the debug probe.
            self._cleanup()
            raise
        return self

    def __exit__(self, *exc):
        self._cleanup()
        return False

    def send(self, line):
        self.sock.sendall((line + "\n").encode("ascii"))

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("agentio connection closed mid-transfer")
            buf += chunk
        return buf

    def recv_line(self):
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(1)
            if not chunk:
                raise RuntimeError("agentio connection closed")
            buf += chunk
        return buf.decode("ascii", "replace").strip()

def agentio_command(line):
    """Send one command expecting an OK/ERR reply. Returns the reply text."""
    with _Agentio() as a:
        a.send(line)
        return a.recv_line()

def agentio_capture(surface, crop, scale, out_path):
    """CAP + decode + PNG. Returns (w, h)."""
    x, y, w, h = crop if crop else (0, 0, 0, 0)
    with _Agentio() as a:
        a.send(f"CAP {SURFACES[surface]} {x} {y} {w} {h} {scale}")
        # Every ERR reply ("ERR rect\n", "ERR surface\n", ...) is shorter than
        # the 18-byte capture header, so an unconditional recv_exact(18) can
        # never return for one — it would hang until the socket times out
        # instead of surfacing the server's error text. Check the 4-byte
        # magic first; only read the rest of the header once it matches.
        magic = a.recv_exact(4)
        if magic != AGENTIO_MAGIC:
            rest = a.recv_line()   # finish reading the "ERR <reason>" line
            raise RuntimeError((magic.decode("ascii", "replace") + rest).strip())
        hdr = magic + a.recv_exact(AGENTIO_HEADER_LEN - 4)
        ow, oh = struct.unpack(">HH", hdr[10:14])
        payload_len = struct.unpack(">I", hdr[14:18])[0]
        payload = a.recv_exact(payload_len)
    pixels = packbits_decode(payload, ow * oh)
    png_write(out_path, ow, oh, pixels)
    return ow, oh

def main(argv=None):
    p = argparse.ArgumentParser(prog="fw")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("build", "flash"):
        sp = sub.add_parser(name); sp.add_argument("app", nargs="?", default=DEFAULT_APP)
        sp.add_argument("--print", dest="show", action="store_true")
        if name == "flash":
            sp.add_argument("--replace-display-firmware", action="store_true",
                            help="allow writing QSPI flash, replacing the stock "
                                 "DISPLAY firmware (maintenance workflow only)")
    sp = sub.add_parser("configure")
    sp.add_argument("--clean", action="store_true", help="wipe build/ before configuring")
    sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("rtt")
    sp.add_argument("--print", dest="show", action="store_true")
    sp.add_argument("-s", "--seconds", type=int, default=0,
                    help="capture for N seconds then exit (0 = until Ctrl+C)")
    sp = sub.add_parser("test"); sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("new-app"); sp.add_argument("name")
    sp = sub.add_parser("install-app")
    sp.add_argument("uf2", nargs="+", help="one or more app UF2s to copy in one SD handoff")
    sp.add_argument("--folder", help="relative subfolder under /apps (for example beta/team)")
    sp.add_argument("--device", help="fwFinder device serial (required when multiple devices are connected)")
    sp.add_argument("--port", help="explicit MAIN serial port if fwFinder cannot identify legacy hardware")
    sp.add_argument("--timeout", type=float, default=25,
                    help="seconds to wait for the USB SD reader (default: 25)")
    sp = sub.add_parser("install-bundle")
    sp.add_argument("uf2", help="DISPLAY app UF2 to copy into /apps")
    sp.add_argument("esp_build", help="ESP32 idf.py build folder (contains flasher_args.json)")
    sp.add_argument("--name", help="bundle name under /appdata (default: the UF2 stem)")
    sp.add_argument("--folder", help="relative subfolder under /apps for the UF2")
    sp.add_argument("--no-flash-esp", dest="flash_esp", action="store_false",
                    help="copy the ESP32 image but do not flash it now")
    sp.add_argument("--device", help="fwFinder device serial (required when multiple devices are connected)")
    sp.add_argument("--port", help="explicit MAIN serial port if fwFinder cannot identify legacy hardware")
    sp.add_argument("--timeout", type=float, default=25,
                    help="seconds to wait for the USB SD reader (default: 25)")

    sp = sub.add_parser("run-app")
    sp.add_argument("path", help="UF2 path relative to /apps")
    sp.add_argument("--device")
    sp.add_argument("--port")
    sp.add_argument("--timeout", type=float, default=120)

    sp = sub.add_parser("screenshot")
    sp.add_argument("-o", "--out", default="screenshot.png")
    sp.add_argument("--surface", choices=sorted(SURFACES), default="lcd")
    sp.add_argument("--crop", help="x,y,w,h")
    sp.add_argument("--scale", type=int, default=1)
    sp.add_argument("--print", dest="show", action="store_true")
    for name in ("press", "hold", "release"):
        sp = sub.add_parser(name); sp.add_argument("buttons")
    sp = sub.add_parser("touch")
    sp.add_argument("x", type=int); sp.add_argument("y", type=int)
    sp.add_argument("--down", action="store_true")
    sp.add_argument("--up", action="store_true")
    sp = sub.add_parser("type"); sp.add_argument("text")

    a = p.parse_args(argv)
    if a.cmd == "configure":
        if a.show:
            _run(configure_command(), True)
        else:
            run_configure(clean=a.clean)
    elif a.cmd == "build":
        # Self-healing: a missing build/ — or one left over from another SDK
        # version — is configured (wiping first on a version change) before the
        # build, so `rm -rf build` no longer strands the tree on whatever SDK
        # happens to be in the shell environment.
        if not a.show and needs_configure():
            run_configure(clean=BUILD_DIR.exists())
        _run(build_command(a.app), a.show)
    elif a.cmd == "flash":
        try:
            command = flash_command(a.app, a.replace_display_firmware)
        except ValueError as exc:
            # This guard exists to be read, so print it rather than burying the
            # actionable part under a traceback.
            print(f"fw flash: refusing to program {a.app}\n{exc}", file=sys.stderr)
            return 2
        _run(command, a.show)
    elif a.cmd == "rtt":
        if a.show:
            _run(rtt_command(), True)
        else:
            return run_rtt(a.seconds)
    elif a.cmd == "test":  _run(test_command(), a.show)
    elif a.cmd == "new-app":
        print("created", new_app(a.name))
    elif a.cmd == "install-app":
        install_app(a.uf2, a.device, a.timeout, a.port, a.folder)
    elif a.cmd == "install-bundle":
        install_bundle(a.uf2, a.esp_build, a.name, a.device, a.timeout, a.port,
                       a.flash_esp, a.folder)
    elif a.cmd == "run-app":
        run_app(a.path, a.device, a.timeout, a.port)
    elif a.cmd == "screenshot":
        crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None
        if crop is not None and len(crop) != 4:
            print("--crop needs x,y,w,h", file=sys.stderr)
            return 1
        if a.show:
            print(f"CAP {SURFACES[a.surface]} "
                  f"{crop[0] if crop else 0} {crop[1] if crop else 0} "
                  f"{crop[2] if crop else 0} {crop[3] if crop else 0} {a.scale}")
            return 0
        w, h = agentio_capture(a.surface, crop, a.scale, a.out)
        print(f"wrote {a.out} ({w}x{h})")
    elif a.cmd in ("press", "hold", "release"):
        try:
            idx = [BUTTONS.index(b.strip()) for b in a.buttons.split(",")]
        except ValueError:
            print(f"unknown button; known: {', '.join(BUTTONS)}", file=sys.stderr)
            return 1
        if a.cmd == "press":
            for i in idx:
                print(agentio_command(f"TAP {i}"))
        else:
            mask = 0 if a.cmd == "release" else sum(1 << i for i in idx)
            print(agentio_command(f"BTN {mask:X}"))
    elif a.cmd == "touch":
        mode = 1 if a.down else (0 if a.up else 2)
        print(agentio_command(f"TCH {a.x} {a.y} {mode}"))
    elif a.cmd == "type":
        print(agentio_command(f"TYPE {a.text}"))
    return 0

if __name__ == "__main__":
    sys.exit(main())
