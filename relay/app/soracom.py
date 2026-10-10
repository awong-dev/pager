"""Soracom API client for Beam enrollment (docs/SORACOM_DESIGN.md §5, §8).

Shared by `tools/soracom_beam.py` (CLI) and the admin routes. Auth is one
`POST /auth` per `SoracomClient`; the key id, key, API key and token are never
logged, returned or put in exception text (`SoracomError.detail` is a status
code or an exception class name only). Tests drive it through the
module-level `_transport` seam (an `httpx.MockTransport`).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Self

import httpx
from pydantic import BaseModel

BASE_URL = "https://g.api.soracom.io/v1"
BEAM_KEY = "mqtt://beam.soracom.io:1883"
DEFAULT_DESTINATION = "mqtts://s1289801.ala.us-east-1.emqxsl.com:8883"
GROUP_ID_PLACEHOLDER = "{group_id}"
TIMEOUT_S = 30.0
PAGE_LIMIT = 100
MAX_PAGES = 10
NEXT_KEY_HEADER = "x-soracom-next-key"

# Test seam: an `httpx` transport used instead of the network.
_transport: httpx.BaseTransport | None = None


class SoracomError(Exception):
    """A Soracom API failure. `detail` is `HTTP <status>` or an exception class
    name -- never a response body, URL or credential."""

    def __init__(self, detail: str) -> None:
        super().__init__(detail)
        self.detail = detail


@dataclass(frozen=True)
class Call:
    method: str
    path: str
    json: Any = None
    params: dict[str, str] | None = None
    note: str = ""


class SimRow(BaseModel):
    """One `/subscribers` row; every field optional (public-spec shapes, unverified live).
    The MSISDN is deliberately not modelled."""

    imsi: str | None = None
    iccid: str | None = None
    status: str | None = None
    groupId: str | None = None
    name: str | None = None
    subscription: str | None = None


def beam_entry(destination: str) -> list[dict[str, Any]]:
    return [
        {
            "key": BEAM_KEY,
            "value": {
                "name": "pager-emqx",
                "destination": destination,
                "enabled": True,
                "version": "201912",
                "useClientCredentials": True,
                "useClientCert": False,
                "addSubscriberHeader": False,
            },
        }
    ]


def build_auth_call(key_id: str, key: str) -> Call:
    return Call("POST", "/auth", {"authKeyId": key_id, "authKey": key}, note="authenticate")


def build_calls(group: str, imsi: str, destination: str, group_id: str | None = None) -> list[Call]:
    """The calls after /auth. With `group_id` unknown, the lookup/create calls come first and
    later paths carry the `{group_id}` placeholder the executor fills in."""
    gid = group_id or GROUP_ID_PLACEHOLDER
    calls: list[Call] = []
    if group_id is None:
        calls.append(
            Call("GET", "/groups", params={"tag_name": "name", "tag_value": group, "limit": "10"},
                 note="find group by name tag")
        )
        calls.append(Call("POST", "/groups", {"tags": {"name": group}},
                          note="create group only if the lookup found none"))
    calls.append(Call("PUT", f"/groups/{gid}/configuration/SoracomBeam", beam_entry(destination),
                      note="MQTT entry point, credential pass-through"))
    calls.append(Call("POST", f"/subscribers/{imsi}/set_group", {"groupId": gid},
                      note="move the SIM into the group"))
    calls.append(Call("GET", f"/groups/{gid}", note="print the resulting configuration"))
    return calls


def mask_imsi(imsi: str) -> str:
    return "..." + imsi[-4:]


class SoracomClient:
    """`httpx.Client` wrapper: authenticates once, lazily, on the first request."""

    def __init__(self, key_id: str, key: str, *, base_url: str = BASE_URL) -> None:
        self._key_id = key_id
        self._key = key
        # g.api.soracom.io publishes AAAA records and Cloud Run has no IPv6
        # egress: left to itself httpx tries v6 first and sits in the connect
        # timeout (30 s) on every call before falling back (a 3-call listing
        # took 92 s live, 10 Oct 2026). Binding the local side to 0.0.0.0
        # forces IPv4.
        transport = _transport or httpx.HTTPTransport(local_address="0.0.0.0")
        self._http = httpx.Client(base_url=base_url, timeout=TIMEOUT_S, transport=transport)
        self._headers: dict[str, str] | None = None

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        self._http.close()

    def _redact(self, text: str) -> str:
        for secret in (self._key_id, self._key):
            if secret:
                text = text.replace(secret, "<redacted>")
        return text

    def _send(self, method: str, path: str, **kw: Any) -> httpx.Response:
        try:
            resp = self._http.request(method, path, **kw)
        except httpx.HTTPError as e:
            raise SoracomError(self._redact(type(e).__name__)) from None
        if resp.status_code >= 300:
            raise SoracomError(f"HTTP {resp.status_code}")
        return resp

    def _auth(self) -> dict[str, str]:
        if self._headers is None:
            call = build_auth_call(self._key_id, self._key)
            resp = self._send(call.method, call.path, json=call.json)
            try:
                body = resp.json()
                self._headers = {
                    "X-Soracom-API-Key": body["apiKey"],
                    "X-Soracom-Token": body["token"],
                }
            except (ValueError, KeyError, TypeError):
                raise SoracomError("bad auth response") from None
        return self._headers

    def request(self, call: Call) -> httpx.Response:
        return self._send(call.method, call.path, json=call.json, params=call.params,
                          headers=self._auth())

    def send(self, call: Call) -> Any:
        resp = self.request(call)
        if not resp.content:
            return None
        try:
            return resp.json()
        except ValueError:
            raise SoracomError("bad response") from None


def list_sims(client: SoracomClient) -> list[SimRow]:
    rows: list[SimRow] = []
    key: str | None = None
    for _ in range(MAX_PAGES):
        params = {"limit": str(PAGE_LIMIT)}
        if key:
            params["last_evaluated_key"] = key
        resp = client.request(Call("GET", "/subscribers", params=params))
        try:
            data = resp.json()
        except ValueError:
            raise SoracomError("bad response") from None
        for item in data if isinstance(data, list) else []:
            if not isinstance(item, dict):
                continue
            tags = item.get("tags")
            name = tags.get("name") if isinstance(tags, dict) else None
            sub = item.get("subscription") or item.get("plan")
            rows.append(
                SimRow(
                    imsi=_s(item.get("imsi")),
                    iccid=_s(item.get("iccid")),
                    status=_s(item.get("status")),
                    groupId=_s(item.get("groupId")),
                    name=_s(name),
                    subscription=_s(sub),
                )
            )
        key = resp.headers.get(NEXT_KEY_HEADER)
        if not key:
            break
    return rows


def _s(v: object) -> str | None:
    return None if v is None else str(v)


def find_group(client: SoracomClient, group_name: str) -> str | None:
    lookup = build_calls(group_name, "0", "")[0]
    found = client.send(lookup) or []
    for g in found:
        if isinstance(g, dict) and (g.get("tags") or {}).get("name") == group_name:
            return str(g["groupId"])
    return None


def ensure_group_created(
    client: SoracomClient, group_name: str, destination: str
) -> tuple[str, bool]:
    """(group id, created): find or create the group, then PUT the Beam config."""
    gid = find_group(client, group_name)
    created = gid is None
    if gid is None:
        create = build_calls(group_name, "0", "")[1]
        body = client.send(create)
        try:
            gid = str(body["groupId"])
        except (KeyError, TypeError):
            raise SoracomError("bad response") from None
    put = build_calls(group_name, "0", destination, group_id=gid)[0]
    client.send(put)
    return gid, created


def ensure_group(client: SoracomClient, group_name: str, destination: str) -> str:
    return ensure_group_created(client, group_name, destination)[0]


def enroll(client: SoracomClient, imsi: str, group_id: str) -> None:
    """Move the SIM into the group (idempotent)."""
    client.send(Call("POST", f"/subscribers/{imsi}/set_group", {"groupId": group_id}))
