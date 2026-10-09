#!/usr/bin/env python3
"""A stand-in for the bridge phone -- docs/BRIDGE_PHONE_DESIGN.md decision 14.

Speaks the exact `/bridge/*` contract the Android app does (pair, events,
outbox poll, ack, heartbeat) so the relay's bridge path can be exercised
end to end without a phone. Two things live here:

* `BridgeSim` -- the phone's side: pairs with a code, polls the outbox (with
  `wait`, which the real phone does not use, O2), records each item and acks
  it `sent` (tier 1) unless a failure is armed, heartbeats, and forwards
  injected events. Standard library only (`urllib`), so `tools/e2e_v2.py` can
  import it, or drive a running sim through `BridgeSimClient`.
* A small control-plane HTTP server (FastAPI, imported lazily) on `:8020`:
  `POST /_pair {code, simNumber?, voiceNumber?}`, `POST /_inject {event}`
  (or `{events: [...]}`), `GET /_outbox`, `POST /_fail_next {times}`,
  `POST /_reset`, `GET /_status`. The compose service `bridge-sim` runs it
  (`tools/mocks/bridge_sim.Dockerfile`).

Environment: `RELAY_URL` (default `http://relay:8000`), `BRIDGE_SIM_PAIR_CODE`
(pair on start), `BRIDGE_SIM_SIM_NUMBER` (default `+15550007777`),
`BRIDGE_SIM_VOICE_NUMBER` (default none), `BRIDGE_SIM_HEARTBEAT_S` (30),
`BRIDGE_SIM_POLL_WAIT_S` (5), `BRIDGE_SIM_WHATSAPP` (default on; `0` pairs
without `caps.whatsapp`).

WhatsApp (WA1-WA5): the sim reports `caps.whatsapp` at pair and
`status.whatsapp` on every heartbeat; `whatsapp_dm_event()` /
`whatsapp_group_event()` build JID-shaped events (`<digits>@s.whatsapp.net`,
`<id>@g.us`) for `inject`. `POST /_pair` accepts `whatsapp: false` to pair
without the cap.
"""


import argparse
import json
import os
import threading
import time
import urllib.error
import urllib.request
from typing import Any

DEFAULT_SIM_NUMBER = "+15550007777"


def whatsapp_dm_event(
    phone: str, text: str, *, name: str = "Contact", event_id: str | None = None
) -> dict[str, Any]:
    """A WhatsApp DM as the phone reports it: the conversation id is the
    shortcut-id JID, the phone is already extracted from it (WA2)."""
    event: dict[str, Any] = {
        "source": "whatsapp",
        "conversation": {"id": f"{phone.lstrip('+')}@s.whatsapp.net", "isGroup": False},
        "sender": {"name": name, "phone": phone},
        "text": text,
    }
    if event_id:
        event["id"] = event_id
    return event


def whatsapp_group_event(
    group_id: str, title: str, sender: str, text: str, *, event_id: str | None = None
) -> dict[str, Any]:
    """A WhatsApp group message: id = `<group_id>@g.us`, sender as shown
    (possibly `~ Name`, WA3)."""
    event: dict[str, Any] = {
        "source": "whatsapp",
        "conversation": {"id": f"{group_id}@g.us", "title": title, "isGroup": True},
        "sender": {"name": sender},
        "text": text,
    }
    if event_id:
        event["id"] = event_id
    return event
_UNSET: Any = object()


class RelayError(Exception):
    def __init__(self, status: int, body: Any) -> None:
        super().__init__(f"relay returned {status}: {body}")
        self.status = status
        self.body = body


def _request(
    method: str,
    url: str,
    *,
    body: Any = None,
    token: str | None = None,
    timeout: float = 30.0,
) -> tuple[int, Any]:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            parsed = json.loads(raw) if raw else None
        except ValueError:
            parsed = raw.decode(errors="replace")
        return exc.code, parsed


