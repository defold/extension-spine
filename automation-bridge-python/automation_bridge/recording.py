"""Native video recording for the running Defold engine."""

from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional, Tuple, Union


class VideoRecordingError(RuntimeError):
    """Raised when a native recording cannot be started or finalized."""


@dataclass(frozen=True)
class VideoRecordingCapabilities:
    """Capabilities reported by the native recorder in the running engine."""

    backend: str
    available: bool
    application_window: bool
    application_audio: bool
    resize_output: bool
    frame_rate: bool
    containers: Tuple[str, ...]
    video_codecs: Tuple[str, ...]
    minimum_platform_version: Optional[str] = None
    reason: Optional[str] = None

    @classmethod
    def from_raw(cls, raw: Mapping[str, Any]) -> "VideoRecordingCapabilities":
        return cls(
            backend=str(raw.get("backend", "native")),
            available=bool(raw.get("available", False)),
            application_window=bool(raw.get("application_window", False)),
            application_audio=bool(raw.get("application_audio", False)),
            resize_output=bool(raw.get("resize_output", False)),
            frame_rate=bool(raw.get("frame_rate", False)),
            containers=tuple(str(value) for value in raw.get("containers", ())),
            video_codecs=tuple(str(value) for value in raw.get("video_codecs", ())),
            minimum_platform_version=raw.get("minimum_platform_version"),
            reason=raw.get("reason"),
        )


@dataclass(frozen=True)
class VideoRecordingMetadata:
    """Native state and output metadata for one recording."""

    path: Optional[str]
    backend: str
    active: bool
    finalized: bool
    audio: bool
    width: int
    height: int
    fps: int
    video_codec: str
    container: str
    started_wall_time_us: Optional[int] = None
    started_monotonic_time_us: Optional[int] = None
    stopped_wall_time_us: Optional[int] = None
    stopped_monotonic_time_us: Optional[int] = None
    duration_seconds: Optional[float] = None
    failure: Optional[str] = None

    @classmethod
    def from_raw(cls, raw: Mapping[str, Any]) -> "VideoRecordingMetadata":
        return cls(
            path=raw.get("path"),
            backend=str(raw.get("backend", "native")),
            active=bool(raw.get("active", False)),
            finalized=bool(raw.get("finalized", False)),
            audio=bool(raw.get("audio", False)),
            width=int(raw.get("width", 0)),
            height=int(raw.get("height", 0)),
            fps=int(raw.get("fps", 0)),
            video_codec=str(raw.get("video_codec", "h264")),
            container=str(raw.get("container", "mp4")),
            started_wall_time_us=raw.get("started_wall_time_us"),
            started_monotonic_time_us=raw.get("started_monotonic_time_us"),
            stopped_wall_time_us=raw.get("stopped_wall_time_us"),
            stopped_monotonic_time_us=raw.get("stopped_monotonic_time_us"),
            duration_seconds=raw.get("duration_seconds"),
            failure=raw.get("failure"),
        )

    def to_dict(self) -> Dict[str, Any]:
        """Return JSON-serializable metadata."""
        return asdict(self)


class VideoRecordingClient:
    """Control the platform recorder embedded in the running Defold engine."""

    def __init__(self, bridge: Any):
        self.bridge = bridge

    def capabilities(self) -> VideoRecordingCapabilities:
        """Return native recorder availability without starting capture."""
        return VideoRecordingCapabilities.from_raw(
            self.bridge.request("GET", "/recording/capabilities")
        )

    def status(self) -> VideoRecordingMetadata:
        """Return the current or most recently finalized recording state."""
        return VideoRecordingMetadata.from_raw(
            self.bridge.request("GET", "/recording/status")
        )

    def start(
        self,
        path: Union[str, Path],
        *,
        size: Optional[Tuple[int, int]] = None,
        fps: int = 30,
        audio: Optional[bool] = None,
    ) -> "VideoRecordingSession":
        """Start recording the current game window to an H.264 MP4 file.

        The recorder runs inside the engine, selects the largest on-screen
        window owned by the current Defold process, and captures only its
        undecorated game content. Omitted ``audio`` uses the backend default:
        enabled on macOS and disabled by the current Windows implementation.
        """
        if isinstance(fps, bool) or not isinstance(fps, int) or not 1 <= fps <= 60:
            raise ValueError("fps must be an integer between 1 and 60")
        if audio is not None and not isinstance(audio, bool):
            raise TypeError("audio must be a boolean or None")
        params: Dict[str, Any] = {"fps": fps}
        if audio is not None:
            params["audio"] = audio
        if size is not None:
            if (
                not isinstance(size, tuple)
                or len(size) != 2
                or any(isinstance(value, bool) or not isinstance(value, int) for value in size)
                or any(value < 1 or value > 16384 for value in size)
            ):
                raise ValueError("size must be a (width, height) tuple with values between 1 and 16384")
            params.update({"width": size[0], "height": size[1]})

        output = Path(path).expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        params["path"] = str(output)
        raw = self.bridge.request("POST", "/recording/start", json=params)
        metadata = VideoRecordingMetadata.from_raw(raw)
        self.bridge._trace_record("video_recording_started", metadata.to_dict())
        return VideoRecordingSession(self, metadata)


class VideoRecordingSession:
    """A native recording that finalizes its MP4 when stopped or exited."""

    def __init__(self, client: VideoRecordingClient, metadata: VideoRecordingMetadata):
        self.client = client
        self.metadata = metadata

    def __enter__(self) -> "VideoRecordingSession":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> bool:
        try:
            self.stop()
        except BaseException:
            if exc_type is None:
                raise
        return False

    def stop(self) -> VideoRecordingMetadata:
        """Stop capture, finalize the MP4, and return native metadata."""
        if not self.metadata.active:
            return self.metadata
        raw = self.client.bridge.request("POST", "/recording/stop")
        self.metadata = VideoRecordingMetadata.from_raw(raw)
        if not self.metadata.finalized:
            raise VideoRecordingError(self.metadata.failure or "the native recorder did not finalize its MP4 output")
        self.client.bridge._trace_record("video_recording_stopped", self.metadata.to_dict())
        return self.metadata
