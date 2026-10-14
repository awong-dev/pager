import hashlib
import json
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import fwpub

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


def test_withdraw_moves_build_and_merge_keeps_it():
    def b(v, i, ts):
        return {"id": i * 64, "version": v, "published": ts, "deltas": []}
    idx = {"v": 1, "builds": [b("v2", "b", 2), b("v1", "a", 1)]}
    out = fwpub.withdraw_index(idx, "v2", "broke unlock", 9)
    assert [x["version"] for x in out["builds"]] == ["v1"]
    assert out["withdrawn"] == [{"id": "b" * 64, "version": "v2", "reason": "broke unlock", "at": 9}]
    merged = fwpub.merge_index(out, b("v3", "c", 3))
    assert [x["version"] for x in merged["builds"]] == ["v3", "v1"] and merged["withdrawn"] == out["withdrawn"]
    with pytest.raises(KeyError):
        fwpub.withdraw_index(out, "v2", "again", 10)


# --- assets-publish / assets-withdraw (docs/ASSETS_OTA_DESIGN.md §3) ---------
EMOJI = Path(__file__).resolve().parent.parent / "build" / "images" / "assets-dejavu-emoji.bin"


def synth_assets(seed: bytes = b"a", total: int | None = None, ver: int = 1) -> bytes:
    body = seed * 2000
    n = 16 + len(body)
    hdr = b"PGFA" + bytes([ver]) + b"en\0" + (n if total is None else total).to_bytes(4, "little") + b"\2\0\0\0"
    return hdr + body


def validate_assets_index(idx):
    hx64 = re.compile(r"^[0-9a-f]{64}$")
    assert idx["v"] == 1 and idx["withdrawn"] is not None
    for b in idx["builds"]:
        assert set(b) == {"id", "label", "lang", "fv", "size", "published", "full"}
        assert hx64.match(b["id"]) and 1 <= b["fv"] <= 255 and 0 < b["size"] <= 0x200000 - 4096
        assert b["full"]["path"] == f"as/{b['id'][:16]}/full.z"
        assert hx64.match(b["full"]["osha"]) and 0 < b["full"]["osz"] <= 0x200000


def test_check_assets_rules():
    info = fwpub.check_assets(synth_assets())
    assert info["fv"] == 1 and info["lang"] == "en" and info["id16"] == info["id"][:16]
    for bad in (synth_assets(total=99), synth_assets(ver=2), synth_assets(ver=0),
                b"XXXX" + synth_assets()[4:], synth_assets()[:10]):
        with pytest.raises(ValueError):
            fwpub.check_assets(bad)
    big = synth_assets(b"z")
    big = big[:8] + (0x200000 - 4095).to_bytes(4, "little") + big[12:16] + b"\0" * (0x200000 - 4095 - 16)
    with pytest.raises(ValueError):
        fwpub.check_assets(big)


def test_assets_publish_dry_run_synthetic_and_withdraw(tmp_path):
    p = tmp_path / "a.bin"
    p.write_bytes(synth_assets(b"a"))
    q = tmp_path / "b.bin"
    q.write_bytes(synth_assets(b"b"))
    out = tmp_path / "out"
    assert fwpub.main(["assets-publish", str(p), "--bucket", "x", "--label", "one",
                       "--dry-run", "--out", str(out)]) == 0
    assert fwpub.main(["assets-publish", str(q), "--bucket", "x", "--label", "two",
                       "--dry-run", "--out", str(out)]) == 0
    idx = json.loads((out / "as" / "index.json").read_text())
    validate_assets_index(idx)
    assert {b["label"] for b in idx["builds"]} == {"one", "two"}
    b = idx["builds"][0]
    assert (out / b["full"]["path"]).exists() and (out / "as" / b["id"][:16] / "manifest.json").exists()
    assert hashlib.sha256((out / b["full"]["path"]).read_bytes()).hexdigest() == b["full"]["osha"]
    # republish same bytes ok; tampered object refused
    assert fwpub.main(["assets-publish", str(p), "--bucket", "x", "--label", "one",
                       "--dry-run", "--out", str(out)]) == 0
    assert len(json.loads((out / "as" / "index.json").read_text())["builds"]) == 2
    # wrong total refused (no write)
    bad = tmp_path / "bad.bin"
    bad.write_bytes(synth_assets(b"c", total=5))
    assert fwpub.main(["assets-publish", str(bad), "--bucket", "x", "--label", "bad",
                       "--dry-run", "--out", str(out)]) == 1
    # withdraw by label moves the entry; publish then refuses it
    assert fwpub.main(["assets-withdraw", "one", "--bucket", "x", "--reason", "oops",
                       "--dry-run", "--out", str(out)]) == 0
    idx = json.loads((out / "as" / "index.json").read_text())
    validate_assets_index(idx)
    assert [b["label"] for b in idx["builds"]] == ["two"]
    assert idx["withdrawn"][0]["label"] == "one" and idx["withdrawn"][0]["reason"] == "oops"
    assert fwpub.main(["assets-publish", str(p), "--bucket", "x", "--label", "one",
                       "--dry-run", "--out", str(out)]) == 1
    assert fwpub.main(["assets-withdraw", "nope", "--bucket", "x", "--reason", "r",
                       "--dry-run", "--out", str(out)]) == 1
    (out / "as" / "index.json").unlink()
    full = out / "as" / fwpub.check_assets(q.read_bytes())["id16"] / "full.z"
    full.write_bytes(b"junk")
    assert fwpub.main(["assets-publish", str(q), "--bucket", "x", "--label", "two",
                       "--dry-run", "--out", str(out)]) == 1


@pytest.mark.skipif(not EMOJI.exists(), reason="build/images absent")
def test_assets_publish_real_emoji_image(tmp_path):
    assert fwpub.main(["assets-publish", str(EMOJI), "--bucket", "x", "--label", "dejavu+emoji",
                       "--dry-run", "--out", str(tmp_path)]) == 0
    idx = json.loads((tmp_path / "as" / "index.json").read_text())
    validate_assets_index(idx)
    b = idx["builds"][0]
    assert b["full"]["osz"] == 379008 and b["size"] == 736237 and b["fv"] == 1
