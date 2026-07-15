"""Helpers for Defold's Remotery websocket profiler stream."""

import base64
import fnmatch
import hashlib
import math
import os
import re
import socket
import struct
import threading
import time
import urllib.parse
from dataclasses import dataclass
from typing import Any, Callable, Dict, Iterator, List, Mapping, Optional, Sequence, Set, Tuple, Union

from .client import AutomationBridgeError
from .lifecycle import FinalizationHooks


DEFAULT_REMOTERY_PORT = 17815
_WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
_PROPERTY_TYPES = {
    0: "group",
    1: "bool",
    2: "s32",
    3: "u32",
    4: "f32",
    5: "s64",
    6: "u64",
    7: "f64",
}
NumericValue = Union[int, float, bool]
_ROOT_PROPERTY_NAME_HASH = 0
_ROOT_PROPERTY_NAME = "Root Property"


def _best_effort(callback: Callable[[], Any]) -> None:
    """Run finalization without replacing an exception already in flight."""
    try:
        callback()
    except BaseException:
        pass


class RemoteryError(AutomationBridgeError):
    """Base class for profiler connection errors."""

    pass


class RemoteryProtocolError(RemoteryError):
    """Raised when the profiler returns malformed or unexpected stream data."""

    pass


class RemoteryTimeoutError(RemoteryError):
    """Raised when the profiler does not produce data before the requested timeout."""

    pass


@dataclass(frozen=True)
class RemoterySampleAggregate:
    """Totals for all samples sharing one profiler sample name hash."""

    name_hash: int
    name: Optional[str]
    total_us: int
    self_us: int
    call_count: int
    occurrences: int
    max_depth: int

    @property
    def label(self) -> str:
        """Return the resolved sample name, or a stable hash label when unknown."""
        return self.name if self.name is not None else f"0x{self.name_hash:08x}"

    @property
    def total_ms(self) -> float:
        """Return `total_us` in milliseconds."""
        return self.total_us / 1000.0

    @property
    def self_ms(self) -> float:
        """Return `self_us` in milliseconds."""
        return self.self_us / 1000.0


@dataclass(frozen=True)
class RemoteryTimingStats:
    """Timing distribution for a set of profiler samples."""

    count: int
    total_us: int
    min_us: int
    max_us: int
    avg_us: float
    median_us: float
    p90_us: float
    p95_us: float
    p99_us: float

    @property
    def total_ms(self) -> float:
        """Return total time in milliseconds."""
        return self.total_us / 1000.0

    @property
    def min_ms(self) -> float:
        """Return minimum sample time in milliseconds."""
        return self.min_us / 1000.0

    @property
    def max_ms(self) -> float:
        """Return maximum sample time in milliseconds."""
        return self.max_us / 1000.0

    @property
    def avg_ms(self) -> float:
        """Return average sample time in milliseconds."""
        return self.avg_us / 1000.0

    @property
    def median_ms(self) -> float:
        """Return median sample time in milliseconds."""
        return self.median_us / 1000.0

    @property
    def p90_ms(self) -> float:
        """Return p90 sample time in milliseconds."""
        return self.p90_us / 1000.0

    @property
    def p95_ms(self) -> float:
        """Return p95 sample time in milliseconds."""
        return self.p95_us / 1000.0

    @property
    def p99_ms(self) -> float:
        """Return p99 sample time in milliseconds."""
        return self.p99_us / 1000.0


@dataclass(frozen=True)
class RemoteryValueStats:
    """Numeric distribution for profiler counters/properties."""

    count: int
    total: float
    min: float
    max: float
    avg: float
    median: float
    p90: float
    p95: float
    p99: float


@dataclass(frozen=True)
class RemoteryScopeStats:
    """Cross-frame statistics for one profiled scope path on one thread."""

    thread_name: str
    path: str
    name: str
    name_hash: int
    frames_seen: int
    occurrences: int
    calls_total: int
    total: RemoteryTimingStats
    self: RemoteryTimingStats

    @property
    def calls_avg(self) -> float:
        """Return average calls per frame where this scope appeared."""
        if self.frames_seen == 0:
            return 0.0
        return self.calls_total / self.frames_seen


@dataclass(frozen=True)
class RemoteryCounterStats:
    """Cross-frame statistics for one profiler property/counter path."""

    path: str
    name: str
    name_hash: int
    type: str
    frames_seen: int
    last_value: NumericValue
    values: RemoteryValueStats


@dataclass(frozen=True)
class RemoterySample:
    """One node in a profiler sample tree."""

    name_hash: int
    name: Optional[str]
    unique_id: int
    colour: Tuple[int, int, int]
    depth: int
    start_us: int
    duration_us: int
    self_us: int
    gpu_to_cpu_us: int
    call_count: int
    max_recursion_depth: int
    children: Tuple["RemoterySample", ...] = ()

    @property
    def label(self) -> str:
        """Return the resolved sample name, or a stable hash label when unknown."""
        return self.name if self.name is not None else f"0x{self.name_hash:08x}"

    @property
    def end_us(self) -> int:
        """Return the sample end timestamp in microseconds."""
        return self.start_us + self.duration_us

    @property
    def duration_ms(self) -> float:
        """Return `duration_us` in milliseconds."""
        return self.duration_us / 1000.0

    @property
    def self_ms(self) -> float:
        """Return `self_us` in milliseconds."""
        return self.self_us / 1000.0

    def walk(self) -> Iterator["RemoterySample"]:
        """Yield this sample and all descendants in wire order."""
        yield self
        for child in self.children:
            yield from child.walk()


