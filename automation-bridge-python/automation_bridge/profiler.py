"""Helpers for Defold's built-in engine profiler endpoints."""

from dataclasses import dataclass
from typing import Callable, List, Optional

from .client import AutomationBridgeError, HttpError, request_raw
from . import remotery as _remotery


# Public profiler vocabulary backed by the internal Remotery protocol module.
ProfilerError = _remotery.RemoteryError
ProfilerProtocolError = _remotery.RemoteryProtocolError
ProfilerTimeoutError = _remotery.RemoteryTimeoutError
ProfilerConnection = _remotery.RemoteryClient
ProfilerSampleAggregate = _remotery.RemoterySampleAggregate
ProfilerTimingStats = _remotery.RemoteryTimingStats
ProfilerValueStats = _remotery.RemoteryValueStats
ProfilerCapture = _remotery.RemoteryCapture
ProfilerRecording = _remotery.RemoteryRecording
ProfilerSample = _remotery.RemoterySample
ProfilerFrame = _remotery.RemoteryFrame
ProfilerProperty = _remotery.RemoteryProperty
ProfilerPropertyEntry = _remotery.RemoteryPropertyEntry
ProfilerPropertyFrame = _remotery.RemoteryPropertyFrame
ProfilerScopeStats = _remotery.RemoteryScopeStats
ProfilerCounterStats = _remotery.RemoteryCounterStats

_PROFILER_TYPES = {
    "ProfilerError": ProfilerError,
    "ProfilerProtocolError": ProfilerProtocolError,
    "ProfilerTimeoutError": ProfilerTimeoutError,
    "ProfilerConnection": ProfilerConnection,
    "ProfilerSampleAggregate": ProfilerSampleAggregate,
    "ProfilerTimingStats": ProfilerTimingStats,
    "ProfilerValueStats": ProfilerValueStats,
    "ProfilerCapture": ProfilerCapture,
    "ProfilerRecording": ProfilerRecording,
    "ProfilerSample": ProfilerSample,
    "ProfilerFrame": ProfilerFrame,
    "ProfilerProperty": ProfilerProperty,
    "ProfilerPropertyEntry": ProfilerPropertyEntry,
    "ProfilerPropertyFrame": ProfilerPropertyFrame,
    "ProfilerScopeStats": ProfilerScopeStats,
    "ProfilerCounterStats": ProfilerCounterStats,
}
for _public_name, _type in _PROFILER_TYPES.items():
    _type.__name__ = _public_name
    _type.__qualname__ = _public_name
    _type.__module__ = __name__

__all__ = [
    "ProfilerCapture",
    "ProfilerClient",
    "ProfilerConnection",
    "ProfilerCounterStats",
    "ProfilerDataError",
    "ProfilerError",
    "ProfilerFrame",
    "ProfilerProperty",
    "ProfilerPropertyEntry",
    "ProfilerPropertyFrame",
    "ProfilerProtocolError",
    "ProfilerRecording",
    "ProfilerSample",
    "ProfilerSampleAggregate",
    "ProfilerScopeStats",
    "ProfilerTimeoutError",
    "ProfilerTimingStats",
    "ProfilerValueStats",
    "ResourceProfileEntry",
]


@dataclass(frozen=True)
class ResourceProfileEntry:
    """One resource row returned by Defold's `/resources_data` profiler endpoint."""

    name: str
    type: str
    size: int
    size_on_disc: int
    ref_count: int


class ProfilerDataError(AutomationBridgeError):
    """Raised when a profiler endpoint returns malformed or unexpected binary data."""

    pass


