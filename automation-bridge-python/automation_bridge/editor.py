"""Public editor discovery, OpenAPI wrappers, and engine bootstrap."""

from __future__ import annotations

import configparser
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator, Mapping, Optional, Sequence, TYPE_CHECKING, Union

from .client import AutomationBridgeError, HttpError, request_json, request_raw
from .preferences import PreferenceKey, Preferences
from .waits import wait_until

if TYPE_CHECKING:
    from .client import Client as EngineClient


_AUTOMATION_BRIDGE_ENDPOINT_TEXT = "Automation Bridge endpoint registered"
_ENGINE_SERVICE_PORT_PATTERNS = (
    re.compile(r"Engine service started on port (\d+)"),
    re.compile(r"Log server started on port (\d+)"),
)
_REMOTERY_URL_PATTERN = re.compile(r"Initialized Remotery \((ws://[^)\s]+)\)")
_SUPPORTED_COMMANDS = frozenset({
    "build", "clean-build", "build-html5", "fetch-libraries", "hot-reload",
    "rebundle", "reload-extensions", "reload-stylesheets", "debugger-start",
    "debugger-stop", "debugger-break", "debugger-continue", "debugger-detach",
    "debugger-step-into", "debugger-step-out", "debugger-step-over",
})
_SUPPORTED_PATHS = frozenset({
    ("/command/{command}", "post"),
    ("/console", "get"),
    ("/console/stream", "get"),
    ("/prefs/{path}", "get"),
    ("/prefs/{path}", "post"),
    ("/preview/{path}", "get"),
    ("/ref", "get"),
})
_EXCLUDED_COMMANDS = frozenset({
    "asset-portal", "documentation", "donate-page", "editor-logs",
    "engine-profiler", "engine-resource-profiler", "issues", "report-issue",
    "report-suggestion", "support-forum", "show-build-errors", "show-console",
    "show-curve-editor", "toggle-pane-bottom", "toggle-pane-left", "toggle-pane-right",
})
_EXCLUDED_PATHS = frozenset({("/eval", "post")})


Error = AutomationBridgeError


class NotRunningError(Error):
    """Raised when no healthy editor is available for a project."""


class LaunchError(Error):
    """Raised when the Defold editor cannot be launched."""


class UnsupportedOperationError(Error):
    """Raised when the connected editor does not advertise an operation."""


class CommandError(Error):
    """Raised when an editor command is rejected or fails."""


class BuildError(CommandError):
    """Raised when a build-and-run command reports compilation issues."""

    def __init__(self, issues: Sequence["BuildIssue"]):
        self.issues = tuple(issues)
        super().__init__(f"Defold build failed: {list(self.issues)!r}")


def _macos_gui_launch_is_sandboxed() -> bool:
    """Return whether Codex marked this macOS process as Seatbelt-restricted."""
    return sys.platform == "darwin" and bool(os.environ.get("CODEX_SANDBOX"))


class PreviewError(Error):
    """Raised when an editor preview cannot be rendered."""


class PreferenceError(Error):
    """Raised when a preference request is invalid or rejected."""


@dataclass(frozen=True)
class SourcePosition:
    line: int
    character: int


@dataclass(frozen=True)
class SourceRange:
    start: SourcePosition
    end: SourcePosition


@dataclass(frozen=True)
class BuildIssue:
    severity: str
    message: str
    resource: Optional[str] = None
    range: Optional[SourceRange] = None

    @classmethod
    def from_raw(cls, raw: Mapping[str, Any]) -> "BuildIssue":
        raw_range = raw.get("range")
        source_range = None
        if isinstance(raw_range, Mapping):
            start = raw_range.get("start")
            end = raw_range.get("end")
            if isinstance(start, Mapping) and isinstance(end, Mapping):
                source_range = SourceRange(
                    SourcePosition(int(start.get("line", 0)), int(start.get("character", 0))),
                    SourcePosition(int(end.get("line", 0)), int(end.get("character", 0))),
                )
        return cls(
            severity=str(raw.get("severity", "error")),
            message=str(raw.get("message", "")),
            resource=str(raw["resource"]) if raw.get("resource") is not None else None,
            range=source_range,
        )


