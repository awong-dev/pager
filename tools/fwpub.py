#!/usr/bin/env python3
"""
`tools/fwpub.py` -- publish a firmware release to the public OTA bucket
(docs/OTA_DESIGN.md D1/D2/D11).

  fwpub.py publish <release-app.bin> --bucket <name> [--base <bin>]... [--k 3]
                   [--allow-large] [--dry-run --out <dir>]
  fwpub.py withdraw <id16|version> --bucket <name> --reason <text>
  fwpub.py probe --bucket <name>
  fwpub.py assets-publish <assets.bin> --bucket <name> --label <text> [--dry-run --out <dir>]
  fwpub.py assets-withdraw <id16|label> --bucket <name> --reason <text> [--dry-run --out <dir>]

Font (assets) images (docs/ASSETS_OTA_DESIGN.md §3) live under the same bucket:
  as/<id16>/full.z, as/<id16>/manifest.json   (immutable)
  as/index.json (no-store)  {v:1, builds:[{id,label,lang,fv,size,published,
                             full:{path,osz,osha}}], withdrawn:[]}
id = sha256(whole file). Full images only.

Image id = the SHA-256 esptool appends to every app .bin (last 32 bytes);
id16 = its first 16 hex chars. Objects (content-addressed, immutable):
  fw/<id16>/full.z               zlib(bin, level 9, 32 KB window)
  fw/<id16>/from-<base16>.dz     zlib(detools sequential patch, no compression)
  fw/<id16>/manifest.json        this build's index entry
and fw/index.json (no-store), newest build first. A withdrawn (bad) build is
moved from `builds` to `withdrawn` (objects kept, so a pager running it still
takes a delta off it); publish refuses a withdrawn id. Needs `pip install detools`
and `gcloud` (not needed with --dry-run).
"""
import argparse
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
import zlib

MAX_APP = 0x200000
# assets_ota is 2 MiB; its last 4 KiB sector holds the commit record.
MAX_ASSETS = 0x200000 - 4096
ASSETS_FV_MAX = 1  # == gfx.h GFX_ASSETS_VER_MAX
LARGE = 1_000_000
IMMUTABLE = "public, max-age=31536000, immutable"
NO_STORE = "no-store"


def check_image(data: bytes, allow_large: bool = False) -> dict:
    """Validate an esp app image; return {id, id16, version, size}. Raises ValueError."""
    if len(data) < 0x100:
        raise ValueError("image too short")
    if data[0] != 0xE9:
        raise ValueError("bad magic (expected 0xE9)")
    if data[23] != 1:
        raise ValueError("hash_appended (header byte 23) is not 1")
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError("appended SHA-256 does not match image")
    if len(data) > MAX_APP:
        raise ValueError(f"image {len(data)} B exceeds app slot {MAX_APP} B")
    if len(data) > LARGE and not allow_large:
        raise ValueError(
            f"image is {len(data)} B (> {LARGE}); release images are about 685 KB and "
            "debug images about 1.3 MB, so this is almost certainly a debug build. "
            "Pass --allow-large to publish anyway.")
    ident = data[-32:].hex()
    version = data[0x30:0x50].split(b"\0", 1)[0].decode("ascii", "replace")
    return {"id": ident, "id16": ident[:16], "version": version, "size": len(data)}


def check_assets(data: bytes) -> dict:
    """Validate a PGFA font image (tools/mkassets.py header: magic, u8 version,
    3-byte lang, u32 total, ...). Returns {id, id16, fv, lang, size}."""
    if len(data) < 16:
        raise ValueError("assets image too short")
    if data[:4] != b"PGFA":
        raise ValueError("bad magic (expected PGFA)")
    if not 1 <= data[4] <= ASSETS_FV_MAX:
        raise ValueError(f"unknown assets format version {data[4]} (max {ASSETS_FV_MAX})")
    total = int.from_bytes(data[8:12], "little")
    if total != len(data):
        raise ValueError(f"header total {total} != file length {len(data)}")
    if len(data) > MAX_ASSETS:
        raise ValueError(f"image {len(data)} B exceeds {MAX_ASSETS} B")
    ident = hashlib.sha256(data).hexdigest()
    lang = data[5:8].replace(b"\0", b"").decode("ascii", "replace")
    return {"id": ident, "id16": ident[:16], "fv": data[4], "lang": lang, "size": len(data)}