@dataclass(frozen=True)
class RemoteryFrame:
    """One sample tree captured from a profiled thread."""

    thread_name: str
    root: RemoterySample
    partial: bool = False

    @property
    def start_us(self) -> int:
        """Return the frame start timestamp in microseconds."""
        return self.root.start_us

    @property
    def end_us(self) -> int:
        """Return the latest sample end timestamp in microseconds."""
        return max(sample.end_us for sample in self.samples())

    @property
    def duration_us(self) -> int:
        """Return the frame span in microseconds."""
        return self.end_us - self.start_us

    @property
    def duration_ms(self) -> float:
        """Return `duration_us` in milliseconds."""
        return self.duration_us / 1000.0

    def samples(self) -> Iterator[RemoterySample]:
        """Yield all samples in the frame in wire order."""
        yield from self.root.walk()

    def missing_name_hashes(self) -> Set[int]:
        """Return unresolved sample name hashes in this frame."""
        return {sample.name_hash for sample in self.samples() if sample.name is None}

    def resolve_names(self, names: Mapping[int, str]) -> "RemoteryFrame":
        """Return a copy of this frame with names resolved from `names`."""
        return RemoteryFrame(
            thread_name=self.thread_name,
            root=_resolve_sample_names(self.root, names),
            partial=self.partial,
        )

    def aggregate(self, include_root: bool = True) -> List[RemoterySampleAggregate]:
        """Aggregate sample timing by name hash, largest total time first."""
        totals: Dict[int, Dict[str, object]] = {}
        for index, sample in enumerate(self.samples()):
            if index == 0 and not include_root:
                continue
            entry = totals.setdefault(
                sample.name_hash,
                {
                    "name": sample.name,
                    "total_us": 0,
                    "self_us": 0,
                    "call_count": 0,
                    "occurrences": 0,
                    "max_depth": sample.depth,
                },
            )
            if entry["name"] is None and sample.name is not None:
                entry["name"] = sample.name
            entry["total_us"] = int(entry["total_us"]) + sample.duration_us
            entry["self_us"] = int(entry["self_us"]) + sample.self_us
            entry["call_count"] = int(entry["call_count"]) + sample.call_count
            entry["occurrences"] = int(entry["occurrences"]) + 1
            entry["max_depth"] = max(int(entry["max_depth"]), sample.depth)

        aggregates = [
            RemoterySampleAggregate(
                name_hash=name_hash,
                name=entry["name"] if isinstance(entry["name"], str) else None,
                total_us=int(entry["total_us"]),
                self_us=int(entry["self_us"]),
                call_count=int(entry["call_count"]),
                occurrences=int(entry["occurrences"]),
                max_depth=int(entry["max_depth"]),
            )
            for name_hash, entry in totals.items()
        ]
        return sorted(aggregates, key=lambda aggregate: aggregate.total_us, reverse=True)


@dataclass(frozen=True)
class RemoteryProperty:
    """One property/counter row from a profiler snapshot."""

    name_hash: int
    name: Optional[str]
    unique_id: int
    depth: int
    type: str
    value: NumericValue
    previous_value: NumericValue
    previous_value_frame: int
    child_count: int

    @property
    def label(self) -> str:
        """Return the resolved property name, or a stable hash label when unknown."""
        return self.name if self.name is not None else f"0x{self.name_hash:08x}"


@dataclass(frozen=True)
class RemoteryPropertyEntry:
    """One profiler property/counter with its resolved tree path."""

    path: str
    property: RemoteryProperty

    @property
    def name(self) -> str:
        """Return the resolved property name, or a stable hash label."""
        return self.property.label

    @property
    def type(self) -> str:
        """Return the profiler property type name."""
        return self.property.type

    @property
    def value(self) -> NumericValue:
        """Return the current property value."""
        return self.property.value


@dataclass(frozen=True)
class RemoteryPropertyFrame:
    """One profiler property/counter snapshot."""

    property_frame: int
    properties: Tuple[RemoteryProperty, ...]

    def missing_name_hashes(self) -> Set[int]:
        """Return unresolved property name hashes in this snapshot."""
        return {prop.name_hash for prop in self.properties if prop.name is None}

    def resolve_names(self, names: Mapping[int, str]) -> "RemoteryPropertyFrame":
        """Return a copy of this snapshot with names resolved from `names`."""
        return RemoteryPropertyFrame(
            property_frame=self.property_frame,
            properties=tuple(_resolve_property_name(prop, names) for prop in self.properties),
        )

    def entries(self, include_groups: bool = True) -> List[RemoteryPropertyEntry]:
        """Return all properties with resolved tree paths."""
        return [
            RemoteryPropertyEntry(path=prop_path, property=prop)
            for prop_path, prop in _walk_properties_with_paths(self.properties)
            if include_groups or prop.type != "group"
        ]

    def find(
        self,
        text: str,
        include_groups: bool = True,
        case_sensitive: bool = False,
    ) -> List[RemoteryPropertyEntry]:
        """Return properties whose name or path contains `text`."""
        return [
            entry
            for entry in self.entries(include_groups=include_groups)
            if _matches_contains(entry.path, entry.name, text, case_sensitive)
        ]


