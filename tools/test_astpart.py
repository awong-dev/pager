import hashlib
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import astpart

IMG = Path(__file__).resolve().parent.parent / "build" / "images" / "assets-dejavu-emoji.bin"


def synth() -> bytes:
    body = bytearray(b"PGFA\x01sc\0" + (5000).to_bytes(4, "little") + b"\x02\0\0\0")
    body += hashlib.sha256(b"x").digest() * 150
    return bytes(body[:5000])


def image() -> bytes:
    return IMG.read_bytes() if IMG.exists() else synth()


def test_record_matches_file(tmp_path):
    data = image()
    src = tmp_path / "a.bin"
    src.write_bytes(data)
    out = tmp_path / "p.bin"
    assert astpart.main(["build", str(src), "-o", str(out)]) == 0
    part = out.read_bytes()
    assert len(part) == 0x200000
    assert part[:len(data)] == data
    assert set(part[len(data):0x200000 - 4096]) == {0xFF}
    sector = part[-4096:]
    assert set(sector[64:]) == {0xFF}
    length, sha, fv = astpart.decode_rec(sector, 0x200000)
    assert length == len(data) and fv == data[4]
    assert sha == hashlib.sha256(data).digest()
    assert int.from_bytes(sector[60:64], "little") == zlib.crc32(sector[:60])


def test_real_image_size():
    if not IMG.exists():
        pytest.skip("build/images absent")
    assert IMG.stat().st_size == 736237


def test_set_version_and_fv():
    data = synth()
    part = astpart.build(data, set_version=2)
    assert part[4] == 2
    _length, sha, fv = astpart.decode_rec(part[-4096:], 0x200000)
    assert fv == 2 and sha == hashlib.sha256(part[:len(data)]).digest()
    part = astpart.build(data, fv=1, set_version=2)
    assert astpart.decode_rec(part[-4096:], 0x200000)[2] == 1


def test_corruptions_fail_decode():
    data = synth()
    assert astpart.decode_rec(astpart.build(data, bad_crc=True)[-4096:], 0x200000) is None
    assert astpart.decode_rec(astpart.build(data, corrupt_magic=True)[-4096:], 0x200000) is None


def test_too_big():
    with pytest.raises(ValueError):
        astpart.build(b"\0" * (0x200000 - 4095))
