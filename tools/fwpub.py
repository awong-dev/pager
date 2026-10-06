#!/usr/bin/env python3
"""
`tools/fwpub.py` -- publish a firmware release to the public OTA bucket
(docs/OTA_DESIGN.md D1/D2/D11).

  fwpub.py publish <release-app.bin> --bucket <name> [--base <bin>]... [--k 3]
                   [--allow-large] [--dry-run --out <dir>]
  fwpub.py probe --bucket <name>

Image id = the SHA-256 esptool appends to every app .bin (last 32 bytes);
id16 = its first 16 hex chars. Objects (content-addressed, immutable):
  fw/<id16>/full.z               zlib(bin, level 9, 32 KB window)
  fw/<id16>/from-<base16>.dz     zlib(detools sequential patch, no compression)
  fw/<id16>/manifest.json        this build's index entry
and fw/index.json (no-store), newest build first. Needs `pip install detools`
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
    return {"v": 1, "builds": builds}


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
    return probe(args)


if __name__ == "__main__":
    sys.exit(main())