@dataclass(frozen=True)
class RemoteryCapture:
    """A multi-frame profiler capture with query helpers for scopes and counters."""

    frames: Tuple[RemoteryFrame, ...]
    property_frames: Tuple[RemoteryPropertyFrame, ...] = ()

    def aggregate(self, sort: str = "self_p95_ms", include_root: bool = True) -> List[RemoteryScopeStats]:
        """Return scope statistics. Alias for `scopes()`."""
        return self.scopes(sort=sort, include_root=include_root)

    def scopes(
        self,
        name: Optional[str] = None,
        path: Optional[str] = None,
        thread: Optional[str] = None,
        contains: Optional[str] = None,
        regex: Optional[str] = None,
        sort: str = "self_p95_ms",
        include_root: bool = True,
        case_sensitive: bool = False,
    ) -> List[RemoteryScopeStats]:
        """Return scope statistics filtered by name, path, thread, text, or regex."""
        rows: Dict[Tuple[str, str, int], Dict[str, object]] = {}
        pattern = _compile_regex(regex, case_sensitive)
        for frame_index, frame in enumerate(self.frames):
            if thread is not None and frame.thread_name != thread:
                continue
            for sample_path, sample in _walk_samples_with_paths(frame.root):
                if sample is frame.root and not include_root:
                    continue
                if not _matches_sample(sample, sample_path, name, path, contains, pattern, case_sensitive):
                    continue
                key = (frame.thread_name, sample_path, sample.name_hash)
                entry = rows.setdefault(
                    key,
                    {
                        "thread_name": frame.thread_name,
                        "path": sample_path,
                        "name": sample.label,
                        "name_hash": sample.name_hash,
                        "frames": set(),
                        "total_values": [],
                        "self_values": [],
                        "calls_total": 0,
                    },
                )
                frames_seen = entry["frames"]
                if isinstance(frames_seen, set):
                    frames_seen.add(frame_index)
                total_values = entry["total_values"]
                self_values = entry["self_values"]
                if isinstance(total_values, list):
                    total_values.append(sample.duration_us)
                if isinstance(self_values, list):
                    self_values.append(sample.self_us)
                entry["calls_total"] = int(entry["calls_total"]) + sample.call_count

        stats = [_scope_stats_from_entry(entry) for entry in rows.values()]
        return sorted(stats, key=lambda item: _scope_sort_value(item, sort), reverse=True)

    def scope(
        self,
        selector: str,
        thread: Optional[str] = None,
        include_root: bool = True,
    ) -> RemoteryScopeStats:
        """Return exactly one scope matching `selector` by path or name."""
        matches = self.scopes(thread=thread, include_root=include_root)
        matches = [item for item in matches if item.path == selector or item.name == selector]
        if not matches:
            raise RemoteryError(f"no profiler scope matched {selector!r}")
        if len(matches) > 1:
            raise RemoteryError(f"multiple profiler scopes matched {selector!r}; use scopes() with a path or thread")
        return matches[0]

    def counters(
        self,
        name: Optional[str] = None,
        path: Optional[str] = None,
        contains: Optional[str] = None,
        regex: Optional[str] = None,
        sort: str = "max",
        include_groups: bool = False,
        case_sensitive: bool = False,
    ) -> List[RemoteryCounterStats]:
        """Return counter/property statistics filtered by name, path, text, or regex."""
        rows: Dict[Tuple[str, int], Dict[str, object]] = {}
        pattern = _compile_regex(regex, case_sensitive)
        for frame in self.property_frames:
            for prop_path, prop in _walk_properties_with_paths(frame.properties):
                if prop.type == "group" and not include_groups:
                    continue
                if not _matches_property(prop, prop_path, name, path, contains, pattern, case_sensitive):
                    continue
                key = (prop_path, prop.name_hash)
                entry = rows.setdefault(
                    key,
                    {
                        "path": prop_path,
                        "name": prop.label,
                        "name_hash": prop.name_hash,
                        "type": prop.type,
                        "frame_ids": set(),
                        "values": [],
                        "last_value": prop.value,
                    },
                )
                frame_ids = entry["frame_ids"]
                values = entry["values"]
                if isinstance(frame_ids, set):
                    frame_ids.add(frame.property_frame)
                if isinstance(values, list):
                    values.append(float(prop.value))
                entry["last_value"] = prop.value

        stats = [_counter_stats_from_entry(entry) for entry in rows.values()]
        return sorted(stats, key=lambda item: _counter_sort_value(item, sort), reverse=True)

    def counter(self, selector: str) -> RemoteryCounterStats:
        """Return exactly one counter/property matching `selector` by path or name."""
        matches = [item for item in self.counters() if item.path == selector or item.name == selector]
        if not matches:
            raise RemoteryError(f"no profiler counter matched {selector!r}")
        if len(matches) > 1:
            raise RemoteryError(f"multiple profiler counters matched {selector!r}; use counters() with a path")
        return matches[0]


class RemoteryRecording:
    """Background profiler recording that can be stopped at an arbitrary time."""

    def __init__(
        self,
        client: "RemoteryClient",
        warmup_frames: int = 0,
        thread: Optional[str] = None,
        include_properties: bool = True,
        resolve_names: bool = True,
        read_timeout: float = 0.25,
        max_frames: Optional[int] = None,
        close_on_stop: Optional[bool] = None,
        on_finalize: Optional[Callable[[RemoteryCapture], None]] = None,
        on_abort: Optional[Callable[[BaseException, RemoteryCapture], None]] = None,
    ):
        """Create a recording session for `client`.

        Use `start()` to begin collecting frames on a background thread and
        `stop()` returns a `ProfilerCapture` for the frames collected so far.
        """
        if warmup_frames < 0:
            raise ValueError("warmup_frames must be non-negative")
        if read_timeout <= 0:
            raise ValueError("read_timeout must be positive")
        if max_frames is not None and max_frames <= 0:
            raise ValueError("max_frames must be positive")
        self._client = client
        self._warmup_frames = warmup_frames
        self._thread_name = thread
        self._include_properties = include_properties
        self._resolve_names = resolve_names
        self._read_timeout = read_timeout
        self._max_frames = max_frames
        self._close_on_stop = close_on_stop
        self.hooks = FinalizationHooks(on_finalize=on_finalize, on_abort=on_abort)
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._thread: Optional[threading.Thread] = None
        self._sample_frames: List[RemoteryFrame] = []
        self._property_frames: List[RemoteryPropertyFrame] = []
        self._accepted_frames = 0
        self._error: Optional[BaseException] = None
        self._capture: Optional[RemoteryCapture] = None
        self._finalized: Optional[str] = None

    def __enter__(self) -> "RemoteryRecording":
        if self._thread is None:
            return self.start()
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if exc_type is None:
            self.stop()
            return
        cause = exc if isinstance(exc, BaseException) else RuntimeError(str(exc_type))
        _best_effort(lambda: self.abort(cause))

    @property
    def running(self) -> bool:
        """Return whether the background reader is still running."""
        thread = self._thread
        return thread is not None and thread.is_alive()

    @property
    def frame_count(self) -> int:
        """Return the number of captured sample frames, excluding warmup frames."""
        with self._lock:
            return len(self._sample_frames)

    @property
    def property_frame_count(self) -> int:
        """Return the number of captured property/counter snapshots."""
        with self._lock:
            return len(self._property_frames)

    def start(self) -> "RemoteryRecording":
        """Start recording profiler frames on a background thread."""
        if self._thread is not None:
            raise RemoteryError("profiler recording is already started")
        was_connected = self._client.connected
        if not was_connected:
            self._client.start()
        if self._close_on_stop is None:
            self._close_on_stop = not was_connected
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._run, name="RemoteryRecording", daemon=True)
        self._thread.start()
        return self

    def stop(self, timeout: float = 2.0) -> RemoteryCapture:
        """Finalize recording and run ``on_finalize`` with the capture.

        If stopping itself is interrupted, the websocket is closed and
        ``on_abort`` is called best-effort before the original exception is
        re-raised. Repeated calls return the already finalized capture.
        """
        if self._capture is not None and self._finalized is not None:
            return self._capture
        try:
            self._finish_reader(timeout)
            if self._error is not None:
                raise self._error
        except BaseException as exc:
            self._emergency_close(timeout)
            self._capture = self.snapshot()
            self._finalized = "aborted"
            self.hooks.abort(exc, self._capture, suppress=True)
            raise

        self._capture = self.snapshot()
        self._finalized = "finalized"
        self.hooks.finalize(self._capture)
        return self._capture

    def abort(
        self,
        cause: Optional[BaseException] = None,
        timeout: float = 2.0,
    ) -> RemoteryCapture:
        """Abort recording, close its stream, and run ``on_abort``.

        Background reader errors are intentionally not raised from this path;
        callers use it while another exception is already in flight. An
        explicitly supplied ``cause`` is passed to the hook.
        """
        if self._capture is not None and self._finalized is not None:
            return self._capture
        self._emergency_close(timeout)
        self._capture = self.snapshot()
        self._finalized = "aborted"
        abort_cause = cause or RemoteryError("recording aborted")
        self.hooks.abort(abort_cause, self._capture)
        return self._capture

    def _finish_reader(self, timeout: float) -> None:
        self._stop_event.set()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=timeout)
            if thread.is_alive():
                self._client.stop()
                thread.join(timeout=timeout)
        if self._close_on_stop:
            self._client.stop()

    def _emergency_close(self, timeout: float) -> None:
        self._stop_event.set()
        _best_effort(self._client.stop)
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            _best_effort(lambda: thread.join(timeout=timeout))

    def snapshot(self) -> RemoteryCapture:
        """Return a `ProfilerCapture` for frames collected so far without stopping."""
        with self._lock:
            return RemoteryCapture(frames=tuple(self._sample_frames), property_frames=tuple(self._property_frames))

    def _run(self) -> None:
        try:
            while not self._stop_event.is_set():
                if self._max_frames is not None and self.frame_count >= self._max_frames:
                    break
                try:
                    message_id, body = self._client._next_message(_deadline(self._read_timeout))
                except RemoteryTimeoutError:
                    continue
                except RemoteryError as exc:
                    if self._stop_event.is_set():
                        break
                    self._error = exc
                    break

                if message_id == "SMPL":
                    self._record_sample_frame(body)
                elif message_id == "PSNP" and self._include_properties and self._accepted_frames >= self._warmup_frames:
                    self._record_property_frame(body)
        except BaseException as exc:
            if not self._stop_event.is_set():
                self._error = exc

    def _record_sample_frame(self, body: bytes) -> None:
        frame = parse_sample_frame(body, self._client._sample_names)
        if self._thread_name is not None and frame.thread_name != self._thread_name:
            return
        if self._resolve_names:
            frame = self._client._resolve_frame_names(frame, _deadline(self._read_timeout))
        with self._lock:
            if self._accepted_frames >= self._warmup_frames:
                self._sample_frames.append(frame)
            self._accepted_frames += 1

    def _record_property_frame(self, body: bytes) -> None:
        properties = parse_property_frame(body, self._client._sample_names)
        if self._resolve_names:
            properties = self._client._resolve_property_names(properties, _deadline(self._read_timeout))
        with self._lock:
            self._property_frames.append(properties)


