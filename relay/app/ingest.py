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
import time
from typing import Any, Literal

from app import devauth, location, wire
from app.broker import BrokerClient
from app.routing import Routing
from app.store import device_secrets as device_secrets_store
from app.store import devices as devices_store
from app.store import messages as messages_store
from app.wire import (
    SYSTEM_ALIAS,
    LocEnvelope,
    StatusEnvelope,
    UpEnvelope,
    build_down_payload,
    resolve_ts,
)

logger = logging.getLogger("relay.ingest")

# PROTOCOL.md §4.2 case 3: the one exception to "never auto-reply on MQTT".
UNKNOWN_RECIPIENT_BODY = "unknown recipient"


def _device_id_from_topic(topic: str, expected_suffix: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "pager" or parts[2] != expected_suffix:
        return None
    return parts[1]


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
        try:
            env = UpEnvelope.model_validate(data)
        except Exception as exc:  # noqa: BLE001 -- pydantic.ValidationError, narrowly caught above
            wire.log_malformed(topic, payload, str(exc))
            return

        if env.is_ack:
            self._handle_ack(device_id, env)
        else:
            self._handle_up_message(device_id, device, env)

    def _handle_ack(self, device_id: str, env: UpEnvelope) -> None:
        assert env.ack is not None and env.ack in ("shown", "read")
        ack_ts = resolve_ts(env.ts)

        # A down message's wire `id` is the Firestore `messages/{id}` doc id
        # itself (see `app/backends/pager.py`'s module docstring).
        msg = messages_store.get_message(env.id)
        if msg is None:
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
        payload = build_down_payload(
            msg_id=msg_id, ts=int(time.time()), body=body, from_=SYSTEM_ALIAS
        )
        self._broker.publish(f"pager/{device_id}/down", payload, qos=1, retain=False)

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
        )

        if env.state != "online":
            # §5.3: bare offline (retained or LWT) -> mark offline, no
            # message state changes, no republish.
            return

        session_changed = previous_status.session != env.session
        offline_to_online = previous_status.state == "offline"
        if session_changed or offline_to_online:
            self._republish_unacked(device_id)

    def _republish_unacked(self, device_id: str) -> None:
        """PROTOCOL.md §5.3's online-edge re-publish, sourced from
        `pendingDeviceIds` (oldest first, capped at 10, both enforced by
        `messages_store.list_pending_for_device`).

        §5.3's selection rule also *excludes* `kind:"loc_req"` ("a location
        request that missed its window is worthless")."""
        for msg in messages_store.list_pending_for_device(device_id):
            if msg.kind == "loc_req":
                continue
            logger.info("re-publishing unacked message %s to device %s", msg.id, device_id)
            self._routing.redeliver_pager(msg, device_id)

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
