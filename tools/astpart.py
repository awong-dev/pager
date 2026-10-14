#!/usr/bin/env python3
"""Build a full-size `assets_ota` partition image (docs/ASSETS_OTA_DESIGN.md section 2).

  astpart.py build <assets.bin> -o <part.bin> [--size 0x200000] [--fv N]
             [--set-version N] [--bad-crc] [--corrupt-magic]

The image is the assets file (header version byte at offset 4 patched with --set-version),
0xFF padding, and the 64 B commit record alone in the last 4 KiB sector (rest 0xFF). Flash it
with `esptool.py write_flash 0x620000 <part.bin>`. Record layout (little-endian), shared with
firmware/main/astpart.c:
  0 "PGAC" | 4 u32 1 | 8 u32 len | 12 sha256[32] | 44 u8 fv | 45..59 zero | 60 u32 crc32(0..59)
"""
import argparse
import hashlib
import struct
import sys
import zlib

SECTOR = 4096
REC_SIZE = 64
MAGIC = b"PGAC"


def encode_rec(length: int, sha: bytes, fv: int) -> bytes:
    body = MAGIC + struct.pack("<II", 1, length) + sha + bytes([fv]) + b"\0" * 15
    assert len(body) == 60
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def decode_rec(rec: bytes, part_size: int):
    """Return (len, sha, fv) or None, with the same rules as astpart_rec_decode()."""
    if len(rec) < REC_SIZE or rec[:4] != MAGIC:
        return None
    ver, length = struct.unpack_from("<II", rec, 4)
    if ver != 1 or struct.unpack_from("<I", rec, 60)[0] != zlib.crc32(rec[:60]) & 0xFFFFFFFF:
        return None
    if not 16 <= length <= part_size - SECTOR:
        return None
    return length, bytes(rec[12:44]), rec[44]


def build(data: bytes, size: int = 0x200000, fv=None, set_version=None,
          bad_crc: bool = False, corrupt_magic: bool = False) -> bytes:
    if len(data) < 16:
        raise ValueError("assets file shorter than its 16-byte header")
    if len(data) > size - SECTOR:
        raise ValueError(f"assets file {len(data)} B exceeds {size - SECTOR} B")
    data = bytearray(data)
    if set_version is not None:
        data[4] = set_version
    if fv is None:
        fv = data[4]
    sha = hashlib.sha256(bytes(data)).digest()
    rec = bytearray(encode_rec(len(data), sha, fv))
    if bad_crc:
        rec[60] ^= 0xFF
    if corrupt_magic:
        rec[0] ^= 0xFF
    part = bytearray(b"\xff" * size)
    part[:len(data)] = data
    part[size - SECTOR:size - SECTOR + REC_SIZE] = rec
    return bytes(part)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("assets")
    b.add_argument("-o", "--out", required=True)
    b.add_argument("--size", type=lambda s: int(s, 0), default=0x200000)
    b.add_argument("--fv", type=lambda s: int(s, 0), default=None)
    b.add_argument("--set-version", type=lambda s: int(s, 0), default=None)
    b.add_argument("--bad-crc", action="store_true")
    b.add_argument("--corrupt-magic", action="store_true")
    a = ap.parse_args(argv)
    with open(a.assets, "rb") as f:
        data = f.read()
    try:
        part = build(data, a.size, a.fv, a.set_version, a.bad_crc, a.corrupt_magic)
    except ValueError as e:
        print(f"astpart: {e}", file=sys.stderr)
        return 1
    with open(a.out, "wb") as f:
        f.write(part)
    print(f"astpart: wrote {a.out} ({len(part)} B), image {len(data)} B, "
          f"sha256 {hashlib.sha256(part[:len(data)]).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
