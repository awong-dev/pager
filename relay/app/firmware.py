"""docs/OTA_DESIGN.md D2/D10/§4/§5: the relay's view of the OTA firmware
bucket. `tools/fwpub.py` publishes `fw/index.json`; this module fetches it
(cached for a minute), picks the delta-or-full object for a device's running
image, and builds the `cfg.ota` sub-map. Pure apart from the one HTTP GET."""

from __future__ import annotations

import hashlib
import io
import logging
import re
import threading
import time
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, Literal

import httpx
from pydantic import BaseModel, ConfigDict, field_validator

from app.config import Settings

logger = logging.getLogger("relay.firmware")

MAX_IMAGE_BYTES = 0x200000
INDEX_TTL_S = 60.0
INDEX_TIMEOUT_S = 10.0

_HEX64 = re.compile(r"[0-9a-f]{64}")
_PATH_RE = re.compile(
    r"fw/[0-9a-f]{16}/(full\.z|from-[0-9a-f]{16}\.dz)|fw-cache/[0-9a-f]{16}/from-[0-9a-f]{16}\.dz"
)


class FirmwareNotConfigured(Exception):
    """FW_INDEX_URL / FW_BUCKET_BASE unset (route -> 503)."""


class FirmwareIndexUnavailable(Exception):
    """The index could not be fetched or did not validate (route -> 502)."""


def _check_hex64(v: str) -> str:
    if not _HEX64.fullmatch(v):
        raise ValueError("must be 64 lowercase hex chars")
    return v


def _check_size(v: int) -> int:
    if not (0 < v <= MAX_IMAGE_BYTES):
        raise ValueError(f"size must be in 1..{MAX_IMAGE_BYTES}")
    return v


class FwObject(BaseModel):
    """One downloadable object: a bucket path plus the decoded size/hash."""

    model_config = ConfigDict(extra="ignore")

    path: str
    osz: int
    osha: str

    @field_validator("path")
    @classmethod
    def _path(cls, v: str) -> str:
        if not _PATH_RE.fullmatch(v):
            raise ValueError("bad object path")
        return v

    @field_validator("osz")
    @classmethod
    def _osz(cls, v: int) -> int:
        return _check_size(v)

    @field_validator("osha")
    @classmethod
    def _osha(cls, v: str) -> str:
        return _check_hex64(v)


class FwDelta(FwObject):
    base: str
    psz: int

    @field_validator("base")
    @classmethod
    def _base(cls, v: str) -> str:
        return _check_hex64(v)

    @field_validator("psz")
    @classmethod
    def _psz(cls, v: int) -> int:
        return _check_size(v)


class FwBuild(BaseModel):
    model_config = ConfigDict(extra="ignore")

    id: str
    version: str
    size: int
    published: int
    full: FwObject
    deltas: list[FwDelta] = []

    @field_validator("id")
    @classmethod
    def _id(cls, v: str) -> str:
        return _check_hex64(v)

    @field_validator("size")
    @classmethod
    def _size(cls, v: int) -> int:
        return _check_size(v)

    @property
    def id16(self) -> str:
        return self.id[:16]


class FirmwareIndex(BaseModel):
    model_config = ConfigDict(extra="ignore")

    v: Literal[1]
    builds: list[FwBuild]

    def find(self, id16: str) -> FwBuild | None:
        for b in self.builds:
            if b.id16 == id16:
                return b
        return None


@dataclass(frozen=True)
class Choice:
    kind: Literal["full", "delta"]
    build: FwBuild
    obj: FwObject | FwDelta
    # D12: the delta is a relay-generated `fw-cache/` object, not a published one.
    on_demand: bool = False


# Test seam: an `httpx` transport (e.g. `httpx.MockTransport`) used instead of
# the network.
_transport: httpx.BaseTransport | None = None
_cache_lock = threading.Lock()
_cache: tuple[float, str, FirmwareIndex] | None = None


def reset_cache() -> None:
    global _cache
    with _cache_lock:
        _cache = None


def load_index(settings: Settings) -> FirmwareIndex:
    """Fetches and validates `fw/index.json`, cached in-process for 60 s."""
    global _cache
    if not settings.fw_index_url or not settings.fw_bucket_base:
        raise FirmwareNotConfigured("OTA not configured")
    now = time.monotonic()
    with _cache_lock:
        if _cache and _cache[1] == settings.fw_index_url and now - _cache[0] < INDEX_TTL_S:
            return _cache[2]
    try:
        with httpx.Client(timeout=INDEX_TIMEOUT_S, transport=_transport) as client:
            resp = client.get(settings.fw_index_url)
            resp.raise_for_status()
            index = FirmwareIndex.model_validate_json(resp.content)
    except Exception as exc:
        logger.error("firmware index unavailable: %s", exc)
        raise FirmwareIndexUnavailable(str(exc)) from exc
    with _cache_lock:
        _cache = (now, settings.fw_index_url, index)
    return index