class RemoteryClient:
    """Client for Defold's live profiler stream.

    Use `start()` to open `ws://host:port/rmt`, `get_frame()` or
    `get_properties()` to read one Remotery message, `capture()` to aggregate a
    fixed window of frames, `start_recording()` to collect until an arbitrary
    stop point, and `stop()` to close the websocket.
    """

    def __init__(
        self,
        port: int = DEFAULT_REMOTERY_PORT,
        timeout: float = 10.0,
        host: str = "127.0.0.1",
        path: str = "/rmt",
    ):
        """Create a profiler connection for an already-running Defold engine."""
        self.host = host
        self.port = int(port)
        self.timeout = timeout
        self.path = path if path.startswith("/") else f"/{path}"
        self.url = f"ws://{self.host}:{self.port}{self.path}"
        self._socket: Optional[socket.socket] = None
        self._pending_messages: List[Tuple[str, bytes]] = []
        self._sample_names: Dict[int, str] = {_ROOT_PROPERTY_NAME_HASH: _ROOT_PROPERTY_NAME}

    @classmethod
    def from_url(cls, url: str, timeout: float = 10.0) -> "RemoteryClient":
        """Create a profiler connection from a `ws://host:port/rmt` URL."""
        parsed = urllib.parse.urlparse(url)
        if parsed.scheme != "ws":
            raise RemoteryError(f"unsupported profiler URL scheme: {parsed.scheme!r}")
        if parsed.hostname is None or parsed.port is None:
            raise RemoteryError(f"profiler URL must include host and port: {url!r}")
        path = parsed.path or "/rmt"
        if parsed.query:
            path = f"{path}?{parsed.query}"
        return cls(port=parsed.port, host=parsed.hostname, path=path, timeout=timeout)

    def __enter__(self) -> "RemoteryClient":
        return self.start()

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if exc_type is None:
            self.stop()
        else:
            _best_effort(self.stop)

    @property
    def connected(self) -> bool:
        """Return whether this client currently has an open websocket."""
        return self._socket is not None

    @property
    def sample_names(self) -> Mapping[int, str]:
        """Return the currently resolved profiler sample-name map."""
        return dict(self._sample_names)

    def start(self) -> "RemoteryClient":
        """Open the profiler stream connection and reset captured state."""
        self.stop()
        self._pending_messages.clear()
        self._sample_names = {_ROOT_PROPERTY_NAME_HASH: _ROOT_PROPERTY_NAME}
        try:
            self._socket = socket.create_connection((self.host, self.port), timeout=self.timeout)
        except OSError as exc:
            raise RemoteryError(f"failed to connect to {self.url}: {exc}") from exc
        try:
            self._handshake()
        except BaseException:
            _best_effort(self.stop)
            raise
        return self

    def stop(self) -> None:
        """Close the profiler stream connection."""
        sock = self._socket
        self._socket = None
        if sock is None:
            return
        try:
            self._send_frame(0x8, b"", sock=sock)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

    def get_frame(
        self,
        timeout: Optional[float] = None,
        thread: Optional[str] = None,
        resolve_names: bool = True,
    ) -> RemoteryFrame:
        """Read and return one profiler sample frame.

        When `resolve_names` is true, unresolved sample hashes are requested from
        the Remotery string table before returning. Name resolution is best
        effort; if the frame arrives before all names can be resolved, unknown
        samples keep `name=None` and expose `label` as the hexadecimal hash.
        """
        deadline = _deadline(self.timeout if timeout is None else timeout)
        while True:
            message_id, body = self._next_message(deadline)
            if message_id != "SMPL":
                continue

            frame = parse_sample_frame(body, self._sample_names)
            if thread is not None and frame.thread_name != thread:
                continue
            if resolve_names:
                frame = self._resolve_frame_names(frame, deadline)
            return frame

    def aggregate_frame(
        self,
        timeout: Optional[float] = None,
        thread: Optional[str] = None,
        include_root: bool = True,
    ) -> List[RemoterySampleAggregate]:
        """Read one frame and aggregate its samples by name hash."""
        return self.get_frame(timeout=timeout, thread=thread).aggregate(include_root=include_root)

    def start_recording(
        self,
        warmup_frames: int = 0,
        thread: Optional[str] = None,
        include_properties: bool = True,
        resolve_names: bool = True,
        read_timeout: float = 0.25,
        max_frames: Optional[int] = None,
        close_on_stop: Optional[bool] = None,
        on_finalize: Optional[Callable[[RemoteryCapture], None]] = None,
        on_abort: Optional[Callable[[BaseException, RemoteryCapture], None]] = None,
    ) -> RemoteryRecording:
        """Start background recording and return a session stopped by `stop()`.

        This is useful when a Python script drives gameplay and does not know in
        advance how many frames the interesting moment will take. Do not call
        `get_frame()`, `get_properties()`, or `capture()` on the same client
        while a recording is running.
        """
        return RemoteryRecording(
            self,
            warmup_frames=warmup_frames,
            thread=thread,
            include_properties=include_properties,
            resolve_names=resolve_names,
            read_timeout=read_timeout,
            max_frames=max_frames,
            close_on_stop=close_on_stop,
            on_finalize=on_finalize,
            on_abort=on_abort,
        ).start()

    def get_properties(
        self,
        timeout: Optional[float] = None,
        resolve_names: bool = True,
    ) -> RemoteryPropertyFrame:
        """Read and return one profiler property/counter snapshot."""
        deadline = _deadline(self.timeout if timeout is None else timeout)
        while True:
            message_id, body = self._next_message(deadline)
            if message_id != "PSNP":
                continue

            properties = parse_property_frame(body, self._sample_names)
            if resolve_names:
                properties = self._resolve_property_names(properties, deadline)
            return properties

    def capture(
        self,
        frames: int = 300,
        warmup_frames: int = 0,
        timeout: Optional[float] = None,
        thread: Optional[str] = None,
        include_properties: bool = True,
        resolve_names: bool = True,
    ) -> RemoteryCapture:
        """Capture sample frames and property snapshots for aggregate analysis."""
        if frames <= 0:
            raise ValueError("frames must be positive")
        if warmup_frames < 0:
            raise ValueError("warmup_frames must be non-negative")

        deadline = _deadline(self.timeout if timeout is None else timeout)
        accepted_frames = 0
        sample_frames: List[RemoteryFrame] = []
        property_frames: List[RemoteryPropertyFrame] = []

        while len(sample_frames) < frames:
            message_id, body = self._next_message(deadline)
            if message_id == "SMPL":
                frame = parse_sample_frame(body, self._sample_names)
                if thread is not None and frame.thread_name != thread:
                    continue
                if resolve_names:
                    frame = self._resolve_frame_names(frame, deadline)
                if accepted_frames >= warmup_frames:
                    sample_frames.append(frame)
                accepted_frames += 1
            elif message_id == "PSNP" and include_properties and accepted_frames >= warmup_frames:
                properties = parse_property_frame(body, self._sample_names)
                if resolve_names:
                    properties = self._resolve_property_names(properties, deadline)
                property_frames.append(properties)

        return RemoteryCapture(frames=tuple(sample_frames), property_frames=tuple(property_frames))

    def _resolve_frame_names(self, frame: RemoteryFrame, deadline: Optional[float]) -> RemoteryFrame:
        missing = {name_hash for name_hash in frame.missing_name_hashes() if name_hash not in self._sample_names}
        for name_hash in missing:
            self._send_text(f"GSMP{name_hash}")

        while missing:
            try:
                message_id, body = self._read_message(deadline)
            except RemoteryTimeoutError:
                break

            if message_id == "SSMP":
                missing.difference_update(self._sample_names.keys())
            elif message_id == "PING":
                continue
            else:
                self._pending_messages.append((message_id, body))

        return frame.resolve_names(self._sample_names)

    def _resolve_property_names(
        self,
        properties: RemoteryPropertyFrame,
        deadline: Optional[float],
    ) -> RemoteryPropertyFrame:
        missing = {name_hash for name_hash in properties.missing_name_hashes() if name_hash not in self._sample_names}
        for name_hash in missing:
            self._send_text(f"GSMP{name_hash}")

        while missing:
            try:
                message_id, body = self._read_message(deadline)
            except RemoteryTimeoutError:
                break

            if message_id == "SSMP":
                missing.difference_update(self._sample_names.keys())
            elif message_id == "PING":
                continue
            else:
                self._pending_messages.append((message_id, body))

        return properties.resolve_names(self._sample_names)

    def _next_message(self, deadline: Optional[float]) -> Tuple[str, bytes]:
        if self._pending_messages:
            return self._pending_messages.pop(0)
        return self._read_message(deadline)

    def _read_message(self, deadline: Optional[float]) -> Tuple[str, bytes]:
        payload = self._recv_frame(deadline)
        message_id, body = parse_message(payload)
        if message_id == "SSMP":
            name_hash, name = parse_sample_name(body)
            self._sample_names[name_hash] = name
        return message_id, body

    def _handshake(self) -> None:
        sock = self._require_socket()
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        ).encode("ascii")
        try:
            sock.sendall(request)
        except OSError as exc:
            raise RemoteryError(f"failed to send profiler stream handshake: {exc}") from exc

        response = self._recv_until(b"\r\n\r\n", _deadline(self.timeout)).decode("iso-8859-1", "replace")
        lines = response.split("\r\n")
        if not lines or " 101 " not in lines[0]:
            first_line = lines[0] if lines else ""
            raise RemoteryProtocolError(f"websocket handshake failed: {first_line}")

        headers = {}
        for line in lines[1:]:
            if ":" not in line:
                continue
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()

        expected = base64.b64encode(hashlib.sha1((key + _WEBSOCKET_GUID).encode("ascii")).digest()).decode("ascii")
        if headers.get("sec-websocket-accept") != expected:
            raise RemoteryProtocolError("websocket handshake returned an invalid Sec-WebSocket-Accept header")

    def _recv_frame(self, deadline: Optional[float]) -> bytes:
        while True:
            header = self._recv_exact(2, deadline)
            fin = (header[0] & 0x80) != 0
            opcode = header[0] & 0x0F
            masked = (header[1] & 0x80) != 0
            length = header[1] & 0x7F

            if length == 126:
                length = struct.unpack("!H", self._recv_exact(2, deadline))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._recv_exact(8, deadline))[0]

            mask = self._recv_exact(4, deadline) if masked else b""
            payload = self._recv_exact(length, deadline) if length else b""
            if masked:
                payload = bytes(byte ^ mask[index & 3] for index, byte in enumerate(payload))

            if opcode == 0x8:
                self.stop()
                raise RemoteryProtocolError("profiler stream was closed by the engine")
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode not in (0x1, 0x2):
                raise RemoteryProtocolError(f"unsupported websocket opcode: {opcode}")
            if not fin:
                raise RemoteryProtocolError("fragmented websocket frames are not supported")
            return payload

    def _send_text(self, text: str) -> None:
        try:
            self._send_frame(0x1, text.encode("utf-8"))
        except OSError as exc:
            raise RemoteryError(f"failed to send profiler stream data: {exc}") from exc

    def _send_frame(self, opcode: int, payload: bytes, sock: Optional[socket.socket] = None) -> None:
        target = self._require_socket() if sock is None else sock
        length = len(payload)
        if length <= 125:
            header = bytes([0x80 | opcode, 0x80 | length])
        elif length <= 0xFFFF:
            header = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack("!H", length)
        else:
            header = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack("!Q", length)

        mask = os.urandom(4)
        masked_payload = bytes(byte ^ mask[index & 3] for index, byte in enumerate(payload))
        target.sendall(header + mask + masked_payload)

    def _recv_until(self, marker: bytes, deadline: Optional[float]) -> bytes:
        data = bytearray()
        while marker not in data:
            if len(data) > 65536:
                raise RemoteryProtocolError("websocket handshake response was too large")
            data.extend(self._recv_exact(1, deadline))
        return bytes(data)

    def _recv_exact(self, size: int, deadline: Optional[float]) -> bytes:
        sock = self._require_socket()
        data = bytearray()
        while len(data) < size:
            remaining = _remaining(deadline)
            if remaining is not None and remaining <= 0:
                raise RemoteryTimeoutError("timed out waiting for profiler stream data")
            sock.settimeout(remaining)
            try:
                chunk = sock.recv(size - len(data))
            except socket.timeout as exc:
                raise RemoteryTimeoutError("timed out waiting for profiler stream data") from exc
            if not chunk:
                raise RemoteryProtocolError("profiler stream disconnected")
            data.extend(chunk)
        return bytes(data)

    def _require_socket(self) -> socket.socket:
        if self._socket is None:
            raise RemoteryError("profiler stream is not started; call start() first")
        return self._socket


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def align(self, size: int) -> None:
        self.offset += (size - (self.offset % size)) % size

    def read_bytes(self, size: int, label: str) -> bytes:
        end = self.offset + size
        if end > len(self.data):
            raise RemoteryProtocolError(f"truncated profiler data while reading {label}")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def read_string(self, label: str) -> str:
        size = self.read_u32(f"{label} length")
        return self.read_bytes(size, label).decode("utf-8", "replace")

    def read_u8(self, label: str) -> int:
        return self.read_bytes(1, label)[0]

    def read_u32(self, label: str) -> int:
        return struct.unpack_from("<I", self.read_bytes(4, label))[0]

    def read_s64(self, label: str) -> int:
        return struct.unpack_from("<q", self.read_bytes(8, label))[0]

    def read_u64(self, label: str) -> int:
        return struct.unpack_from("<Q", self.read_bytes(8, label))[0]

    def read_f64(self, label: str) -> float:
        return struct.unpack_from("<d", self.read_bytes(8, label))[0]


