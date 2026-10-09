"""`app.policy` -- docs/FAMILIES_DESIGN.md §2, §1 decision 7."""

from __future__ import annotations

from dataclasses import dataclass

import pytest

from app import policy


@dataclass
class _FakePolicy:
    out: str
    in_: str


@dataclass
class _FakeUser:
    kind: str
    policy: _FakePolicy


def _person(out: str, in_: str) -> _FakeUser:
    return _FakeUser(kind="person", policy=_FakePolicy(out=out, in_=in_))


def _external() -> _FakeUser:
    # Externals have no policy of their own (§2's semantics paragraph); the
    # value never matters since `check()` skips an external side entirely.
    return _FakeUser(kind="external", policy=_FakePolicy(out="", in_=""))


# docs/FAMILIES_DESIGN.md §2's two tables, transcribed exactly: for each
# code, (people-column rule, numbers-column rule). The full 5x2x2 matrix:
# 5 codes per table x {people, numbers} columns x {out, in} tables.
OUTBOUND_TABLE: dict[str, tuple[policy.Rule, policy.Rule]] = {
    "open": ("any", "any"),
    "people": ("approved", "none"),
    "people_sms": ("approved", "approved"),
    "sms": ("none", "approved"),
    "any_sms": ("none", "any"),
}

INBOUND_TABLE: dict[str, tuple[policy.Rule, policy.Rule]] = {
    "any": ("any", "any"),
    "people": ("approved", "none"),
    "people_sms": ("approved", "approved"),
    "sms": ("none", "approved"),
    "any_sms": ("none", "any"),
}


@pytest.mark.parametrize("code,expected", sorted(OUTBOUND_TABLE.items()))
def test_rule_outbound_table(code: str, expected: tuple[policy.Rule, policy.Rule]) -> None:
    people_rule, numbers_rule = expected
    assert policy.rule(code, "person") == people_rule
    assert policy.rule(code, "external") == numbers_rule


@pytest.mark.parametrize("code,expected", sorted(INBOUND_TABLE.items()))
def test_rule_inbound_table(code: str, expected: tuple[policy.Rule, policy.Rule]) -> None:
    people_rule, numbers_rule = expected
    assert policy.rule(code, "person") == people_rule
    assert policy.rule(code, "external") == numbers_rule


def test_out_and_in_machine_code_sets_match_the_tables() -> None:
    assert policy.OUT == set(OUTBOUND_TABLE)
    assert policy.IN == set(INBOUND_TABLE)


# ---- check() ----


def test_check_open_any_passes_with_no_edges_at_all() -> None:
    """Admin defaults (`open`/`any`, §2) -- either side's rule is `any` for
    both peer kinds, so no edge is required in either direction."""
    sender = _person("open", "any")
    recipient = _person("open", "any")
    assert policy.check(sender, recipient, has_edge_out=False, has_edge_in=False) is None


def test_check_people_people_requires_both_directed_edges() -> None:
    """Member defaults (`people`/`people`) -- `approved` on both sides means
    both `allow/{S}_{R}.message` and `allow/{R}_{S}.message` must exist."""
    sender = _person("people", "people")
    recipient = _person("people", "people")

    assert policy.check(sender, recipient, has_edge_out=True, has_edge_in=True) is None
    assert (
        policy.check(sender, recipient, has_edge_out=False, has_edge_in=True) == "not_allowed"
    )
    assert (
        policy.check(sender, recipient, has_edge_out=True, has_edge_in=False) == "not_allowed"
    )


def test_check_sender_none_refuses_policy_out_even_with_edges() -> None:
    """`sms` outbound to a person is `none` (§2: numbers-only) -- refused as
    `policy_out` regardless of any edge."""
    sender = _person("sms", "any")
    recipient = _person("open", "any")
    assert (
        policy.check(sender, recipient, has_edge_out=True, has_edge_in=True) == "policy_out"
    )


def test_check_recipient_none_refuses_policy_in() -> None:
    """The sender's own outbound side (`open` -> any/any) passes
    unconditionally; the recipient's inbound `sms` (numbers-only) has no
    rule for a person peer (`none`), refusing the pair as `policy_in`."""
    sender = _person("open", "any")
    recipient = _person("open", "sms")
    assert (
        policy.check(sender, recipient, has_edge_out=True, has_edge_in=True) == "policy_in"
    )


def test_check_sender_none_checked_before_recipient_in() -> None:
    """When both sides would refuse, the sender's own `policy_out` wins --
    `check()`'s documented evaluation order."""
    sender = _person("sms", "any")
    recipient = _person("open", "sms")
    assert (
        policy.check(sender, recipient, has_edge_out=False, has_edge_in=False) == "policy_out"
    )


def test_check_external_sender_skips_senders_own_side() -> None:
    """An external has no policy of its own -- only the recipient's inbound
    rule for `external` peers governs."""
    sender = _external()
    recipient = _person("open", "any")
    assert policy.check(sender, recipient, has_edge_out=False, has_edge_in=False) is None

    recipient_blocked = _person("open", "people")
    assert (
        policy.check(sender, recipient_blocked, has_edge_out=False, has_edge_in=False)
        == "policy_in"
    )


def test_check_external_recipient_skips_recipients_own_side() -> None:
    """Symmetric case: sending *to* an external only checks the sender's
    outbound rule for `external` peers."""
    sender = _person("any_sms", "any")
    recipient = _external()
    assert policy.check(sender, recipient, has_edge_out=False, has_edge_in=False) is None

    sender_blocked = _person("people", "any")
    assert (
        policy.check(sender_blocked, recipient, has_edge_out=False, has_edge_in=False)
        == "policy_out"
    )


def test_check_both_external_is_always_none() -> None:
    assert policy.check(_external(), _external(), False, False) is None


def test_chat_external_is_a_person_peer():
    """docs/BRIDGE_PHONE_DESIGN.md decision 9(d): an external with `chat` set
    reads the people column, so the subscribe edge approves it; a plain
    external still reads the numbers column."""

    @dataclass
    class _ChatExternal(_FakeUser):
        chat: dict | None = None

    chat_ext = _ChatExternal(kind="external", policy=_FakePolicy(out="", in_=""), chat={"bridgeId": "b"})
    owner = _person("people", "people")
    # people column: approved -> needs the edge; the numbers column would be `none`.
    assert policy.check(owner, chat_ext, False, False) == "not_allowed"
    assert policy.check(owner, chat_ext, True, True) is None
    assert policy.check(chat_ext, owner, False, True) is None
    assert policy.check(chat_ext, owner, False, False) == "not_allowed"
    plain = _external()
    assert policy.check(owner, plain, True, True) == "policy_out"
