import pathlib
import struct
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import uf2check


def uf2_block(address, payload, *, declared_size=None, block_no=0, block_count=1):
    data = bytearray(uf2check.BLOCK)
    size = len(payload) if declared_size is None else declared_size
    struct.pack_into(
        "<8I", data, 0,
        uf2check.UF2_MAGIC0, uf2check.UF2_MAGIC1, 0,
        address, size, block_no, block_count, 0xE48BFF59,
    )
    data[32:32 + len(payload)] = payload
    struct.pack_into("<I", data, uf2check.BLOCK - 4, uf2check.UF2_MAGIC_END)
    return bytes(data)


def test_rejects_payload_larger_than_uf2_capacity():
    block = uf2_block(0x11000000, b"", declared_size=477)

    with pytest.raises(SystemExit, match="oversized payload 477"):
        list(uf2check.parse_blocks(block))


def test_rejects_payload_crossing_window_end(tmp_path):
    vector = struct.pack("<II", 0x20082000, 0x11000001) + bytes(248)
    image = (
        uf2_block(0x11000000, vector, block_no=0, block_count=2)
        + uf2_block(0x117FFF80, bytes(256), block_no=1, block_count=2)
    )
    path = tmp_path / "crossing.uf2"
    path.write_bytes(image)

    assert uf2check.main([str(path), "--window", "psram", "--quiet"]) == 1