"""Conversation policy -- docs/FAMILIES_DESIGN.md §2, §1 decision 7.

Two independent pickers live on `users/{uid}.policy` (`app/store/users.py`'s
`Policy` model): `out` ("who @kid can start or continue a chat with") and
`in_`/`"in"` ("who may reach @kid"). Each picks one of five machine codes
(`OUT`/`IN` below); §2's two tables collapse, per code, to a rule against a
*person* peer and a rule against an *external* (SMS) peer -- `rule()` looks
that up. `check()` is `app/routing.py`'s single gate: it evaluates both
sides of a DM (the sender's `out` against the recipient's `kind`, the
recipient's `in` against the sender's `kind`) and returns the one machine
reason (if any) `routing.send` should reject with. Externals have no policy
of their own (§2's semantics paragraph), so a side whose user is
`kind == 'external'` is skipped entirely -- it contributes no verdict, not
an automatic pass.
"""

from __future__ import annotations

from typing import Literal, Protocol

# docs/FAMILIES_DESIGN.md §2: the machine codes each picker stores.
OUT = {"open", "people", "people_sms", "sms", "any_sms"}
IN = {"any", "people", "people_sms", "sms", "any_sms"}

PeerKind = Literal["person", "external"]
Rule = Literal["any", "approved", "none"]
Reason = Literal["policy_out", "policy_in", "not_allowed"]

# §2's two tables, collapsed to (people-column, numbers-column) per code.
# `open` (outbound-only) and `any` (inbound-only) are the same any/any row;
# the other four codes are shared verbatim between the two pickers.
_RULES: dict[str, tuple[Rule, Rule]] = {
    "open": ("any", "any"),
    "any": ("any", "any"),
    "people": ("approved", "none"),
    "people_sms": ("approved", "approved"),
    "sms": ("none", "approved"),
    "any_sms": ("none", "any"),
}


def rule(policy_code: str, peer_kind: PeerKind) -> Rule:
    """§2's two tables: `policy_code` is one of `OUT`/`IN`'s machine codes,
    `peer_kind` is the *other* party's `users.kind` (`'person'` reads the
    "people" column, `'external'` the "numbers" column)."""
    people_rule, numbers_rule = _RULES[policy_code]
    return people_rule if peer_kind == "person" else numbers_rule


def _peer_kind(user: _UserLike) -> PeerKind:
    """docs/BRIDGE_PHONE_DESIGN.md decision 9(d): an external that stands for
    a subscribed Google Chat conversation (`chat` set) is read as a *person*
    peer -- the subscribe edge is what approves it, and the numbers column
    stays about phone numbers."""
    if user.kind == "external" and getattr(user, "chat", None):
        return "person"
    return user.kind  # type: ignore[return-value]


class _PolicyLike(Protocol):
    out: str
    in_: str


class _UserLike(Protocol):
    kind: str
    policy: _PolicyLike


def check(
    sender: _UserLike, recipient: _UserLike, has_edge_out: bool, has_edge_in: bool
) -> Reason | None:
    """`app/routing.py`'s DM/group per-recipient gate (§2's semantics
    paragraph): `None` means the pair may exchange this message. Evaluated
    in order -- sender's outbound rule, then recipient's inbound rule -- so
    a pair failing both sides is reported by the sender's side first, the
    same "whichever this sender's own restriction would explain" ordering
    the wire's `system` reply text (docs/SERVER_PLAN.md) already assumes.

    `has_edge_out` is `allow/{sender}_{recipient}.message` (the sender's own
    "approved people/numbers" list, §1 decision 7); `has_edge_in` is
    `allow/{recipient}_{sender}.message` (the recipient's). `'approved'`
    with the edge missing keeps the pre-existing `'not_allowed'` reason
    (unchanged from before this module existed) rather than a new
    `policy_*` one -- a missing approval is not the same failure as a
    policy that refuses the peer kind outright."""
    if sender.kind != "external":
        out_rule = rule(sender.policy.out, _peer_kind(recipient))
        if out_rule == "none":
            return "policy_out"
        if out_rule == "approved" and not has_edge_out:
            return "not_allowed"

    if recipient.kind != "external":
        in_rule = rule(recipient.policy.in_, _peer_kind(sender))
        if in_rule == "none":
            return "policy_in"
        if in_rule == "approved" and not has_edge_in:
            return "not_allowed"

    return None