class BridgeSim:
    """The phone. `start()` runs the poll and heartbeat loops in daemon threads."""

    def __init__(
        self,
        relay_url: str,
        *,
        sim_number: str | None = DEFAULT_SIM_NUMBER,
        voice_number: str | None = None,
        heartbeat_s: float = 30.0,
        poll_wait_s: int = 5,
        whatsapp: bool = True,
    ) -> None:
        self.relay_url = relay_url.rstrip("/")
        self.whatsapp = whatsapp
        self.sim_number = sim_number
        self.voice_number = voice_number
        self.heartbeat_s = heartbeat_s
        self.poll_wait_s = poll_wait_s
        self.token: str | None = None
        self.bridge_id: str | None = None
        self.outbox: list[dict[str, Any]] = []
        self.fail_next = 0
        self.heartbeats = 0
        self.last_error: str | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._counter = 0

    # ---- the contract ----

    def status_body(self) -> dict[str, Any]:
        return {
            "battery": 90,
            "listenerBound": True,
            "smsDefault": True,
            "accessibility": True,
            "whatsapp": self.whatsapp,
            "accounts": ["kid@example.com"],
            "simNumber": self.sim_number,
            "voiceNumber": self.voice_number,
            "version": "sim-1",
        }

    def pair(
        self,
        code: str,
        *,
        sim_number: Any = _UNSET,
        voice_number: Any = _UNSET,
        whatsapp: Any = _UNSET,
    ) -> dict[str, Any]:
        if whatsapp is not _UNSET:
            self.whatsapp = bool(whatsapp)
        if sim_number is not _UNSET:
            self.sim_number = sim_number
        if voice_number is not _UNSET:
            self.voice_number = voice_number
        body: dict[str, Any] = {
            "code": code,
            "version": "sim-1",
            "accounts": ["kid@example.com"],
            "caps": {"sms": True, "gchat": True, "gvoice": True, "whatsapp": self.whatsapp},
        }
        if self.sim_number:
            body["simNumber"] = self.sim_number
        if self.voice_number:
            body["voiceNumber"] = self.voice_number
        status, out = _request("POST", f"{self.relay_url}/bridge/pair", body=body)
        if status != 200:
            raise RelayError(status, out)
        with self._lock:
            self.token = out["token"]
            self.bridge_id = out["bridgeId"]
            self.last_error = None
        return out

    def heartbeat(self) -> dict[str, Any] | None:
        if not self.token:
            return None
        status, out = _request(
            "POST",
            f"{self.relay_url}/bridge/heartbeat",
            body={"status": self.status_body()},
            token=self.token,
        )
        if status == 200:
            self.heartbeats += 1
            return out
        if status == 401:
            self.token = None
        raise RelayError(status, out)

    def events(self, events: list[dict[str, Any]]) -> dict[str, Any]:
        if not self.token:
            raise RuntimeError("not paired")
        status, out = _request(
            "POST", f"{self.relay_url}/bridge/events", body={"events": events}, token=self.token
        )
        if status != 200:
            raise RelayError(status, out)
        return out

    def inject(self, event: dict[str, Any]) -> dict[str, Any]:
        """One event, with a generated `id` if it has none."""
        event = dict(event)
        if "id" not in event:
            self._counter += 1
            event["id"] = f"sim{int(time.time())}x{self._counter}"
        event.setdefault("ts", int(time.time()))
        return self.events([event])

    def poll_outbox(self, wait: int = 0) -> list[dict[str, Any]]:
        if not self.token:
            return []
        status, out = _request(
            "GET",
            f"{self.relay_url}/bridge/outbox?wait={wait}",
            token=self.token,
            timeout=wait + 15,
        )
        if status != 200:
            raise RelayError(status, out)
        return out["items"]

    def ack(self, ob_id: str, state: str, reason: str | None = None, tier: int = 1) -> None:
        status, out = _request(
            "POST",
            f"{self.relay_url}/bridge/outbox/{ob_id}/ack",
            body={"state": state, "reason": reason, "tier": tier},
            token=self.token,
        )
        if status != 204:
            raise RelayError(status, out)

    def handle_outbox_once(self, wait: int = 0) -> int:
        """Polls once, records and acks every item. Returns how many."""
        items = self.poll_outbox(wait)
        for item in items:
            with self._lock:
                fail = self.fail_next > 0
                if fail:
                    self.fail_next -= 1
            state, reason = ("failed", "sim_failed") if fail else ("sent", None)
            record = {**item, "ackState": state, "reason": reason, "tier": 1, "at": time.time()}
            with self._lock:
                self.outbox.append(record)
            self.ack(item["id"], state, reason, 1)
        return len(items)

    # ---- loops ----

    def start(self) -> None:
        self._stop.clear()
        for target in (self._poll_loop, self._heartbeat_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self._threads.append(thread)

    def stop(self) -> None:
        self._stop.set()

    def _poll_loop(self) -> None:
        while not self._stop.is_set():
            if not self.token:
                self._stop.wait(0.5)
                continue
            try:
                self.handle_outbox_once(self.poll_wait_s)
            except Exception as exc:  # noqa: BLE001 -- a sim keeps going
                self.last_error = str(exc)
                self._stop.wait(1.0)

    def _heartbeat_loop(self) -> None:
        while not self._stop.is_set():
            if self.token:
                try:
                    self.heartbeat()
                except Exception as exc:  # noqa: BLE001
                    self.last_error = str(exc)
            self._stop.wait(self.heartbeat_s)

    def reset(self) -> None:
        with self._lock:
            self.outbox.clear()
            self.fail_next = 0
            self.token = None
            self.bridge_id = None
            self.heartbeats = 0
            self.last_error = None


class BridgeSimClient:
    """Drives a running sim's control plane (what `tools/e2e_v2.py` uses)."""

    def __init__(self, base_url: str = "http://localhost:8020") -> None:
        self.base_url = base_url.rstrip("/")

    def _call(self, method: str, path: str, body: Any = None) -> Any:
        status, out = _request(method, f"{self.base_url}{path}", body=body, timeout=60)
        if status >= 400:
            raise RelayError(status, out)
        return out

    def pair(self, code: str, **numbers: Any) -> dict[str, Any]:
        """`simNumber=None` / `voiceNumber=None` mean "none"; omit to keep the defaults."""
        return self._call("POST", "/_pair", {"code": code, **numbers})

    def inject(self, event: dict[str, Any]) -> dict[str, Any]:
        return self._call("POST", "/_inject", {"event": event})

    def whatsapp_dm(self, phone: str, text: str, **kw: Any) -> dict[str, Any]:
        return self.inject(whatsapp_dm_event(phone, text, **kw))

    def whatsapp_group(
        self, group_id: str, title: str, sender: str, text: str, **kw: Any
    ) -> dict[str, Any]:
        return self.inject(whatsapp_group_event(group_id, title, sender, text, **kw))

    def outbox(self) -> list[dict[str, Any]]:
        return self._call("GET", "/_outbox")

    def fail_next(self, times: int = 1) -> None:
        self._call("POST", "/_fail_next", {"times": times})

    def status(self) -> dict[str, Any]:
        return self._call("GET", "/_status")

    def reset(self) -> None:
        self._call("POST", "/_reset", {})


def create_app(sim: BridgeSim):
    from fastapi import FastAPI, HTTPException, Request

    app = FastAPI(title="Bridge phone simulator")

    async def _json(request: Request) -> dict[str, Any]:
        try:
            body = await request.json()
        except ValueError:
            return {}
        return body if isinstance(body, dict) else {}

    @app.post("/_pair")
    async def pair(request: Request) -> dict[str, Any]:
        body = await _json(request)
        kwargs: dict[str, Any] = {}
        if "simNumber" in body:
            kwargs["sim_number"] = body["simNumber"]
        if "voiceNumber" in body:
            kwargs["voice_number"] = body["voiceNumber"]
        if "whatsapp" in body:
            kwargs["whatsapp"] = body["whatsapp"]
        try:
            out = sim.pair(str(body.get("code", "")), **kwargs)
        except RelayError as exc:
            raise HTTPException(status_code=exc.status, detail=exc.body) from exc
        try:
            sim.heartbeat()
        except RelayError:
            pass
        return {"bridgeId": out["bridgeId"]}

    @app.post("/_inject")
    async def inject(request: Request) -> dict[str, Any]:
        body = await _json(request)
        try:
            if "events" in body:
                return sim.events(body["events"])
            return sim.inject(body.get("event") or {})
        except RelayError as exc:
            raise HTTPException(status_code=exc.status, detail=exc.body) from exc
        except RuntimeError as exc:
            raise HTTPException(status_code=409, detail=str(exc)) from exc

    @app.get("/_outbox")
    def outbox() -> list[dict[str, Any]]:
        with sim._lock:
            return list(sim.outbox)

    @app.post("/_fail_next")
    async def fail_next(request: Request) -> dict[str, int]:
        body = await _json(request)
        with sim._lock:
            sim.fail_next = int(body.get("times", 1))
        return {"failNext": sim.fail_next}

    @app.post("/_reset")
    def reset() -> dict[str, bool]:
        sim.reset()
        return {"ok": True}

    @app.get("/_status")
    def status() -> dict[str, Any]:
        return {
            "paired": sim.token is not None,
            "bridgeId": sim.bridge_id,
            "simNumber": sim.sim_number,
            "voiceNumber": sim.voice_number,
            "whatsapp": sim.whatsapp,
            "heartbeats": sim.heartbeats,
            "lastError": sim.last_error,
        }

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", type=int, default=8020)
    parser.add_argument("--host", default="0.0.0.0")  # noqa: S104 -- container-local test double
    args = parser.parse_args()

    import uvicorn

    sim = BridgeSim(
        os.environ.get("RELAY_URL", "http://relay:8000"),
        sim_number=os.environ.get("BRIDGE_SIM_SIM_NUMBER", DEFAULT_SIM_NUMBER) or None,
        voice_number=os.environ.get("BRIDGE_SIM_VOICE_NUMBER") or None,
        heartbeat_s=float(os.environ.get("BRIDGE_SIM_HEARTBEAT_S", "30")),
        poll_wait_s=int(os.environ.get("BRIDGE_SIM_POLL_WAIT_S", "5")),
        whatsapp=os.environ.get("BRIDGE_SIM_WHATSAPP", "1") != "0",
    )
    sim.start()
    code = os.environ.get("BRIDGE_SIM_PAIR_CODE")
    if code:
        try:
            sim.pair(code)
        except RelayError as exc:
            print(f"bridge-sim: pairing on start failed: {exc}", flush=True)
    uvicorn.run(create_app(sim), host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
