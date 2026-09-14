"""`/internal/tick`, `/internal/sweep` -- docs/SERVER_PLAN.md §5.1, §5.8,
§9.2 ("Cloud Scheduler / Cloud Tasks; OIDC token").

**No OIDC verification yet** -- that is explicitly Phase 8 hardening
(`HANDOFF_V2.md` §5's Phase 8 checklist: "`/internal/*` OIDC verification").
Gated on `Settings.dev_mode` instead, the same "invisible (404), not just
403, when disabled" pattern `app/routers/dev.py` already uses for
`POST /api/dev/token` -- a real deployment never sets `DEV_MODE=1`, so these
routes simply do not exist there until Phase 8 adds real auth to them.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Request

from app import jobs
from app.routing import Routing

router = APIRouter()


def _require_dev_mode(request: Request) -> None:
    settings = request.app.state.settings
    if not settings.dev_mode:
        raise HTTPException(status_code=404, detail="not found")


@router.post("/internal/tick")
def tick(request: Request) -> jobs.TickResult:
    _require_dev_mode(request)
    routing: Routing = request.app.state.routing
    return jobs.tick(routing)


@router.post("/internal/sweep")
def sweep(request: Request) -> dict[str, bool]:
    _require_dev_mode(request)
    # Real retention logic (docs/SERVER_PLAN.md §5.7: count+unit settings,
    # BulkWriter deletes of messages/wireIds and locations, resumable) is
    # Phase 5/8 -- this only wires the route's shape so Cloud Scheduler and
    # `tools/pager_client.py`'s `sweep` command have something to call in
    # the meantime.
    return {"ok": True}
