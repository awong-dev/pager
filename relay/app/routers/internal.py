"""`/internal/tick`, `/internal/sweep` -- docs/SERVER_PLAN.md §5.1, §5.8,
§9.2 ("Cloud Scheduler / Cloud Tasks; OIDC token").

`_require_internal_caller` is the one gate both routes share:

- `Settings.dev_mode` (unchanged local-dev behaviour, `DEV_MODE=1`): bypassed
  entirely, no bearer token needed -- the same "never true in a real
  deployment" carve-out `app/routers/dev.py`'s `POST /api/dev/token` uses,
  and `infra/modules/relay-service/main.tf` never sets `DEV_MODE` on the
  real Cloud Run service (see that module's own comment on why it is
  deliberately not even exposed as a Terraform variable).
- Otherwise: a real Google-signed OIDC ID token is now a **hard
  requirement**, not optional -- `app.auth.verify_internal_oidc_token`
  (reusing `app/backends/gchat.py`'s Google-issued-JWT verification
  primitives) checks signature, `aud` == `OIDC_AUDIENCE`, and `email` is one
  of `OIDC_ALLOWED_EMAILS`. A missing or invalid token is a 401.

**Config**: `OIDC_AUDIENCE` -- matching
`infra/modules/schedule/main.tf`'s `oidc_token { audience =
var.relay_service_url }`, this must be set to the Cloud Run service's own
URL (`infra/modules/relay-service`'s `service_url` output), **not**
`PUBLIC_BASE_URL` (the Hosting-fronted public domain `app/backends/
sms_twilio.py`/`app/backends/gchat.py` use for Twilio/Chat console
configuration) -- Cloud Scheduler's `http_target.uri` calls the Cloud Run
URL directly, bypassing Hosting's rewrites entirely, so that is the origin
its minted token's `aud` claim actually carries. `OIDC_ALLOWED_EMAILS` --
comma-separated list of caller service-account emails; today that's
`infra/modules/schedule`'s `google_service_account.scheduler`'s email
(`pager-scheduler@<project>.iam.gserviceaccount.com`, per that module's
`account_id = "pager-scheduler"`), with room to add a second entry once
`app/tasks.py`'s `TASKS_SERVICE_ACCOUNT_EMAIL` (Cloud Tasks' own caller
identity) is wired to actually call an `/internal/task` route (not yet
built -- see `app/tasks.py`'s module docstring's "known gap").

**Known gap**:
`infra/modules/relay-service/main.tf` does not set `OIDC_AUDIENCE` or
`OIDC_ALLOWED_EMAILS` as environment variables on the Cloud Run service --
only `infra/modules/schedule` provisions the caller identity and points its
own `oidc_token.audience` at `var.relay_service_url`. **Until that is wired,
every Scheduler invocation of these two routes 401s** (fail-closed, by
design) -- no tick retries and no retention sweep run in a real deployment.

Wiring it does **not** need a two-apply bootstrap. Feeding
the service's own computed `.uri` back into its own `env` block would indeed
be self-referential, but the audience need not be the run.app URL at all:
`google_cloud_run_v2_service` accepts `custom_audiences`, so one shared
Terraform variable (e.g. `"https://pager-relay"`) can be used for *both*
this service's `OIDC_AUDIENCE` env var and `infra/modules/schedule`'s
`oidc_token.audience`, resolving in a single apply. Likewise
`OIDC_ALLOWED_EMAILS` needs no module cycle: `schedule`'s `account_id` is
the literal `"pager-scheduler"`, so the email is deterministic
(`pager-scheduler@${project_id}.iam.gserviceaccount.com`) and can be
computed in `envs/prod` and passed into `relay-service` directly (cleanest:
hoist the service account into `envs/prod` and pass its email into both
modules). See `infra/modules/schedule/variables.tf`'s module docstring.
"""

from __future__ import annotations

import logging
import os

from fastapi import APIRouter, HTTPException, Request

from app import jobs
from app.auth import verify_internal_oidc_token
from app.routing import Routing

logger = logging.getLogger("relay.routers.internal")

router = APIRouter()


def _oidc_audience() -> str:
    return os.environ.get("OIDC_AUDIENCE", "")


def _oidc_allowed_emails() -> frozenset[str]:
    raw = os.environ.get("OIDC_ALLOWED_EMAILS", "")
    return frozenset(email.strip() for email in raw.split(",") if email.strip())


def _require_internal_caller(request: Request) -> None:
    settings = request.app.state.settings
    if settings.dev_mode:
        return
    auth_header = request.headers.get("authorization", "")
    if not auth_header.lower().startswith("bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    # `partition`, not `split(None, 1)[1]`: a header of exactly `"Bearer "`
    # (trailing space, no token) passes the `startswith` check above but
    # makes `split` return a 1-element list, i.e. an unhandled IndexError ->
    # 500 instead of 401 on a trivially attacker-supplied header.
    token = auth_header.partition(" ")[2].strip()
    if not token:
        raise HTTPException(status_code=401, detail="missing bearer token")
    try:
        verify_internal_oidc_token(
            token, audience=_oidc_audience(), allowed_emails=_oidc_allowed_emails()
        )
    except Exception as exc:  # noqa: BLE001 -- verify_internal_oidc_token's own contract: any failure -> 401
        logger.warning("internal OIDC verification rejected: %r", exc)
        raise HTTPException(status_code=401, detail="invalid OIDC token") from None


@router.post("/internal/tick")
def tick(request: Request) -> jobs.TickResult:
    _require_internal_caller(request)
    routing: Routing = request.app.state.routing
    return jobs.tick(routing)


@router.post("/internal/sweep")
def sweep(request: Request) -> jobs.SweepResult:
    _require_internal_caller(request)
    return jobs.sweep()