def assets_entry(info: dict, label: str, full: bytes, published: int) -> dict:
    return {
        "id": info["id"], "label": label, "lang": info["lang"], "fv": info["fv"],
        "size": info["size"], "published": published,
        "full": {"path": f"as/{info['id16']}/full.z", "osz": len(full),
                 "osha": hashlib.sha256(full).hexdigest()},
    }


def assets_merge_index(index: dict | None, entry: dict) -> dict:
    builds = [b for b in (index or {}).get("builds", []) if b["id"] != entry["id"]]
    builds.append(entry)
    builds.sort(key=lambda b: b["published"], reverse=True)
    return {"v": 1, "builds": builds, "withdrawn": list((index or {}).get("withdrawn", []))}


def assets_withdraw_index(index: dict, key: str, reason: str, at: int) -> dict:
    """Moves the build whose id16 or label is `key` to `withdrawn`. Raises KeyError."""
    hit = [b for b in index.get("builds", []) if b["id"][:16] == key or b["label"] == key]
    if len(hit) != 1:
        raise KeyError(f"{len(hit)} assets builds match {key!r}")
    b = hit[0]
    return {
        "v": 1,
        "builds": [x for x in index["builds"] if x is not b],
        "withdrawn": list(index.get("withdrawn", []))
        + [{"id": b["id"], "label": b["label"], "reason": reason, "at": at}],
    }


def make_full(data: bytes) -> bytes:
    # zlib with the default 32 KB window: the device runs tinfl with a zlib header.
    return zlib.compress(data, 9)


def make_delta(base: bytes, new: bytes) -> tuple[bytes, int]:
    """Return (dz, psz) and verify the round trip."""
    import detools
    patch = io.BytesIO()
    detools.create_patch(io.BytesIO(base), io.BytesIO(new), patch,
                         patch_type="sequential", compression="none")
    raw = patch.getvalue()
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(raw), out)
    if out.getvalue() != new:
        raise RuntimeError("delta does not round-trip")
    return zlib.compress(raw, 9), len(raw)


def entry_for(info: dict, full: bytes, deltas: list, published: int) -> dict:
    i16 = info["id16"]
    return {
        "id": info["id"], "version": info["version"], "size": info["size"],
        "published": published,
        "full": {"path": f"fw/{i16}/full.z", "osz": len(full),
                 "osha": hashlib.sha256(full).hexdigest()},
        "deltas": [{"base": d["base"], "path": f"fw/{i16}/from-{d['base'][:16]}.dz",
                    "osz": len(d["dz"]), "osha": hashlib.sha256(d["dz"]).hexdigest(),
                    "psz": d["psz"]} for d in deltas],
    }


def merge_index(index: dict | None, entry: dict) -> dict:
    builds = [b for b in (index or {}).get("builds", []) if b["id"] != entry["id"]]
    builds.append(entry)
    builds.sort(key=lambda b: b["published"], reverse=True)
    out = {"v": 1, "builds": builds}
    if (index or {}).get("withdrawn"):
        out["withdrawn"] = index["withdrawn"]
    return out


def withdraw_index(index: dict, key: str, reason: str, at: int) -> dict:
    """Moves the build whose id16 or version is `key` to `withdrawn`. Raises KeyError."""
    hit = [b for b in index.get("builds", []) if b["id"][:16] == key or b["version"] == key]
    if len(hit) != 1:
        raise KeyError(f"{len(hit)} builds match {key!r}")
    b = hit[0]
    out = {"v": 1, "builds": [x for x in index["builds"] if x is not b]}
    out["withdrawn"] = list(index.get("withdrawn", [])) + [
        {"id": b["id"], "version": b["version"], "reason": reason, "at": at}]
    return out


