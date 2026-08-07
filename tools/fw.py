#!/usr/bin/env python3
"""fw — FreeWili2 BSP task runner (cross-platform).

Commands:
  fw configure       configure build/ against the pinned Pico SDK (--clean wipes first)
  fw build [app]     configure+build an app for the RP2350B target (default hello_display)
  fw flash [app]     program the app over the cmsis-dap debug probe via OpenOCD
  fw rtt             stream SEGGER RTT diagnostics
  fw test            build+run host unit tests (CTest, no hardware)
  fw new-app <name>  scaffold apps/<name> (--window FLASH|SRAM|PSRAM)
  fw install-app UF2 copy an app to the device SD card's /apps directory
  fw run-app <name>  launch /apps/<name> on the display CPU and report the result
  fw list-apps       list the SD card's /apps directory
  fw peek <addr>     read memory from the running target (never halts)
  fw alive           is the display CPU executing? (checks TIMER0 advances)
  fw thaw            release a debug-halted core that has TIMER0 paused
Add --print to any build/flash/test command to print the command(s) instead of running.
"""
import argparse, os, pathlib, shutil, socket, stat, struct, subprocess, sys, time, zlib

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
SURFACES = {"lcd": 0, "dvi": 1}
# Button indices must match uartkbd_btn_t in bsp/input/uartkbd_parse.h.
BUTTONS = ["grey", "yellow", "green", "blue", "red", "nav_center", "nav_up",
           "nav_down", "nav_left", "nav_right", "home", "ok", "cancel", "page"]

