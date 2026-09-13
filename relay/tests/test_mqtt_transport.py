"""Regression tests for `app.mqtt_transport.PahoTransport`'s paho v2 callback
adapters.

`tests/fake_transport.py` drives `MqttGateway` callbacks directly with plain
Python ints (see `FakeTransport.simulate_disconnect`), so it never exercises
`PahoTransport._handle_connect` / `_handle_disconnect` themselves and is
blind to bugs in how those adapters unwrap paho's real callback arguments.
paho-mqtt 2.x's VERSION2 callback API passes a `paho.mqtt.reasoncodes.
ReasonCode` object (not a plain int) for connect/disconnect reason codes;
`ReasonCode` has no `__int__`, so code that does `int(reason_code)` raises
`TypeError` -- uncaught, since this fires inside paho's own callback
dispatch on the `loop_start()` background thread, silently killing it and
leaving the relay permanently disconnected from the broker.

These tests build a real `ReasonCode` (not a mock that happens to support
`int()`) and drive it straight through `PahoTransport`'s private handler
methods, without opening a socket.
"""

from __future__ import annotations

from paho.mqtt.packettypes import PacketTypes
from paho.mqtt.reasoncodes import ReasonCode

from app.mqtt_transport import PahoTransport


def _make_transport() -> PahoTransport:
    # No `connect()`/`start()` call -- constructing the client and wiring up
    # the on_connect/on_disconnect/on_message/on_publish handlers doesn't
    # touch the network, so this is safe to run without a broker.
    return PahoTransport(client_id="test-relay", host="localhost", port=1883)


def test_handle_disconnect_with_real_reason_code_object_does_not_raise() -> None:
    """A real `ReasonCode` (as paho 2.x's VERSION2 API actually passes) must
    not blow up `_handle_disconnect` the way `int(reason_code)` did."""
    transport = _make_transport()
    received: list[int] = []
    transport.set_handlers(
        on_connect=lambda session_present: None,
        on_disconnect=received.append,
        on_message=lambda topic, payload: None,
        on_publish=lambda mid: None,
    )

    # "Normal disconnection" (value 0) -- what a clean disconnect() sends.
    normal_rc = ReasonCode(PacketTypes.DISCONNECT, aName="Success", identifier=0)
    assert not hasattr(normal_rc, "__int__")
    # Sanity check this is really the failure mode from production, not a
    # weaker stand-in: `int()` must fail on this object exactly as it did on
    # the real ReasonCode from a live disconnect.
    try:
        int(normal_rc)
    except TypeError:
        pass
    else:
        raise AssertionError(
            "ReasonCode now supports int() -- this test's premise is stale, "
            "update it to reflect the new paho behaviour"
        )

    transport._handle_disconnect(None, None, None, normal_rc, None)
    assert received == [0]

    # A non-zero / abnormal reason code (unexpected disconnect, e.g. broker
    # restart or network blip) must also come through as a plain int.
    abnormal_rc = ReasonCode(
        PacketTypes.DISCONNECT, aName="Unspecified error", identifier=128
    )
    transport._handle_disconnect(None, None, None, abnormal_rc, None)
    assert received == [0, 128]


def test_handle_disconnect_with_none_reason_code_defaults_to_zero() -> None:
    transport = _make_transport()
    received: list[int] = []
    transport.set_handlers(
        on_connect=lambda session_present: None,
        on_disconnect=received.append,
        on_message=lambda topic, payload: None,
        on_publish=lambda mid: None,
    )
    transport._handle_disconnect(None, None, None, None, None)
    assert received == [0]


def test_handle_connect_with_real_reason_code_object_does_not_raise() -> None:
    """`_handle_connect` doesn't convert reason_code to int today, but guard
    against a future regression that adds one without using `.value`."""
    transport = _make_transport()
    received: list[bool] = []
    transport.set_handlers(
        on_connect=received.append,
        on_disconnect=lambda rc: None,
        on_message=lambda topic, payload: None,
        on_publish=lambda mid: None,
    )

    class _ConnectFlags:
        session_present = True

    success_rc = ReasonCode(PacketTypes.CONNACK, aName="Success", identifier=0)
    transport._handle_connect(None, None, _ConnectFlags(), success_rc, None)
    assert received == [True]