def _read_integer_f64(reader: _Reader, label: str) -> int:
    value = reader.read_f64(label)
    if not math.isfinite(value) or not value.is_integer():
        raise RemoteryProtocolError(f"invalid integer value for {label}: {value!r}")
    return int(value)


def parse_message(data: bytes) -> Tuple[str, bytes]:
    """Parse one Remotery websocket payload into `(message_id, body)`."""
    if len(data) < 8:
        raise RemoteryProtocolError("truncated Remotery message header")
    message_id = data[:4].decode("ascii", "replace")
    message_length = struct.unpack_from("<I", data, 4)[0]
    if message_length < 8:
        raise RemoteryProtocolError(f"invalid Remotery message length: {message_length}")
    if message_length > len(data):
        raise RemoteryProtocolError(
            f"truncated Remotery message {message_id}: expected {message_length} bytes, got {len(data)}"
        )
    return message_id, data[8:message_length]


def parse_sample_name(data: bytes) -> Tuple[int, str]:
    """Parse one Remotery `SSMP` sample-name body."""
    reader = _Reader(data)
    name_hash = reader.read_u32("sample name hash")
    name = reader.read_string("sample name")
    return name_hash, name


def parse_sample_frame(data: bytes, sample_names: Optional[Mapping[int, str]] = None) -> RemoteryFrame:
    """Parse one Remotery `SMPL` sample-tree body."""
    names = sample_names or {}
    reader = _Reader(data)
    thread_name = reader.read_string("thread name")
    sample_count = reader.read_u32("sample count")
    partial = reader.read_u32("partial tree") != 0
    if sample_count <= 0:
        raise RemoteryProtocolError("Remotery sample frame did not contain any samples")

    reader.align(4)
    root = _read_sample(reader, names)
    parsed_count = sum(1 for _ in root.walk())
    if parsed_count != sample_count:
        raise RemoteryProtocolError(f"Remotery sample count mismatch: header={sample_count} parsed={parsed_count}")
    if reader.offset + _padding_size(reader.offset, 4) > len(data):
        raise RemoteryProtocolError("truncated Remotery sample padding")
    return RemoteryFrame(thread_name=thread_name, root=root, partial=partial)