SD_HOST_COMMAND = r"h\x\k"
UF2_MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)

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
        if 0x10000000 <= address < 0x11000000:
            raise ValueError(f"UF2 block {index} targets QSPI flash at 0x{address:08x}")
        windows = (("SRAM", 0x20000000, 0x20070000),
                   ("PSRAM", 0x11000000, 0x11800000))
        here = next((name for name, start, stop in windows
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
        while time.monotonic() < deadline:
            line = wire.readline().decode("utf-8", "replace").strip()
            if not line.startswith("[" + SD_HOST_COMMAND + " "):
                continue
            if line.endswith(" 1]"):
                wire.write(b"\x02")       # leave firmware navigation at the root
                return
            raise RuntimeError(f"device rejected {command!r}: {line}")
    raise RuntimeError(f"timeout waiting for MAIN to acknowledge {command!r}")

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
        # Do not use `mountvol /p`: it marks a permanently attached USB card
        # reader offline, and changing the reader's media ownership does not
        # necessarily create the PnP reconnect Windows needs to bring it back.
        drive = str(volume).rstrip("\\/")
        script = ("$item=(New-Object -ComObject Shell.Application)"
                  f".Namespace(17).ParseName('{drive}');"
                  "if($null -eq $item){exit 1};$item.InvokeVerb('Eject')")
        subprocess.run(["powershell", "-NoProfile", "-Command", script], check=True)
        deadline = time.monotonic() + 10
        while volume in _mounted_volumes() and time.monotonic() < deadline:
            time.sleep(0.2)
        if volume in _mounted_volumes():
            raise RuntimeError(f"Windows did not safely remove {volume}")
    elif sys.platform == "darwin":
        subprocess.run(["diskutil", "unmount", str(volume)], check=True)
    else:
        subprocess.run(["udisksctl", "unmount", "-b",
                        subprocess.check_output(["findmnt", "-n", "-o", "SOURCE", str(volume)], text=True).strip()],
                       check=True)

def install_app(uf2, serial_number=None, timeout=25, port=None):
    """Hand the SD to the PC, atomically copy UF2 into /apps, eject, hand it back."""
    source = pathlib.Path(uf2).resolve()
    if source.suffix.lower() != ".uf2" or not source.is_file():
        raise ValueError(f"expected an existing .uf2 file, got {uf2!r}")
    target = check_app_uf2(source)
    print(f"verified {target} app: no QSPI-flash payloads")
    port = port or _fwfinder_main_port(serial_number)
    baseline = _mounted_volumes()
    pc_selected = False
    volume = None
    unmounted = False
    try:
        _set_sd_host(port, True)
        pc_selected = True
        volume = _wait_for_sd(baseline, timeout)
        apps = volume / "apps"
        apps.mkdir(exist_ok=True)
        temporary = apps / (source.name + ".tmp")
        shutil.copyfile(source, temporary)
        # Windows' CRT rejects fsync() on a read-only descriptor. Open for
        # update without changing the already-copied contents.
        with temporary.open("r+b") as copied:
            os.fsync(copied.fileno())
        destination = apps / source.name
        os.replace(temporary, destination)
        _eject_volume(volume)
        unmounted = True
    except BaseException as primary:
        # Once a filesystem has mounted, never move the mux while it may still
        # be live or dirty. Try one cleanup eject after copy/fsync/replace (or
        # after an initial eject failure), and return ownership only when that
        # succeeds. Otherwise leave the card with the PC: recoverable and much
        # safer than corrupting it under a mounted host filesystem.
        if volume is not None and not unmounted:
            try:
                _eject_volume(volume)
                unmounted = True
            except BaseException as cleanup:
                raise RuntimeError(
                    f"app install failed and {volume} could not be safely unmounted; "
                    "SD remains assigned to the PC. Close open files, safely eject "
                    "the volume, then return the SD to MAIN"
                ) from primary
        if pc_selected and unmounted:
            _set_sd_host(port, False)
        raise
    else:
        _set_sd_host(port, False)
        print(f"installed {source.name} to {destination}")

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

def flash_command(app):
    elf = f"build/apps/{app}/{app}.elf"
    return _openocd_base() + ["-c", f"program {elf} verify reset exit"]

def run_app(name, reset=True, timeout=90.0):
    r"""Launch /apps/<name> on the display CPU and report the real outcome.

    Two things make the naive `a\r <file>` unreliable, and both are handled here
    so no caller has to know them:

    1. **The display must be reset first.** Once it is running an app -- or a
       crashed one -- the fused bootloader does not answer hop 1, and the launch
       fails with "display bootloader did not answer". `h\v\x` first.

    2. **`a\r` returns a dispatch ack immediately; the real result arrives later
       in a deferred [d ...] frame.** A two-hop PSRAM stage streams the whole
       image over the 8 Mbaud link, so a short read window misses it and a
       *successful* launch looks like it silently did nothing.

    The destination is inferred from the image, not the filename: an SRAM UF2 is
    staged into RAM, a PSRAM-window one through FW2PsramStub, and anything else
    is written to FLASH -- which replaces the stock display firmware. Build apps
    with fw2_finalize_app(SRAM|PSRAM) and that last case cannot arise.
    """
    from fw_console import Console

    with Console() as con:
        if reset:
            print("resetting display...")
            _, frames = con.call(r"h\v\x", wait=10.0, idle=3.0)
            for path, _ts, seq, resp in frames:
                print(f"  [{path}] seq={seq} {resp.strip()}")
            time.sleep(5)     # let the fused bootloader come up and listen

        print(f"launching {name}...")
        _, frames = con.call(rf"a\r {name}", wait=timeout, idle=25.0)

    if not frames:
        print("no response frame — the launch may still be streaming, or the "
              "console was lost", file=sys.stderr)
        return 1

    rc = 0
    for path, _ts, seq, resp in frames:
        resp = resp.strip()
        print(f"  [{path}] seq={seq} {resp}")
        # The deferred frame carries the verdict. "Ok" means the stub answered,
        # the image streamed and CRC-verified, and it jumped.
        if path == "d" and not resp.lower().startswith("ok"):
            rc = 1
    if rc:
        print("\nlaunch FAILED. If the image is on the card and well-formed, the "
              "usual causes are:\n"
              "  - the display was already running an app (reset first)\n"
              "  - the file is not in /apps/ (check with `fw list-apps`)\n"
              "  - the UF2 targets a window the loader will not stage",
              file=sys.stderr)
    return rc


def list_apps():
    r"""Print the card's /apps directory (h\v\l), so a failed launch can be told
    apart from a missing file."""
    from fw_console import Console
    with Console() as con:
        _, frames = con.call(r"h\v\l", wait=20.0, idle=4.0)
    if not frames:
        print("no response frame", file=sys.stderr)
        return 1
    for path, _ts, seq, resp in frames:
        print(f"[{path}] seq={seq} {resp.strip()}")
    return 0


TIMER0_TIMERAWL = 0x400B0028   # free-running microsecond counter, read-to-peek

def peek_command(addr, count):
    """Read memory from a RUNNING target. `init` + `read_memory`, never `halt`.

    A halt is not a neutral observation on this chip: TIMER0's DBGPAUSE pauses
    the timer while a core is debug-halted, so halting to look at something
    stops the app's timebase underneath it. The config defers cm1's examine so
    that can no longer become permanent, but there is still no reason to halt
    just to read a word."""
    return _openocd_base() + [
        "-c", "init", "-c", f"read_memory 0x{addr:08x} 32 {count}", "-c", "shutdown"]

def _openocd_words(cmd):
    """Run an OpenOCD command list and collect the hex words it printed.

    OpenOCD logs to stderr -- including `read_memory` results -- so both streams
    have to be scanned. Info/Error lines are skipped so their addresses and
    hex ids are not mistaken for data."""
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    words = []
    for stream in (out.stdout, out.stderr):
        for line in stream.splitlines():
            if line.startswith(("Info :", "Warn :", "Error:", "Debug:")):
                continue
            for w in line.split():
                try:
                    words.append(int(w, 16) if w.startswith("0x") else None)
                except ValueError:
                    words.append(None)
    return [w for w in words if w is not None], out

def run_peek(addr, count):
    words, out = _openocd_words(peek_command(addr, count))
    if not words:
        sys.stderr.write(out.stderr)
        print("peek: no data — is the probe connected?", file=sys.stderr)
        return 1
    for i, w in enumerate(words[:count]):
        print(f"0x{addr + 4 * i:08x}: 0x{w:08x}")
    return 0

def run_alive():
    """Is the display CPU executing? Reads TIMER0 twice and looks for movement.

    This is the liveness test that does not perturb what it measures -- no halt,
    no reset. A frozen counter means the timebase is stopped, which presents as
    a hung UI (the LVGL tick stops, so redraws and input polling stop) even
    though the main loop may still be servicing things that need no timer."""
    wa, _ = _openocd_words(peek_command(TIMER0_TIMERAWL, 1))
    time.sleep(0.2)
    wb, _ = _openocd_words(peek_command(TIMER0_TIMERAWL, 1))

    t0 = wa[0] if wa else None
    t1 = wb[0] if wb else None
    if t0 is None or t1 is None:
        print("alive: could not read TIMER0 — is the probe connected?", file=sys.stderr)
        return 1
    if t1 != t0:
        print(f"alive: RUNNING (TIMER0 {t0:#010x} -> {t1:#010x})")
        return 0
    print(f"alive: TIMER0 IS FROZEN at {t0:#010x}\n"
          "  The timebase is stopped, so the LVGL tick and every LVGL timer are\n"
          "  stopped too: no redraws, no input polling. Anything driven straight\n"
          "  off the main loop still responds, so this looks like a hung UI.\n"
          "  Usual cause is a debug-halted core (TIMER0 DBGPAUSE). Try `fw thaw`.",
          file=sys.stderr)
    return 1

def run_thaw():
    """Release a core left debug-halted, which un-pauses TIMER0.

    Should be unnecessary now that the OpenOCD config defers cm1's examine, but
    a board frozen by an older config (or a hand-rolled `halt`) still needs
    rescuing, and that does not require relaunching the app."""
    cmd = _openocd_base() + ["-c", "init",
                             "-c", "targets rp2350.cm1", "-c", "poll",
                             "-c", "catch {resume}",
                             "-c", "targets rp2350.cm0", "-c", "catch {resume}",
                             "-c", "shutdown"]
    subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    return run_alive()

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

APP_WINDOWS = ("FLASH", "SRAM", "PSRAM")

def new_app(name, window="FLASH", repo_root=REPO_ROOT):
    """Scaffold apps/<name> from apps/template, targeting `window`.

    The window is a single token in the generated CMakeLists because everything
    that follows from it -- binary type, linker overrides, runtime-init
    overrides, UF2 emission, and the build-time check that the artifact matches
    -- lives in fw2_finalize_app(). Choosing SRAM or PSRAM here is the whole of
    what an app has to do to be loadable from the SD card without touching the
    stock display firmware."""
    window = window.upper()
    if window not in APP_WINDOWS:
        raise ValueError(f"window must be one of {', '.join(APP_WINDOWS)}")
    src = pathlib.Path(repo_root) / "apps" / "template"
    dest = pathlib.Path(repo_root) / "apps" / name
    if dest.exists():
        raise FileExistsError(dest)
    shutil.copytree(src, dest)
    cml = dest / "CMakeLists.txt"
    text = cml.read_text().replace("template", name)
    text = text.replace(f"fw2_finalize_app({name} FLASH)",
                        f"fw2_finalize_app({name} {window})")
    cml.write_text(text)
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
                sys.stdout.write(data.decode("ascii", "replace"))
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
    sp = sub.add_parser("configure")
    sp.add_argument("--clean", action="store_true", help="wipe build/ before configuring")
    sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("rtt")
    sp.add_argument("--print", dest="show", action="store_true")
    sp.add_argument("-s", "--seconds", type=int, default=0,
                    help="capture for N seconds then exit (0 = until Ctrl+C)")
    sp = sub.add_parser("test"); sp.add_argument("--print", dest="show", action="store_true")
    sp = sub.add_parser("new-app"); sp.add_argument("name")
    sp.add_argument("--window", default="FLASH", choices=["FLASH", "SRAM", "PSRAM",
                                                          "flash", "sram", "psram"],
                    help="where the app runs: FLASH replaces the stock display "
                         "firmware and is programmed over SWD; SRAM and PSRAM are "
                         "loadable from the SD card (default: FLASH)")
    sp = sub.add_parser("install-app")
    sp.add_argument("uf2", help="app UF2 to copy into /apps on the device SD card")
    sp.add_argument("--device", help="fwFinder device serial (required when multiple devices are connected)")
    sp.add_argument("--port", help="explicit MAIN serial port if fwFinder cannot identify legacy hardware")
    sp.add_argument("--timeout", type=float, default=25,
                    help="seconds to wait for the USB SD reader (default: 25)")

    sp = sub.add_parser("run-app")
    sp.add_argument("name", help="filename in /apps on the SD card, e.g. myapp.uf2")
    sp.add_argument("--no-reset", action="store_true",
                    help="skip the display reset (the launch will usually fail)")
    sp.add_argument("--timeout", type=float, default=90,
                    help="seconds to wait for the deferred result frame (default: 90)")
    sub.add_parser("list-apps")

    sp = sub.add_parser("peek")
    sp.add_argument("addr", help="address, e.g. 0x20012f44")
    sp.add_argument("--count", type=int, default=1, help="words to read (default: 1)")
    sub.add_parser("alive")
    sub.add_parser("thaw")

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
    elif a.cmd == "flash": _run(flash_command(a.app), a.show)
    elif a.cmd == "rtt":
        if a.show:
            _run(rtt_command(), True)
        else:
            return run_rtt(a.seconds)
    elif a.cmd == "test":  _run(test_command(), a.show)
    elif a.cmd == "new-app":
        print("created", new_app(a.name, a.window))
    elif a.cmd == "install-app":
        install_app(a.uf2, a.device, a.timeout, a.port)
    elif a.cmd == "run-app":
        return run_app(a.name, reset=not a.no_reset, timeout=a.timeout)
    elif a.cmd == "list-apps":
        return list_apps()
    elif a.cmd == "peek":
        return run_peek(int(a.addr, 0), a.count)
    elif a.cmd == "alive":
        return run_alive()
    elif a.cmd == "thaw":
        return run_thaw()
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
        try:
            w, h = agentio_capture(a.surface, crop, a.scale, a.out)
        except RuntimeError as e:
            if "measuring" in str(e):
                print("screenshot refused: the app is inside a timed region.\n"
                      "  A capture streams 307,200 bytes from the app's own main "
                      "loop, which would\n  land between its measurements and skew "
                      "the result -- so it is refused rather\n  than silently "
                      "corrupting it (agentio_measure_begin).\n"
                      "  Wait for the run to finish, or capture before starting it.",
                      file=sys.stderr)
                return 1
            if "busy" in str(e):
                print("screenshot refused: an injected press or TYPE is still "
                      "pending. Retry shortly.", file=sys.stderr)
                return 1
            raise
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