def choose(index: FirmwareIndex, target_id16: str, device_img16: str | None) -> Choice:
    """The delta whose `base` starts with the device's `img` (exact 16-char
    match), else the full object. Raises `KeyError` for an unknown target."""
    build = index.find(target_id16)
    if build is None:
        raise KeyError(target_id16)
    if device_img16:
        for d in build.deltas:
            # A delta from the target to itself is never valid.
            if d.base[:16] == device_img16 and d.base[:16] != build.id16:
                return Choice("delta", build, d)
    return Choice("full", build, build.full)


def build_cfg_ota(choice: Choice, settings: Settings) -> dict[str, Any]:
    """The `cfg.ota` JSON shape of docs/OTA_DESIGN.md §5."""
    if not settings.fw_bucket_base:
        raise FirmwareNotConfigured("OTA not configured")
    obj = choice.obj
    cfg: dict[str, Any] = {
        "img": choice.build.id,
        "isz": choice.build.size,
        "url": settings.fw_bucket_base + obj.path,
        "osz": obj.osz,
        "osha": obj.osha,
        "fmt": choice.kind,
    }
    if isinstance(obj, FwDelta):
        cfg["base"] = obj.base
        cfg["psz"] = obj.psz
    return cfg


def estimate_bytes(obj_osz: int) -> int:
    """OTA_DESIGN.md §4: object + 4.5 % TCP/TLS overhead + 7 KB (handshake and
    headers)."""
    return int(obj_osz * 1.045) + 7168


# ---- D12: on-demand deltas cached under `fw-cache/` ----

DELTA_BUDGET_S = 60.0
HEAD_TTL_S = 60.0

# Test seams: `_transport` (above) serves the public HEAD/GET side;
# `_storage_client_factory(settings)` returns a `google.cloud.storage.Client`
# look-alike for the one write.
_storage_client_factory: Callable[[Settings], Any] | None = None
_head_cache: dict[str, tuple[float, FwDelta | None]] = {}


def reset_delta_cache() -> None:
    with _cache_lock:
        _head_cache.clear()


def cache_path(target16: str, base16: str) -> str:
    return f"fw-cache/{target16}/from-{base16}.dz"


def bucket_name(bucket_base: str) -> str:
    """`https://storage.googleapis.com/<name>/` -> `<name>`."""
    m = re.fullmatch(r"https://storage\.googleapis\.com/([a-z0-9][a-z0-9._-]*)/?", bucket_base)
    if m is None:
        raise FirmwareNotConfigured("FW_BUCKET_BASE is not a storage.googleapis.com bucket URL")
    return m.group(1)


def _delta_from_meta(
    path: str, osz: int, meta: dict[str, str | None], base: FwBuild
) -> FwDelta | None:
    try:
        psz, osha, mbase = meta.get("psz"), meta.get("osha"), meta.get("base")
        if psz is None or osha is None or mbase is None or mbase != base.id:
            return None
        return FwDelta(path=path, osz=osz, osha=osha, base=mbase, psz=int(psz))
    except (ValueError, TypeError):
        return None


def _lookup_cached(
    settings: Settings, base: FwBuild, target: FwBuild, *, use_cache: bool = True
) -> FwDelta | None:
    """HEAD the public cache object; None = absent (or anything unexpected)."""
    assert settings.fw_bucket_base
    path = cache_path(target.id16, base.id16)
    now = time.monotonic()
    if use_cache:
        with _cache_lock:
            hit = _head_cache.get(path)
        if hit and now - hit[0] < HEAD_TTL_S:
            return hit[1]
    delta: FwDelta | None = None
    try:
        with httpx.Client(timeout=INDEX_TIMEOUT_S, transport=_transport) as client:
            resp = client.head(settings.fw_bucket_base + path)
        if resp.status_code == 200:
            h = resp.headers
            delta = _delta_from_meta(
                path,
                int(h.get("content-length", "")),
                {k: h.get("x-goog-meta-" + k) for k in ("psz", "osha", "base")},
                base,
            )
    except (httpx.HTTPError, ValueError) as exc:
        logger.warning("fw cache HEAD %s failed: %s", path, exc)
    with _cache_lock:
        _head_cache[path] = (now, delta)
    return delta