def parse_property_frame(
    data: bytes,
    sample_names: Optional[Mapping[int, str]] = None,
) -> RemoteryPropertyFrame:
    """Parse one Remotery `PSNP` property/counter snapshot body."""
    names = sample_names or {}
    reader = _Reader(data)
    property_count = reader.read_u32("property count")
    property_frame = reader.read_u32("property frame")
    properties = []
    for _ in range(property_count):
        properties.append(_read_property(reader, names))
    return RemoteryPropertyFrame(property_frame=property_frame, properties=tuple(properties))


def build_message(message_id: str, body: bytes = b"") -> bytes:
    """Build a Remotery message payload. Intended for tests and fixtures."""
    if len(message_id) != 4:
        raise ValueError("Remotery message IDs are four bytes")
    message_length = 8 + len(body)
    message_length += _padding_size(message_length, 4)
    payload = message_id.encode("ascii") + struct.pack("<I", message_length) + body
    return payload + (b"\x00" * (message_length - len(payload)))


def build_sample_name(name_hash: int, name: str) -> bytes:
    """Build a Remotery `SSMP` body. Intended for tests and fixtures."""
    encoded = name.encode("utf-8")
    return struct.pack("<II", name_hash, len(encoded)) + encoded


def _read_sample(reader: _Reader, names: Mapping[int, str]) -> RemoterySample:
    name_hash = reader.read_u32("sample name hash")
    unique_id = reader.read_u32("sample unique id")
    colour = (
        reader.read_u8("sample red"),
        reader.read_u8("sample green"),
        reader.read_u8("sample blue"),
    )
    depth = reader.read_u8("sample depth")
    start_us = _read_integer_f64(reader, "sample start")
    duration_us = _read_integer_f64(reader, "sample duration")
    self_us = _read_integer_f64(reader, "sample self time")
    gpu_to_cpu_us = _read_integer_f64(reader, "sample gpu-to-cpu time")
    call_count = reader.read_u32("sample call count")
    max_recursion_depth = reader.read_u32("sample max recursion depth")
    child_count = reader.read_u32("sample child count")
    children = tuple(_read_sample(reader, names) for _ in range(child_count))
    return RemoterySample(
        name_hash=name_hash,
        name=names.get(name_hash),
        unique_id=unique_id,
        colour=colour,
        depth=depth,
        start_us=start_us,
        duration_us=duration_us,
        self_us=self_us,
        gpu_to_cpu_us=gpu_to_cpu_us,
        call_count=call_count,
        max_recursion_depth=max_recursion_depth,
        children=children,
    )


