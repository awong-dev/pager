#!/usr/bin/env python3
"""A tiny stand-in for Twilio's Messages API -- pager v2 Phase 5,
docs/SERVER_PLAN.md §6.4/§8 scenario 7.

Exists so `relay/app/backends/sms_stub.py` (this phase's stub) and, later,
Phase 7's real `backends/sms_twilio.py` adapter can both be pointed at a
`TWILIO_BASE_URL` override and exercise a full outbound-send round trip
without a real Twilio account, a real phone number, or any network access.

**Endpoint shape chosen (documented per this phase's brief -- "your
choice"):** `POST /2010-04-01/Accounts/{AccountSid}/Messages.json`, the
*exact* path Twilio's own REST API uses for creating a message resource
(https://www.twilio.com/docs/sms/api/message-resource#create-a-message-
resource), accepting the same `application/x-www-form-urlencoded` body
(`To`, `From`, `Body`) Twilio's API expects and HTTP Basic auth (account
SID : auth token) exactly as Twilio's client would send it -- ignored here
(any credentials are accepted; this mock is not testing Twilio's own
authentication). Mirroring Twilio's real path/shape means a real
`sms_twilio.py` adapter built directly against the `twilio` Python SDK in
Phase 7 needs only to override the SDK's base URL (`TWILIO_BASE_URL`) to
point at this mock in tests/dev, with no change to its request-building
code -- exactly the property this phase's brief asks for. `POST /messages`
was considered (a shorter, made-up path) and rejected: it would work for
this phase's httpx-based stub but would force Phase 7's *real* adapter to
special-case test/dev mode in its request path, defeating the point of a
"drop-in" mock.

**Failure simulation (documented per this phase's brief -- "your choice"):**
`POST /_fail_next` (optional JSON body `{"times": <int>}`, default 1) makes
the next `<times>` calls to the Messages-create endpoint fail with a
500-ish, Twilio-shaped error body
(https://www.twilio.com/docs/api/errors) instead of succeeding, then reverts
to succeeding. `TWILIO_MOCK_FAIL_NEXT` (an env var, read once at process
start) seeds the same counter, for a docker-compose deployment that wants
every send to fail from the moment the container starts rather than issuing
an HTTP call first.

**Introspection:** `GET /_sent` returns every send recorded so far (oldest
first) as a JSON list of `{"sid", "to", "from", "body", "status",
"dateCreated"}` objects -- what `tools/e2e_v2.py`'s `fanout` scenario reads
to confirm the sms backend actually reached this mock. `POST /_reset` clears
both the recorded list and the fail-next counter (test isolation between
scenarios/test runs that reuse one long-lived mock container).

Run directly (`python3 tools/mocks/twilio_mock.py [--port 8010]`) or via
`uvicorn twilio_mock:app` (see `tools/mocks/Dockerfile`, wired into
`relay/docker-compose.yml` as the `twilio-mock` service).
"""

from __future__ import annotations

import argparse
import os
import time
import uuid
from dataclasses import dataclass, field
from typing import Any

from fastapi import FastAPI, Request, Response
from pydantic import BaseModel

app = FastAPI(title="Twilio Messages API mock (pager v2 Phase 5)")


@dataclass
class _State:
    sent: list[dict[str, Any]] = field(default_factory=list)
    fail_next: int = 0


_state = _State(fail_next=int(os.environ.get("TWILIO_MOCK_FAIL_NEXT", "0")))


class FailNextRequest(BaseModel):
    times: int = 1


@app.post("/2010-04-01/Accounts/{account_sid}/Messages.json")
async def create_message(account_sid: str, request: Request) -> Response:
    form = await request.form()
    to = form.get("To")
    from_ = form.get("From")
    body = form.get("Body")

    if _state.fail_next > 0:
        _state.fail_next -= 1
        # Shaped like a real Twilio REST error response
        # (https://www.twilio.com/docs/api/errors) -- an HTTP 500 with a
        # JSON body naming a `code`/`message`, not just a bare 500.
        return Response(
            content=(
                '{"code": 20500, "message": "Internal error (simulated by '
                'twilio_mock.py via /_fail_next)", '
                '"more_info": "https://www.twilio.com/docs/errors/20500", '
                '"status": 500}'
            ),
            status_code=500,
            media_type="application/json",
        )

    sid = "SM" + uuid.uuid4().hex
    record = {
        "sid": sid,
        "accountSid": account_sid,
        "to": to,
        "from": from_,
        "body": body,
        "status": "sent",
        "dateCreated": time.strftime("%a, %d %b %Y %H:%M:%S +0000", time.gmtime()),
    }
    _state.sent.append(record)
    return Response(content=_to_json(record), status_code=201, media_type="application/json")


@app.get("/_sent")
def list_sent() -> list[dict[str, Any]]:
    return _state.sent


@app.post("/_fail_next")
def fail_next(req: FailNextRequest | None = None) -> dict[str, int]:
    times = req.times if req is not None else 1
    _state.fail_next = max(_state.fail_next, 0) + times
    return {"failNext": _state.fail_next}


@app.post("/_reset")
def reset() -> dict[str, bool]:
    _state.sent.clear()
    _state.fail_next = 0
    return {"ok": True}


@app.get("/healthz")
def healthz() -> dict[str, bool]:
    return {"ok": True}


def _to_json(obj: dict[str, Any]) -> str:
    import json

    return json.dumps(obj)


def main() -> None:
    import uvicorn

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=int(os.environ.get("PORT", "8010")))
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
