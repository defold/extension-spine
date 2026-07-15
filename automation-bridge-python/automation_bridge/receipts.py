"""Typed receipts for rendered captures and transient scene observations."""

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Mapping, Optional

from .nodes import Element


@dataclass(frozen=True)
class ScreenshotReceipt:
    """Atomic screenshot completion state returned by the native bridge.

    The object is path-like for compatibility with callers that previously
    received a :class:`pathlib.Path` from ``bridge.screenshot()``.
    """

    raw: Mapping[str, Any]

    @property
    def capture_id(self) -> int:
        return int(self.raw.get("capture_id", 0))

    @property
    def state(self) -> str:
        return str(self.raw.get("state", "unknown"))

    @property
    def path(self) -> Path:
        return Path(str(self.raw.get("path", "")))

    @property
    def frame(self) -> int:
        return int(self.raw.get("engine_frame", 0))

    @property
    def scene_sequence(self) -> int:
        return int(self.raw.get("scene_sequence", 0))

    @property
    def width(self) -> int:
        return int(self.raw.get("width", 0))

    @property
    def height(self) -> int:
        return int(self.raw.get("height", 0))

    @property
    def sha256(self) -> Optional[str]:
        value = self.raw.get("sha256")
        return str(value) if value else None

    @property
    def failure_reason(self) -> Optional[str]:
        value = self.raw.get("failure_reason")
        return str(value) if value else None

    def __fspath__(self) -> str:
        return str(self.path)

    def __str__(self) -> str:
        return str(self.path)

    def exists(self) -> bool:
        return self.path.exists()

    def stat(self):
        return self.path.stat()

    def read_bytes(self) -> bytes:
        return self.path.read_bytes()


@dataclass(frozen=True)
class ObservationReceipt:
    """Frame/sequence evidence collected while observing a transient element."""

    element: Optional[Element]
    identity: str
    first_frame: int
    last_frame: int
    first_scene_sequence: int
    last_scene_sequence: int
    observed_frames: int
    disappeared_frame: Optional[int] = None
    disappeared_scene_sequence: Optional[int] = None

    def as_dict(self) -> Dict[str, Any]:
        """Return a JSON-serializable form suitable for trace output."""
        return {
            "element": self.element.raw if self.element else None,
            "identity": self.identity,
            "first_frame": self.first_frame,
            "last_frame": self.last_frame,
            "first_scene_sequence": self.first_scene_sequence,
            "last_scene_sequence": self.last_scene_sequence,
            "observed_frames": self.observed_frames,
            "disappeared_frame": self.disappeared_frame,
            "disappeared_scene_sequence": self.disappeared_scene_sequence,
        }
