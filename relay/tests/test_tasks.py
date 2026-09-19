"""`app/tasks.py` -- docs/SERVER_PLAN.md §5's `tasks.py` line.

`TASKS_MODE=inline` (default/dev, unchanged) is already covered indirectly
by every `app/jobs.py` test that passes `InlineTaskQueue()` explicitly; this
file covers the mode switch itself and the new `TASKS_MODE=cloud_tasks`
path -- construction and the exact task payload/target URL `CloudTasksQueue.
enqueue()` would create, with the Cloud Tasks client library's own
`create_task` call mocked at the boundary, without actually enqueueing
anything. There is no real Cloud Tasks queue in this
environment (or in CI) for `enqueue()` to actually reach.
"""

from __future__ import annotations

import json

import pytest

from app import tasks


def test_build_task_queue_defaults_to_inline(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.delenv("TASKS_MODE", raising=False)
    queue = tasks.build_task_queue()
    assert isinstance(queue, tasks.InlineTaskQueue)


def test_build_task_queue_inline_mode_explicit():
    assert isinstance(tasks.build_task_queue("inline"), tasks.InlineTaskQueue)


def test_build_task_queue_unknown_mode_raises():
    with pytest.raises(NotImplementedError):
        tasks.build_task_queue("carrier-pigeon")


def test_build_task_queue_cloud_tasks_mode_constructs_cloud_tasks_queue(
    monkeypatch: pytest.MonkeyPatch,
):
    # build_task_queue("cloud_tasks") goes through CloudTasksQueue()'s
    # no-client branch, which otherwise constructs a REAL
    # tasks_v2.CloudTasksClient() -- that eagerly calls google.auth.default()
    # in its constructor, which raises DefaultCredentialsError in any
    # environment without real GCP credentials (confirmed failing in CI,
    # which has none by design -- this workflow's own header comment: "No
    # paid services, no secrets"). Every other CloudTasksQueue test in this
    # file already avoids this by passing client=_FakeCloudTasksClient()
    # directly; this one has to patch the library class instead, since its
    # whole point is exercising build_task_queue()'s no-client construction
    # path.
    from google.cloud import tasks_v2

    class _FakeClient:
        pass

    monkeypatch.setattr(tasks_v2, "CloudTasksClient", _FakeClient)

    queue = tasks.build_task_queue("cloud_tasks")
    assert isinstance(queue, tasks.CloudTasksQueue)


def test_queue_path_uses_configured_project_location_queue(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("GOOGLE_CLOUD_PROJECT", "demo-pager")
    monkeypatch.setenv("TASKS_LOCATION", "us-west1")
    monkeypatch.setenv("TASKS_QUEUE", "my-queue")
    assert tasks.queue_path() == "projects/demo-pager/locations/us-west1/queues/my-queue"


def test_build_task_targets_internal_task_with_the_retry_name_and_oidc_token(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setenv("PUBLIC_BASE_URL", "https://relay.example.com")
    monkeypatch.setenv("TASKS_SERVICE_ACCOUNT_EMAIL", "relay-tasks@demo-pager.iam.gserviceaccount.com")

    task = tasks.build_task("pager-retry:m_deadbeef:pgr-0001")

    http_request = task["http_request"]
    assert http_request["url"] == "https://relay.example.com/internal/task"
    assert http_request["http_method"] == "POST"
    assert json.loads(http_request["body"]) == {"name": "pager-retry:m_deadbeef:pgr-0001"}
    assert http_request["oidc_token"] == {
        "service_account_email": "relay-tasks@demo-pager.iam.gserviceaccount.com"
    }


def test_build_task_omits_oidc_token_when_no_service_account_configured(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setenv("PUBLIC_BASE_URL", "https://relay.example.com")
    monkeypatch.delenv("TASKS_SERVICE_ACCOUNT_EMAIL", raising=False)

    task = tasks.build_task("some-retry")
    assert "oidc_token" not in task["http_request"]


class _FakeCloudTasksClient:
    """Mocks the Cloud Tasks client library's `create_task` call at the
    boundary -- records every call it would have made, never touches the
    network."""

    def __init__(self) -> None:
        self.calls: list[dict] = []

    def create_task(self, *, parent: str, task: dict):
        self.calls.append({"parent": parent, "task": task})
        return task


def test_cloud_tasks_queue_enqueue_builds_correct_payload_and_target(
    monkeypatch: pytest.MonkeyPatch,
):
    monkeypatch.setenv("GOOGLE_CLOUD_PROJECT", "demo-pager")
    monkeypatch.setenv("TASKS_LOCATION", "us-central1")
    monkeypatch.setenv("TASKS_QUEUE", "relay-retries")
    monkeypatch.setenv("PUBLIC_BASE_URL", "https://relay.example.com")
    monkeypatch.delenv("TASKS_SERVICE_ACCOUNT_EMAIL", raising=False)

    fake_client = _FakeCloudTasksClient()
    queue = tasks.CloudTasksQueue(client=fake_client)

    # `fn` is never invoked in cloud_tasks mode -- see app/tasks.py's
    # docstring's "known gap" section.
    called = False

    def _should_never_run() -> None:
        nonlocal called
        called = True

    queue.enqueue(_should_never_run, name="sms-retry:m_abc123:bid1")

    assert called is False
    assert len(fake_client.calls) == 1
    call = fake_client.calls[0]
    assert call["parent"] == "projects/demo-pager/locations/us-central1/queues/relay-retries"
    assert call["task"]["http_request"]["url"] == "https://relay.example.com/internal/task"
    assert json.loads(call["task"]["http_request"]["body"]) == {"name": "sms-retry:m_abc123:bid1"}


def test_cloud_tasks_queue_enqueue_swallows_create_task_failure(monkeypatch: pytest.MonkeyPatch):
    monkeypatch.setenv("PUBLIC_BASE_URL", "https://relay.example.com")

    class _BrokenClient:
        def create_task(self, *, parent: str, task: dict):
            raise RuntimeError("cloud tasks unreachable")

    queue = tasks.CloudTasksQueue(client=_BrokenClient())
    # Must not raise -- same "never let a retry-scheduling failure raise
    # into the caller's request" contract InlineTaskQueue already has.
    queue.enqueue(lambda: None, name="whatever")
