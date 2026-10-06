"""docs/OTA_DESIGN.md D2/D10/§4/§5: the relay's view of the OTA firmware
bucket. `tools/fwpub.py` publishes `fw/index.json`; this module fetches it
(cached for a minute), picks the delta-or-full object for a device's running
image, and builds the `cfg.ota` sub-map. Pure apart from the one HTTP GET."""

from __future__ import annotations

import logging
import re
import threading
import time
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
_PATH_RE = re.compile(r"fw/[0-9a-f]{16}/(full\.z|from-[0-9a-f]{16}\.dz)")


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
            if d.base[:16] == device_img16:
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
