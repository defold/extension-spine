"""Typed snapshot wrappers for Automation Bridge scene elements."""

from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, Optional


@dataclass(frozen=True)
class Bounds:
    """Screen, center, and normalized bounds for a runtime element snapshot."""

    raw: Mapping[str, Any]

    @property
    def screen(self) -> Mapping[str, Any]:
        return self.raw.get("screen", {})

    @property
    def center(self) -> Mapping[str, Any]:
        return self.raw.get("center", {})

    @property
    def normalized(self) -> Mapping[str, Any]:
        return self.raw.get("normalized", {})

    @property
    def x(self) -> Optional[float]:
        return self.screen.get("x")

    @property
    def y(self) -> Optional[float]:
        return self.screen.get("y")

    @property
    def w(self) -> Optional[float]:
        return self.screen.get("w")

    @property
    def h(self) -> Optional[float]:
        return self.screen.get("h")


@dataclass(frozen=True)
class Element:
    """Snapshot of one inspectable game object, component, or GUI element."""

    raw: Dict[str, Any]

    @property
    def id(self) -> str:
        return self.raw.get("id", "")

    @property
    def snapshot_id(self) -> str:
        """Return the path-derived identity for this particular scene shape."""
        return self.raw.get("snapshot_id", self.id)

    @property
    def instance_id(self) -> Optional[str]:
        """Return Defold's instance identifier when this element has an HInstance."""
        return self.raw.get("instance_id")

    @property
    def instance_generation(self) -> Optional[int]:
        """Return Defold's allocation generation for the backing instance."""
        value = self.raw.get("instance_generation")
        return int(value) if isinstance(value, (int, float)) else None

    @property
    def logical_id(self) -> Optional[str]:
        """Return the bridge identity combining instance identifier and generation."""
        return self.raw.get("logical_id")

    @property
    def created_scene_sequence(self) -> Optional[int]:
        value = self.raw.get("created_scene_sequence")
        return int(value) if isinstance(value, (int, float)) else None

    @property
    def scene_sequence(self) -> int:
        return int(self.raw.get("scene_sequence", 0))

    @property
    def engine_frame(self) -> int:
        return int(self.raw.get("engine_frame", 0))

    @property
    def name(self) -> str:
        return self.raw.get("name", "")

    @property
    def type(self) -> str:
        return self.raw.get("type", "")

    @property
    def kind(self) -> str:
        return self.raw.get("kind", "")

    @property
    def path(self) -> str:
        return self.raw.get("path", "")

    @property
    def parent_id(self) -> Optional[str]:
        """Return the parent element id, if this snapshot has one."""
        return self.raw.get("parent")

    @property
    def text(self) -> Optional[str]:
        return self.raw.get("text")

    @property
    def url(self) -> Optional[str]:
        return self.raw.get("url")

    @property
    def automation_id(self) -> Optional[str]:
        """Return the stable application-supplied automation id, if annotated."""
        return self.raw.get("automation_id")

    @property
    def localization_key(self) -> Optional[str]:
        """Return the application-supplied localization key, if annotated."""
        return self.raw.get("localization_key")

    @property
    def role(self) -> Optional[str]:
        """Return the application-supplied semantic role, if annotated."""
        return self.raw.get("role")

    @property
    def visible(self) -> bool:
        return bool(self.raw.get("visible"))

    @property
    def enabled(self) -> bool:
        return bool(self.raw.get("enabled"))

    @property
    def bounds(self) -> Optional[Bounds]:
        bounds = self.raw.get("bounds")
        if not isinstance(bounds, dict):
            return None
        return Bounds(bounds)

    @property
    def center(self) -> Optional[Mapping[str, Any]]:
        bounds = self.bounds
        if not bounds:
            return None
        return bounds.center

    @property
    def children(self) -> List["Element"]:
        children = self.raw.get("children", [])
        if not isinstance(children, list):
            return []
        return [Element(child) for child in children if isinstance(child, dict)]

    def compact(self) -> str:
        """Return a one-line diagnostic summary for selector errors and logs."""
        center = self.center
        center_text = ""
        if center and "x" in center and "y" in center:
            center_text = f" center=({center['x']},{center['y']})"
        text = f" text={self.text!r}" if self.text is not None else ""
        return (
            f"id={self.id!r} logical_id={self.logical_id!r} name={self.name!r} type={self.type!r}{text} "
            f"automation_id={self.automation_id!r} "
            f"path={self.path!r} visible={self.visible} enabled={self.enabled}{center_text}"
        )