@dataclass(frozen=True)
class LibraryResult:
    uri: str
    success: bool
    message: Optional[str] = None


@dataclass(frozen=True)
class FetchLibrariesResult:
    success: bool
    libraries: tuple[LibraryResult, ...]


@dataclass(frozen=True)
class ConsoleRegion:
    raw: Mapping[str, Any]


@dataclass(frozen=True)
class ConsoleSnapshot:
    lines: tuple[str, ...]
    regions: tuple[ConsoleRegion, ...]


class ConsoleStream:
    """Context-managed iterator over the editor's streaming console response."""

    def __init__(self, response: Any):
        self._response = response

    def __enter__(self) -> "ConsoleStream":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.close()

    def __iter__(self) -> Iterator[str]:
        return self

    def __next__(self) -> str:
        line = self.readline()
        if line is None:
            raise StopIteration
        return line

    def readline(self, timeout: Optional[float] = None) -> Optional[str]:
        if self._response is None:
            return None
        if timeout is not None:
            try:
                self._response.fp.raw._sock.settimeout(timeout)
            except (AttributeError, OSError):
                pass
        try:
            line = self._response.readline()
        except TimeoutError:
            return None
        if not line:
            return None
        return line.decode("utf-8", "replace").rstrip("\r\n")

    def close(self) -> None:
        response = self._response
        self._response = None
        if response is not None:
            response.close()


@dataclass(frozen=True)
class Installation:
    """One Defold installation discovered through the editor registry."""

    launcher_path: Path
    install_path: Path
    last_launched_at: str


class Commands:
    def __init__(self, client: "Client"):
        self._client = client

    def fetch_libraries(self, timeout: float = 60.0) -> FetchLibrariesResult:
        status, response = self._client._json_command("fetch-libraries", timeout)
        libraries = tuple(
            LibraryResult(
                uri=str(item.get("uri", "")),
                success=bool(item.get("success", False)),
                message=str(item["message"]) if item.get("message") is not None else None,
            )
            for item in response.get("libraries", ())
            if isinstance(item, Mapping)
        )
        result = FetchLibrariesResult(bool(response.get("success", False)), libraries)
        if status >= 400:
            raise CommandError(f"fetch-libraries failed: {response!r}")
        return result

    def hot_reload(self, timeout: float = 60.0) -> None:
        self._client._empty_command("hot-reload", timeout)

    def rebundle(self, timeout: float = 60.0) -> None:
        self._client._empty_command("rebundle", timeout)

    def reload_extensions(self, timeout: float = 60.0) -> None:
        self._client._empty_command("reload-extensions", timeout)

    def reload_stylesheets(self, timeout: float = 60.0) -> None:
        self._client._empty_command("reload-stylesheets", timeout)


class Debugger:
    def __init__(self, client: "Client"):
        self._client = client

    def _run(self, name: str, timeout: float) -> None:
        self._client._empty_command(f"debugger-{name}", timeout)

    def start(self, timeout: float = 60.0) -> None:
        self._run("start", timeout)

    def stop(self, timeout: float = 10.0) -> None:
        self._run("stop", timeout)

    def break_(self, timeout: float = 10.0) -> None:
        self._run("break", timeout)

    def continue_(self, timeout: float = 10.0) -> None:
        self._run("continue", timeout)

    def detach(self, timeout: float = 10.0) -> None:
        self._run("detach", timeout)

    def step_into(self, timeout: float = 10.0) -> None:
        self._run("step-into", timeout)

    def step_out(self, timeout: float = 10.0) -> None:
        self._run("step-out", timeout)

    def step_over(self, timeout: float = 10.0) -> None:
        self._run("step-over", timeout)


