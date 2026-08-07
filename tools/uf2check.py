#!/usr/bin/env python3
"""Validate a FreeWili 2 display app UF2 against the window it claims to target.

    tools/uf2check.py <app.uf2> --window {flash,sram,psram}

Exits non-zero, with the offending block named, when the image would not do what
the build says it does. `fw2_finalize_app()` runs this on every app at build
time, so an app cannot reach an SD card in a state the loader will mishandle.

This exists because these were prose rules that every consumer project had to
read, remember, and apply by hand -- and at least one did not:

  - AGENTS.md: "/apps/ is a non-destructive DISPLAY launch surface. App UF2s may
    target only SRAM or PSRAM, never QSPI flash ... a write at flash base
    replaces the stock DISPLAY firmware, not the loader."
  - AGENTS.md invariant 12: "Keep the vector table first in PSRAM, with its
    initial stack in SRAM ... Verify symbol addresses plus every UF2 target
    block."

A downstream project shipped a flash-targeted UF2 to `/apps/` and overwrote
FW2Display, having reasoned its way past the first rule; `fw install-app`
catches that, but only if you use it, and the card-swap recipe most projects
actually use does not. A build-time check cannot be routed around.

The windows are the same ones `fw.py` enforces at install time:

    flash  0x10000000..0x11000000   stock DISPLAY firmware lives here
    psram  0x11000000..0x11800000   8 MB APS6404L, staged by FW2PsramStub
    sram   0x20000000..0x20070000   fused bootloader stages here directly
                                    (it runs from 0x20070000, hence the top)
"""
import argparse
import struct
import sys

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
BLOCK = 512

# UF2 flags. A "no flash" block carries no payload for the target and must not
# be range-checked -- picotool emits one at the top of flash on RP2350 as an
# erratum workaround, and it is not part of the image.
FLAG_NOT_MAIN_FLASH = 0x00000001

WINDOWS = {
    "flash": (0x10000000, 0x11000000),
    "psram": (0x11000000, 0x11800000),
    "sram":  (0x20000000, 0x20070000),
}

# The initial stack pointer must land in SRAM for every binary type: a PSRAM app
# executes from the window but must not put its stack there, and the SDK's own
# layouts always park SP at the top of SRAM.
SRAM_LO, SRAM_HI = 0x20000000, 0x20082001  # inclusive top: SP is usually 0x20082000


def parse_blocks(data):
    """Yield (index, flags, addr, payload) for each well-formed UF2 block."""
    if len(data) % BLOCK:
        raise SystemExit(f"uf2check: not a UF2 -- {len(data)} bytes is not a "
                         f"multiple of {BLOCK}")
    for i in range(len(data) // BLOCK):
        off = i * BLOCK
        magic0, magic1, flags, addr, size, _blk, _n, _fam = struct.unpack_from(
            "<8I", data, off)
        magic_end, = struct.unpack_from("<I", data, off + BLOCK - 4)
        if magic0 != UF2_MAGIC0 or magic1 != UF2_MAGIC1 or magic_end != UF2_MAGIC_END:
            raise SystemExit(f"uf2check: block {i} has bad UF2 magic")
        if size > 476:
            raise SystemExit(f"uf2check: block {i} declares oversized payload {size}")
        payload = data[off + 32: off + 32 + size]
        yield i, flags, addr, payload


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("uf2")
    ap.add_argument("--window", required=True, choices=sorted(WINDOWS))
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    lo, hi = WINDOWS[a.window]
    with open(a.uf2, "rb") as fh:
        data = fh.read()

    blocks = [(i, f, addr, pl) for i, f, addr, pl in parse_blocks(data)]
    payload = [b for b in blocks if not (b[1] & FLAG_NOT_MAIN_FLASH)]
    if not payload:
        raise SystemExit("uf2check: no payload blocks")

    errors = []

    # 1. Every payload block inside the declared window. This is the check that
    #    keeps an app off the stock firmware's address.
    stray = [(i, addr) for i, _f, addr, pl in payload
             if not (lo <= addr and addr + len(pl) <= hi)]
    if stray:
        i, addr = stray[0]
        where = next((n for n, (wlo, whi) in WINDOWS.items() if wlo <= addr < whi),
                     "outside every known window")
        errors.append(
            f"{len(stray)} of {len(payload)} blocks fall outside the {a.window} "
            f"window {lo:#010x}..{hi:#010x};\n"
            f"    first is block {i} at {addr:#010x}, which is in {where}.\n"
            f"    The build says {a.window} but the image targets somewhere else -- "
            f"fix the binary type or the declared window, not this check.")
        if where == "flash":
            errors.append(
                "flash is where the stock DISPLAY firmware lives. Installing this "
                "to /apps/ would\n    REPLACE FW2Display. See AGENTS.md, "
                "'/apps/ is a non-destructive DISPLAY launch surface'.")

    # 2. Vector table first, at the window base. The loader jumps via word 0/1 of
    #    the base block, so an image whose first bytes are not a vector table
    #    launches into nothing. (AGENTS.md invariant 12.)
    base = min(addr for _i, _f, addr, _pl in payload)
    if base != lo:
        errors.append(
            f"image starts at {base:#010x}, not the {a.window} base {lo:#010x}.\n"
            f"    The loader classifies on the first block and jumps via the "
            f"vector table at the base.")
    else:
        first = next(pl for _i, _f, addr, pl in payload if addr == base)
        if len(first) < 8:
            errors.append("base block is too short to hold a vector table")
        else:
            sp, pc = struct.unpack_from("<II", first, 0)
            if not (SRAM_LO <= sp <= SRAM_HI):
                errors.append(
                    f"initial SP is {sp:#010x}, which is not in SRAM "
                    f"({SRAM_LO:#010x}..{SRAM_HI:#010x}).\n"
                    f"    Invariant 12: the vector table goes first in the window, "
                    f"its stack in SRAM.")
            if not (lo <= (pc & ~1) < hi):
                errors.append(
                    f"reset vector is {pc:#010x}, which is not in the {a.window} "
                    f"window.")
            elif not (pc & 1):
                errors.append(
                    f"reset vector {pc:#010x} has the thumb bit clear; the core "
                    f"will fault on entry.")

    if errors:
        sys.stderr.write(f"\nuf2check: {a.uf2} FAILED as a '{a.window}' app\n")
        for e in errors:
            sys.stderr.write(f"  - {e}\n")
        sys.stderr.write("\n")
        return 1

    if not a.quiet:
        top = max(addr for _i, _f, addr, _pl in payload)
        extra = len(blocks) - len(payload)
        note = f" (+{extra} non-payload)" if extra else ""
        print(f"uf2check: {a.window} OK -- {len(payload)} blocks{note}, "
              f"{base:#010x}..{top:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
