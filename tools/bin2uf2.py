#!/usr/bin/env python3
"""Wrap a raw binary into a UF2 at an arbitrary base address.

    tools/bin2uf2.py <in.bin> <out.uf2> --base 0x11000000

Needed because picotool refuses a PSRAM-window image: `picotool uf2 convert`
fails with "entry point is not in mapped part of file" for anything based at
0x11000000, since it only knows about the flash window at 0x10000000 and SRAM.
The container itself is trivial, so we emit it directly.

Layout per github.com/microsoft/uf2: 512-byte blocks, 32-byte header, up to 476
payload bytes. 256 is used here, matching what picotool emits for RP2350 and what
fwImageStream/the fused bootloader are exercised with.

The FreeWili display-app loader classifies on the FIRST block's target address
(fwDisplayAppLoader::classifyImage), so the base is what routes the image:
    0x10000000  -> flash
    0x11000000  -> PSRAM, staged by FW2PsramStub
    0x20000000  -> SRAM, staged by the fused bootloader (192 KiB ceiling)
"""
import argparse
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30

UF2_FLAG_FAMILY_ID = 0x00002000

FAMILIES = {
    "rp2350-arm-s": 0xE48BFF59,
    "rp2350-arm-ns": 0xE48BFF5B,
    "rp2350-riscv": 0xE48BFF5A,
    "rp2040": 0xE48BFF56,
}

PAYLOAD = 256
BLOCK = 512


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--base", required=True,
                    help="target address of the first byte, e.g. 0x11000000")
    ap.add_argument("--family", default="rp2350-arm-s", choices=sorted(FAMILIES))
    args = ap.parse_args(argv)

    base = int(args.base, 0)
    family = FAMILIES[args.family]

    with open(args.src, "rb") as f:
        data = f.read()
    if not data:
        sys.exit("empty input")

    # Pad the tail so every block carries a full payload; the loader pads to 128
    # on the wire anyway and a short final block is legal but pointless.
    if len(data) % PAYLOAD:
        data += b"\x00" * (PAYLOAD - len(data) % PAYLOAD)

    nblocks = len(data) // PAYLOAD
    with open(args.dst, "wb") as out:
        for i in range(nblocks):
            chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
            hdr = struct.pack("<8I",
                              UF2_MAGIC_START0, UF2_MAGIC_START1,
                              UF2_FLAG_FAMILY_ID,
                              base + i * PAYLOAD,
                              PAYLOAD, i, nblocks, family)
            out.write(hdr + chunk + b"\x00" * (BLOCK - 32 - PAYLOAD - 4)
                      + struct.pack("<I", UF2_MAGIC_END))

    print(f"{args.dst}: {nblocks} blocks, {len(data)} payload bytes, "
          f"0x{base:08X}..0x{base + len(data):08X}, family {args.family}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