class Console:
    def __init__(self, client: "Client"):
        self._client = client

    def read(self) -> ConsoleSnapshot:
        self._client._require_operation("/console", "get")
        status, response = request_json(f"{self._client.base_url}/console", timeout=10.0)
        if status >= 400:
            raise HttpError("GET", f"{self._client.base_url}/console", str(response), status=status)
        return ConsoleSnapshot(
            tuple(str(line) for line in response.get("lines", ())),
            tuple(ConsoleRegion(region) for region in response.get("regions", ()) if isinstance(region, Mapping)),
        )

    def stream(
        self,
        *,
        connect_timeout: float = 10.0,
        read_timeout: Optional[float] = None,
    ) -> ConsoleStream:
        self._client._require_operation("/console/stream", "get")
        url = f"{self._client.base_url}/console/stream"
        try:
            response = urllib.request.urlopen(url, timeout=connect_timeout)
        except (urllib.error.URLError, OSError) as exc:
            raise HttpError("GET", url, str(exc)) from exc
        stream = ConsoleStream(response)
        if read_timeout is not None:
            try:
                response.fp.raw._sock.settimeout(read_timeout)
            except (AttributeError, OSError):
                pass
        return stream


class Reference:
    def __init__(self, client: "Client"):
        self._client = client

    def search(
        self,
        *,
        environment: Optional[str] = None,
        language: Optional[str] = None,
        query: Optional[str] = None,
    ) -> list[dict]:
        self._client._require_operation("/ref", "get")
        params = {key: value for key, value in (("environment", environment), ("language", language), ("q", query)) if value is not None}
        url = f"{self._client.base_url}/ref"
        if params:
            url += "?" + urllib.parse.urlencode(params)
        status, body = request_raw(url, timeout=10.0)
        if status >= 400:
            raise HttpError("GET", url, body.decode("utf-8", "replace"), status=status)
        value = json.loads(body.decode("utf-8"))
        if not isinstance(value, list):
            raise HttpError("GET", url, "reference response was not an array", status=status)
        return [dict(item) for item in value if isinstance(item, Mapping)]


class Preview:
    def __init__(self, client: "Client"):
        self._client = client

    def render(
        self,
        path: Union[str, Path],
        *,
        width: Optional[int] = None,
        height: Optional[int] = None,
        resolution_multiplier: Optional[float] = None,
        timeout: float = 30.0,
    ) -> bytes:
        return self._client._render_preview(
            path,
            width=width,
            height=height,
            resolution_multiplier=resolution_multiplier,
            timeout=timeout,
        )


