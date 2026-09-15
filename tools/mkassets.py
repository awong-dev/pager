#!/usr/bin/env python3
"""mkassets.py — rasterises Noto Sans + Noto Sans CJK into the `assets`
partition image gfx.c mmaps at boot.

Authority: docs/DEVICE_PLAN.md §5.2 ("Text and layout primitives"), §9
(decisions: Noto Sans + Noto Sans CJK, default language `sc`). This file is
the only *encoder* of the binary format; gfx.c (firmware/main/gfx.h's header
comment has the authoritative byte-for-byte layout, reproduced in
_HEADER_LAYOUT_NOTE below for anyone reading this file first) is the only
decoder — the two must be kept in lock-step by hand, there is no shared
schema file.

Requires FreeType via `pip install freetype-py`. Rendering is monochrome,
hinted (FT_LOAD_TARGET_MONO), matching gfx.c's 1-bit blit.

Font sources: this script takes real Noto Sans / Noto Sans CJK font files as
input (--sans-font / --cjk-font); it does not fetch, embed, or fabricate any
font data itself. See the docstring in the repo's F6.1 task notes for which
files were used to exercise this script in an offline sandbox (a real Noto
Sans + Noto Sans CJK pair, found already installed on the build host).

CJK repertoire note (docs/DEVICE_PLAN.md §5.2 names four legacy standards —
GB2312, Big5, JIS X 0208, KS X 1001 — as the exact repertoire per language).
This script does NOT embed those legacy code tables (not available in this
environment without network access to fetch them); instead it selects the
first N codepoints of the relevant modern Unicode block(s) that the chosen
CJK font actually has a glyph for, where N matches the character count
DEVICE_PLAN.md §5.2 cites for that language. This is a documented
approximation of the repertoire, not the authentic legacy mapping — swap
_cjk_ranges()/_CJK_TARGET_COUNT below for a real GB2312/Big5/JIS/KS table
when one is available, no other part of the pipeline needs to change.
"""
import argparse
import struct
import sys
from pathlib import Path

import freetype

MAGIC = b"PGFA"
VERSION = 1
SIZES = (12, 16)

# (start, end, font) inclusive codepoint ranges shared by every language.
# 'sans' = Noto Sans (Latin/Greek/Cyrillic/general punctuation), 'cjk' =
# Noto Sans CJK's regional face (CJK punctuation, fullwidth forms — Noto
# Sans itself does not cover these).
_COMMON_RANGES = (
    (0x0020, 0x007E, "sans"),  # Basic Latin (printable)
    (0x00A0, 0x00FF, "sans"),  # Latin-1 Supplement
    (0x0100, 0x017F, "sans"),  # Latin Extended-A
    (0x0370, 0x03FF, "sans"),  # Greek and Coptic
    (0x0400, 0x04FF, "sans"),  # Cyrillic
    (0x2000, 0x206F, "sans"),  # General Punctuation
    (0x3000, 0x303F, "cjk"),   # CJK Symbols and Punctuation
    (0xFF00, 0xFFEF, "cjk"),   # Halfwidth and Fullwidth Forms
)

# Per-language CJK repertoire ranges (see the module docstring's caveat) and
# the target glyph count DEVICE_PLAN.md §5.2 cites for that language.
_CJK_RANGES = {
    "sc": ((0x4E00, 0x9FFF),),  # stand-in for GB2312's 6763 hanzi
    "tc": ((0x4E00, 0x9FFF),),  # stand-in for Big5's 5401 hanzi
    "jp": ((0x3040, 0x309F), (0x30A0, 0x30FF), (0x4E00, 0x9FFF)),  # kana + stand-in for JIS X 0208
    "kr": ((0xAC00, 0xD7A3), (0x4E00, 0x9FFF)),  # Hangul syllables + stand-in for KS X 1001 hanja
}
_CJK_TARGET_COUNT = {"sc": 6763, "tc": 5401, "jp": 6879, "kr": 7238}

_CJK_FACE_NAME_HINT = {"sc": "SC", "tc": "TC", "jp": "JP", "kr": "KR"}


