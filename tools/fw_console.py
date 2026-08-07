r"""OneWili console client for the FreeWili 2 MAIN CPU's app-USB CDC.

Used by `fw run-app` / `fw console`. MAIN's console is the tty whose USB parent
is 093c:2060 ("FW2 v04"); this finds it by walking /dev/ttyACM*. It is NOT
2e8a:0009 -- that is a raw Pico SDK build, not stock firmware -- and the two
CDCs on the debug probe (2e8a:000c) are UART bridges, not this console.

No pyserial dependency: this drives the tty through termios directly, so it
works on a bare checkout.

Two things are needed and neither is discoverable:

  - **Assert DTR.** The SDK's stdio_usb only transmits while tud_cdc_connected(),
    which requires the host to raise DTR. A raw open leaves it low and the board
    looks completely dead.
  - **Send leaf paths, not navigation.** Menus do not answer: 'h' produces
    nothing. '?' and full backslash paths with args do. A \x02 prefix returns
    the menu to root first.

Responses are frames: [<echoed path> <hexTimestampNs> <seq> <response>]

Paths this module uses -- full set in libs/onewili/include/onewili.h:

    ?             identify
    h\x\k <n>     SDCard Host Select: 0 = MAIN, 1 = USB reader / PC
    h\v\l         List Display Apps (the card's /apps directory)
    h\v\x         Reset Display CPU (RUN pulse)
    a\r <file>    Run App -- destination inferred from the IMAGE, not the name:
                  an SRAM-targeted UF2 is staged into RAM, a PSRAM-window one
                  through FW2PsramStub, anything else is written to flash
"""
import fcntl
import glob
import os
import re
import select
import struct
import subprocess
import termios
import time

RESET_QUIET = b"\x02"
FRAME_RE = re.compile(
    r"\[([a-zA-Z0-9?](?:\\[a-zA-Z0-9])*)\s+([0-9a-fA-F]+)\s+(\d+)\s*(.*?)\]", re.S)

TIOCMBIS = 0x5416
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004

CONSOLE_VID, CONSOLE_PID = "093c", "2060"

NO_CONSOLE_HELP = (
    f"no {CONSOLE_VID}:{CONSOLE_PID} console found.\n"
    "  Is MAIN running, and is every debugger detached? A debug-halted core "
    "stops TIMER0\n  via DBGPAUSE, and MAIN then wedges in a busy-wait before "
    "its USB init.\n"
    "  Override the port with OW_TTY=/dev/ttyACMx if autodetection is wrong.")


def find_console():
    """The tty whose USB parent is the MAIN CPU's app CDC."""
    for dev in sorted(glob.glob("/dev/ttyACM*")):
        try:
            out = subprocess.run(["udevadm", "info", "-q", "property", "-n", dev],
                                 capture_output=True, text=True, timeout=5).stdout
        except Exception:
            continue
        if (f"ID_VENDOR_ID={CONSOLE_VID}" in out
                and f"ID_MODEL_ID={CONSOLE_PID}" in out):
            return dev
    return None


def open_port(dev, baud=115200):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    speed = getattr(termios, "B%d" % baud)
    cc = list(termios.tcgetattr(fd)[6])
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW,
                      [0, 0, termios.CS8 | termios.CREAD | termios.CLOCAL, 0,
                       speed, speed, cc])
    termios.tcflush(fd, termios.TCIOFLUSH)
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))
    time.sleep(0.05)
    return fd


def read_for(fd, secs, idle=None):
    """Read until `secs` elapse, or until `idle` seconds pass with no new data."""
    out = b""
    deadline = time.time() + secs
    last = time.time()
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                chunk = b""
            if chunk:
                out += chunk
                last = time.time()
        elif idle is not None and out and time.time() - last > idle:
            break
    return out


def call(fd, cmd, wait=15.0, idle=4.0):
    """Send one leaf path and collect the frames it produces."""
    os.write(fd, RESET_QUIET + cmd.encode("ascii") + b"\n")
    raw = read_for(fd, wait, idle=idle).decode("latin1")
    return raw, FRAME_RE.findall(raw)


class Console:
    """Context manager around the MAIN console."""

    def __init__(self, dev=None):
        self.dev = dev or os.environ.get("OW_TTY") or find_console()
        if not self.dev:
            raise SystemExit(NO_CONSOLE_HELP)
        self.fd = None

    def __enter__(self):
        self.fd = open_port(self.dev)
        read_for(self.fd, 0.4)          # drain whatever was already buffered
        return self

    def __exit__(self, *exc):
        if self.fd is not None:
            os.close(self.fd)
        return False

    def call(self, cmd, wait=15.0, idle=4.0):
        return call(self.fd, cmd, wait=wait, idle=idle)