class Client:
    """Small wrapper around the Defold editor HTTP API for one project."""

    def __init__(self, root: Union[str, Path], port: Optional[int] = None):
        self.root = Path(root).resolve()
        if port is None:
            port_path = self.root / ".internal" / "editor.port"
            if not port_path.exists():
                raise FileNotFoundError(f"Defold editor port file is missing: {port_path}")
            port = int(port_path.read_text(encoding="utf-8").strip())
        self.port = int(port)
        self.base_url = f"http://localhost:{self.port}"
        self._engine_service_port: Optional[int] = self._read_cached_engine_service_port()
        self._cached_engine_identity = self._read_cached_engine_identity()
        self._remotery_url: Optional[str] = self._read_cached_remotery_url()
        self._last_build_had_engine_service_port: Optional[bool] = None
        self._lifecycle_events = []
        self._openapi_document: Optional[dict] = None
        self.commands = Commands(self)
        self.debugger = Debugger(self)
        self.console = Console(self)
        self.reference = Reference(self)
        self.preview = Preview(self)
        self.preferences = Preferences(self)

    @classmethod
    def _open_project(
        cls,
        root: Union[str, Path] = ".",
        *,
        start_if_needed: bool = True,
        timeout: float = 30.0,
        launcher: Optional[Union[str, Path]] = None,
    ) -> "Client":
        """Connect to this project's editor, launching Defold when necessary."""
        project_root = Path(root).resolve()
        try:
            client = cls(project_root)
        except (FileNotFoundError, ValueError):
            client = None
        if client is not None and client._is_running(timeout=min(1.0, timeout)):
            client._record_lifecycle("editor_reused", port=client.port)
            return client
        if not start_if_needed:
            raise NotRunningError(f"Defold editor is not running for {project_root}")

        project_file = project_root / "game.project"
        if not project_file.is_file():
            raise FileNotFoundError(f"Defold project file is missing: {project_file}")
        launcher_path = Path(launcher).expanduser().resolve() if launcher is not None else cls._latest_installation().launcher_path
        if not launcher_path.is_file():
            raise FileNotFoundError(f"Defold launcher is missing: {launcher_path}")
        if _macos_gui_launch_is_sandboxed():
            raise LaunchError(
                "Defold cannot be launched from this restricted macOS sandbox; "
                "start Defold manually or rerun the Automation Bridge bootstrap "
                "with escalated/unsandboxed execution"
            )
        try:
            process = subprocess.Popen(
                [str(launcher_path), str(project_file)],
                cwd=project_root,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=sys.platform != "win32",
            )
        except OSError as exc:
            raise LaunchError(f"cannot launch Defold from {launcher_path}: {exc}") from exc

        def ready() -> Optional["Client"]:
            try:
                candidate = cls(project_root)
                return candidate if candidate._is_running(timeout=min(1.0, timeout)) else None
            except (AutomationBridgeError, FileNotFoundError, OSError, ValueError):
                return None

        client = wait_until(
            ready,
            timeout=timeout,
            interval=0.1,
            message=f"Defold editor did not start for {project_root}",
            retry_exceptions=(AutomationBridgeError,),
        )
        client._record_lifecycle(
            "editor_started",
            launcher=str(launcher_path),
            process_id=process.pid,
            port=client.port,
        )
        return client

    @classmethod
    def _installations(cls) -> list[Installation]:
        """Return registered Defold installations, newest launch first."""
        registry_path = cls._installation_registry_path()
        try:
            value = json.loads(registry_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return []
        except (OSError, json.JSONDecodeError) as exc:
            raise AutomationBridgeError(f"cannot read Defold installation registry {registry_path}: {exc}") from exc
        if not isinstance(value, list):
            raise AutomationBridgeError(f"Defold installation registry is not an array: {registry_path}")
        installations = []
        for item in value:
            if not isinstance(item, dict):
                continue
            launcher_path = item.get("launcherPath")
            install_path = item.get("installPath")
            last_launched_at = item.get("lastLaunchedAt")
            if not all(isinstance(field, str) and field for field in (launcher_path, install_path, last_launched_at)):
                continue
            launcher = Path(launcher_path).expanduser()
            if not launcher.is_file():
                continue
            installations.append(
                Installation(
                    launcher_path=launcher,
                    install_path=Path(install_path).expanduser(),
                    last_launched_at=last_launched_at,
                )
            )
        return sorted(installations, key=lambda installation: installation.last_launched_at, reverse=True)

    @classmethod
    def _latest_installation(cls) -> Installation:
        """Return the most recently launched registered Defold installation."""
        installations = cls._installations()
        if not installations:
            raise AutomationBridgeError(
                f"no Defold installation was found in {cls._installation_registry_path()}"
            )
        return installations[0]

    @staticmethod
    def _installation_registry_path() -> Path:
        """Return the platform-specific registry path introduced by Defold #12699."""
        if sys.platform == "darwin":
            return Path.home() / "Library" / "Application Support" / "Defold" / "installations.json"
        if sys.platform == "win32":
            base = os.environ.get("LOCALAPPDATA")
            if not base:
                raise AutomationBridgeError("LOCALAPPDATA is not set")
            return Path(base) / "Defold" / "installations.json"
        base = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
        return base / "Defold" / "installations.json"

    def _is_running(self, timeout: float = 1.0) -> bool:
        """Return whether this project's recorded editor port serves the editor API."""
        try:
            status, document = request_json(f"{self.base_url}/openapi.json", timeout=timeout)
            if 200 <= status < 300:
                self._openapi_document = document
            return 200 <= status < 300
        except AutomationBridgeError:
            return False

    def _openapi(self) -> dict:
        if self._openapi_document is None:
            status, document = request_json(f"{self.base_url}/openapi.json", timeout=10.0)
            if status < 200 or status >= 300:
                raise HttpError("GET", f"{self.base_url}/openapi.json", str(document), status=status)
            self._openapi_document = document
        return self._openapi_document

    def _require_operation(self, path: str, method: str) -> Mapping[str, Any]:
        if (path, method.lower()) not in _SUPPORTED_PATHS:
            raise UnsupportedOperationError(f"editor operation is outside the supported API: {method.upper()} {path}")
        operation = self._openapi().get("paths", {}).get(path, {}).get(method.lower())
        if not isinstance(operation, Mapping):
            raise UnsupportedOperationError(f"editor does not advertise {method.upper()} {path}")
        return operation

    def _require_command(self, command: str) -> None:
        operation = self._require_operation("/command/{command}", "post")
        names = set()
        for parameter in operation.get("parameters", ()):
            if isinstance(parameter, Mapping) and parameter.get("name") == "command":
                schema = parameter.get("schema", {})
                if isinstance(schema, Mapping):
                    names.update(str(value) for value in schema.get("enum", ()))
        if command not in names:
            raise UnsupportedOperationError(f"editor does not advertise command {command!r}")

    def _empty_command(self, command: str, timeout: float) -> None:
        self._require_command(command)
        url = f"{self.base_url}/command/{command}"
        status, body = request_raw(url, method="POST", timeout=timeout)
        if status < 200 or status >= 300:
            raise CommandError(f"{command} failed with HTTP {status}: {body.decode('utf-8', 'replace')}")

    def _json_command(self, command: str, timeout: float) -> tuple[int, dict]:
        self._require_command(command)
        return request_json(f"{self.base_url}/command/{command}", method="POST", timeout=timeout)

    def connect_engine(
        self,
        *,
        timeout: float = 20.0,
        required_capabilities: Sequence[str] = (),
    ) -> EngineClient:
        from .client import Client as EngineClient

        return EngineClient._from_editor(
            self,
            build_command=None,
            timeout=timeout,
            required_capabilities=required_capabilities,
        )

    def build_and_run(
        self,
        *,
        timeout: float = 60.0,
        required_capabilities: Sequence[str] = (),
    ) -> EngineClient:
        from .client import Client as EngineClient

        return EngineClient._from_editor(
            self,
            build_command="build",
            timeout=timeout,
            required_capabilities=required_capabilities,
        )

    def clean_build_and_run(
        self,
        *,
        timeout: float = 60.0,
        required_capabilities: Sequence[str] = (),
    ) -> EngineClient:
        from .client import Client as EngineClient

        return EngineClient._from_editor(
            self,
            build_command="clean-build",
            timeout=timeout,
            required_capabilities=required_capabilities,
        )

    def build_and_run_html5(self, *, timeout: float = 60.0) -> None:
        self._empty_command("build-html5", timeout)

    def _build_and_run_command(self, command: str, timeout: float = 60.0) -> None:
        """Execute a desktop build-and-run command and await endpoint registration."""
        if command not in {"build", "clean-build"}:
            raise ValueError(f"unsupported desktop build-and-run command: {command}")
        self._require_command(command)
        self._record_lifecycle("editor_build_started")
        previous_lines = self._console_lines()
        previous_registration_count = self._endpoint_registered_count(previous_lines)
        previous_registration_ports = self._latest_registration_engine_service_ports(previous_lines)
        previous_port = self._engine_service_port_value()
        if previous_port is not None:
            self._engine_service_port = previous_port
        try:
            status, response = request_json(f"{self.base_url}/command/{command}", method="POST", timeout=timeout)
        except Exception as exc:
            self._record_lifecycle("editor_build_failed", error=str(exc))
            raise
        if status >= 400 or not response.get("success"):
            issues = tuple(BuildIssue.from_raw(item) for item in response.get("issues", ()) if isinstance(item, Mapping))
            self._record_lifecycle("editor_build_failed", issues=issues)
            raise BuildError(issues)
        self._record_lifecycle("editor_build_completed")

        try:
            wait_until(
                lambda: self._has_fresh_endpoint_registration(
                    previous_registration_count,
                    previous_registration_ports,
                ),
                timeout=timeout,
                interval=0.1,
                message="Defold build completed, but Automation Bridge endpoint did not register",
            )
        except AssertionError as exc:
            self._record_lifecycle("new_engine_registration_failed", error=str(exc))
            raise AutomationBridgeError(str(exc)) from exc
        self._record_lifecycle("new_engine_registered")
        self._last_build_had_engine_service_port = self._latest_registration_has_engine_service_port()
        time.sleep(0.2)

    def _console_lines(self) -> list:
        """Return current editor console lines."""
        return list(self.console.read().lines)

    def _render_preview(
        self,
        path: Union[str, Path],
        *,
        width: Optional[int] = None,
        height: Optional[int] = None,
        resolution_multiplier: Optional[float] = None,
        timeout: float = 30.0,
    ) -> bytes:
        """Render a scene resource through the editor and return PNG bytes.

        Supported resources are those with a scene view, such as collections,
        game objects, GUI scenes, particle effects, and tile maps. Omitted
        dimensions use the project display dimensions. Use
        ``resolution_multiplier`` for a smaller project-aspect-ratio preview.
        """
        if resolution_multiplier is not None:
            if width is not None or height is not None:
                raise ValueError("resolution_multiplier is mutually exclusive with width and height")
            if (
                not isinstance(resolution_multiplier, (int, float))
                or isinstance(resolution_multiplier, bool)
                or not 0.01 <= float(resolution_multiplier) <= 1.0
            ):
                raise ValueError("preview resolution_multiplier must be from 0.01 through 1.0")
            display_width, display_height = self._project_display_size()
            width = max(1, round(display_width * float(resolution_multiplier)))
            height = max(1, round(display_height * float(resolution_multiplier)))
        params = {}
        for name, value in (("width", width), ("height", height)):
            if value is not None:
                if not isinstance(value, int) or isinstance(value, bool) or not 1 <= value <= 4096:
                    raise ValueError(f"preview {name} must be an integer from 1 through 4096")
                params[name] = value
        project_path = str(path).replace("\\", "/").lstrip("/")
        if not project_path:
            raise ValueError("preview path must identify a project resource")
        self._require_operation("/preview/{path}", "get")
        encoded_path = urllib.parse.quote(project_path, safe="/")
        url = f"{self.base_url}/preview/{encoded_path}"
        if params:
            url += "?" + urllib.parse.urlencode(params)
        status, body = request_raw(url, timeout=timeout)
        if status < 200 or status >= 300:
            message = body[:300].decode("utf-8", "replace").strip() or f"unexpected status {status}"
            raise PreviewError(f"GET {url} failed: {message}")
        if not body.startswith(b"\x89PNG\r\n\x1a\n"):
            raise PreviewError(f"GET {url} failed: editor preview did not return a PNG")
        return body

    def _project_display_size(self) -> tuple[int, int]:
        config = configparser.ConfigParser(interpolation=None, strict=False)
        project_path = self.root / "game.project"
        try:
            with project_path.open(encoding="utf-8") as stream:
                config.read_file(stream)
            width = config.getint("display", "width", fallback=960)
            height = config.getint("display", "height", fallback=640)
        except (OSError, configparser.Error, ValueError) as exc:
            raise AutomationBridgeError(f"cannot read project display dimensions from {project_path}: {exc}") from exc
        if width <= 0 or height <= 0:
            raise AutomationBridgeError(
                f"project display dimensions must be positive, got {width}x{height}"
            )
        return width, height

    def _engine_service_port_value(self) -> Optional[int]:
        """Return the service port before endpoint registration, or the cached reused port."""
        ports = self._engine_service_ports()
        return ports[0] if ports else None

    def _engine_service_ports(self) -> list:
        """Return candidate engine service ports from current logs, then the cached port."""
        candidates = self._current_registration_engine_service_ports()

        if self._engine_service_port is not None:
            self._append_port_candidate(candidates, self._engine_service_port)
        return candidates

    def _current_registration_engine_service_ports(self) -> list:
        """Return ports logged before the latest Automation Bridge endpoint registration only."""
        return self._latest_registration_engine_service_ports(self._console_lines())

    def _remotery_url_value(self) -> Optional[str]:
        """Return the Remotery websocket URL from current logs, then the cached URL."""
        urls = self._remotery_urls()
        return urls[0] if urls else None

    def _remotery_urls(self) -> list:
        """Return candidate Remotery websocket URLs from current logs, then the cached URL."""
        candidates = self._current_registration_remotery_urls()
        if self._remotery_url is not None:
            self._append_unique_candidate(candidates, self._remotery_url)
        return candidates

    def _current_registration_remotery_urls(self) -> list:
        """Return Remotery websocket URLs logged before the latest endpoint registration only."""
        return self._latest_registration_remotery_urls(self._console_lines())

    @classmethod
    def _latest_registration_engine_service_ports(cls, lines: list) -> list:
        search_lines = cls._latest_registration_window(lines)
        if search_lines is None:
            search_lines = lines
        candidates = []
        for line in reversed(search_lines):
            for candidate_pattern in _ENGINE_SERVICE_PORT_PATTERNS:
                match = candidate_pattern.search(line)
                if match:
                    cls._append_port_candidate(candidates, int(match.group(1)))
        return candidates

    @classmethod
    def _latest_registration_remotery_urls(cls, lines: list) -> list:
        search_lines = cls._latest_registration_window(lines)
        if search_lines is None:
            search_lines = lines
        candidates = []
        for line in reversed(search_lines):
            match = _REMOTERY_URL_PATTERN.search(line)
            if match:
                cls._append_unique_candidate(candidates, match.group(1))
        return candidates

    def _latest_registration_has_engine_service_port(self) -> bool:
        """Return whether the latest endpoint registration had a fresh port nearby."""
        return bool(self._current_registration_engine_service_ports())

    @staticmethod
    def _endpoint_registered_count(lines: list) -> int:
        return sum(1 for line in lines if _AUTOMATION_BRIDGE_ENDPOINT_TEXT in line)

    def _has_fresh_endpoint_registration(self, previous_count: int, previous_ports: list) -> bool:
        lines = self._console_lines()
        current_count = self._endpoint_registered_count(lines)
        if current_count > previous_count:
            return True
        current_ports = self._latest_registration_engine_service_ports(lines)
        return bool(current_count and current_ports and current_ports != previous_ports)

    @staticmethod
    def _latest_registration_window(lines: list) -> Optional[list]:
        endpoint_index: Optional[int] = None
        previous_endpoint_index: Optional[int] = None
        for index, line in enumerate(lines):
            if _AUTOMATION_BRIDGE_ENDPOINT_TEXT in line:
                previous_endpoint_index = endpoint_index
                endpoint_index = index

        if endpoint_index is None:
            return None
        start_index = previous_endpoint_index + 1 if previous_endpoint_index is not None else 0
        return lines[start_index:endpoint_index]

    def _remember_engine_service_port(
        self,
        port: int,
        engine_instance_id: Optional[str] = None,
        project_identity: Optional[str] = None,
    ) -> None:
        """Remember a validated port and identity so stale port reuse is detectable."""
        self._engine_service_port = int(port)
        self._write_cached_engine_service_port(self._engine_service_port)
        self._cached_engine_identity = {
            "port": self._engine_service_port,
            "engine_instance_id": engine_instance_id,
            "project_identity": project_identity,
        }
        self._write_cached_engine_identity(self._cached_engine_identity)

    def _validate_cached_engine_health(self, port: int, health: dict, fresh_build: bool = False) -> bool:
        """Reject a cached-only port when it now belongs to a different engine/project.

        Fresh editor registrations are authoritative and replace the cache. When attaching
        without a build, an instance mismatch means the operating system reused the port.
        """
        cached = self._cached_engine_identity
        if not isinstance(cached, dict) or cached.get("port") != int(port):
            return True
        identity = health.get("identity", {}) if isinstance(health, dict) else {}
        if not isinstance(identity, dict):
            return not cached.get("engine_instance_id")
        cached_project = cached.get("project_identity")
        current_project = identity.get("project_identity")
        if cached_project and current_project and cached_project != current_project:
            self._record_lifecycle("cached_port_rejected", port=port, reason="project_identity_mismatch")
            return False
        cached_instance = cached.get("engine_instance_id")
        current_instance = identity.get("engine_instance_id")
        if not fresh_build and cached_instance and current_instance != cached_instance:
            self._record_lifecycle("cached_port_rejected", port=port, reason="engine_instance_mismatch")
            return False
        return True

    def _record_lifecycle(self, stage: str, **details: object) -> None:
        """Record an observable editor/bootstrap lifecycle transition."""
        event = {"stage": stage, "state": "completed", "monotonic": time.monotonic()}
        event.update(details)
        self._lifecycle_events.append(event)

    @property
    def lifecycle_events(self) -> list:
        """Return a copy of editor/bootstrap lifecycle events observed by this client."""
        return [dict(event) for event in self._lifecycle_events]

    def _remember_remotery_url(self, url: str) -> None:
        """Remember the Remotery websocket URL for reused-engine builds."""
        self._remotery_url = url
        self._write_cached_remotery_url(url)

    @staticmethod
    def _append_port_candidate(candidates: list, port: int) -> None:
        if port not in candidates:
            candidates.append(port)

    @staticmethod
    def _append_unique_candidate(candidates: list, value: str) -> None:
        if value not in candidates:
            candidates.append(value)

    @property
    def _engine_service_port_cache_path(self) -> Path:
        return self.root / ".internal" / "automation_bridge.engine.port"

    def _read_cached_engine_service_port(self) -> Optional[int]:
        path = self._engine_service_port_cache_path
        try:
            value = path.read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            return None
        if not value:
            return None
        try:
            return int(value)
        except ValueError:
            return None

    def _write_cached_engine_service_port(self, port: int) -> None:
        path = self._engine_service_port_cache_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"{port}\n", encoding="utf-8")

    @property
    def _engine_identity_cache_path(self) -> Path:
        return self.root / ".internal" / "automation_bridge.engine.identity.json"

    def _read_cached_engine_identity(self) -> Optional[dict]:
        try:
            value = json.loads(self._engine_identity_cache_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError, OSError):
            return None
        return value if isinstance(value, dict) else None

    def _write_cached_engine_identity(self, value: dict) -> None:
        path = self._engine_identity_cache_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, sort_keys=True) + "\n", encoding="utf-8")

    @property
    def _remotery_url_cache_path(self) -> Path:
        return self.root / ".internal" / "automation_bridge.remotery.url"

    def _read_cached_remotery_url(self) -> Optional[str]:
        path = self._remotery_url_cache_path
        try:
            value = path.read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            return None
        return value or None

    def _write_cached_remotery_url(self, url: str) -> None:
        path = self._remotery_url_cache_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"{url}\n", encoding="utf-8")