def _open_cjk_face(path, lang, face_index):
    """Opens `path` as a FreeType face. If `face_index` is None and `path`
    is a font collection (.ttc/.otc), auto-picks the Regular-weight face
    whose family name mentions the language's CJK region tag (see
    _CJK_FACE_NAME_HINT) — this is how a single Noto Sans CJK .ttc (which
    ships all four/five regional variants as separate faces) is turned into
    "Noto Sans CJK {sc|tc|jp|kr}" per docs/DEVICE_PLAN.md §5.2."""
    if face_index is not None:
        return freetype.Face(str(path), face_index)

    probe = freetype.Face(str(path), 0)
    num_faces = probe.num_faces
    if num_faces <= 1:
        return probe

    hint = _CJK_FACE_NAME_HINT[lang]
    best = None
    for i in range(num_faces):
        f = freetype.Face(str(path), i)
        family = f.family_name.decode("utf-8", "replace") if f.family_name else ""
        style = f.style_name.decode("utf-8", "replace") if f.style_name else ""
        if hint in family and style == "Regular" and "Mono" not in family:
            best = f
            break
    if best is None:
        raise SystemExit(
            f"mkassets: could not find a '{hint}' Regular face in font collection "
            f"{path} ({num_faces} faces) — pass --cjk-face-index explicitly"
        )
    return best


def _glyph_coverage(face, cp):
    return face.get_char_index(cp) != 0


def _collect_codepoints(lang, sans_face, cjk_face):
    """Returns an ordered list of (codepoint, font) pairs, font in
    {'sans','cjk'}, deduplicated and finally sorted by codepoint (gfx.c's
    binary search requires strictly ascending, unique codepoints — if a
    codepoint were reachable from both fonts we keep the first one seen,
    'sans' before 'cjk' per _COMMON_RANGES' order)."""
    chosen = {}  # codepoint -> font
    for start, end, font in _COMMON_RANGES:
        face = sans_face if font == "sans" else cjk_face
        for cp in range(start, end + 1):
            if cp in chosen:
                continue
            if _glyph_coverage(face, cp):
                chosen[cp] = font

    target = _CJK_TARGET_COUNT[lang]
    cjk_count = sum(1 for f in chosen.values() if f == "cjk")
    for start, end in _CJK_RANGES[lang]:
        if cjk_count >= target:
            break
        for cp in range(start, end + 1):
            if cjk_count >= target:
                break
            if cp in chosen:
                continue
            if _glyph_coverage(cjk_face, cp):
                chosen[cp] = "cjk"
                cjk_count += 1

    return sorted(chosen.items())


def _render_glyph(face, size_px, cp):
    """Renders codepoint `cp` at `size_px` with FreeType, monochrome,
    hinted (FT_LOAD_TARGET_MONO). Returns a dict with the fields gfx.c's
    glyph record needs, plus 'bitmap' (tightly packed rows, ceil(w/8)
    bytes/row, MSB-first, 1=ink) already re-packed from FreeType's own
    (possibly differently padded/oriented) buffer."""
    face.set_pixel_sizes(0, size_px)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    face.load_char(cp, flags)
    g = face.glyph
    bmp = g.bitmap
    w = bmp.width
    rows = bmp.rows
    adv = round(g.advance.x / 64)
    bearing_x = g.bitmap_left
    bearing_y = g.bitmap_top

    row_bytes = (w + 7) // 8
    packed = bytearray(row_bytes * rows)
    pitch = bmp.pitch
    src_row_bytes = abs(pitch)
    buf = bytes(bmp.buffer)
    for r in range(rows):
        src_row_index = r if pitch >= 0 else (rows - 1 - r)
        src_off = src_row_index * src_row_bytes
        src_row = buf[src_off:src_off + src_row_bytes]
        packed[r * row_bytes:(r + 1) * row_bytes] = src_row[:row_bytes].ljust(row_bytes, b"\x00")

    for v, name in ((w, "w"), (adv, "adv"), (rows, "rows")):
        if not (0 <= v <= 255):
            raise SystemExit(f"mkassets: U+{cp:04X} {name}={v} does not fit in a byte")
    for v, name in ((bearing_x, "bearing_x"), (bearing_y, "bearing_y")):
        if not (-128 <= v <= 127):
            raise SystemExit(f"mkassets: U+{cp:04X} {name}={v} does not fit in a signed byte")

    return {
        "w": w,
        "adv": adv,
        "bearing_x": bearing_x,
        "bearing_y": bearing_y,
        "rows": rows,
        "bitmap": bytes(packed),
    }