# --- storage backends --------------------------------------------------------
class Gcs:
    def __init__(self, bucket):
        self.bucket = bucket

    def url(self, path):
        return f"gs://{self.bucket}/{path}"

    def get(self, path):
        r = subprocess.run(["gcloud", "storage", "cat", self.url(path)], capture_output=True)
        return r.stdout if r.returncode == 0 else None

    def put(self, path, data, cache):
        with tempfile.NamedTemporaryFile() as f:
            f.write(data)
            f.flush()
            subprocess.run(["gcloud", "storage", "cp", f"--cache-control={cache}",
                            f.name, self.url(path)], check=True)


class Local:
    def __init__(self, root):
        self.root = root

    def get(self, path):
        p = os.path.join(self.root, path)
        return open(p, "rb").read() if os.path.exists(p) else None

    def put(self, path, data, cache):
        p = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def fetch_base_from_index(store, build: dict) -> bytes | None:
    z = store.get(build["full"]["path"])
    if z is None:
        return None
    data = zlib.decompress(z)
    if data[-32:].hex() != build["id"]:
        raise RuntimeError(f"published base {build['id'][:16]} is corrupt")
    return data


def publish(args) -> int:
    data = open(args.image, "rb").read()
    info = check_image(data, args.allow_large)
    store = Local(args.out) if args.dry_run else Gcs(args.bucket)
    raw_index = store.get("fw/index.json")
    index = json.loads(raw_index) if raw_index else None

    if any(w["id"] == info["id"] for w in (index or {}).get("withdrawn", [])):
        print(f"refusing: {info['version']} {info['id16']} is withdrawn", file=sys.stderr)
        return 1

    full = make_full(data)
    existing = store.get(f"fw/{info['id16']}/full.z")
    if existing is not None and existing != full:
        print(f"refusing: fw/{info['id16']}/full.z exists with different bytes", file=sys.stderr)
        return 1

    bases = {}  # id -> bytes, newest published first then --base files
    for b in (index or {}).get("builds", []):
        if len(bases) >= args.k:
            break
        if b["id"] == info["id"]:
            continue
        got = fetch_base_from_index(store, b)
        if got is not None:
            bases[b["id"]] = got
    for path in args.base:
        bd = open(path, "rb").read()
        bi = check_image(bd, True)
        if bi["id"] != info["id"]:
            bases[bi["id"]] = bd

    deltas = []
    for bid, bd in bases.items():
        dz, psz = make_delta(bd, data)
        deltas.append({"base": bid, "dz": dz, "psz": psz})
        print(f"delta from {bid[:16]}: patch {psz} B, object {len(dz)} B")

    entry = entry_for(info, full, deltas, int(time.time()))
    i16 = info["id16"]
    store.put(f"fw/{i16}/full.z", full, IMMUTABLE)
    for d in deltas:
        store.put(f"fw/{i16}/from-{d['base'][:16]}.dz", d["dz"], IMMUTABLE)
    store.put(f"fw/{i16}/manifest.json", json.dumps(entry, indent=1).encode(), IMMUTABLE)
    new_index = merge_index(index, entry)
    store.put("fw/index.json", json.dumps(new_index, indent=1).encode(), NO_STORE)
    print(f"published {info['version']} id16={i16} full={len(full)} B deltas={len(deltas)}")
    return 0


def withdraw(args) -> int:
    store = Gcs(args.bucket)
    raw = store.get("fw/index.json")
    if not raw:
        print("no fw/index.json", file=sys.stderr)
        return 1
    try:
        new_index = withdraw_index(json.loads(raw), args.build, args.reason, int(time.time()))
    except KeyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    store.put("fw/index.json", json.dumps(new_index, indent=1).encode(), NO_STORE)
    w = new_index["withdrawn"][-1]
    print(f"withdrew {w['version']} id16={w['id'][:16]}")
    return 0


