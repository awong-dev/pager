"""Business logic for inbound device traffic, arriving over
`POST /webhooks/mqtt` (see app/routers/webhooks.py) rather than over a live
MQTT session the relay holds open: the broker's rule engine posts each
`pager/+/up`, `/status` and `/loc` message here (docs/SERVER_PLAN.md §4.8,
§5 `ingest.py`). The ack state machine, dedup, and the online-edge republish
rule are exactly docs/PROTOCOL.md §4/§5.

Every code path here assumes the uid-addressed model (docs/SERVER_PLAN.md §3:
`messages/{id}`, the allow-list, per-backend `deliveries`), reached through
`app.routing.Routing`. A `device_id` that is not a registered
`devices/{deviceId}` document (i.e. was never created through
`POST /api/admin/devices`) has nowhere to route to: its traffic is logged and
dropped, the same class of event as an unknown message id.

No injected `Store`: `app/store/*` talk to the one process-wide Firestore
client (`app/db/firestore.py`). `Ingest` takes a `BrokerClient` (for the
§4.2 `system` reply) and, since it needs to fan an up-message out through the
allow-list and backend adapters, an `app.routing.Routing` -- both stay
injectable for tests (`tests/fake_transport.FakeBrokerClient`, a fake/real
`Routing`).
"""

from __future__ import annotations

import hashlib
import logging
import re
import time
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, field_validator

from app import devauth, devcfg, devsetup, location, wire
from app.broker import BrokerClient
from app.routing import Routing
from app.store import contacts as contacts_store
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.store import sms as sms_store
from app.wire import (
    SYSTEM_ALIAS,
    LocEnvelope,
    SmsLogEnvelope,
    StatusEnvelope,
    UpEnvelope,
    resolve_ts,
    stage_name,
)

logger = logging.getLogger("relay.ingest")

# PROTOCOL.md §4.2 case 3: the one exception to "never auto-reply on MQTT".
UNKNOWN_RECIPIENT_BODY = "unknown recipient"

# S4.1: the one `system` down reply for a `contact_req` beyond the per-device
# pending cap (docs/PROTOCOL.md §3.2, docs/DEVICE_TASKS.md S4.1's exact
# wording).
TOO_MANY_PENDING_BODY = "too many pending requests"

def record_bad_sig(device_id: str) -> None:
    """docs/PROTOCOL.md §14.4's "count as `sigFailures`" + "more than 20
    failures in 10 minutes ... sets `devices/{d}.status.authAlarm`",
    factored out of `Ingest._verify_and_decode`'s bad-signature branch so
    `app/routers/device_book.py`'s §14.7 step 3 (a bad `X-Sig` on the book
    fetch) can raise exactly the same counters and alarm rather than
    duplicating this logic -- a bad MQTT envelope signature and a bad HTTPS
    request tag are the same class of event, just on two different
    surfaces. Callers still log their own "SECURITY bad-sig ..." line first
    (with the topic/endpoint and payload that differ between the two
    surfaces); this function only does the counting and the alarm."""
    device_secrets_store.bump_sig_failures(device_id)
    window_count = devices_store.record_sig_failure(device_id)
    if window_count >= devices_store.AUTH_ALARM_THRESHOLD:
        logger.error(
            "SECURITY authAlarm device=%s: %d bad signatures in the last %ds",
            device_id,
            window_count,
            devices_store.AUTH_ALARM_WINDOW_S,
        )
        devices_store.set_auth_alarm(device_id, True)


# docs/PROTOCOL.md §3.2/§3.1: `name` is 1-16 code points, <=48 UTF-8 bytes.
_CONTACT_NAME_MAX_CODEPOINTS = 16
_CONTACT_NAME_MAX_UTF8_BYTES = 48
# Same shape as app/backends/sms_twilio.py's `_E164_RE` (not imported from
# there to avoid a private cross-module reference: `+` then 7-15 digits,
# first digit 1-9).
_PHONE_E164_RE = re.compile(r"^\+[1-9]\d{6,14}$")


