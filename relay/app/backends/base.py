"""The adapter contract every message backend implements --
docs/SERVER_PLAN.md §6.1.

```python
class Backend(Protocol):
    kind: ClassVar[str]
    config_schema: ClassVar[type[BaseModel]]          # validated per-user config
    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult: ...
    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None: ...   # e.g. send a code
    def complete_link(self, backend: BackendRow, proof: str) -> bool: ...
    def render_state(self, delivery: Delivery) -> str: ...  # "sent" / "shown" / "read" for the UI
```

Inbound (replies) is not part of this protocol: an adapter that receives
messages (SMS, Google Chat) registers its own webhook router and calls
`app.routing.Routing.send()` itself, passing its own `kind` as
`origin_backend_kind` and, once it has one, the specific per-user backend
row's id as `origin_backend_id` (docs/SERVER_PLAN.md §3/§5.2 --
`pager`/`webapp` pass `None`, see `app/routing.py`'s module docstring). A
new backend is one module here + one line in `registry.py` + a settings form
in the web app (§7.4) -- nothing in `routing.py` changes.

`deliver()` owns its own *success-path* store write (e.g. `messages_store.
mark_delivery_sent_if_queued`) rather than `routing.py` reaching into
`deliveries.*` itself for that part -- keeps each adapter's success
transition self-contained, matching how `pager`/`webapp` are described in
§6.2/§6.3. The returned `DeliverResult` is not discarded, though --
`app/routing.py`'s `_deliver_one` (the one
place that sees every `deliver()` call, success or failure, inline or a
tick/online-edge retry) applies it through `messages_store.
record_delivery_attempt` for the attempts/error/max-5→failed bookkeeping
docs/SERVER_PLAN.md §5.2 describes, which is cross-cutting rather than
adapter-specific and so does not belong duplicated in every backend module.
"""

from __future__ import annotations

from typing import ClassVar, Protocol

from pydantic import BaseModel, ConfigDict

from app.store.backends import Backend as BackendRow
from app.store.messages import Delivery, DeliveryState, Message
from app.store.users import User


class DeliverResult(BaseModel):
    model_config = ConfigDict(extra="ignore")

    ok: bool
    state: DeliveryState
    external_id: str | None = None
    error: str | None = None


class LinkStep(BaseModel):
    """What a user's client should do next to finish linking a backend
    (e.g. "enter the code we texted you"). Kept deliberately generic --
    each backend's `config_schema` and its own settings-page form (§7.4)
    give the field meaning."""

    model_config = ConfigDict(extra="ignore")

    instructions: str
    expiresInS: int | None = None


class NoConfig(BaseModel):
    """Empty `config_schema` for a backend that needs no user-supplied
    configuration (`webapp`)."""

    model_config = ConfigDict(extra="ignore")


class Backend(Protocol):
    kind: ClassVar[str]
    config_schema: ClassVar[type[BaseModel]]

    def deliver(self, msg: Message, delivery: Delivery, backend: BackendRow) -> DeliverResult: ...

    def start_link(self, user: User, backend: BackendRow) -> LinkStep | None: ...

    def complete_link(self, backend: BackendRow, proof: str) -> bool: ...

    def render_state(self, delivery: Delivery) -> str: ...