def assets_publish(args) -> int:
    data = open(args.image, "rb").read()
    info = check_assets(data)
    store = Local(args.out) if args.dry_run else Gcs(args.bucket)
    raw_index = store.get("as/index.json")
    index = json.loads(raw_index) if raw_index else None
    if any(w["id"] == info["id"] for w in (index or {}).get("withdrawn", [])):
        print(f"refusing: assets {info['id16']} is withdrawn", file=sys.stderr)
        return 1
    full = make_full(data)
    existing = store.get(f"as/{info['id16']}/full.z")
    if existing is not None and existing != full:
        print(f"refusing: as/{info['id16']}/full.z exists with different bytes", file=sys.stderr)
        return 1
    entry = assets_entry(info, args.label, full, int(time.time()))
    i16 = info["id16"]
    store.put(f"as/{i16}/full.z", full, IMMUTABLE)
    store.put(f"as/{i16}/manifest.json", json.dumps(entry, indent=1).encode(), IMMUTABLE)
    store.put("as/index.json", json.dumps(assets_merge_index(index, entry), indent=1).encode(),
              NO_STORE)
    print(f"published assets {args.label!r} id16={i16} size={info['size']} full={len(full)} B")
    return 0


def assets_withdraw(args) -> int:
    store = Local(args.out) if args.dry_run else Gcs(args.bucket)
    raw = store.get("as/index.json")
    if not raw:
        print("no as/index.json", file=sys.stderr)
        return 1
    try:
        new_index = assets_withdraw_index(json.loads(raw), args.build, args.reason, int(time.time()))
    except KeyError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    store.put("as/index.json", json.dumps(new_index, indent=1).encode(), NO_STORE)
    w = new_index["withdrawn"][-1]
    print(f"withdrew assets {w['label']!r} id16={w['id'][:16]}")
    return 0


def probe(args) -> int:
    data = os.urandom(4096)
    Gcs(args.bucket).put("fw/probe-4k.bin", data, NO_STORE)
    print(hashlib.sha256(data).hexdigest())
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("publish")
    p.add_argument("image")
    p.add_argument("--bucket", required=True)
    p.add_argument("--base", action="append", default=[])
    p.add_argument("--k", type=int, default=3)
    p.add_argument("--allow-large", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--out")
    w = sub.add_parser("withdraw")
    w.add_argument("build")
    w.add_argument("--bucket", required=True)
    w.add_argument("--reason", required=True)
    ap_ = sub.add_parser("assets-publish")
    ap_.add_argument("image")
    ap_.add_argument("--bucket", required=True)
    ap_.add_argument("--label", required=True)
    ap_.add_argument("--dry-run", action="store_true")
    ap_.add_argument("--out")
    aw = sub.add_parser("assets-withdraw")
    aw.add_argument("build")
    aw.add_argument("--bucket", required=True)
    aw.add_argument("--reason", required=True)
    aw.add_argument("--dry-run", action="store_true")
    aw.add_argument("--out")
    q = sub.add_parser("probe")
    q.add_argument("--bucket", required=True)
    args = ap.parse_args(argv)
    if args.cmd == "publish":
        if args.dry_run and not args.out:
            ap.error("--dry-run needs --out DIR")
        try:
            return publish(args)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    if args.cmd in ("assets-publish", "assets-withdraw"):
        if args.dry_run and not args.out:
            ap.error("--dry-run needs --out DIR")
        try:
            return assets_publish(args) if args.cmd == "assets-publish" else assets_withdraw(args)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    if args.cmd == "withdraw":
        return withdraw(args)
    return probe(args)


if __name__ == "__main__":
    sys.exit(main())