def is_running(root: Union[str, Path] = ".", *, timeout: float = 1.0) -> bool:
    """Return whether the project's recorded editor endpoint is healthy."""
    try:
        return Client(root)._is_running(timeout=timeout)
    except (AutomationBridgeError, FileNotFoundError, OSError, ValueError):
        return False


def open_project(
    root: Union[str, Path] = ".",
    *,
    start_if_needed: bool = True,
    timeout: float = 30.0,
    launcher: Optional[Union[str, Path]] = None,
) -> Client:
    """Open or reuse a Defold editor for one project."""
    return Client._open_project(
        root,
        start_if_needed=start_if_needed,
        timeout=timeout,
        launcher=launcher,
    )


def installations() -> list[Installation]:
    return Client._installations()


def latest_installation() -> Installation:
    return Client._latest_installation()


def installation_registry_path() -> Path:
    return Client._installation_registry_path()


__all__ = [
    "BuildError",
    "BuildIssue",
    "Client",
    "CommandError",
    "Commands",
    "Console",
    "ConsoleRegion",
    "ConsoleSnapshot",
    "ConsoleStream",
    "Debugger",
    "Error",
    "FetchLibrariesResult",
    "HttpError",
    "Installation",
    "LaunchError",
    "LibraryResult",
    "NotRunningError",
    "PreferenceError",
    "PreferenceKey",
    "Preferences",
    "Preview",
    "PreviewError",
    "Reference",
    "SourcePosition",
    "SourceRange",
    "UnsupportedOperationError",
    "installation_registry_path",
    "installations",
    "is_running",
    "latest_installation",
    "open_project",
]