def _read_property(reader: _Reader, names: Mapping[int, str]) -> RemoteryProperty:
    name_hash = reader.read_u32("property name hash")
    unique_id = reader.read_u32("property unique id")
    reader.read_bytes(3, "property colour")
    depth = reader.read_u8("property depth")
    property_type = reader.read_u32("property type")
    type_name = _PROPERTY_TYPES.get(property_type)
    if type_name is None:
        raise RemoteryProtocolError(f"unknown Remotery property type: {property_type}")

    value, previous_value = _read_property_values(reader, property_type)
    previous_value_frame = reader.read_u32("property previous value frame")
    child_count = reader.read_u32("property child count")
    return RemoteryProperty(
        name_hash=name_hash,
        name=names.get(name_hash),
        unique_id=unique_id,
        depth=depth,
        type=type_name,
        value=value,
        previous_value=previous_value,
        previous_value_frame=previous_value_frame,
        child_count=child_count,
    )


def _read_property_values(reader: _Reader, property_type: int) -> Tuple[NumericValue, NumericValue]:
    if property_type == 0:
        reader.read_bytes(16, "group property value")
        return 0, 0
    if property_type == 1:
        return bool(reader.read_f64("bool property value")), bool(reader.read_f64("bool previous property value"))
    if property_type in (2, 3):
        return _read_integer_f64(reader, "integer property value"), _read_integer_f64(
            reader, "integer previous property value"
        )
    if property_type in (4, 7):
        return reader.read_f64("float property value"), reader.read_f64("float previous property value")
    if property_type == 5:
        return _read_integer_f64(reader, "s64 property value"), _read_integer_f64(
            reader, "s64 previous property value"
        )
    if property_type == 6:
        return _read_integer_f64(reader, "u64 property value"), _read_integer_f64(
            reader, "u64 previous property value"
        )
    raise RemoteryProtocolError(f"unknown Remotery property type: {property_type}")


def _resolve_sample_names(sample: RemoterySample, names: Mapping[int, str]) -> RemoterySample:
    return RemoterySample(
        name_hash=sample.name_hash,
        name=names.get(sample.name_hash, sample.name),
        unique_id=sample.unique_id,
        colour=sample.colour,
        depth=sample.depth,
        start_us=sample.start_us,
        duration_us=sample.duration_us,
        self_us=sample.self_us,
        gpu_to_cpu_us=sample.gpu_to_cpu_us,
        call_count=sample.call_count,
        max_recursion_depth=sample.max_recursion_depth,
        children=tuple(_resolve_sample_names(child, names) for child in sample.children),
    )


def _resolve_property_name(prop: RemoteryProperty, names: Mapping[int, str]) -> RemoteryProperty:
    return RemoteryProperty(
        name_hash=prop.name_hash,
        name=names.get(prop.name_hash, prop.name),
        unique_id=prop.unique_id,
        depth=prop.depth,
        type=prop.type,
        value=prop.value,
        previous_value=prop.previous_value,
        previous_value_frame=prop.previous_value_frame,
        child_count=prop.child_count,
    )


def _walk_samples_with_paths(
    sample: RemoterySample,
    parents: Tuple[str, ...] = (),
) -> Iterator[Tuple[str, RemoterySample]]:
    path_parts = parents + (sample.label,)
    yield "/".join(path_parts), sample
    for child in sample.children:
        yield from _walk_samples_with_paths(child, path_parts)


def _walk_properties_with_paths(properties: Sequence[RemoteryProperty]) -> Iterator[Tuple[str, RemoteryProperty]]:
    stack: List[str] = []
    for prop in properties:
        del stack[prop.depth :]
        stack.append(prop.label)
        yield "/".join(stack), prop


def _matches_sample(
    sample: RemoterySample,
    path_value: str,
    name: Optional[str],
    path: Optional[str],
    contains: Optional[str],
    pattern: Optional[re.Pattern],
    case_sensitive: bool,
) -> bool:
    if name is not None and sample.label != name:
        return False
    if path is not None and not _matches_path(path_value, path):
        return False
    if contains is not None and not _matches_contains(path_value, sample.label, contains, case_sensitive):
        return False
    if pattern is not None and pattern.search(path_value) is None and pattern.search(sample.label) is None:
        return False
    return True


