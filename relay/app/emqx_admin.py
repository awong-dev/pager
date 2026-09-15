"""Pushes device/boot MQTT credentials and ACLs to EMQX's HTTP management
API (docs/DEVICE_TASKS.md S2.1, docs/DEVICE_PLAN.md §3.2 steps 2-3).

**Why this exists.** `docs/DEVICE_PLAN.md` §1 found that the broker's
password + ACL gate -- meant to be the *first* of two gates in front of a
device, the relay's own HMAC verification (`docs/PROTOCOL.md` §14) being the
second, end-to-end one -- was never actually provisioned: EMQX ran with
anonymous connections and an allow-all ACL. `relay/emqx/emqx.conf` turns on
EMQX's `built_in_database` authentication and authorization backends
(deny-by-default authorization -- see that file's own comments); this module
is what populates them, one device (or bootstrap credential) at a time,
through EMQX's REST API rather than the dashboard.

**Credential, not identity.** `username` here is the MQTT login name EMQX
authenticates (today, always the device's `device_id` -- see
`docs/PROTOCOL.md` §1's "MQTT client id (device) = device_id" -- but kept as
a separate parameter from `device_id` because the ACL topics are always
built from `device_id`, independent of what the login name happens to be).
`ensure_boot_user`'s login name is always `boot-{bid}` (`docs/DEVICE_PLAN.md`
§3.2 step 3); its ACL topics are built from `bid`, not a `device_id`, because
a booting device has no `device_id`-shaped identity yet.

**ACL, mirror image of `docs/PROTOCOL.md` §2 / §13.1.** A device credential
may *publish* `pager/{device_id}/up`, `/status`, `/loc` and *subscribe*
`pager/{device_id}/down`, nothing else -- enforced at the broker via
`built_in_database`'s per-user rule list (`PUT
.../authorization/sources/built_in_database/rules/users/{username}`, which
replaces that user's whole rule list, so this module never needs to read
the existing list before writing it). A boot credential's ACL is the
`docs/DEVICE_PLAN.md` §3.2 step 3 pair: publish `pager/boot/{bid}/up`,
subscribe `pager/boot/{bid}/down`, nothing else. `emqx.conf`'s
`no_match = deny` is what makes "nothing else" a real refusal rather than an
implicit allow -- this module does not need to write an explicit deny rule
for every other topic.

**Auth.** Every call reuses the exact HTTP Basic pair `app/broker.py`'s
`BrokerClient.publish` already sends to EMQX's REST API
(`settings.broker_api_key`/`broker_api_secret`) against
`settings.broker_api_url` (`.../api/v5`) -- confirmed by hand against a live
EMQX 5.8.0 that this API-key credential is accepted on the
`/authentication/...` and `/authorization/...` management paths with no
separate dashboard login, the same way it is on `/publish`. No new secret,
no new config surface.

**`BROKER_MANAGES_AUTH=0`.** Read directly from the environment (not
`app/config.Settings`, which this task's file list does not touch) so a
deployment that provisions the broker credential/ACL by some other means
(or not at all, e.g. a bare local `mosquitto_pub` smoke test) can flip every
method here into a no-op that returns `"manual"` -- never silently succeeds,
never raises.

**Idempotent, per `docs/DEVICE_TASKS.md`'s "Safe to re-run" convention
(`tools/emqx_setup.py`).** `ensure_device`/`ensure_boot_user` create the
authentication user if absent, update its password in place if the user
already exists (a `409 ALREADY_EXISTS` on create falls back to a `PUT`), and
always `PUT` the full ACL rule list (itself a replace, not an append)."""

from __future__ import annotations

import logging
import os
from typing import Any, Literal

import httpx

from app.config import Settings

logger = logging.getLogger("relay.emqx_admin")

REQUEST_TIMEOUT_S = 5.0

# EMQX 5 built_in_database authentication mechanism id, fixed by
# `relay/emqx/emqx.conf`'s `authentication[0]` block (mechanism =
# password_based, backend = built_in_database).
_AUTHN_MECHANISM = "password_based:built_in_database"

EmqxResult = Literal["ok", "manual", "error"]