def _build_size_block(size_px, entries, sans_face, cjk_face):
    """entries: sorted [(codepoint, font), ...]. Returns the block's bytes
    (sub-header + codepoint table + record table + bitmap blob), per
    gfx.h's format doc."""
    sans_face.set_pixel_sizes(0, size_px)
    baseline = round(sans_face.size.ascender / 64)
    baseline = max(0, min(255, baseline))

    codepoints_bytes = bytearray()
    records_bytes = bytearray()
    bitmap_blob = bytearray()

    for cp, font in entries:
        face = sans_face if font == "sans" else cjk_face
        g = _render_glyph(face, size_px, cp)
        codepoints_bytes += struct.pack("<I", cp)
        offset = len(bitmap_blob)
        records_bytes += struct.pack(
            "<BBbbBBI",
            g["w"],
            g["adv"],
            g["bearing_x"],
            g["bearing_y"],
            g["rows"],
            0,
            offset,
        )
        bitmap_blob += g["bitmap"]

    sub_header_len = 12
    block_bytes = sub_header_len + len(codepoints_bytes) + len(records_bytes) + len(bitmap_blob)
    sub_header = struct.pack("<BBHII", size_px, baseline, 0, len(entries), block_bytes)
    return bytes(sub_header) + bytes(codepoints_bytes) + bytes(records_bytes) + bytes(bitmap_blob)


def build_assets(lang, sans_font, cjk_font, cjk_face_index):
    sans_face = freetype.Face(str(sans_font))
    cjk_face = _open_cjk_face(cjk_font, lang, cjk_face_index)

    entries = _collect_codepoints(lang, sans_face, cjk_face)
    cjk_count = sum(1 for _, f in entries if f == "cjk")
    sans_count = len(entries) - cjk_count
    print(
        f"mkassets: lang={lang} codepoints={len(entries)} (sans={sans_count} cjk={cjk_count}, "
        f"target cjk={_CJK_TARGET_COUNT[lang]})",
        file=sys.stderr,
    )

    blocks = [_build_size_block(size_px, entries, sans_face, cjk_face) for size_px in SIZES]

    header_len = 16
    total = header_len + sum(len(b) for b in blocks)
    lang_bytes = lang.encode("ascii")[:3].ljust(3, b"\x00")
    header = struct.pack("<4sB3sIB3s", MAGIC, VERSION, lang_bytes, total, len(SIZES), b"\x00\x00\x00")
    return header + b"".join(blocks)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lang", choices=sorted(_CJK_RANGES), default="sc",
                     help="CJK repertoire (default: sc, docs/DEVICE_PLAN.md §9)")
    ap.add_argument("-o", "--out", default="build/assets.bin", help="output assets.bin path")
    ap.add_argument("--sans-font", required=True, help="Noto Sans (or substitute) font file")
    ap.add_argument("--cjk-font", required=True,
                     help="Noto Sans CJK font file (a .ttc collection, or a single-face font "
                          "already specific to one region)")
    ap.add_argument("--cjk-face-index", type=int, default=None,
                     help="face index within --cjk-font; default auto-selects by --lang "
                          "(see _open_cjk_face)")
    args = ap.parse_args()

    data = build_assets(args.lang, Path(args.sans_font), Path(args.cjk_font), args.cjk_face_index)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)

    print(f"mkassets: wrote {out_path} ({len(data)} bytes, {len(data) / 1024:.1f} KiB)")
    if len(data) >= 1024 * 1024:
        print("mkassets: WARNING — exceeds the 1 MiB `assets` partition", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
