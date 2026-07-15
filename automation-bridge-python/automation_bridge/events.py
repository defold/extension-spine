"""Typed event, state, and command synchronization primitives."""

from dataclasses import dataclass
from typing import Any, List, Mapping, Optional, TYPE_CHECKING, Union

if TYPE_CHECKING:
    from .client import Client


@dataclass(frozen=True)
class Event:
    """One immutable entry from the native bounded event timeline."""

    raw: Mapping[str, Any]

    @property
    def sequence(self) -> int:
        return int(self.raw.get("sequence", 0))

    @property
    def type(self) -> str:
        return str(self.raw.get("type", ""))

    @property
    def name(self) -> str:
        return str(self.raw.get("name", ""))

    @property
    def data(self) -> Any:
        return self.raw.get("data")

    @property
    def frame(self) -> int:
        return int(self.raw.get("frame", 0))

    @property
    def native_timestamp_us(self) -> int:
        return int(self.raw.get("native_timestamp_us", 0))

    @property
    def recording_timestamp_us(self) -> Optional[int]:
        value = self.raw.get("recording_timestamp_us")
        return int(value) if value is not None else None

    @property
    def engine_instance_id(self) -> str:
        return str(self.raw.get("engine_instance_id", ""))

    @property
    def scene_sequence(self) -> int:
        return int(self.raw.get("scene_sequence", 0))


@dataclass(frozen=True)
class StateSnapshot:
    """One published state value with the revision that produced it."""

    raw: Mapping[str, Any]
    path: Optional[str] = None
    selected_value: Any = None

    @property
    def name(self) -> str:
        return str(self.raw.get("name", ""))

    @property
    def value(self) -> Any:
        return self.selected_value if self.path is not None else self.raw.get("value")

    @property
    def revision(self) -> int:
        return int(self.raw.get("revision", 0))

    @property
    def frame(self) -> int:
        return int(self.raw.get("frame", 0))


class EventStream:
    """Cursor subscription over the server's bounded event ring.

    Constructing the stream resolves ``from_cursor='now'`` immediately. This
    makes ``with bridge.events('now')`` race-free: the cursor exists before the
    action inside the context is sent.
    """

    def __init__(self, client: "Client", from_cursor: Union[str, int] = "now"):
        self.client = client
        cursor_data = client.request("GET", "/events/cursor")
        if from_cursor == "now":
            self.cursor = int(cursor_data["cursor"])
        elif from_cursor == "oldest":
            self.cursor = int(cursor_data["oldest_cursor"])
        elif isinstance(from_cursor, int) and from_cursor >= 0:
            self.cursor = from_cursor
        else:
            raise ValueError("from_cursor must be 'now', 'oldest', or a non-negative integer")
        self._pending: List[Event] = []
        self._closed = False

    def __enter__(self) -> "EventStream":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    def close(self) -> None:
        """Stop this logical subscription; no persistent socket is retained."""
        self._closed = True
        self._pending.clear()

    def poll(self, timeout: float = 0.0, limit: int = 100) -> List[Event]:
        """Long-poll once and advance the cursor after all returned events."""
        if self._closed:
            raise RuntimeError("event stream is closed")
        if timeout < 0:
            raise ValueError("timeout must be non-negative")
        if limit < 1 or limit > 256:
            raise ValueError("limit must be between 1 and 256")
        safe_wait = min(timeout, max(0.0, float(self.client.timeout) - 0.1), 30.0)
        data = self.client.request(
            "GET", "/events",
            params={"cursor": self.cursor, "timeout_ms": int(safe_wait * 1000), "limit": limit},
        )
        if data.get("overflow"):
            raise EventBufferOverflow(
                requested_cursor=self.cursor,
                oldest_cursor=int(data.get("oldest_cursor", 0)),
                latest_cursor=int(data.get("latest_cursor", 0)),
            )
        self.cursor = int(data.get("next_cursor", self.cursor))
        return [Event(item) for item in data.get("events", []) if isinstance(item, Mapping)]

    def wait(
        self,
        name: str,
        where: Optional[Mapping[str, Any]] = None,
        timeout: float = 10.0,
        event_type: Optional[str] = None,
    ) -> Event:
        """Wait for a named event whose JSON data contains ``where`` values."""
        import time

        deadline = time.monotonic() + timeout
        while True:
            for index, event in enumerate(self._pending):
                if event.name == name and (event_type is None or event.type == event_type) and _mapping_contains(event.data, where):
                    return self._pending.pop(index)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"event {name!r} was not observed within {timeout:g}s; cursor={self.cursor}")
            self._pending.extend(self.poll(timeout=min(remaining, 1.0)))


class EventBufferOverflow(RuntimeError):
    """Raised when a requested cursor has already fallen out of the event ring."""

    def __init__(self, requested_cursor: int, oldest_cursor: int, latest_cursor: int):
        self.requested_cursor = requested_cursor
        self.oldest_cursor = oldest_cursor
        self.latest_cursor = latest_cursor
        super().__init__(
            f"event cursor {requested_cursor} overflowed; retained range starts at "
            f"{oldest_cursor} and next cursor is {latest_cursor}"
        )


class CommandTimeout(TimeoutError):
    """Raised when a command did not reach a terminal state before its client timeout."""

    def __init__(self, command_id: int, timeout: float, cancellation_error: Optional[BaseException] = None):
        self.command_id = command_id
        self.timeout = timeout
        self.cancellation_error = cancellation_error
        suffix = f"; cancellation failed: {cancellation_error}" if cancellation_error else "; pending command was cancelled"
        super().__init__(f"command {command_id} did not finish within {timeout:g}s{suffix}")


def _mapping_contains(value: Any, expected: Optional[Mapping[str, Any]]) -> bool:
    if not expected:
        return True
    if not isinstance(value, Mapping):
        return False
    for key, expected_value in expected.items():
        actual = value.get(key)
        if isinstance(expected_value, Mapping):
            if not _mapping_contains(actual, expected_value):
                return False
        elif actual != expected_value:
            return False
    return True


def select_state_path(
    entries: List[Mapping[str, Any]],
    path: str,
    state_name: Optional[str] = None,
) -> Optional[StateSnapshot]:
    """Select the longest published-state name prefix and traverse its JSON value."""
    candidates = []
    for entry in entries:
        name = str(entry.get("name", ""))
        if state_name is not None:
            if name == state_name:
                candidates.append(entry)
        elif path == name or path.startswith(name + "."):
            candidates.append(entry)
    if not candidates:
        return None
    entry = max(candidates, key=lambda item: len(str(item.get("name", ""))))
    name = str(entry.get("name", ""))
    remainder = path[len(name) :].lstrip(".")
    value = entry.get("value")
    if remainder:
        for part in remainder.split("."):
            if not isinstance(value, Mapping) or part not in value:
                return None
            value = value[part]
    return StateSnapshot(entry, path=path, selected_value=value)
