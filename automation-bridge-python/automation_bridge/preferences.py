"""Typed built-in Defold preference paths plus custom string access."""

import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Mapping, Optional, Union

from ._preferences_catalog import CATALOG
from .client import HttpError


def _immutable(value: Any) -> Any:
    if isinstance(value, list):
        return tuple(_immutable(item) for item in value)
    if isinstance(value, dict):
        return tuple(sorted((str(key), _immutable(item)) for key, item in value.items()))
    return value


@dataclass(frozen=True)
class PreferenceKey:
    name: str
    path: str
    type: str
    scope: str
    default: object
    has_default: bool
    description: str
    group: bool
    enum_values: tuple[object, ...] = ()
    ui: object = None

    def __str__(self) -> str:
        return self.path


BUILTIN_PREFERENCES = tuple(
    PreferenceKey(
        name=item["name"],
        path=item["path"],
        type=item["type"],
        scope=item["scope"],
        default=_immutable(item["default"]),
        has_default=bool(item["has_default"]),
        description=item["description"],
        group=bool(item["group"]),
        enum_values=tuple(_immutable(value) for value in (item.get("enum_values") or ())),
        ui=_immutable(item.get("ui")),
    )
    for item in CATALOG
)
_BY_PATH = {item.path: item for item in BUILTIN_PREFERENCES}


class Preferences:
    """Read and write built-in or editor-script preference paths."""

    def __init__(self, client: Any):
        self._client = client

    @staticmethod
    def _path(preference: Union[str, PreferenceKey]) -> str:
        if isinstance(preference, PreferenceKey):
            return preference.path
        if not isinstance(preference, str) or not preference.strip("/"):
            raise ValueError("preference must be a non-empty path string or PreferenceKey")
        return preference.strip("/")

    def get(self, preference: Union[str, PreferenceKey]) -> object:
        self._client._require_operation("/prefs/{path}", "get")
        path = self._path(preference)
        url = f"{self._client.base_url}/prefs/{urllib.parse.quote(path, safe='/')}"
        request = urllib.request.Request(url, method="GET")
        try:
            with urllib.request.urlopen(request, timeout=10.0) as response:
                body = response.read().decode("utf-8")
                status = response.getcode()
        except urllib.error.HTTPError as exc:
            raise self._preference_error("GET", url, exc.code, exc.read()) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise HttpError("GET", url, str(exc)) from exc
        try:
            return json.loads(body)
        except json.JSONDecodeError as exc:
            raise HttpError("GET", url, "preference response was not JSON", status=status) from exc

    def set(self, preference: Union[str, PreferenceKey], value: object) -> None:
        self._client._require_operation("/prefs/{path}", "post")
        path = self._path(preference)
        url = f"{self._client.base_url}/prefs/{urllib.parse.quote(path, safe='/')}"
        data = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=10.0) as response:
                if response.getcode() < 200 or response.getcode() >= 300:
                    raise self._preference_error("POST", url, response.getcode(), response.read())
        except urllib.error.HTTPError as exc:
            raise self._preference_error("POST", url, exc.code, exc.read()) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise HttpError("POST", url, str(exc)) from exc

    def list(
        self,
        *,
        prefix: Optional[str] = None,
        type: Optional[str] = None,
        scope: Optional[str] = None,
        include_groups: bool = False,
    ) -> tuple[PreferenceKey, ...]:
        normalized_prefix = prefix.strip("/") if prefix else None
        return tuple(
            item
            for item in BUILTIN_PREFERENCES
            if (include_groups or not item.group)
            and (normalized_prefix is None or item.path == normalized_prefix or item.path.startswith(normalized_prefix + "/"))
            and (type is None or item.type == type)
            and (scope is None or item.scope == scope)
        )

    def describe(self, preference: Union[str, PreferenceKey]) -> Optional[PreferenceKey]:
        if isinstance(preference, PreferenceKey):
            return preference
        if not isinstance(preference, str):
            raise TypeError("preference must be a string or PreferenceKey")
        return _BY_PATH.get(preference.strip("/"))

    @staticmethod
    def _preference_error(method: str, url: str, status: int, body: bytes):
        from .editor import PreferenceError

        message = body.decode("utf-8", "replace").strip() or f"HTTP {status}"
        return PreferenceError(f"{method} {url} failed: {message}")


for _preference in BUILTIN_PREFERENCES:
    setattr(Preferences, _preference.name, _preference)


__all__ = ["BUILTIN_PREFERENCES", "PreferenceKey", "Preferences"]
