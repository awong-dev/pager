"""`/api/book` -- docs/ADDRESS_BOOK_DESIGN.md decision 5: the derived
address book (`app/book.py`) and per-owner nicknames.

Authorised for the owner, a family admin of the owner's family, or super;
anyone else gets 404 (never 403, so a uid's existence is not probed).
"""

from __future__ import annotations

from typing import Annotated, Literal

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from pydantic import BaseModel

from app import book as book_module
from app import devcfg
from app.auth import AuthedUser, require_user
from app.broker import BrokerClient
from app.routers import admin as admin_router
from app.store import devices as devices_store
from app.store import rate_limits as rate_limits_store
from app.store import users as users_store
from app.store.users import User

router = APIRouter(prefix="/api/book")


def get_broker(request: Request) -> BrokerClient:
    return request.app.state.broker


class BookEntryOut(BaseModel):
    uid: str | None
    alias: str
    kind: Literal["person", "external", "group"]
    displayName: str
    nick: str | None
    label: str
    phone: str | None = None
    inFamily: bool
    sendable: bool
    reason: str | None = None
    onPager: bool


class BookOut(BaseModel):
    ownerUid: str
    bv: int | None
    pagerCap: int
    truncated: bool
    entries: list[BookEntryOut]


class NickRequest(BaseModel):
    nick: str


def _authorize(authed: AuthedUser, owner_uid: str) -> User:
    owner = users_store.get_user(owner_uid)
    if owner is None:
        raise HTTPException(status_code=404, detail="no such book")
    me = authed.user
    allowed = (
        authed.uid == owner_uid
        or me.role == "super"
        or (me.role == "admin" and me.familyId is not None and me.familyId == owner.familyId)
    )
    if not allowed:
        raise HTTPException(status_code=404, detail="no such book")
    return owner


def _rate_limit(authed: AuthedUser) -> None:
    limit, window_s = admin_router._admin_write_rate_limit()
    if not rate_limits_store.check_and_increment(
        f"admin:{authed.uid}", limit=limit, window_s=window_s
    ):
        raise HTTPException(status_code=429, detail="admin rate limit exceeded; try again later")


def _view(owner_uid: str) -> BookOut:
    entries = book_module.entries_for(owner_uid)
    devices = sorted(devices_store.list_devices(owner_uid=owner_uid), key=lambda d: d.id)
    default_alias = devcfg._default_alias(devices[0]) if devices else None
    ordered = devcfg._ordered_contacts(owner_uid, default_alias)
    on_pager = {c["a"] for c in ordered[: devcfg.MAX_PULL_CONTACTS]}
    out = [
        BookEntryOut(
            uid=e.uid,
            alias=e.alias,
            kind=e.kind,
            displayName=e.displayName,
            nick=e.nick,
            label=e.label,
            phone=e.phone,
            inFamily=e.inFamily,
            sendable=e.sendable,
            reason=e.reason,
            onPager=e.sendable and e.alias in on_pager,
        )
        for e in entries
    ]
    out.sort(key=lambda e: (e.label.casefold(), e.alias))
    return BookOut(
        ownerUid=owner_uid,
        bv=max((d.bookVersion for d in devices), default=None),
        pagerCap=devcfg.MAX_PULL_CONTACTS,
        truncated=len(ordered) > devcfg.MAX_PULL_CONTACTS,
        entries=out,
    )


def _entry_out(owner_uid: str, peer_uid: str) -> BookEntryOut:
    for e in _view(owner_uid).entries:
        if e.uid == peer_uid:
            return e
    raise HTTPException(status_code=404, detail="no such entry")


def _require_peer(owner_uid: str, peer_uid: str) -> None:
    if not any(e.uid == peer_uid and e.kind != "group" for e in book_module.entries_for(owner_uid)):
        raise HTTPException(status_code=404, detail="no such entry")


@router.get("")
def get_book(authed: Annotated[AuthedUser, Depends(require_user)], uid: Annotated[str | None, Query()] = None) -> BookOut:
    owner_uid = uid or authed.uid
    _authorize(authed, owner_uid)
    return _view(owner_uid)


@router.put("/{owner_uid}/entries/{peer_uid}")
def put_nick(
    owner_uid: str,
    peer_uid: str,
    req: NickRequest,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> BookEntryOut:
    _authorize(authed, owner_uid)
    _rate_limit(authed)
    _require_peer(owner_uid, peer_uid)
    try:
        nick = book_module.validate_nick(req.nick)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    book_module.set_nick(owner_uid, peer_uid, nick, authed.uid)
    book_module.push_only(owner_uid, broker)
    return _entry_out(owner_uid, peer_uid)


@router.delete("/{owner_uid}/entries/{peer_uid}")
def delete_nick(
    owner_uid: str,
    peer_uid: str,
    authed: Annotated[AuthedUser, Depends(require_user)],
    broker: Annotated[BrokerClient, Depends(get_broker)],
) -> BookEntryOut:
    _authorize(authed, owner_uid)
    _rate_limit(authed)
    _require_peer(owner_uid, peer_uid)
    book_module.set_nick(owner_uid, peer_uid, None, authed.uid)
    book_module.push_only(owner_uid, broker)
    return _entry_out(owner_uid, peer_uid)