class ContactReqEnvelope(BaseModel):
    """A payload received on `pager/{device_id}/up` with `kind:"contact_req"`
    (docs/PROTOCOL.md §3.2, docs/DEVICE_PLAN.md §4.2). Deliberately modelled
    here rather than in `app/wire.py`: that module is outside this task's
    (S4.1) `Files` list, and `wire.UpEnvelope` already treats any non-null
    `kind` as an unrecognised-kind rejection (§3.4) -- `Ingest.handle_up`
    below checks `kind` on the raw decoded dict and routes to this model
    *before* ever calling `UpEnvelope.model_validate`, so that rejection path
    is never reached for a `contact_req`.

    **`ph` is overloaded** per docs/PROTOCOL.md §3.1's field table ("Phone
    number `+…` or alias reference") -- flagged as a documentation
    ambiguity in this task's report: §3.2's prose ("either `ph` (E.164 phone
    number) or neither `ph` nor `body`... for an alias reference") does not
    by itself explain how an alias reference would ever be carried on the
    wire, since there is no separate `alias` key in either §3.1's field
    table or §10's normative CBOR keymap. The field-table wording is taken
    as authoritative here: a `ph` value starting with `+` is a phone number,
    any other non-empty value is an alias reference -- distinguished by
    `phone`/`alias` below.
    """

    model_config = ConfigDict(extra="ignore")

    v: int = 1
    id: str
    ts: int
    name: str
    ph: str | None = None
    n: int | None = None

    @field_validator("id")
    @classmethod
    def _check_id(cls, value: str) -> str:
        if not wire.ID_RE.match(value):
            raise ValueError("invalid id format")
        return value

    @field_validator("ts")
    @classmethod
    def _check_ts(cls, value: int) -> int:
        wire.validate_ts(value)
        return value

    @field_validator("n")
    @classmethod
    def _check_n(cls, value: int | None) -> int | None:
        if value is not None and not (0 <= value < wire.N_MAX_EXCLUSIVE):
            raise ValueError("n out of range")
        return value

    @field_validator("name")
    @classmethod
    def _check_name(cls, value: str) -> str:
        if not value:
            raise ValueError("contact_req name must not be empty")
        if wire.CONTROL_CHAR_RE.search(value):
            raise ValueError("contact_req name contains control characters")
        if len(value) > _CONTACT_NAME_MAX_CODEPOINTS:
            raise ValueError("contact_req name exceeds 16 code points")
        if len(value.encode("utf-8")) > _CONTACT_NAME_MAX_UTF8_BYTES:
            raise ValueError("contact_req name exceeds 48 UTF-8 bytes")
        return value

    @field_validator("ph")
    @classmethod
    def _check_ph(cls, value: str | None) -> str | None:
        if value is None:
            return value
        if value.startswith("+"):
            if not _PHONE_E164_RE.match(value):
                raise ValueError("contact_req ph is not a valid E.164 phone number")
        elif not wire.is_valid_alias(value):
            raise ValueError("contact_req ph is not a valid E.164 phone number or alias")
        return value

    @property
    def phone(self) -> str | None:
        return self.ph if self.ph is not None and self.ph.startswith("+") else None

    @property
    def alias(self) -> str | None:
        return self.ph if self.ph is not None and not self.ph.startswith("+") else None