def _matches_property(
    prop: RemoteryProperty,
    path_value: str,
    name: Optional[str],
    path: Optional[str],
    contains: Optional[str],
    pattern: Optional[re.Pattern],
    case_sensitive: bool,
) -> bool:
    if name is not None and prop.label != name:
        return False
    if path is not None and not _matches_path(path_value, path):
        return False
    if contains is not None and not _matches_contains(path_value, prop.label, contains, case_sensitive):
        return False
    if pattern is not None and pattern.search(path_value) is None and pattern.search(prop.label) is None:
        return False
    return True


def _matches_contains(path_value: str, label: str, text: str, case_sensitive: bool) -> bool:
    if case_sensitive:
        return text in path_value or text in label
    needle = text.lower()
    return needle in path_value.lower() or needle in label.lower()


def _matches_path(value: str, selector: str) -> bool:
    if "*" in selector or "?" in selector or "[" in selector:
        return fnmatch.fnmatchcase(value, selector)
    return value == selector


def _compile_regex(regex: Optional[str], case_sensitive: bool) -> Optional[re.Pattern]:
    if regex is None:
        return None
    flags = 0 if case_sensitive else re.IGNORECASE
    return re.compile(regex, flags)


def _scope_stats_from_entry(entry: Mapping[str, object]) -> RemoteryScopeStats:
    frames = entry["frames"]
    total_values = entry["total_values"]
    self_values = entry["self_values"]
    if not isinstance(frames, set) or not isinstance(total_values, list) or not isinstance(self_values, list):
        raise RemoteryError("invalid internal scope stats entry")
    return RemoteryScopeStats(
        thread_name=str(entry["thread_name"]),
        path=str(entry["path"]),
        name=str(entry["name"]),
        name_hash=int(entry["name_hash"]),
        frames_seen=len(frames),
        occurrences=len(total_values),
        calls_total=int(entry["calls_total"]),
        total=_timing_stats(total_values),
        self=_timing_stats(self_values),
    )


def _counter_stats_from_entry(entry: Mapping[str, object]) -> RemoteryCounterStats:
    frame_ids = entry["frame_ids"]
    values = entry["values"]
    if not isinstance(frame_ids, set) or not isinstance(values, list):
        raise RemoteryError("invalid internal counter stats entry")
    return RemoteryCounterStats(
        path=str(entry["path"]),
        name=str(entry["name"]),
        name_hash=int(entry["name_hash"]),
        type=str(entry["type"]),
        frames_seen=len(frame_ids),
        last_value=entry["last_value"],  # type: ignore[arg-type]
        values=_value_stats(values),
    )


def _timing_stats(values: Sequence[int]) -> RemoteryTimingStats:
    if not values:
        return RemoteryTimingStats(0, 0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0)
    sorted_values = sorted(values)
    total = sum(sorted_values)
    return RemoteryTimingStats(
        count=len(sorted_values),
        total_us=total,
        min_us=sorted_values[0],
        max_us=sorted_values[-1],
        avg_us=total / len(sorted_values),
        median_us=_percentile(sorted_values, 50),
        p90_us=_percentile(sorted_values, 90),
        p95_us=_percentile(sorted_values, 95),
        p99_us=_percentile(sorted_values, 99),
    )


def _value_stats(values: Sequence[float]) -> RemoteryValueStats:
    if not values:
        return RemoteryValueStats(0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
    sorted_values = sorted(values)
    total = sum(sorted_values)
    return RemoteryValueStats(
        count=len(sorted_values),
        total=total,
        min=sorted_values[0],
        max=sorted_values[-1],
        avg=total / len(sorted_values),
        median=_percentile(sorted_values, 50),
        p90=_percentile(sorted_values, 90),
        p95=_percentile(sorted_values, 95),
        p99=_percentile(sorted_values, 99),
    )


def _percentile(values: Sequence[Union[int, float]], percentile: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    position = (len(values) - 1) * percentile / 100.0
    lower_index = int(position)
    upper_index = min(lower_index + 1, len(values) - 1)
    fraction = position - lower_index
    return float(values[lower_index]) + (float(values[upper_index]) - float(values[lower_index])) * fraction


def _scope_sort_value(stats: RemoteryScopeStats, sort: str) -> float:
    sort_keys = {
        "total_ms": stats.total.total_ms,
        "self_ms": stats.self.total_ms,
        "total_avg_ms": stats.total.avg_ms,
        "self_avg_ms": stats.self.avg_ms,
        "total_median_ms": stats.total.median_ms,
        "self_median_ms": stats.self.median_ms,
        "total_p90_ms": stats.total.p90_ms,
        "self_p90_ms": stats.self.p90_ms,
        "total_p95_ms": stats.total.p95_ms,
        "self_p95_ms": stats.self.p95_ms,
        "total_p99_ms": stats.total.p99_ms,
        "self_p99_ms": stats.self.p99_ms,
        "max_ms": stats.total.max_ms,
        "self_max_ms": stats.self.max_ms,
        "calls": float(stats.calls_total),
        "calls_avg": stats.calls_avg,
        "occurrences": float(stats.occurrences),
        "frames": float(stats.frames_seen),
    }
    if sort not in sort_keys:
        raise RemoteryError(f"unknown scope sort key: {sort!r}")
    return sort_keys[sort]


def _counter_sort_value(stats: RemoteryCounterStats, sort: str) -> float:
    sort_keys = {
        "avg": stats.values.avg,
        "median": stats.values.median,
        "p90": stats.values.p90,
        "p95": stats.values.p95,
        "p99": stats.values.p99,
        "min": stats.values.min,
        "max": stats.values.max,
        "last": float(stats.last_value),
        "frames": float(stats.frames_seen),
    }
    if sort not in sort_keys:
        raise RemoteryError(f"unknown counter sort key: {sort!r}")
    return sort_keys[sort]


def _deadline(timeout: Optional[float]) -> Optional[float]:
    if timeout is None:
        return None
    return time.monotonic() + max(0.0, timeout)


def _remaining(deadline: Optional[float]) -> Optional[float]:
    if deadline is None:
        return None
    return max(0.0, deadline - time.monotonic())


def _padding_size(offset: int, alignment: int) -> int:
    return (alignment - (offset % alignment)) % alignment