class EmqxAdmin:
    """REST-only client for EMQX's authentication/authorization management
    API. Holds no socket and no background state, same shape as
    `app.broker.BrokerClient` -- safe to construct fresh per request."""

    def __init__(self, settings: Settings, *, manages_auth: bool | None = None) -> None:
        self._base_url = settings.broker_api_url.rstrip("/")
        self._auth = (
            (settings.broker_api_key, settings.broker_api_secret or "")
            if settings.broker_api_key
            else None
        )
        self._manages_auth = _broker_manages_auth() if manages_auth is None else manages_auth

    # -- public API -----------------------------------------------------

    def ensure_device(self, username: str, password: str, device_id: str) -> EmqxResult:
        """Creates/updates the device's real broker credential and pushes
        the mirror-image ACL of `docs/PROTOCOL.md` §2/§13.1: publish-only
        on `pager/{device_id}/up|status|loc`, subscribe-only on
        `pager/{device_id}/down`."""
        if not self._manages_auth:
            return "manual"
        return self._ensure_user_and_acl(username, password, _device_rules(device_id))

    def ensure_boot_user(self, bid: str, password: str) -> EmqxResult:
        """Creates/updates the bootstrap credential `boot-{bid}`
        (`docs/DEVICE_PLAN.md` §3.2 step 3): publish-only on
        `pager/boot/{bid}/up`, subscribe-only on `pager/boot/{bid}/down`."""
        if not self._manages_auth:
            return "manual"
        username = _boot_username(bid)
        return self._ensure_user_and_acl(username, password, _boot_rules(bid))

    def delete_user(self, username: str) -> EmqxResult:
        """Removes both the authentication user and its ACL rule list --
        used for revoke (any device credential) and for the bootstrap
        credential's own cleanup on `pager/boot/+/up`
        (`docs/DEVICE_PLAN.md` §3.2's "delete the bootstrap credential").
        A user/rule list that is already gone (404) counts as success:
        deleting an absent thing achieves the caller's goal."""
        if not self._manages_auth:
            return "manual"
        user_ok = self._delete(f"{self._authn_users_url()}/{username}")
        rule_ok = self._delete(f"{self._authz_users_url()}/{username}")
        return "ok" if user_ok and rule_ok else "error"

    # -- internals --------------------------------------------------------

    def _authn_users_url(self) -> str:
        return f"{self._base_url}/authentication/{_AUTHN_MECHANISM}/users"

    def _authz_users_url(self) -> str:
        return f"{self._base_url}/authorization/sources/built_in_database/rules/users"

    def _request(self, method: str, url: str, json_body: dict[str, Any] | None) -> httpx.Response | None:
        try:
            return httpx.request(
                method, url, json=json_body, auth=self._auth, timeout=REQUEST_TIMEOUT_S
            )
        except httpx.HTTPError:
            logger.warning("emqx_admin %s %s failed", method, url, exc_info=True)
            return None

    def _delete(self, url: str) -> bool:
        resp = self._request("DELETE", url, None)
        if resp is None:
            return False
        # 404 = already gone, which is the caller's desired end state.
        if resp.status_code == 404 or 200 <= resp.status_code < 300:
            return True
        logger.warning("emqx_admin DELETE %s rejected: %s %r", url, resp.status_code, resp.text[:200])
        return False

    def _ensure_user_and_acl(
        self, username: str, password: str, rules: list[dict[str, str]]
    ) -> EmqxResult:
        if not self._ensure_authn_user(username, password):
            return "error"
        if not self._put_acl(username, rules):
            return "error"
        return "ok"

    def _ensure_authn_user(self, username: str, password: str) -> bool:
        create = self._request(
            "POST",
            self._authn_users_url(),
            {"user_id": username, "password": password, "is_superuser": False},
        )
        if create is not None and create.status_code in (200, 201):
            return True
        if create is not None and create.status_code == 409:
            update = self._request(
                "PUT",
                f"{self._authn_users_url()}/{username}",
                {"password": password, "is_superuser": False},
            )
            if update is not None and 200 <= update.status_code < 300:
                return True
            logger.warning(
                "emqx_admin: update user %s failed: %s",
                username,
                update.status_code if update is not None else "request error",
            )
            return False
        logger.warning(
            "emqx_admin: create user %s failed: %s",
            username,
            create.status_code if create is not None else "request error",
        )
        return False

    def _put_acl(self, username: str, rules: list[dict[str, str]]) -> bool:
        resp = self._request(
            "PUT",
            f"{self._authz_users_url()}/{username}",
            {"username": username, "rules": rules},
        )
        if resp is not None and 200 <= resp.status_code < 300:
            return True
        logger.warning(
            "emqx_admin: ACL rule push for %s failed: %s",
            username,
            resp.status_code if resp is not None else "request error",
        )
        return False


def _broker_manages_auth() -> bool:
    # Default on: provisioning the broker's own auth/ACL is the point of
    # this module (docs/DEVICE_PLAN.md §3.2 steps 2-3). "0" opts out
    # explicitly for a deployment that manages EMQX auth by some other
    # means.
    return os.environ.get("BROKER_MANAGES_AUTH", "1") != "0"


def _boot_username(bid: str) -> str:
    return f"boot-{bid}"


def _device_rules(device_id: str) -> list[dict[str, str]]:
    return [
        {"topic": f"pager/{device_id}/up", "permission": "allow", "action": "publish"},
        {"topic": f"pager/{device_id}/status", "permission": "allow", "action": "publish"},
        {"topic": f"pager/{device_id}/loc", "permission": "allow", "action": "publish"},
        {"topic": f"pager/{device_id}/down", "permission": "allow", "action": "subscribe"},
    ]


def _boot_rules(bid: str) -> list[dict[str, str]]:
    return [
        {"topic": f"pager/boot/{bid}/up", "permission": "allow", "action": "publish"},
        {"topic": f"pager/boot/{bid}/down", "permission": "allow", "action": "subscribe"},
    ]