def cached_delta(index: FirmwareIndex, base16: str, target16: str, settings: Settings) -> FwDelta | None:
    """Preview side: the cached object if there is one (never generates)."""
    base, target = index.find(base16), index.find(target16)
    if base is None or target is None or base16 == target16 or not settings.fw_bucket_base:
        return None
    return _lookup_cached(settings, base, target)


def can_delta_on_demand(index: FirmwareIndex, base16: str | None, target16: str) -> bool:
    return bool(base16) and base16 != target16 and index.find(base16 or "") is not None


def _fetch_image(client: httpx.Client, settings: Settings, build: FwBuild) -> bytes:
    assert settings.fw_bucket_base
    resp = client.get(settings.fw_bucket_base + build.full.path)
    resp.raise_for_status()
    d = zlib.decompressobj()
    data = d.decompress(resp.content, MAX_IMAGE_BYTES + 1)
    if d.unconsumed_tail or not d.eof:
        raise ValueError(f"image {build.id16} is too large or truncated")
    # The image id is the SHA-256 esptool appends: the file's last 32 bytes,
    # which is the digest of everything before them.
    if (
        len(data) != build.size
        or data[-32:].hex() != build.id
        or hashlib.sha256(data[:-32]).hexdigest() != build.id
    ):
        raise ValueError(f"image {build.id16} does not match the index")
    return data


def _make_patch(base: bytes, new: bytes) -> bytes:
    import detools

    patch = io.BytesIO()
    detools.create_patch(
        io.BytesIO(base), io.BytesIO(new), patch, patch_type="sequential", compression="none"
    )
    raw = patch.getvalue()
    out = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(raw), out)
    if out.getvalue() != new:
        raise RuntimeError("delta does not round-trip")
    return raw


def _upload(settings: Settings, base: FwBuild, target: FwBuild, dz: bytes, psz: int) -> FwDelta:
    from google.api_core.exceptions import PreconditionFailed

    assert settings.fw_bucket_base
    path = cache_path(target.id16, base.id16)
    osha = hashlib.sha256(dz).hexdigest()
    if _storage_client_factory is not None:
        client = _storage_client_factory(settings)
    else:
        from google.cloud import storage

        client = storage.Client(project=settings.google_cloud_project)
    bucket = client.bucket(bucket_name(settings.fw_bucket_base))
    blob = bucket.blob(path)
    blob.metadata = {"psz": str(psz), "osha": osha, "base": base.id, "img": target.id}
    blob.cache_control = "no-store"
    try:
        blob.upload_from_string(dz, content_type="application/octet-stream", if_generation_match=0)
    except PreconditionFailed:
        # Another request won the race: use what it stored.
        existing = bucket.get_blob(path)
        if existing is None:
            raise
        found = _delta_from_meta(path, int(existing.size), dict(existing.metadata or {}), base)
        if found is None:
            raise
        return found
    return FwDelta(path=path, osz=len(dz), osha=osha, base=base.id, psz=psz)


def delta_on_demand(
    index: FirmwareIndex, base16: str, target16: str, settings: Settings
) -> FwDelta | None:
    """D12: the cached or freshly generated `base -> target` delta, or None
    (the caller pushes the full object). Never raises; capped at 60 s."""
    try:
        base, target = index.find(base16), index.find(target16)
        if base is None or target is None or base16 == target16 or not settings.fw_bucket_base:
            return None
        deadline = time.monotonic() + DELTA_BUDGET_S
        found = _lookup_cached(settings, base, target, use_cache=False)
        if found is not None:
            return found

        def left() -> float:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError("delta generation exceeded its budget")
            return rem

        with httpx.Client(timeout=min(left(), 20.0), transport=_transport) as client:
            base_img = _fetch_image(client, settings, base)
            left()
            new_img = _fetch_image(client, settings, target)
        left()
        raw = _make_patch(base_img, new_img)
        left()
        delta = _upload(settings, base, target, zlib.compress(raw, 9), len(raw))
        with _cache_lock:
            _head_cache[delta.path] = (time.monotonic(), delta)
        return delta
    except Exception as exc:  # noqa: BLE001 -- generation must never fail the push
        logger.warning("fw delta on demand failed base=%s target=%s err=%s", base16, target16, exc)
        return None
