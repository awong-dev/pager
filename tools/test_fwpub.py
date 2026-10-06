import hashlib
import json
import os
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import fwpub  # noqa: E402

IMAGES = Path(__file__).resolve().parent.parent / "build" / "images"
MAIN = IMAGES / "main-release-app.bin"
KEYLAT = IMAGES / "keylat-release-app.bin"


def synth(seed: bytes = b"a", n: int = 5000) -> bytes:
    body = bytearray(hashlib.sha256(seed).digest() * (n // 32))
    body[0] = 0xE9
    body[23] = 1
    body[0x30:0x50] = b"v-test".ljust(32, b"\0")
    return bytes(body) + hashlib.sha256(bytes(body)).digest()


def test_synthetic_accepts_and_extracts():
    img = synth()
    info = fwpub.check_image(img)
    assert info["id"] == img[-32:].hex() and len(info["id16"]) == 16
    assert info["version"] == "v-test"


def test_rejects_truncated_and_bad_tail():
    img = synth()
    with pytest.raises(ValueError):
        fwpub.check_image(img[:-100])
    with pytest.raises(ValueError):
        fwpub.check_image(img[:-1] + bytes([img[-1] ^ 1]))
    with pytest.raises(ValueError):
        fwpub.check_image(b"\x00" + img[1:])


def test_large_needs_flag():
    body = bytearray(1_100_000)
    body[0] = 0xE9
    body[23] = 1
    img = bytes(body) + hashlib.sha256(bytes(body)).digest()
    with pytest.raises(ValueError, match="debug"):
        fwpub.check_image(img)
    assert fwpub.check_image(img, allow_large=True)["size"] == len(img)


@pytest.mark.skipif(not MAIN.exists(), reason="build/images absent")
def test_real_main_accepted():
    assert fwpub.check_image(MAIN.read_bytes())["size"] == 685168


def validate_index(idx):
    hx64 = re.compile(r"^[0-9a-f]{64}$")
    path = re.compile(r"^fw/[0-9a-f]{16}/(full\.z|from-[0-9a-f]{16}\.dz)$")
    assert idx["v"] == 1
    for b in idx["builds"]:
        assert hx64.match(b["id"]) and 0 < b["size"] <= 0x200000
        for o in [b["full"]] + b["deltas"]:
            assert path.match(o["path"]) and hx64.match(o["osha"]) and 0 < o["osz"] <= 0x200000
        for d in b["deltas"]:
            assert hx64.match(d["base"]) and d["psz"] > 0
    assert [b["published"] for b in idx["builds"]] == sorted(
        (b["published"] for b in idx["builds"]), reverse=True)


def test_dry_run_synthetic(tmp_path):
    a, b = synth(b"a"), synth(b"b")
    pa, pb = tmp_path / "a.bin", tmp_path / "b.bin"
    pa.write_bytes(a)
    pb.write_bytes(b)
    out = tmp_path / "out"
    assert fwpub.main(["publish", str(pb), "--bucket", "x", "--base", str(pa),
                       "--dry-run", "--out", str(out)]) == 0
    idx = json.loads((out / "fw" / "index.json").read_text())
    validate_index(idx)
    assert len(idx["builds"][0]["deltas"]) == 1
    # republishing identical bytes is fine; a tampered object is refused
    assert fwpub.main(["publish", str(pb), "--bucket", "x", "--dry-run", "--out", str(out)]) == 0
    full = out / "fw" / b[-32:].hex()[:16] / "full.z"
    full.write_bytes(b"junk")
    assert fwpub.main(["publish", str(pb), "--bucket", "x", "--dry-run", "--out", str(out)]) == 1


@pytest.mark.skipif(not (MAIN.exists() and KEYLAT.exists()), reason="build/images absent")
def test_dry_run_measured_sizes(tmp_path):
    assert fwpub.main(["publish", str(MAIN), "--bucket", "x", "--base", str(KEYLAT),
                       "--dry-run", "--out", str(tmp_path)]) == 0
    idx = json.loads((tmp_path / "fw" / "index.json").read_text())
    validate_index(idx)
    b = idx["builds"][0]
    assert abs(b["full"]["osz"] - 338784) <= 64
    assert len(b["deltas"]) == 1
    assert abs(b["deltas"][0]["osz"] - 42513) <= 64
    d = tmp_path / b["deltas"][0]["path"]
    assert hashlib.sha256(d.read_bytes()).hexdigest() == b["deltas"][0]["osha"]