def _device_id_from_topic(topic: str, expected_suffix: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager" or parts[2] != expected_suffix:
        return None
    return parts[1]


def _bid_from_boot_up_topic(topic: str) -> str | None:
    """`pager/boot/{bid}/up` (docs/DEVICE_PLAN.md §3.2, docs/PROTOCOL.md §2)
    -- one segment longer than the `pager/{device_id}/{suffix}` shape
    `_device_id_from_topic` parses, since `boot` and `bid` are both static
    to this one namespace."""
    parts = topic.split("/")
    if len(parts) != 4 or parts[0] != "pager" or parts[1] != "boot" or parts[3] != "up":
        return None
    return parts[2]


class Ingest:
    """Owns the relay's reaction to device traffic. Called by the webhook
    router once per inbound event.

    Holds one piece of per-process state: `_secret_cache`, an in-process
    cache of `deviceSecrets/{d}` (docs/DEVICE_PLAN.md §2.6: "cache in-process;
    it changes only on rotate"). It caches only the static key material used
    for §14.3 signature verification -- the mutable replay-window counters
    (`upN`/`upBits`/`downN`) are never cached, and are read fresh from
    Firestore on every call by `app.store.device_secrets.accept_up_n`/
    `next_down_n`, so they can never go stale here."""

    def __init__(self, broker: BrokerClient, routing: Routing | None = None) -> None:
        self._broker = broker
        self._routing = routing if routing is not None else Routing(broker)
        self._secret_cache: dict[str, device_secrets_store.DeviceSecret] = {}

    def invalidate_secret_cache(self, device_id: str) -> None:
        """Called by the key-rotation endpoint once one exists (not yet --
        TODO(orchestrator): wire this into that task) so a freshly rotated
        `hmacKey` is picked up on the next envelope instead of the stale
        cached one."""
        self._secret_cache.pop(device_id, None)

    def _get_secret(self, device_id: str) -> device_secrets_store.DeviceSecret | None:
        cached = self._secret_cache.get(device_id)
        if cached is not None:
            return cached
        secret = device_secrets_store.get(device_id)
        if secret is not None:
            self._secret_cache[device_id] = secret
        return secret

    # ---- docs/PROTOCOL.md §14.4: verify before parse ----

    def _verify_and_decode(
        self,
        device: devices_store.Device | None,
        device_id: str,
        topic: str,
        payload: bytes,
        *,
        lwt_exception: bool = False,
    ) -> tuple[dict[str, Any], wire.EnvelopeEncoding] | None:
        """§14.4's order, applied uniformly to `/up`, `/status` and `/loc`:
        size check -> (if `device.authMode == "hmac"`) verify `sig` in
        constant time, log + count + drop on failure (still 2xx, per
        `app/routers/webhooks.py`) -> decode (CBOR or JSON) -> (if hmac)
        check `n` against the replay window, log + drop on replay ->
        caller applies its own further, kind-specific handling.

        Returns `None` if the caller should drop the envelope (already
        logged); otherwise the decoded envelope (JSON names) and which wire
        encoding it arrived in, so callers can record `devices/{d}.wire`
        (§2.4) for envelopes from a *registered* device.

        `lwt_exception=True` (handle_status only) is §14.6: an *unsigned*
        `/status` is still accepted, but only when it decodes to exactly
        `{v, state:"offline", session}` -- the broker-generated LWT, which
        cannot itself carry a signature.
        """
        if wire.is_oversize(payload):
            wire.log_malformed(topic, payload, "oversize")
            return None

        if device is None or device.authMode != "hmac":
            decoded = wire.decode_envelope_bytes(payload)
            if decoded is None:
                wire.log_malformed(topic, payload, "non-utf8/non-JSON/non-CBOR-object")
                return None
            data, encoding = decoded
            if device is not None:
                devices_store.set_wire(device_id, encoding)
            return data, encoding

        secret = self._get_secret(device_id)
        ok, unsigned = (
            devauth.verify(secret.hmacKey, topic, payload) if secret is not None else (False, b"")
        )
        if not ok:
            if lwt_exception:
                decoded = wire.decode_envelope_bytes(payload)
                if decoded is not None and wire.is_unsigned_lwt_shape(decoded[0]):
                    # §14.6: accepted despite the missing signature -- do
                    # NOT touch `devices/{d}.wire` or the replay window for
                    # this one exception (it carries no `n`).
                    return decoded
            logger.warning(
                "SECURITY bad-sig device=%s topic=%s first64=%r", device_id, topic, payload[:64]
            )
            if secret is not None:
                record_bad_sig(device_id)
            return None

        decoded = wire.decode_envelope_bytes(unsigned)
        if decoded is None:
            wire.log_malformed(topic, payload, "signed payload did not decode")
            return None
        data, encoding = decoded
        n = data.get("n")
        if not isinstance(n, int):
            wire.log_malformed(topic, payload, "hmac envelope missing/invalid n")
            return None
        if not device_secrets_store.accept_up_n(device_id, n):
            logger.warning("SECURITY replay device=%s topic=%s n=%s", device_id, topic, n)
            return None
        devices_store.set_wire(device_id, encoding)
        return data, encoding

    # ---- /up ----

    def handle_up(self, topic: str, payload: bytes) -> None:
        device_id = _device_id_from_topic(topic, "up")
        if device_id is None:
            logger.warning("up webhook for unrecognised topic %s dropped", topic)
            return
        device = devices_store.get_device(device_id)
        result = self._verify_and_decode(device, device_id, topic, payload)
        if result is None:
            return
        data, _encoding = result

        # S4.1: `kind:"contact_req"` is dispatched on the raw decoded dict,
        # *before* `UpEnvelope.model_validate` -- that model treats any
        # non-null `kind` as an unrecognised kind (§3.4) and would otherwise
        # reject every contact_req. See `ContactReqEnvelope`'s docstring for
        # why this lives here rather than in `app/wire.py`.
        if data.get("kind") == "contact_req":
            self._handle_contact_req(device_id, device, data, topic, payload)
            return

        # docs/V02_DESIGN.md §6/§7: `kind:"sms_log"` is dispatched the same
        # way, before `UpEnvelope.model_validate` -- see
        # `wire.SmsLogEnvelope`'s docstring for why (same reasoning as
        # `contact_req` above: `UpEnvelope` rejects any non-null `kind`).
        if data.get("kind") == "sms_log":
            self._handle_sms_log(device_id, device, data, topic, payload)
            return

        try:
            env = UpEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return

        if env.is_ack:
            self._handle_ack(device_id, env)
        else:
            self._handle_up_message(device_id, device, env)

    def _handle_contact_req(
        self,
        device_id: str,
        device: devices_store.Device | None,
        data: dict[str, Any],
        topic: str,
        payload: bytes,
    ) -> None:
        """docs/DEVICE_PLAN.md §4.2, docs/PROTOCOL.md §3.2: store a pending
        `contactRequests/{deviceId}_{id}` row. Same drop rules as any other
        up message for an unregistered/revoked device (`_handle_up_message`),
        checked here too since this path never reaches that function."""
        if device is None:
            logger.warning(
                "contact_req %s from unregistered device %s dropped",
                data.get("id"),
                device_id,
            )
            return
        if device.revokedAt is not None:
            logger.warning(
                "SECURITY contact_req %s from revoked device %s dropped",
                data.get("id"),
                device_id,
            )
            return

        try:
            env = ContactReqEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return

        try:
            request = contacts_store.create_request(
                device_id=device_id,
                owner_uid=device.ownerUid,
                req_id=env.id,
                name=env.name,
                phone=env.phone,
                alias=env.alias,
            )
        except contacts_store.TooManyPending:
            logger.info(
                "contact_req %s from device %s rejected: too many pending requests",
                env.id,
                device_id,
            )
            self._send_system_reply(device_id, TOO_MANY_PENDING_BODY, cause_id=env.id)
            return

        if request is None:
            logger.info(
                "contact_req %s from device %s is a no-op (already pending/approved)",
                env.id,
                device_id,
            )
            return

        logger.info(
            "contact_req %s from device %s stored as pending (key=%s)",
            env.id,
            device_id,
            request.key,
        )

    def _handle_sms_log(
        self,
        device_id: str,
        device: devices_store.Device | None,
        data: dict[str, Any],
        topic: str,
        payload: bytes,
    ) -> None:
        """docs/V02_DESIGN.md §6/§7: store one `devices/{deviceId}/smsLog/
        {logId}` row (`app/store/sms.py`). Not routed to anyone, not a
        thread entry -- this is the whole handling; there is no fan-out, no
        recipient resolution, nothing else to do. Same drop rules as
        `_handle_contact_req` for an unregistered/revoked device."""
        if device is None:
            logger.warning(
                "sms_log %s from unregistered device %s dropped", data.get("id"), device_id
            )
            return
        if device.revokedAt is not None:
            logger.warning(
                "SECURITY sms_log %s from revoked device %s dropped", data.get("id"), device_id
            )
            return

        try:
            env = SmsLogEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return

        created = sms_store.create_log(
            device_id,
            env.id,
            ts=resolve_ts(env.ts),
            sms_ts=env.sms_ts,
            dir_=env.dir,
            peer=env.peer,
            st=env.st,
            body=env.body,
        )
        # §6: "Log one INFO line per entry" -- logged whether this call
        # created the row or found it already there (a broker webhook
        # redelivery, §2's "the push is at-least-once"); the dedup itself is
        # `sms_store.create_log`'s job, silently a no-op on a repeat.
        logger.info(
            "sms_log %s device=%s dir=%s peer=%s st=%s%s",
            env.id,
            device_id,
            env.dir,
            env.peer,
            env.st,
            "" if created else " (duplicate, already stored)",
        )
        if env.st == "blocked":
            logger.warning("SECURITY sms-blocked device=%s peer=%s", device_id, env.peer)

    def _handle_ack(self, device_id: str, env: UpEnvelope) -> None:
        assert env.ack is not None and env.ack in ("shown", "read")
        ack_ts = resolve_ts(env.ts)

        # A down message's wire `id` is the Firestore `messages/{id}` doc id
        # itself (see `app/backends/pager.py`'s module docstring) -- *except*
        # for a `book`/`cfg` id (docs/DEVICE_TASKS.md S4.2), which is never a
        # `messages/{id}` document (`app/devcfg.py`'s module docstring: it
        # addresses a device, not a (sender, recipient) pair). So a `book`/
        # `cfg` ack is checked here, after the ordinary lookup misses,
        # before falling back to "truly unknown id".
        msg = messages_store.get_message(env.id)
        if msg is None:
            if env.ack == "shown" and devcfg.ack(device_id, env.id):
                return
            # §4.1 rule 3: unknown id -> log + drop, never create a row.
            logger.info("ack for unknown message id=%s from device=%s dropped", env.id, device_id)
            return
        self._handle_v2_ack(device_id, msg, env.ack, ack_ts)

    def _handle_v2_ack(
        self,
        device_id: str,
        msg: messages_store.Message,
        ack: Literal["shown", "read"],
        ack_ts: int,
    ) -> None:
        bid = messages_store.find_pager_delivery(msg, device_id)
        if bid is None:
            # §4.1 rule 4: this message has no pager delivery addressed to
            # *this* device -- either it was addressed to a different
            # device, or (very unlikely) some other backend's delivery id
            # happens to collide, either way not a message this device may
            # ack.
            logger.warning(
                "SECURITY wrong-device ack: pager/%s/up acked message %s with no matching "
                "pager delivery",
                device_id,
                msg.id,
            )
            return
        result = messages_store.apply_delivery_ack(msg.id, bid, device_id, ack, ack_ts)
        if result == "noop":
            logger.debug("idempotent ack '%s' for message %s backend %s", ack, msg.id, bid)

    def _handle_up_message(
        self, device_id: str, device: devices_store.Device | None, env: UpEnvelope
    ) -> None:
        if device is None:
            # No registered `devices/{device_id}` doc -- e.g. a device id
            # that was never created through `POST /api/admin/devices`.
            # There is nowhere to route it, so this is a drop + log, the
            # same security class as an unknown recipient.
            logger.warning("up message %s from unregistered device %s dropped", env.id, device_id)
            return
        if device.revokedAt is not None:
            # A revoked device is not allowed to inject anything.
            logger.warning(
                "SECURITY up message %s from revoked device %s dropped", env.id, device_id
            )
            return
        self._handle_v2_up_message(device, env)

    def _handle_v2_up_message(self, device: devices_store.Device, env: UpEnvelope) -> None:
        # `routing.send()` does the allow-list check, the transactional
        # dedup-by-wireId (which covers QoS-1 at-least-once redelivery), and
        # the fan-out to the recipient's enabled backends.
        result = self._routing.send(
            sender_uid=device.ownerUid,
            recipient_alias=env.to,
            kind="text",
            body=env.body,
            origin_backend_kind="pager",
            wire_id=env.id,
            device_id=device.id,
            ts=resolve_ts(env.ts),
        )
        if env.to is not None and result.rejected:
            # PROTOCOL.md §4.2 case 3: an explicit `to` naming an unknown or
            # disallowed recipient -> exactly one `system` down reply, never
            # stored as a message (it is not part of any conversation), rate-
            # limited to one per offending up message.
            self._send_system_reply(device.id, UNKNOWN_RECIPIENT_BODY, cause_id=env.id)

    def _send_system_reply(self, device_id: str, body: str, *, cause_id: str) -> None:
        # The reply's wire `id` is *derived from the offending up message's
        # id*, not freshly minted: §4.2 rate-limits this reply to one per
        # offending up message, and a QoS 1 redelivery of that up message is
        # the same offending message. A fresh id each time would re-render
        # and re-alert on the device (§4.1 rule 7's dedup ring keys on `id`),
        # turning one bad `to` into an alert per redelivery. Same shape as
        # `new_id("m_")` (`m_` + 8 hex), so §3.1/§3.3 are unaffected.
        msg_id = "m_" + hashlib.sha256(f"sys:{device_id}:{cause_id}".encode()).hexdigest()[:8]
        # §14.5: "every `/down` message published by the relay ... includes
        # `n` and `sig`" -- unconditional, no exception for this reply (only
        # the broker-generated LWT is exempt, §14.6). Goes through
        # `BrokerClient.publish_down` -- the one path that takes a fresh `n`
        # and signs per `devices/{d}.authMode` (docs/DEVICE_TASKS.md S1.4) --
        # rather than `wire.build_down_payload` + a direct `self._broker.
        # publish()`, which produced an unsigned envelope an `authMode:
        # "hmac"` device's own `app/devauth.py`-equivalent verification
        # would (correctly) drop. Found and fixed by T1.5 once its simulated
        # device started verifying `/down` signatures instead of decoding
        # raw JSON unconditionally.
        obj: dict[str, Any] = {
            "v": 1,
            "id": msg_id,
            "ts": int(time.time()),
            "from": SYSTEM_ALIAS,
            "body": body,
            "ack": None,
        }
        self._broker.publish_down(device_id, obj)

    # ---- /status ----

    def handle_status(self, topic: str, payload: bytes) -> None:
        device_id = _device_id_from_topic(topic, "status")
        if device_id is None:
            logger.warning("status webhook for unrecognised topic %s dropped", topic)
            return
        device = devices_store.get_device(device_id)
        if device is None:
            # No registered device -- nowhere to store status. (Still run
            # through §14.4/§14.6's verify-before-parse pipeline first would
            # be pointless with no authMode to check; drop here, same as
            # before this task.)
            logger.info("status for unregistered device %s dropped", device_id)
            return
        result = self._verify_and_decode(device, device_id, topic, payload, lwt_exception=True)
        if result is None:
            return
        data, _encoding = result
        try:
            env = StatusEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return

        # Crash diagnostics (this task, docs/PROTOCOL.md §5.1): one INFO
        # line per accepted status, so `gcloud logging read
        # 'textPayload:"status <device_id>"'` shows the reset reason, the
        # main-loop stage name (looked up via `stage_name`, falls back to
        # the raw int for an index this relay's table predates) and the
        # abnormal-reset count -- all `None` and printed as such on
        # firmware that predates these fields.
        logger.info(
            "status %s: state=%s link=%s rst=%s stage=%s abn=%s stallcmd=%s",
            device_id,
            env.state,
            env.link,
            env.rst,
            stage_name(env.stage),
            env.abn,
            env.stallcmd if env.stallcmd is not None else "-",
        )

        previous_status = device.status
        resolved_ts = resolve_ts(env.ts) if env.ts is not None else None
        devices_store.update_status(
            device_id,
            state=env.state,
            mode=env.mode,
            battMv=env.batt_mv,
            rssi=env.rssi,
            session=env.session,
            ts=resolved_ts,
            fw=env.fw,
            locPeriodS=env.loc_period_s,
            locMinS=env.loc_min_s,
            tls=env.tls,
            caFp=env.ca_fp,
            locBackoffS=env.loc_backoff_s,
            smsLost=env.sms_lost,
            link=env.link,
            xport=env.xport,
            rst=env.rst,
            stage=env.stage,
            abn=env.abn,
            bpull=env.bpull,
            stallcmd=env.stallcmd,
        )

        # docs/V02_DESIGN.md §4.3: "on a transition
        # into `broken`, log a security event." `previous_status.tls` is
        # `None` for a device that predates this field or has never
        # reported it, which is correctly *not* "broken" already.
        if env.tls == "broken" and previous_status.tls != "broken":
            logger.error("SECURITY tls-broken device=%s", device_id)

        if env.state != "online":
            # §5.3: bare offline (retained or LWT) -> mark offline, no
            # message state changes, no republish.
            return

        # docs/DEVICE_PLAN.md §3.2 "Relay, on pager/boot/+/up" /
        # docs/DEVICE_TASKS.md S2b.3: "`provisionState` becomes `provisioned`
        # when the first signed `/status` from [the device] arrives." Only
        # `authMode: "hmac"` devices ever have a *signed* `/status` at all --
        # and reaching this point with `env.state == "online"` for such a
        # device is only possible via `_verify_and_decode`'s real signature
        # verification, never its `lwt_exception` carve-out (that one is
        # shaped exactly `state:"offline"`, so it can never produce
        # `"online"` here) -- so no separate "was this signed" flag is
        # needed. A `password`-mode (pre-bootstrap) device never reaches
        # "provisioned" through this path, same as before this task.
        if device.authMode == "hmac" and device.provisionState != "provisioned":
            devices_store.set_provision_state(device_id, "provisioned")

        # §5.3's online-edge republish runs *before* the bv-triggered push
        # below, not after: on a device's very first status ever (or any
        # edge that lands in the same request as a bv-triggered push), doing
        # it in this order means the freshly-pushed book is the one and only
        # publish for this device this request, rather than being published
        # once by `push_book` and then immediately again by the republish
        # (which would otherwise resend the very book `push_book` just set
        # as `pendingBook`) -- both would be harmless on the wire (same id,
        # device dedup, §4.1 rule 7), but this ordering avoids the wasted
        # publish.
        # docs/V02_DESIGN.md §9.5: a silent modem-initiated MQTT session
        # resume within one boot changes `link`, not `session` (that stays
        # the cold-boot id), but leaves the same ≤10 s subscription gap a
        # cold boot does -- so a changed `link` triggers the same republish.
        # Absent-vs-present never counts as "changed": that would fire on
        # every device's very first `link`-carrying `/status` (previously
        # `None`) and on any firmware that never sends it at all.
        link_changed = (
            previous_status.link is not None
            and env.link is not None
            and previous_status.link != env.link
        )
        session_changed = previous_status.session != env.session or link_changed
        offline_to_online = previous_status.state == "offline"
        if session_changed or offline_to_online:
            self._republish_unacked(device_id)

        # docs/PROTOCOL.md §5.3/§3.7 (v0.4): "On every online `/status`: if
        # `bookVersion` is 0, bump it to 1 in a transaction that writes only
        # while it is still 0, and publish. Else if the reported `bv` is
        # lower than `bookVersion`, re-publish the pending nudge when its
        # `bv` equals `bookVersion` (same `id`), otherwise build and publish
        # a new one." Bootstrap is checked first and unconditionally (a
        # fresh device's very first status has `reported_bv` absent or 0,
        # which the plain `<` comparison below would also have caught in
        # the pre-v0.4 code -- `bv 0 < bookVersion 0` never fired -- so
        # bootstrap is the only path that has ever delivered a first book).
        # `StatusEnvelope` (app/wire.py) does not declare `bv` -- wire.py was
        # outside S4.2's `Files` list when this dispatch style was chosen --
        # so it is read from the raw decoded dict directly, the same
        # "dispatch on the raw dict before/around the pydantic model" style
        # `data.get("kind")` already uses above for `contact_req`.
        reported_bv = data.get("bv")
        if contacts_store.bootstrap_book_version(device_id):
            logger.info("status bv=%s bootstraps devices/%s.bookVersion to 1", reported_bv, device_id)
            devcfg.push_book(device_id, self._broker)
        elif isinstance(reported_bv, int) and reported_bv < devcfg.get_book_version(device_id):
            logger.info(
                "status bv=%s behind devices/%s.bookVersion -- re-nudging/re-pushing book",
                reported_bv,
                device_id,
            )
            devcfg.renudge_if_behind(device_id, reported_bv, self._broker)

    def _republish_unacked(self, device_id: str) -> None:
        """PROTOCOL.md §5.3's online-edge re-publish, sourced from
        `pendingDeviceIds` (oldest first, capped at 10, both enforced by
        `messages_store.list_pending_for_device`), plus (docs/DEVICE_TASKS.md
        S4.2) the newest unacked `book`/`cfg`, one each, uncapped against the
        10-message limit above (docs/PROTOCOL.md §5.3: "not counted against
        this cap since they are not thread entries").

        §5.3's selection rule also *excludes* `kind:"loc_req"` ("a location
        request that missed its window is worthless")."""
        for msg in messages_store.list_pending_for_device(device_id):
            if msg.kind == "loc_req":
                continue
            logger.info("re-publishing unacked message %s to device %s", msg.id, device_id)
            self._routing.redeliver_pager(msg, device_id)
        devcfg.republish_pending(device_id, self._broker)

    # ---- /loc ----

    def handle_loc(self, topic: str, payload: bytes) -> None:
        """Validate per docs/PROTOCOL.md §13.2, then hand off to
        `app.location.ingest_loc` for the real handling (dedup,
        `devices/{d}/locations`, `loc_req` fulfilment -- SERVER_PLAN.md
        §5.6). No `Routing` needed here: unlike `_handle_v2_up_message`,
        `ingest_loc` never originates a new delivery -- it only resolves an
        *existing* `loc_req`'s delivery to `fulfilled`, which is a plain
        Firestore transaction, not a broker publish."""
        device_id = _device_id_from_topic(topic, "loc")
        if device_id is None:
            logger.warning("loc webhook for unrecognised topic %s dropped", topic)
            return
        device = devices_store.get_device(device_id)
        result = self._verify_and_decode(device, device_id, topic, payload)
        if result is None:
            return
        data, _encoding = result
        try:
            env = LocEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return
        location.ingest_loc(device_id, env)

    # ---- pager/boot/{bid}/up ----

    def handle_boot_ack(self, topic: str, payload: bytes) -> None:
        """docs/DEVICE_PLAN.md §3.2 "Relay, on `pager/boot/+/up`"
        (docs/DEVICE_TASKS.md S2b.3): the device's bootstrap ack, the CBOR
        equivalent of `{"v":1,"ok":1}` (`app/wirecbor.KEYMAP`'s `v`=0,
        `ok`=29). This namespace has no `deviceSecrets` row to verify
        against -- the bootstrap MQTT credential (`boot-{bid}`/`bpw`, ACL'd
        by the broker to publish only `pager/boot/{bid}/up`) is itself the
        authentication, so unlike `/up`/`/status`/`/loc` there is no
        `_verify_and_decode` step here, just a decode.

        A payload that fails to decode, or does not carry exactly
        `{v:1, ok:1}`, is dropped and logged (§3.4's malformed-payload
        rule) -- `devsetup.complete` is only called once the ack itself has
        been recognised, not on every touch of this topic."""
        bid = _bid_from_boot_up_topic(topic)
        if bid is None:
            logger.warning("boot-ack webhook for unrecognised topic %s dropped", topic)
            return
        decoded = wire.decode_envelope_bytes(payload)
        if decoded is None:
            wire.log_malformed(topic, payload, "boot ack non-utf8/non-JSON/non-CBOR-object")
            return
        data, _encoding = decoded
        if data.get("v") != 1 or data.get("ok") != 1:
            wire.log_malformed(topic, payload, "boot ack is not {v:1, ok:1}")
            return
        logger.info("boot ack received for bid=%s -- completing bootstrap", bid)
        devsetup.complete(bid, broker=self._broker)
