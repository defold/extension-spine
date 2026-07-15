"""Structured Automation Bridge diagnostic traces and best-effort replay."""

import hashlib
import json
import os
import shutil
import time
from dataclasses import asdict, is_dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Union


TRACE_VERSION = 1
REPLAY_PREREQUISITES = ("application_state", "random_seeds", "timing_mode", "external_services")


def _json_value(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Path):
        return str(value)
    if is_dataclass(value):
        return _json_value(asdict(value))
    if isinstance(value, Mapping):
        return {str(key): _json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_json_value(item) for item in value]
    if hasattr(value, "raw"):
        return _json_value(value.raw)
    if hasattr(value, "__dict__"):
        return _json_value(vars(value))
    return repr(value)


def _find_metadata(value: Any, name: str) -> Any:
    if isinstance(value, Mapping):
        if name in value:
            return value[name]
        for item in value.values():
            found = _find_metadata(item, name)
            if found is not None:
                return found
    elif isinstance(value, list):
        for item in value:
            found = _find_metadata(item, name)
            if found is not None:
                return found
    return None


class TraceError(RuntimeError):
    """Raised for invalid trace configuration or failed finalization."""


class TraceSession:
    """Context manager that records diagnostics around client API operations.

    Replay is explicitly best effort. Exact reproduction requires controlled
    application state, random seeds, timing mode, and external services.
    """

    def __init__(
        self,
        client: Any,
        path: Union[str, Path],
        *,
        screenshots: str = "on_error",
        screenshot_directory: Optional[Union[str, Path]] = None,
        prerequisites: Optional[Mapping[str, Any]] = None,
    ):
        if screenshots not in ("never", "on_error", "always"):
            raise ValueError("screenshots must be 'never', 'on_error', or 'always'")
        self.client = client
        self.path = Path(path)
        self.screenshots = screenshots
        self.screenshot_directory = Path(screenshot_directory) if screenshot_directory else self.path.parent
        self._start_monotonic = time.monotonic()
        self._active = False
        self._suspended = False
        self._closed = False
        supplied = dict(prerequisites or {})
        self.data: Dict[str, Any] = {
            "format": "automation-bridge-trace",
            "version": TRACE_VERSION,
            "replay": {
                "mode": "best-effort",
                "prerequisites": {name: supplied.get(name) for name in REPLAY_PREREQUISITES},
                "warning": "Deterministic replay requires application state, random seeds, timing mode, and external services to be recorded and restored.",
            },
            "started": {"wall_time": time.time(), "monotonic": self._start_monotonic},
            "initial": {},
            "timeline": [],
            "screenshots": [],
            "cleanup": [],
        }

    def __enter__(self) -> "TraceSession":
        if self._active or self._closed:
            raise TraceError("trace sessions cannot be re-entered")
        self._active = True
        traces = getattr(self.client, "_active_traces", None)
        if traces is None:
            traces = []
            self.client._active_traces = traces
        traces.append(self)
        self._capture_initial()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> bool:
        original_exception = exc_type is not None
        try:
            if original_exception:
                self.record("interruption" if issubclass(exc_type, (KeyboardInterrupt, SystemExit)) else "exception", {
                    "type": exc_type.__name__, "message": str(exc),
                })
            if self.screenshots == "always" or (self.screenshots == "on_error" and original_exception):
                self.capture_screenshot("error" if original_exception else "final")
            self.record_cleanup("trace_detach", "started")
        except BaseException as capture_error:
            self.record_cleanup("diagnostic_capture", "failed", error=repr(capture_error))
        finally:
            traces = getattr(self.client, "_active_traces", [])
            if self in traces:
                traces.remove(self)
            self._active = False
            self.record_cleanup("trace_detach", "completed")
        try:
            self.close()
        except BaseException:
            if not original_exception:
                raise
            # Preserve KeyboardInterrupt/workflow exceptions if trace writing
            # itself fails. This mirrors recorder finalization semantics.
        return False

    def _capture_initial(self) -> None:
        self._suspended = True
        try:
            for name, operation in (("health", self.client.health), ("geometry", self.client.screen), ("scene", self.client.scene)):
                try:
                    self.data["initial"][name] = _json_value(operation())
                except BaseException as error:
                    self.data["initial"][name] = {"capture_error": repr(error)}
            health = self.data["initial"].get("health", {})
            if isinstance(health, Mapping):
                self.data["engine_instance_id"] = health.get("engine_instance_id") or health.get("instance_id")
                self.data["api_version"] = health.get("version") or health.get("api_version")
                self.data["capabilities"] = health.get("capabilities", [])
        finally:
            self._suspended = False

    def record(self, kind: str, payload: Any) -> None:
        """Append a timestamped JSON-safe timeline item."""
        if self._suspended or self._closed:
            return
        value = _json_value(payload)
        item = {
            "sequence": len(self.data["timeline"]) + 1,
            "kind": kind,
            "wall_time": time.time(),
            "monotonic": time.monotonic(),
            "elapsed": time.monotonic() - self._start_monotonic,
            "payload": value,
        }
        for name in ("engine_frame", "frame", "scene_sequence", "input_id", "request_id"):
            metadata = _find_metadata(value, name)
            if metadata is not None:
                item[name] = metadata
        self.data["timeline"].append(item)

    def record_event(self, event: Any) -> None:
        """Record an application event payload."""
        self.record("application_event", event)

    def record_state(self, name: str, revision: Any, value: Any) -> None:
        """Record one application-published state revision."""
        self.record("application_state", {"name": name, "revision": revision, "value": value})

    def record_input_acknowledgement(self, acknowledgement: Any) -> None:
        """Record an application acknowledgement for an input/action."""
        self.record("application_acknowledgement", acknowledgement)

    def record_profiler(self, capture: Any, *, label: Optional[str] = None) -> None:
        """Record profiler correlation data or a serializable capture summary."""
        self.record("profiler", {"label": label, "capture": capture})

    def record_selector_error(self, selector: Mapping[str, Any], error: BaseException) -> None:
        """Record a failed selector and its diagnostics."""
        self.record("selector_error", {"selector": selector, "error": str(error), "type": type(error).__name__})

    def record_cleanup(self, operation: str, state: str, **details: Any) -> None:
        """Record interruption/cancellation cleanup progress."""
        self.data["cleanup"].append({
            "operation": operation, "state": state, "wall_time": time.time(),
            "monotonic": time.monotonic(), **_json_value(details),
        })

    def capture_screenshot(self, label: str = "diagnostic") -> Optional[Path]:
        """Capture and fingerprint a screenshot without recursively tracing it."""
        self._suspended = True
        try:
            source = Path(self.client.screenshot(wait=True))
            self.screenshot_directory.mkdir(parents=True, exist_ok=True)
            safe_label = "".join(character if character.isalnum() or character in "-_" else "_" for character in label)
            target = self.screenshot_directory / f"{self.path.stem}.{len(self.data['screenshots']) + 1}.{safe_label}{source.suffix or '.png'}"
            if source.resolve() != target.resolve():
                shutil.copy2(source, target)
            digest = hashlib.sha256(target.read_bytes()).hexdigest()
            entry = {"label": label, "path": str(target), "sha256": digest, "size": target.stat().st_size}
            self.data["screenshots"].append(entry)
            return target
        except BaseException as error:
            self.data["screenshots"].append({"label": label, "capture_error": repr(error)})
            return None
        finally:
            self._suspended = False

    def close(self) -> Path:
        """Atomically finalize the JSON bundle and return its path."""
        if self._closed:
            return self.path
        self.data["stopped"] = {"wall_time": time.time(), "monotonic": time.monotonic(), "duration": time.monotonic() - self._start_monotonic}
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_name(f".{self.path.name}.{os.getpid()}.tmp")
        try:
            temporary.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            os.replace(temporary, self.path)
        except BaseException as error:
            try:
                temporary.unlink()
            except OSError:
                pass
            raise TraceError(f"could not finalize trace {self.path}: {error}") from error
        self._closed = True
        return self.path

    @staticmethod
    def replay(client: Any, path: Union[str, Path], *, require_deterministic: bool = False) -> Mapping[str, Any]:
        """Best-effort replay recorded input requests in timeline order.

        When `require_deterministic=True`, every prerequisite must have a
        truthy recorded value. This helper never claims to restore app/external
        state; callers must do that before invoking it.
        """
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if data.get("format") != "automation-bridge-trace":
            raise TraceError("not an Automation Bridge trace")
        prerequisites = data.get("replay", {}).get("prerequisites", {})
        missing = [name for name in REPLAY_PREREQUISITES if not prerequisites.get(name)]
        if require_deterministic and missing:
            raise TraceError("deterministic replay prerequisites were not recorded: " + ", ".join(missing))
        replayed = 0
        failures: List[Mapping[str, Any]] = []
        for item in data.get("timeline", []):
            payload = item.get("payload", {})
            if item.get("kind") != "action" or not isinstance(payload, Mapping):
                continue
            path_value = payload.get("path")
            method = payload.get("method")
            if method != "POST" or not isinstance(path_value, str) or not path_value.startswith("/input/"):
                continue
            try:
                client.request("POST", path_value, params=payload.get("params"))
                replayed += 1
            except BaseException as error:
                failures.append({"sequence": item.get("sequence"), "error": repr(error)})
        return {"mode": "best-effort", "replayed": replayed, "failures": failures, "missing_prerequisites": missing}
