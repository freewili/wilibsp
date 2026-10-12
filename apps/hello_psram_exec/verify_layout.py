"""Fail the build unless the ELF demonstrates the documented split layout."""

from __future__ import annotations

import re
import pathlib
import struct
import subprocess
import sys

PSRAM = range(0x11000000, 0x11800000)
SRAM = range(0x20000000, 0x20082001)


def symbols(elf: str, objdump: str) -> dict[str, int]:
    output = subprocess.check_output([objdump, "-t", elf], text=True)
    found: dict[str, int] = {}
    for line in output.splitlines():
        match = re.match(r"^([0-9a-fA-F]+)\s+.*\s(\S+)$", line)
        if match:
            found[match.group(2)] = int(match.group(1), 16)
    return found


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: verify_layout.py APP.elf OBJDUMP APP.uf2")
    found = symbols(sys.argv[1], sys.argv[2])
    expected = {
        "__vectors": PSRAM,
        "_entry_point": PSRAM,
        "main": PSRAM,
        "fw2_psram_bootstrap": SRAM,
        "board_init_psram": SRAM,
        # Everything board_init_psram() runs between raising clk_sys and
        # re-timing PSRAM, including the libgcc 64-bit division it reaches.
        "vreg_set_voltage": SRAM,
        "set_sys_clock_pll": SRAM,
        "clock_configure": SRAM,
        "psram_configure_params": SRAM,
        "psram_reinitialize": SRAM,
        "__aeabi_uldivmod": SRAM,
        "__udivmoddi4": SRAM,
        "__StackTop": SRAM,
    }
    for name, region in expected.items():
        address = found.get(name)
        if address is None:
            raise SystemExit(f"layout check: missing symbol {name}")
        if address not in region:
            raise SystemExit(
                f"layout check: {name}=0x{address:08x} outside expected region"
            )
    if found["__vectors"] != 0x11000000:
        raise SystemExit("layout check: vector table is not first in PSRAM")

    uf2 = pathlib.Path(sys.argv[3]).read_bytes()
    if not uf2 or len(uf2) % 512:
        raise SystemExit("layout check: malformed UF2 length")
    first_payload = None
    for offset in range(0, len(uf2), 512):
        block = uf2[offset : offset + 512]
        magic0, magic1, _flags, address, size = struct.unpack_from("<5I", block)
        magic_end = struct.unpack_from("<I", block, 508)[0]
        if (magic0, magic1, magic_end) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
            raise SystemExit("layout check: malformed UF2 magic")
        if not (size <= 476 and address in PSRAM and address + size - 1 in PSRAM):
            raise SystemExit(
                f"layout check: UF2 payload at 0x{address:08x} is not in PSRAM"
            )
        if address == 0x11000000:
            first_payload = block[32 : 32 + size]
    if first_payload is None or len(first_payload) < 8:
        raise SystemExit("layout check: UF2 does not contain the vector table")
    initial_sp, reset = struct.unpack_from("<2I", first_payload)
    if initial_sp not in SRAM:
        raise SystemExit(f"layout check: initial SP 0x{initial_sp:08x} is not SRAM")
    if (reset & ~1) not in PSRAM or not (reset & 1):
        raise SystemExit(f"layout check: reset vector 0x{reset:08x} is not Thumb PSRAM")
    print("layout check: PSRAM app + SRAM SDK/BSP bootstrap + PSRAM-only UF2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