class ProfilerClient:
    """Client for Defold's built-in engine profiler endpoints."""

    def __init__(self, port: int, timeout: float = 10.0, profiler_url: Optional[str] = None):
        """Create a profiler client for an already-known Defold engine service `port`."""
        self.port = int(port)
        self.timeout = timeout
        self.base_url = f"http://127.0.0.1:{self.port}"
        self._remotery_url = profiler_url

    @property
    def url(self) -> Optional[str]:
        """Return the discovered profiler stream URL, if one is available."""
        return self._remotery_url

    def resources(self) -> List[ResourceProfileEntry]:
        """Return loaded resources from Defold's `/resources_data` endpoint, largest first."""
        status, body = request_raw(f"{self.base_url}/resources_data", timeout=self.timeout)
        if status < 200 or status >= 300:
            preview = body[:200].decode("utf-8", "replace")
            raise HttpError(
                "GET",
                f"{self.base_url}/resources_data",
                f"unexpected status {status}: {preview}",
                status=status,
            )
        return sorted(parse_resources_data(body), key=lambda resource: resource.size, reverse=True)

    def connect(
        self,
        port: Optional[int] = None,
        host: Optional[str] = None,
        url: Optional[str] = None,
        path: str = "/rmt",
    ) -> ProfilerConnection:
        """Return a live engine profiler connection.

        By default this uses the profiler URL discovered while bootstrapping
        from editor logs. Pass `url`, or `host`/`port`, to override it.
        """
        if url is not None:
            return ProfilerConnection.from_url(url, timeout=self.timeout)
        if port is None and host is None and self._remotery_url is not None:
            return ProfilerConnection.from_url(self._remotery_url, timeout=self.timeout)
        return ProfilerConnection(
            port=_remotery.DEFAULT_REMOTERY_PORT if port is None else port,
            host="127.0.0.1" if host is None else host,
            path=path,
            timeout=self.timeout,
        )

    def capture(
        self,
        frames: int = 300,
        warmup_frames: int = 0,
        timeout: Optional[float] = None,
        thread: Optional[str] = None,
        include_properties: bool = True,
        resolve_names: bool = True,
    ) -> ProfilerCapture:
        """Capture profiler frames/properties and return aggregate query helpers."""
        with self.connect() as connection:
            return connection.capture(
                frames=frames,
                warmup_frames=warmup_frames,
                timeout=timeout,
                thread=thread,
                include_properties=include_properties,
                resolve_names=resolve_names,
            )

    def start_recording(
        self,
        warmup_frames: int = 0,
        thread: Optional[str] = None,
        include_properties: bool = True,
        resolve_names: bool = True,
        read_timeout: float = 0.25,
        max_frames: Optional[int] = None,
        on_finalize: Optional[Callable[[ProfilerCapture], None]] = None,
        on_abort: Optional[Callable[[BaseException, ProfilerCapture], None]] = None,
    ) -> ProfilerRecording:
        """Start background profiling until the returned session is stopped.

        Use this when a Python automation script drives gameplay and decides at
        runtime when the interesting performance window is complete. The
        returned session's `stop()` method returns a `ProfilerCapture`.
        """
        return self.connect().start_recording(
            warmup_frames=warmup_frames,
            thread=thread,
            include_properties=include_properties,
            resolve_names=resolve_names,
            read_timeout=read_timeout,
            max_frames=max_frames,
            close_on_stop=True,
            on_finalize=on_finalize,
            on_abort=on_abort,
        )


class _BinaryReader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def eof(self) -> bool:
        return self.offset >= len(self.data)

    def read_string(self, label: str) -> str:
        size = self.read_u16(f"{label} length")
        end = self.offset + size
        if end > len(self.data):
            raise ProfilerDataError(f"truncated profiler data while reading {label}")
        value = self.data[self.offset:end]
        self.offset = end
        return value.decode("utf-8", "replace")

    def read_u16(self, label: str) -> int:
        return self._read_int(label, 2)

    def read_u32(self, label: str) -> int:
        return self._read_int(label, 4)

    def _read_int(self, label: str, size: int) -> int:
        end = self.offset + size
        if end > len(self.data):
            raise ProfilerDataError(f"truncated profiler data while reading {label}")
        value = int.from_bytes(self.data[self.offset:end], "little", signed=False)
        self.offset = end
        return value


def parse_resources_data(data: bytes) -> List[ResourceProfileEntry]:
    """Parse the custom binary payload returned by Defold's `/resources_data` endpoint."""
    reader = _BinaryReader(data)
    tag = reader.read_string("resource profiler tag")
    if tag != "RESS":
        raise ProfilerDataError(f"unexpected resource profiler tag: {tag!r}")

    resources: List[ResourceProfileEntry] = []
    while not reader.eof():
        name = reader.read_string("resource name")
        resource_type = reader.read_string("resource type")
        size = reader.read_u32("resource size")
        size_on_disc = reader.read_u32("resource size on disc")
        resources.append(
            ResourceProfileEntry(
                name=name,
                type=resource_type,
                size=size if size > 0 else size_on_disc,
                size_on_disc=size_on_disc,
                ref_count=reader.read_u32("resource reference count"),
            )
        )
    return resources
