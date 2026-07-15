"""Dependency-free Python client for the Automation Bridge runtime API."""

import hashlib
import json
import difflib
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from collections import deque
from pathlib import Path
import threading
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

from .nodes import Element
from .receipts import ObservationReceipt, ScreenshotReceipt
from .waits import RetryExceptions, WaitTimeoutError, wait_until
from .events import CommandTimeout, Event, EventStream, StateSnapshot, select_state_path


JsonDict = Dict[str, Any]
Target = Union[Element, str, Mapping[str, Any], Sequence[float]]
_SCREEN_DIMENSION_MAX = 0x7FFFFFFF
_INPUT_EASINGS = {"linear", "ease_in", "ease_out", "ease_in_out"}
PYTHON_PACKAGE_VERSION = "2.0.0"
SUPPORTED_API_VERSION_MIN = 1
SUPPORTED_API_VERSION_MAX = 1


class AutomationBridgeError(RuntimeError):
    """Base class for custom Automation Bridge wrapper errors."""

    pass


class HttpError(AutomationBridgeError):
    """Raised for transport failures, invalid JSON, or unexpected raw responses."""

    def __init__(self, method: str, url: str, message: str, status: Optional[int] = None):
        self.method = method
        self.url = url
        self.status = status
        super().__init__(f"{method} {url} failed: {message}")


class AutomationBridgeApiError(AutomationBridgeError):
    """Raised when the native Automation Bridge API returns `{ "ok": false }`."""

    def __init__(self, code: str, message: str, status: int, response: Mapping[str, Any]):
        self.code = code
        self.message = message
        self.status = status
        self.response = response
        super().__init__(f"Automation Bridge API error {status} {code}: {message}")


class SelectorError(AutomationBridgeError):
    """Raised when an element selector finds too many or too few elements."""

    pass


class InputExecutionError(AutomationBridgeError):
    """Raised when an accepted native input is cancelled or fails before release."""

    def __init__(self, receipt: "InputReceipt"):
        self.receipt = receipt
        super().__init__(
            f"input {receipt.input_id} ended as {receipt.state}: "
            f"{receipt.get('reason') or 'no reason reported'}"
        )


class InputReceipt(dict):
    """Dictionary-compatible native input lifecycle receipt with attribute access."""

    def __getattr__(self, name: str) -> Any:
        try:
            return self[name]
        except KeyError as exc:
            raise AttributeError(name) from exc

    @property
    def input_id(self) -> int:
        return int(self["input_id"])

    @property
    def state(self) -> str:
        return str(self["state"])


class IncompatibleApiVersionError(AutomationBridgeError):
    """Raised when the native API does not overlap the wrapper's supported range."""

    pass


class UnsupportedCapabilityError(AutomationBridgeError):
    """Raised when a declared required capability is unavailable or too old."""

    pass


class EngineLogStream:
    """Context-managed reader for Defold's TCP log service."""

    def __init__(
        self,
        host: str,
        port: int,
        timeout: float = 2.0,
        read_timeout: Optional[float] = None,
    ):
        self.host = host
        self.port = int(port)
        self._socket: Optional[socket.socket] = socket.create_connection(
            (host, self.port), timeout=timeout
        )
        self._buffer = bytearray()
        try:
            status = self._readline_raw(timeout).decode("utf-8", "replace").rstrip("\r\n")
            if status != "0 OK":
                raise AutomationBridgeError(f"log service rejected connection: {status}")
            self._socket.settimeout(read_timeout)
        except BaseException:
            _cleanup_without_masking(self.close)
            raise

    def __enter__(self) -> "EngineLogStream":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if exc_type is None:
            self.close()
        else:
            _cleanup_without_masking(self.close)

    def __iter__(self) -> "EngineLogStream":
        return self

    def __next__(self) -> str:
        line = self.readline()
        if line is None:
            raise StopIteration
        return line

    @property
    def closed(self) -> bool:
        """Return whether the log-service socket is closed."""
        return self._socket is None

    def close(self) -> None:
        """Close the underlying log service socket."""
        sock = self._socket
        self._socket = None
        if sock is None:
            return
        try:
            close = getattr(sock, "close", None)
            if close is not None:
                close()
        except OSError:
            pass

    def readline(self, timeout: Optional[float] = None) -> Optional[str]:
        """Read one log line without its trailing newline; return `None` on timeout or EOF."""
        data = self._readline_raw(timeout)
        if not data:
            return None
        return data.decode("utf-8", "replace").rstrip("\r\n")

    def _readline_raw(self, timeout: Optional[float] = None) -> bytes:
        sock = self._socket
        if sock is None:
            return b""
        previous_timeout = sock.gettimeout()
        timeout_changed = timeout is not None
        if timeout is not None:
            sock.settimeout(timeout)
        try:
            while True:
                newline = self._buffer.find(b"\n")
                if newline >= 0:
                    line = bytes(self._buffer[: newline + 1])
                    del self._buffer[: newline + 1]
                    return line

                chunk = sock.recv(4096)
                if not chunk:
                    if not self._buffer:
                        self.close()
                        return b""
                    line = bytes(self._buffer)
                    self._buffer.clear()
                    self.close()
                    return line
                self._buffer.extend(chunk)
        except socket.timeout:
            return b""
        except BaseException:
            _cleanup_without_masking(self.close)
            raise
        finally:
            if timeout_changed and self._socket is sock:
                unwinding = sys.exc_info()[0] is not None
                try:
                    sock.settimeout(previous_timeout)
                except OSError:
                    # Concurrent socket closure makes restoration irrelevant.
                    pass
                except BaseException:
                    # Socket closure or a second interruption during restoration
                    # must not replace the exception already leaving recv().
                    if not unwinding:
                        raise


def _cleanup_without_masking(cleanup: Any) -> None:
    """Run best-effort cleanup while preserving an already-active exception."""
    try:
        cleanup()
    except BaseException:
        pass


class RuntimeLogs:
    """Bounded background collection from Defold's engine log service."""

    def __init__(self, bridge: "Client", capacity: int = 10000):
        self._bridge = bridge
        self._lines = deque(maxlen=capacity)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._stream: Optional[EngineLogStream] = None
        self._error: Optional[BaseException] = None

    def start(self) -> "RuntimeLogs":
        """Start collecting future engine log lines in a bounded daemon thread."""
        thread = self._thread
        if thread is not None:
            return self
        if self._stop.is_set():
            raise AutomationBridgeError("engine log collector is closed")
        self._ready.clear()
        self._error = None
        self._thread = threading.Thread(
            target=self._collect,
            name=f"automation-bridge-logs-{self._bridge.port}",
            daemon=True,
        )
        self._thread.start()
        return self

    def tail(self, limit: int = 100, *, contains: Optional[str] = None) -> List[str]:
        """Return buffered engine log lines, optionally filtered by text."""
        if not isinstance(limit, int) or isinstance(limit, bool) or limit < 0:
            raise ValueError("log tail limit must be a non-negative integer")
        if contains is not None and not isinstance(contains, str):
            raise TypeError("log tail contains filter must be a string or None")
        self.start()
        self._ready.wait(timeout=min(1.0, max(0.0, float(self._bridge.timeout))))
        with self._lock:
            lines = list(self._lines)
        if self._error is not None and not lines:
            raise AutomationBridgeError(f"engine log collector failed: {self._error}") from self._error
        if contains is not None:
            lines = [line for line in lines if contains in line]
        return lines[-limit:] if limit else []

    def close(self, timeout: float = 1.0) -> None:
        """Stop collection and close its engine log connection."""
        self._stop.set()
        stream = self._stream
        if stream is not None:
            _cleanup_without_masking(stream.close)
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=max(0.0, timeout))

    def _collect(self) -> None:
        try:
            with self._bridge.log_stream(
                timeout=self._bridge.timeout,
                read_timeout=0.25,
            ) as stream:
                self._stream = stream
                self._ready.set()
                while not self._stop.is_set():
                    line = stream.readline(timeout=0.25)
                    if line is not None:
                        with self._lock:
                            self._lines.append(line)
                    elif stream.closed:
                        break
                    else:
                        self._stop.wait(0.01)
        except Exception as error:
            if not self._stop.is_set():
                self._error = error
        finally:
            self._stream = None
            self._ready.set()


class InputInterruptionScope:
    """Flush this client's input session if an enclosed operation is interrupted."""

    def __init__(self, controller: "InputController", flush: bool, release: bool):
        self._controller = controller
        self.flush = flush
        self.release = release

    def __enter__(self) -> "InputInterruptionScope":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if exc_type is None:
            return
        if self.flush:
            _cleanup_without_masking(lambda: self._controller.flush(release=self.release))


class InputController:
    """Queue, receipt, controller-lease, cancellation, and device operations."""

    def __init__(self, bridge: "Client"):
        self._bridge = bridge

    def configure(
        self,
        device: str = "auto",
        visualize: Optional[bool] = None,
        lease: float = 5.0,
    ) -> JsonDict:
        """Acquire/renew control and configure the default exclusive input device."""
        params = self._bridge._input_params(lease=lease)
        params["device"] = device
        if visualize is not None:
            params["visualize"] = visualize
        return self._bridge._request("PUT", "/input/configure", json_body=params)

    def pending(self) -> List[InputReceipt]:
        """Return FIFO-ordered accepted/started input receipts."""
        data = self._bridge._request("GET", "/input/pending")
        return [InputReceipt(item) for item in data.get("inputs", [])]

    def status(self, input_id: int) -> InputReceipt:
        """Return the current or bounded-history receipt for `input_id`."""
        return InputReceipt(self._bridge._request("GET", "/input/status", {"input_id": input_id}))

    def wait(
        self,
        input_or_id: Union[int, Mapping[str, Any]],
        state: str = "released",
        timeout: float = 10.0,
        interval: float = 0.01,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Wait for native `started` or `released`; cancellation/failure raises."""
        if state not in {"accepted", "started", "released"}:
            raise ValueError("input wait state must be accepted, started, or released")
        receipt = InputReceipt(input_or_id) if isinstance(input_or_id, Mapping) else None
        input_id = int(receipt["input_id"] if receipt is not None else input_or_id)
        if state == "accepted" and receipt is not None:
            return receipt
        deadline = time.monotonic() + max(0.0, timeout)
        last = receipt
        try:
            while True:
                if last is None or last.state == "accepted" or state == "released":
                    last = self.status(input_id)
                if last.state in {"cancelled", "failed"}:
                    raise InputExecutionError(last)
                if state == "started" and last.state in {"started", "released"}:
                    return last
                if state == "released" and last.state == "released":
                    return last
                if time.monotonic() >= deadline:
                    raise AutomationBridgeError(
                        f"input {input_id} did not reach {state!r} within {timeout}s; "
                        f"last state was {last.state!r}"
                    )
                time.sleep(max(0.0, min(interval, deadline - time.monotonic())))
        except BaseException:
            if cancel_on_interrupt:
                def cleanup() -> None:
                    if flush_on_interrupt:
                        self.flush(release=True)
                    else:
                        self.cancel(input_id, release=True)

                # Keep cleanup failures from replacing the original timeout,
                # cancellation, KeyboardInterrupt, or API error.
                _cleanup_without_masking(cleanup)
            raise

    def cancel(self, input_id: int, release: bool = True) -> InputReceipt:
        """Request cancellation, releasing active pointer/key state by default."""
        params = self._bridge._input_params()
        params.update({"input_id": input_id, "release": release})
        return InputReceipt(self._bridge._request("POST", "/input/cancel", json_body=params))

    def flush(self, release: bool = True) -> JsonDict:
        """Cancel this session's active and later queued inputs."""
        params = self._bridge._input_params()
        params["release"] = release
        return self._bridge._request("POST", "/input/flush", json_body=params)

    def interruption_scope(
        self,
        flush: bool = True,
        release: bool = True,
    ) -> InputInterruptionScope:
        """Return a context that releases input if any enclosed operation fails.

        This is useful when an interruption may happen outside an input wait,
        for example while waiting for an application event or screenshot after
        queueing input with ``wait=False``. With ``flush=True`` (the default),
        both the active action and later actions owned by this client session
        are cancelled. Cleanup failures never mask the original exception.
        """
        return InputInterruptionScope(self, flush=flush, release=release)


class PointerSession:
    """Leased low-level pointer that guarantees up/cancel cleanup in a context manager."""

    def __init__(self, bridge: "Client", receipt: InputReceipt, lease: float):
        self._bridge = bridge
        self.receipt = receipt
        self.lease = lease
        self.closed = False

    @property
    def input_id(self) -> int:
        return self.receipt.input_id

    def __enter__(self) -> "PointerSession":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if self.closed:
            return
        if exc_type is not None:
            _cleanup_without_masking(self.cancel)
            return
        self.up()

    def move(
        self,
        target: Target,
        duration: float = 0.2,
        easing: str = "linear",
    ) -> InputReceipt:
        """Append one continuous movement segment without releasing the pointer."""
        self._ensure_open()
        x, y = self._bridge._point(target)
        params = self._bridge._input_params(lease=max(5.0, self.lease))
        params.update(
            {
                "input_id": self.input_id,
                "x": x,
                "y": y,
                "duration": duration,
                "easing": easing,
                "pointer_lease": self.lease,
            }
        )
        self.receipt = InputReceipt(self._bridge._request("POST", "/input/pointer/move", json_body=params))
        return self.receipt

    def hold(self, duration: float) -> InputReceipt:
        """Keep the pointer down at its current position for `duration`."""
        self._ensure_open()
        params = self._bridge._input_params(lease=max(5.0, self.lease))
        params.update(
            {
                "input_id": self.input_id,
                "duration": duration,
                "pointer_lease": self.lease,
            }
        )
        self.receipt = InputReceipt(self._bridge._request("POST", "/input/pointer/hold", json_body=params))
        return self.receipt

    def up(self, wait: Union[str, bool] = "released", timeout: float = 10.0) -> InputReceipt:
        """Request one final up event and optionally wait for native release injection."""
        self._ensure_open()
        params = self._bridge._input_params(lease=max(5.0, self.lease))
        params.update({"input_id": self.input_id, "pointer_lease": self.lease})
        self.receipt = InputReceipt(self._bridge._request("POST", "/input/pointer/up", json_body=params))
        self.closed = True
        if wait:
            target_state = "released" if wait is True else str(wait)
            self.receipt = self._bridge.input.wait(self.receipt, target_state, timeout=timeout)
        return self.receipt

    def cancel(self, release: bool = True) -> InputReceipt:
        """Cancel the session and release/cancel the active contact."""
        if self.closed:
            return self.receipt
        self.receipt = self._bridge.input.cancel(self.input_id, release=release)
        self.closed = True
        return self.receipt

    def _ensure_open(self) -> None:
        if self.closed:
            raise AutomationBridgeError(f"pointer session {self.input_id} is closed")


def request_json(
    url: str,
    method: str = "GET",
    timeout: float = 10.0,
    data: Optional[bytes] = None,
    headers: Optional[Mapping[str, str]] = None,
    json_body: Optional[Mapping[str, Any]] = None,
) -> Tuple[int, JsonDict]:
    """Request JSON and return `(status, object)`."""
    request_headers = dict(headers or {})
    if json_body is not None:
        if data is not None:
            raise ValueError("request_json accepts either data or json_body, not both")
        data = json.dumps(json_body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = response.getcode()
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8", "replace")
    except urllib.error.URLError as exc:
        raise HttpError(method, url, str(exc)) from exc
    except OSError as exc:
        raise HttpError(method, url, str(exc)) from exc

    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as exc:
        preview = body[:200].replace("\n", "\\n")
        raise HttpError(method, url, f"invalid JSON response: {exc}; body starts with {preview!r}", status=status) from exc

    if not isinstance(parsed, dict):
        raise HttpError(method, url, "JSON response was not an object", status=status)
    return status, parsed


def request_raw(url: str, method: str = "GET", timeout: float = 10.0) -> Tuple[int, bytes]:
    """Request raw bytes and return `(status, body)`."""
    request = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.getcode(), response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except urllib.error.URLError as exc:
        raise HttpError(method, url, str(exc)) from exc
    except OSError as exc:
        raise HttpError(method, url, str(exc)) from exc


def request_bytes(url: str, data: bytes, method: str = "POST", timeout: float = 10.0) -> Tuple[int, bytes]:
    """POST bytes and return `(status, body)`."""
    request = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.getcode(), response.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except urllib.error.URLError as exc:
        raise HttpError(method, url, str(exc)) from exc
    except OSError as exc:
        raise HttpError(method, url, str(exc)) from exc


def _normalize_include(include: Optional[Union[str, Iterable[str]]]) -> Optional[str]:
    if include is None:
        return None
    if isinstance(include, str):
        return include
    return ",".join(include)


def _encode_param(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def _protobuf_varint(value: int) -> bytes:
    if not isinstance(value, int):
        raise TypeError(f"protobuf varint value must be int, got {type(value).__name__}")
    if value < 0:
        raise ValueError("protobuf varint value must be non-negative")

    encoded = bytearray()
    while value >= 0x80:
        encoded.append((value & 0x7F) | 0x80)
        value >>= 7
    encoded.append(value)
    return bytes(encoded)


def _protobuf_string(field: int, value: str) -> bytes:
    if not isinstance(value, str):
        raise TypeError(f"string field {field} must be str, got {type(value).__name__}")
    encoded = value.encode("utf-8")
    return _protobuf_varint((field << 3) | 2) + _protobuf_varint(len(encoded)) + encoded


def _validate_screen_size(width: int, height: int) -> None:
    if not isinstance(width, int) or not isinstance(height, int):
        raise TypeError(f"resize dimensions must be ints, got {type(width).__name__}x{type(height).__name__}")
    if width <= 0 or height <= 0:
        raise ValueError(f"resize dimensions must be positive, got {width}x{height}")
    if width > _SCREEN_DIMENSION_MAX or height > _SCREEN_DIMENSION_MAX:
        raise ValueError(f"resize dimensions must fit platform window limits, got {width}x{height}")


def _encode_system_reboot(args: Sequence[str]) -> bytes:
    if len(args) > 6:
        raise ValueError("Defold reboot accepts at most six arguments")

    payload = bytearray()
    for index, arg in enumerate(args, start=1):
        payload.extend(_protobuf_string(index, arg))
    return bytes(payload)


class Client:
    """High-level client for `/automation-bridge/v1` scene inspection and input control."""

    SUPPORTED_API_VERSION_MIN = SUPPORTED_API_VERSION_MIN
    SUPPORTED_API_VERSION_MAX = SUPPORTED_API_VERSION_MAX

    _SERVER_FILTERS = {
        "id",
        "instance_id",
        "logical_id",
        "type",
        "type_exact",
        "name",
        "name_exact",
        "text",
        "text_exact",
        "url",
        "url_exact",
        "path",
        "enabled",
        "kind",
        "has_bounds",
        "visible_and_enabled",
        "visible",
        "case_sensitive",
        "automation_id",
        "localization_key",
        "role",
    }
    _CLIENT_FILTERS: set = set()
    _SELECTOR_KEYS = _SERVER_FILTERS | {"include", "limit", "offset", "cursor"}

    def __init__(
        self,
        port: int,
        timeout: float = 10.0,
        profiler_url: Optional[str] = None,
        client_id: Optional[str] = None,
        session_id: Optional[str] = None,
        required_capabilities: Sequence[str] = (),
    ):
        """Create a correlated client for an already-known engine service port."""
        self.port = int(port)
        self.timeout = timeout
        self.base_url = f"http://127.0.0.1:{self.port}/automation-bridge/v1"
        self._remotery_url = profiler_url
        self._last_window_size: Optional[Tuple[int, int]] = None
        self._last_scene_sequence: Optional[int] = None
        # Defold v1 exposes query-only control endpoints with a bounded resource
        # string, so defaults are compact while remaining random per process/session.
        self.client_id = client_id or f"py-{uuid.uuid4().hex[:12]}"
        self.session_id = session_id or f"s-{uuid.uuid4().hex[:12]}"
        self._input_controller = InputController(self)
        self._logs = RuntimeLogs(self)
        self._active_traces: List[Any] = []
        self._required_capabilities = set(required_capabilities)
        self._last_health: Optional[JsonDict] = None

    @property
    def input(self) -> InputController:
        """Return queue, status, cancellation, lease, and device controls."""
        return self._input_controller

    @property
    def logs(self) -> RuntimeLogs:
        """Return the bounded background engine log collector."""
        return self._logs

    @classmethod
    def _from_editor(
        cls,
        editor: Any,
        build_command: Optional[str] = None,
        timeout: float = 20.0,
        required_capabilities: Sequence[str] = (),
    ) -> "Client":
        """Private editor-owned bootstrap hook for engine discovery."""
        fresh_build = build_command is not None
        if fresh_build:
            cls._close_candidate_engine_ports(editor)
            time.sleep(0.5)
            editor._build_and_run_command(build_command, timeout=timeout)

        def bridge_after_build() -> Optional["Client"]:
            service_ports = editor._engine_service_ports()
            registration_ports = editor._current_registration_engine_service_ports()
            profiler_url = cls._editor_profiler_url(editor, fresh_build=fresh_build)
            for service_port in service_ports:
                if not service_port:
                    continue
                try:
                    bridge = cls(
                        service_port,
                        profiler_url=profiler_url,
                        required_capabilities=required_capabilities,
                    )
                    health = bridge.health()
                    registration_is_authoritative = service_port in registration_ports
                    if not editor._validate_cached_engine_health(
                        service_port,
                        health,
                        fresh_build=fresh_build or registration_is_authoritative,
                    ):
                        continue
                except (IncompatibleApiVersionError, UnsupportedCapabilityError):
                    raise
                except AutomationBridgeError:
                    continue
                identity = health.get("identity", {}) if isinstance(health, Mapping) else {}
                editor._remember_engine_service_port(
                    service_port,
                    identity.get("engine_instance_id") if isinstance(identity, Mapping) else None,
                    identity.get("project_identity") if isinstance(identity, Mapping) else None,
                )
                editor._record_lifecycle("bridge_healthy", engine_instance_id=bridge.engine_instance_id)
                lifecycle = health.get("lifecycle", {})
                if isinstance(lifecycle, Mapping) and lifecycle.get("current_stage") == "initial_scene_ready":
                    editor._record_lifecycle("initial_scene_ready", engine_instance_id=bridge.engine_instance_id)
                if profiler_url:
                    editor._remember_remotery_url(profiler_url)
                bridge.logs.start()
                return bridge
            return None

        if fresh_build and cls._last_build_missing_engine_service_port(editor):
            return cls._recover_after_stale_build(editor, bridge_after_build, timeout, build_command)

        try:
            return cls._wait_for_bridge(
                bridge_after_build,
                timeout=timeout,
                message="Automation Bridge endpoint did not become ready",
                retry_exceptions=(AutomationBridgeError,),
            )
        except WaitTimeoutError:
            if not fresh_build:
                raise

        return cls._recover_after_stale_build(editor, bridge_after_build, timeout, build_command)

    @classmethod
    def _recover_after_stale_build(
        cls,
        editor: Any,
        bridge_after_build: Any,
        timeout: float,
        build_command: Optional[str],
    ) -> "Client":
        if build_command is None:
            raise RuntimeError("stale-build recovery requires a build command")
        cls._close_candidate_engine_ports(editor)
        time.sleep(0.5)
        editor._build_and_run_command(build_command, timeout=timeout)
        return cls._wait_for_bridge(
            bridge_after_build,
            timeout=timeout,
            message="Automation Bridge endpoint did not become ready after engine restart",
            retry_exceptions=(AutomationBridgeError,),
        )

    @classmethod
    def _wait_for_bridge(
        cls,
        probe: Any,
        timeout: float,
        message: str,
        retry_exceptions: RetryExceptions = (AutomationBridgeError,),
    ) -> "Client":
        """Poll transport failures while surfacing protocol/capability errors immediately."""
        started = time.monotonic()
        deadline = time.monotonic() + timeout
        last_error: Optional[BaseException] = None
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            try:
                bridge = probe()
                if bridge is not None:
                    return bridge
            except (IncompatibleApiVersionError, UnsupportedCapabilityError):
                raise
            except retry_exceptions as exc:
                last_error = exc
            time.sleep(0.1)
        error = WaitTimeoutError(
            message,
            last_value=None,
            elapsed=time.monotonic() - started,
            attempts=attempts,
            scene_sequence=None,
            last_exception=last_error,
        )
        if last_error is not None:
            raise error from last_error
        raise error

    @staticmethod
    def _last_build_missing_engine_service_port(editor: Any) -> bool:
        return editor._last_build_had_engine_service_port is False

    @staticmethod
    def _editor_profiler_url(editor: Any, fresh_build: bool) -> Optional[str]:
        if fresh_build:
            urls = editor._current_registration_remotery_urls()
            return urls[0] if urls else None
        return editor._remotery_url_value()

    def wait_ready(
        self,
        timeout: float = 20.0,
        retry_exceptions: RetryExceptions = (AutomationBridgeError,),
    ) -> JsonDict:
        """Wait until `/health` succeeds and return the health payload."""
        return wait_until(
            self.health,
            timeout=timeout,
            interval=0.1,
            message="Automation Bridge endpoint did not become ready",
            retry_exceptions=retry_exceptions,
            scene_sequence=lambda: getattr(self, "_last_scene_sequence", None),
        )

    def request(
        self,
        method: str,
        path: str,
        *,
        params: Optional[Mapping[str, Any]] = None,
        json: Optional[Mapping[str, Any]] = None,
    ) -> JsonDict:
        """Call a raw Automation Bridge path and return its ``data`` object."""
        return self._request(method.upper(), path, params, json_body=json)

    def health(self) -> JsonDict:
        """Return and validate API version, capabilities, identity, and backend data."""
        data = self._request("GET", "/health")
        self._validate_health(data)
        self._last_health = data
        screen = data.get("screen")
        if isinstance(screen, Mapping):
            self._remember_window_size(screen)
        return data

    @property
    def engine_instance_id(self) -> Optional[str]:
        """Return the last validated engine-instance id, if the native API supplies one."""
        if not isinstance(self._last_health, Mapping):
            return None
        identity = self._last_health.get("identity")
        if not isinstance(identity, Mapping):
            return None
        value = identity.get("engine_instance_id")
        return value if isinstance(value, str) and value else None

    def lifecycle(self) -> JsonDict:
        """Return native runtime lifecycle stages and their monotonic timestamps."""
        self.require("runtime.lifecycle")
        return self._request("GET", "/lifecycle")

    def require(self, *capabilities: str) -> "Client":
        """Declare mandatory capabilities and fail clearly when any are unavailable.

        A declaration may include a minimum feature version as ``name>=2``.
        """
        self._required_capabilities.update(capabilities)
        self._validate_required_capabilities(self.health())
        return self

    def supports(self, capability: str) -> bool:
        """Return whether the endpoint satisfies one capability declaration."""
        return self._capability_satisfies(capability, self._capability_versions(self.health()))

    def trace_metadata(self) -> JsonDict:
        """Return stable metadata to embed in trace bundles and CI artifacts."""
        health = self.health()
        return {
            "python_package_version": PYTHON_PACKAGE_VERSION,
            "native_version": health.get("native_version"),
            "api_version": health.get("version"),
            "capability_versions": self._capability_versions(health),
            "identity": health.get("identity"),
            "backend": health.get("backend"),
            "platform": health.get("platform"),
        }

    def _validate_health(self, health: Mapping[str, Any]) -> None:
        version = self._parse_version(health.get("version"), "native API")
        if version < self.SUPPORTED_API_VERSION_MIN or version > self.SUPPORTED_API_VERSION_MAX:
            raise IncompatibleApiVersionError(
                f"native Automation Bridge API version {version} is incompatible with Python "
                f"{PYTHON_PACKAGE_VERSION}; supported native range is "
                f"{self.SUPPORTED_API_VERSION_MIN}..{self.SUPPORTED_API_VERSION_MAX}"
            )
        self._validate_required_capabilities(health)

    def _validate_required_capabilities(self, health: Mapping[str, Any]) -> None:
        versions = self._capability_versions(health)
        missing = [declaration for declaration in sorted(self._required_capabilities) if not self._capability_satisfies(declaration, versions)]
        if missing:
            backend = health.get("backend", {})
            raise UnsupportedCapabilityError(
                f"required Automation Bridge capabilities are unavailable or too old: {', '.join(missing)}; "
                f"available={', '.join(sorted(versions)) or 'none'}; backend={backend!r}"
            )

    @staticmethod
    def _parse_version(value: Any, label: str) -> int:
        try:
            return int(str(value).split(".", 1)[0])
        except (TypeError, ValueError) as exc:
            raise IncompatibleApiVersionError(f"{label} returned invalid version {value!r}") from exc

    @classmethod
    def _capability_versions(cls, health: Mapping[str, Any]) -> Dict[str, int]:
        result: Dict[str, int] = {}
        capabilities = health.get("capabilities", [])
        if isinstance(capabilities, (list, tuple, set)):
            for name in capabilities:
                if isinstance(name, str):
                    result[name] = 1
        versions = health.get("capability_versions", {})
        if isinstance(versions, Mapping):
            for name, version in versions.items():
                if isinstance(name, str):
                    result[name] = cls._parse_version(version, f"capability {name}")
        return result

    @classmethod
    def _capability_satisfies(cls, declaration: str, versions: Mapping[str, int]) -> bool:
        name, separator, minimum = declaration.partition(">=")
        required_version = cls._parse_version(minimum, f"capability declaration {declaration}") if separator else 1
        return versions.get(name, 0) >= required_version

    def screen(self) -> JsonDict:
        """Return window, backbuffer, viewport, and coordinate metadata."""
        data = self._request("GET", "/screen")
        self._remember_window_size(data)
        return data

    def scene(
        self,
        visible: Optional[bool] = None,
        include: Optional[Union[str, Iterable[str]]] = None,
    ) -> JsonDict:
        """Return the runtime scene tree."""
        params: Dict[str, Any] = {}
        if visible is not None:
            params["visible"] = visible
        normalized_include = _normalize_include(include)
        if normalized_include:
            params["include"] = normalized_include
        return self._request("GET", "/scene", params)

    def elements(self, **selector: Any) -> List[Element]:
        """Return one server-filtered page of inspectable scene elements."""
        elements, _, _ = self._select_nodes(selector)
        return elements

    def element(self, **selector: Any) -> Element:
        """Return exactly one matching element or raise `SelectorError`."""
        elements, metadata, selector_text = self._select_nodes(selector)
        if len(elements) == 1:
            return elements[0]
        error = SelectorError(self._selector_error("expected exactly one element", selector, selector_text, elements, metadata))
        self._trace_record("selector_error", {"selector": selector, "error": str(error)})
        raise error

    def maybe_element(self, **selector: Any) -> Optional[Element]:
        """Return zero or one matching element, raising if multiple elements match."""
        elements, metadata, selector_text = self._select_nodes(selector)
        if len(elements) <= 1:
            return elements[0] if elements else None
        error = SelectorError(self._selector_error("expected zero or one element", selector, selector_text, elements, metadata))
        self._trace_record("selector_error", {"selector": selector, "error": str(error)})
        raise error

    def element_by_id(
        self,
        id: str,
        include: Optional[Union[str, Iterable[str]]] = "basic,bounds,properties,children",
    ) -> Element:
        """Fetch one element by stable Automation Bridge element id."""
        params: Dict[str, Any] = {"id": id}
        normalized_include = _normalize_include(include)
        if normalized_include:
            params["include"] = normalized_include
        return Element(self._request("GET", "/node", params)["node"])

    def parent(
        self,
        element_or_id: Union[Element, str],
        include: Optional[Union[str, Iterable[str]]] = "basic,bounds,properties",
    ) -> Element:
        """Return the parent element for a component or child element."""
        element = element_or_id if isinstance(element_or_id, Element) else self.element_by_id(element_or_id, include="basic")
        if not element.parent_id:
            raise SelectorError(f"element has no parent: {element.compact()}")
        return self.element_by_id(element.parent_id, include=include)

    def count(self, **selector: Any) -> int:
        """Return the complete native match count, independent of page size."""
        self._validate_selector(selector)
        params = self._server_params(selector, limit=0)
        return int(self._request("GET", "/nodes", params).get("matched", 0))

    def click(
        self,
        target: Union[Target, float, int],
        y: Optional[float] = None,
        wait: Union[str, bool, float] = "released",
        visualize: Optional[bool] = None,
        device: Optional[str] = None,
        pointer_id: int = 0,
        expected_scene_sequence: Optional[int] = None,
        timeout: float = 5.0,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Queue one FIFO click and optionally wait for the native release receipt."""
        if isinstance(target, Element):
            params: Dict[str, Any] = {"id": target.id}
        elif isinstance(target, str):
            params = {"id": target}
        else:
            x_value, y_value = self._point(target, y)
            params = {"x": x_value, "y": y_value}

        params.update(self._input_params())
        params.update({"visualize": visualize, "device": device, "expected_scene_sequence": expected_scene_sequence})
        if pointer_id:
            params["pointer_id"] = pointer_id
        receipt = InputReceipt(self._request("POST", "/input/click", json_body=params))
        return self._wait_input_compat(
            receipt, wait, timeout, cancel_on_interrupt, flush_on_interrupt
        )

    def drag(
        self,
        from_target: Target,
        to_target: Target,
        duration: float = 0.35,
        wait: Union[str, bool, float, None] = "released",
        visualize: Optional[bool] = None,
        easing: str = "linear",
        hold_before: float = 0.0,
        hold_after: float = 0.0,
        device: Optional[str] = None,
        pointer_id: int = 0,
        expected_scene_sequence: Optional[int] = None,
        timeout: Optional[float] = None,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Queue one FIFO drag and wait on native lifecycle state, never wall-clock guessing."""
        if self._is_node_ref(from_target) and self._is_node_ref(to_target):
            params: Dict[str, Any] = {
                "from_id": self._node_id(from_target),
                "to_id": self._node_id(to_target),
                "duration": duration,
            }
        else:
            x1, y1 = self._point(from_target)
            x2, y2 = self._point(to_target)
            params = {"x1": x1, "y1": y1, "x2": x2, "y2": y2, "duration": duration}

        controller_lease = min(60.0, max(5.0, duration + hold_before + hold_after + 2.0))
        params.update(self._input_params(lease=controller_lease))
        params.update({"visualize": visualize, "device": device, "expected_scene_sequence": expected_scene_sequence})
        if easing != "linear":
            params["easing"] = easing
        if hold_before:
            params["hold_before"] = hold_before
        if hold_after:
            params["hold_after"] = hold_after
        if pointer_id:
            params["pointer_id"] = pointer_id
        receipt = InputReceipt(self._request("POST", "/input/drag", json_body=params))
        return self._wait_input_compat(
            receipt,
            wait,
            timeout or max(5.0, duration + hold_before + hold_after + 5.0),
            cancel_on_interrupt,
            flush_on_interrupt,
        )

    def drag_path(
        self,
        points: Sequence[Target],
        durations: Sequence[float],
        easing: Union[str, Sequence[str]] = "linear",
        hold_before: float = 0.0,
        hold_after: float = 0.0,
        path: str = "sampled",
        visualize: Optional[bool] = None,
        wait: Union[str, bool, float, None] = "released",
        device: Optional[str] = None,
        pointer_id: int = 0,
        expected_scene_sequence: Optional[int] = None,
        timeout: Optional[float] = None,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Run one down/path/up gesture with native segment timing, easing, holds, and curves."""
        normalized_points = [self._point(point) for point in points]
        if path not in {"sampled", "linear", "quadratic", "cubic"}:
            raise ValueError("path must be sampled, linear, quadratic, or cubic")
        expected_segments = len(normalized_points) - 1 if path in {"sampled", "linear"} else 1
        if len(normalized_points) < 2:
            raise ValueError("drag_path requires at least two points")
        if path == "quadratic" and len(normalized_points) != 3:
            raise ValueError("quadratic drag_path requires exactly three points")
        if path == "cubic" and len(normalized_points) != 4:
            raise ValueError("cubic drag_path requires exactly four points")
        if len(durations) != expected_segments:
            raise ValueError(f"drag_path requires exactly {expected_segments} durations")
        duration_values = [self._input_duration(value, "duration") for value in durations]
        hold_before = self._input_duration(hold_before, "hold_before")
        hold_after = self._input_duration(hold_after, "hold_after")
        total_duration = sum(duration_values) + hold_before + hold_after
        if total_duration > 60.0:
            raise ValueError("total gesture duration exceeds 60 seconds")
        easing_values = [easing] * expected_segments if isinstance(easing, str) else list(easing)
        if len(easing_values) != expected_segments or any(value not in _INPUT_EASINGS for value in easing_values):
            raise ValueError(f"drag_path requires {expected_segments} supported easing values")
        params = self._input_params(lease=min(60.0, max(5.0, total_duration + 2.0)))
        params.update(
            {
                "points": ";".join(f"{x},{y}" for x, y in normalized_points),
                "durations": ",".join(str(value) for value in duration_values),
                "easing": ",".join(easing_values),
                "hold_before": hold_before,
                "hold_after": hold_after,
                "path": path,
                "visualize": visualize,
                "device": device,
                "pointer_id": pointer_id,
                "expected_scene_sequence": expected_scene_sequence,
            }
        )
        receipt = InputReceipt(self._request("POST", "/input/drag_path", json_body=params))
        return self._wait_input_compat(
            receipt,
            wait,
            timeout or max(5.0, total_duration + 5.0),
            cancel_on_interrupt,
            flush_on_interrupt,
        )

    def pointer(
        self,
        start: Target,
        lease: float = 2.0,
        visualize: Optional[bool] = None,
        device: Optional[str] = None,
        pointer_id: int = 0,
        expected_scene_sequence: Optional[int] = None,
    ) -> PointerSession:
        """Press a leased pointer for continuous `move`/`hold` operations and safe cleanup."""
        lease = self._input_duration(lease, "lease")
        if lease <= 0:
            raise ValueError("lease must be greater than zero")
        x, y = self._point(start)
        params = self._input_params(lease=max(5.0, lease))
        params.update(
            {
                "x": x,
                "y": y,
                "pointer_lease": lease,
                "visualize": visualize,
                "device": device,
                "pointer_id": pointer_id,
                "expected_scene_sequence": expected_scene_sequence,
            }
        )
        receipt = InputReceipt(self._request("POST", "/input/pointer/open", json_body=params))
        return PointerSession(self, receipt, lease)

    def type_text(
        self,
        text: str,
        wait: Union[str, bool] = False,
        expected_scene_sequence: Optional[int] = None,
        timeout: float = 10.0,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Queue FIFO text input and optionally wait for native completion."""
        params = self._input_params()
        params.update({"text": text, "expected_scene_sequence": expected_scene_sequence})
        receipt = InputReceipt(self._request("POST", "/input/key", json_body=params))
        return self._wait_input_compat(
            receipt, wait, timeout, cancel_on_interrupt, flush_on_interrupt
        )

    def key(
        self,
        key: str,
        wait: Union[str, bool] = False,
        expected_scene_sequence: Optional[int] = None,
        timeout: float = 10.0,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        """Queue a FIFO special key such as `KEY_ENTER`."""
        keys = key if key.startswith("{") and key.endswith("}") else f"{{{key}}}"
        params = self._input_params()
        params.update({"keys": keys, "expected_scene_sequence": expected_scene_sequence})
        receipt = InputReceipt(self._request("POST", "/input/key", json_body=params))
        return self._wait_input_compat(
            receipt, wait, timeout, cancel_on_interrupt, flush_on_interrupt
        )

    def events(self, from_cursor: Union[str, int] = "now") -> EventStream:
        """Create a cursor subscription, resolving ``'now'`` before returning.

        Use ``with bridge.events('now') as events`` before sending an action to
        avoid the classic subscribe-after-action race.
        """
        return EventStream(self, from_cursor=from_cursor)

    def wait_for_input_acknowledgement(
        self,
        input_id: int,
        timeout: float = 10.0,
        events: Optional[EventStream] = None,
    ) -> Event:
        """Wait for the application's opt-in acknowledgement of ``input_id``.

        Native input release and application acknowledgement are separate
        lifecycle stages. Pass a stream created before the input for strict
        race-free ordering; without one this searches the retained ring.
        """
        stream = events or self.events(from_cursor="oldest")
        try:
            return stream.wait(
                "input.acknowledged",
                where={"input_id": int(input_id)},
                event_type="acknowledgement",
                timeout=timeout,
            )
        finally:
            if events is None:
                stream.close()

    def states(self, name: Optional[str] = None) -> JsonDict:
        """Return published JSON states and the latest global revision."""
        return self._request("GET", "/state", {"name": name} if name is not None else None)

    def state(self, name: str) -> StateSnapshot:
        """Return one published state by its exact application name."""
        entries = self.states(name=name).get("states", [])
        if len(entries) != 1 or not isinstance(entries[0], Mapping):
            raise AutomationBridgeError(f"published state {name!r} is unavailable")
        return StateSnapshot(entries[0])

    def wait_for_state(
        self,
        path: str,
        expected: Any,
        timeout: float = 10.0,
        after_revision: Optional[int] = None,
        state_name: Optional[str] = None,
    ) -> StateSnapshot:
        """Wait for a JSON state path to equal ``expected`` without missing revisions.

        Pass ``after_revision=bridge.state(name).revision`` when an already
        matching value must not satisfy a wait for a *new* publication.
        ``state_name`` disambiguates a path before that state has first appeared.
        """
        snapshot = self.states()
        entries = [item for item in snapshot.get("states", []) if isinstance(item, Mapping)]
        current_revision = int(snapshot.get("revision", 0))
        selected = select_state_path(entries, path, state_name=state_name)
        if after_revision is None and selected is not None and selected.value == expected:
            return selected
        cursor = current_revision if after_revision is None else int(after_revision)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                observed = selected.value if selected is not None else "<unpublished>"
                raise TimeoutError(
                    f"state {path!r} did not become {expected!r} within {timeout:g}s; "
                    f"last value={observed!r}, revision={cursor}"
                )
            safe_wait = min(remaining, max(0.0, float(self.timeout) - 0.1), 1.0)
            changed = self._request(
                "GET", "/state/wait",
                {"after_revision": cursor, "timeout_ms": int(safe_wait * 1000), "name": state_name},
            )
            cursor = max(cursor, int(changed.get("revision", cursor)))
            changed_entries = [item for item in changed.get("states", []) if isinstance(item, Mapping)]
            candidate = select_state_path(changed_entries, path, state_name=state_name)
            if candidate is not None:
                selected = candidate
                if candidate.value == expected and candidate.revision > (after_revision or 0):
                    return candidate

    def start_command(self, name: str, data: Any = None, timeout: float = 30.0) -> JsonDict:
        """Submit a registered named Lua command and return its pending id."""
        if timeout <= 0 or timeout > 300:
            raise ValueError("command timeout must be greater than 0 and at most 300 seconds")
        payload = json.dumps({} if data is None else data, allow_nan=False, separators=(",", ":"))
        if len(payload.encode("utf-8")) > 32768:
            raise ValueError("command JSON payload exceeds 32768 bytes")
        return self._request(
            "POST", "/commands",
            {"name": name, "data": payload, "timeout_ms": max(1, int(timeout * 1000))},
        )

    def command_status(self, command_id: int) -> JsonDict:
        """Return command state, JSON result, error, and timestamps."""
        return self._request("GET", "/commands", {"id": int(command_id)})

    def cancel_command(self, command_id: int) -> JsonDict:
        """Cancel a queued command; running Lua callbacks are never preempted."""
        return self._request("DELETE", "/commands", {"id": int(command_id)})

    def wait_for_command(self, command_id: int, timeout: float = 30.0, interval: float = 0.02) -> JsonDict:
        """Wait for a command result, cancelling a still-pending command on timeout."""
        deadline = time.monotonic() + timeout
        terminal = {"completed", "failed", "cancelled", "timed_out"}
        while True:
            status = self.command_status(command_id)
            if status.get("state") in terminal:
                return status
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                cancellation_error: Optional[BaseException] = None
                try:
                    self.cancel_command(command_id)
                except AutomationBridgeError as exc:
                    cancellation_error = exc
                raise CommandTimeout(command_id, timeout, cancellation_error) from cancellation_error
            time.sleep(min(interval, remaining))

    def command(self, name: str, data: Any = None, timeout: float = 30.0) -> JsonDict:
        """Run a registered command and return its terminal result record."""
        accepted = self.start_command(name, data=data, timeout=timeout)
        result = self.wait_for_command(int(accepted["command_id"]), timeout=timeout)
        if result.get("state") != "completed":
            raise AutomationBridgeError(
                f"command {result.get('command_id')} {result.get('state')}: {result.get('error')}"
            )
        return result

    def mark(
        self,
        name: str,
        data: Any = None,
        recording_timestamp_us: Optional[int] = None,
    ) -> JsonDict:
        """Add a trace marker with native and host recording-clock timestamps."""
        payload = json.dumps({} if data is None else data, allow_nan=False, separators=(",", ":"))
        if len(payload.encode("utf-8")) > 32768:
            raise ValueError("marker JSON payload exceeds 32768 bytes")
        if recording_timestamp_us is None:
            recording_timestamp_us = time.monotonic_ns() // 1000
        return self._request(
            "POST", "/markers",
            {"name": name, "data": payload, "recording_timestamp_us": int(recording_timestamp_us)},
        )

    def screenshot(
        self,
        wait: bool = True,
        timeout: float = 5.0,
        after_frames: int = 0,
        resolution_multiplier: Optional[float] = None,
        retry_exceptions: RetryExceptions = (),
    ) -> ScreenshotReceipt:
        """Capture a PNG, optionally returning a lower-resolution derived image."""
        if not isinstance(after_frames, int) or after_frames < 0 or after_frames > 600:
            raise ValueError("after_frames must be an integer from 0 through 600")
        if resolution_multiplier is not None:
            if (
                not isinstance(resolution_multiplier, (int, float))
                or isinstance(resolution_multiplier, bool)
                or not 0.01 <= float(resolution_multiplier) <= 1.0
            ):
                raise ValueError("screenshot resolution_multiplier must be from 0.01 through 1.0")
            resolution_multiplier = float(resolution_multiplier)
            if not wait and resolution_multiplier < 1.0:
                raise ValueError("a downscaled screenshot requires wait=True")
        response = self._request("GET", "/screenshot", {"after_frames": after_frames})
        receipt = ScreenshotReceipt(response)
        if not wait:
            return receipt
        if receipt.capture_id <= 0:
            raise AutomationBridgeError("native screenshot response did not include a capture_id")

        def completed() -> Optional[ScreenshotReceipt]:
            current = ScreenshotReceipt(self._request("GET", "/screenshot/status", {"capture_id": receipt.capture_id}))
            if current.state == "failed":
                raise AutomationBridgeError(
                    f"screenshot {current.capture_id} failed: {current.failure_reason or 'unknown reason'}"
                )
            return current if current.state == "complete" else None

        completed_receipt = wait_until(
            completed,
            timeout=timeout,
            interval=0.02,
            message=f"screenshot capture {receipt.capture_id} did not complete",
            retry_exceptions=retry_exceptions,
            scene_sequence=lambda: self._last_scene_sequence,
        )
        if resolution_multiplier is None or resolution_multiplier == 1.0:
            return completed_receipt
        return self._scaled_screenshot(completed_receipt, resolution_multiplier)

    @staticmethod
    def _scaled_screenshot(receipt: ScreenshotReceipt, multiplier: float) -> ScreenshotReceipt:
        from .visual import _resize_png

        data, width, height = _resize_png(receipt, multiplier)
        source = receipt.path
        suffix = source.suffix or ".png"
        destination = source.with_name(f"{source.stem}.scale-{multiplier:g}{suffix}")
        destination.write_bytes(data)
        scaled = dict(receipt.raw)
        scaled.update(
            {
                "path": str(destination),
                "width": width,
                "height": height,
                "sha256": hashlib.sha256(data).hexdigest(),
                "source_path": str(source),
                "resolution_multiplier": multiplier,
            }
        )
        return ScreenshotReceipt(scaled)

    def convert_point(
        self,
        point: Union[Mapping[str, Any], Sequence[float]],
        from_space: str,
        to_space: str,
    ) -> Mapping[str, Any]:
        """Convert a top-left-origin point through native window/viewport geometry."""
        x, y = self._point(point)
        data = self._request(
            "POST", "/coordinates/convert", json_body=
            {"point": {"x": x, "y": y}, "from_space": from_space, "to_space": to_space},
        )
        converted = data.get("point")
        if not isinstance(converted, Mapping):
            raise AutomationBridgeError("coordinate conversion response did not include a point")
        return converted

    def engine_info(self) -> JsonDict:
        """Return Defold engine service `/info`, including version, platform, sha1, and log port."""
        status, response = request_json(f"http://127.0.0.1:{self.port}/info", timeout=self.timeout)
        if status < 200 or status >= 300:
            raise HttpError("GET", f"http://127.0.0.1:{self.port}/info", f"unexpected status {status}", status=status)
        return response

    def engine_log_port(self) -> int:
        """Return the Defold TCP log service port from engine `/info`."""
        value = self.engine_info().get("log_port")
        try:
            port = int(value)
        except (TypeError, ValueError) as exc:
            raise AutomationBridgeError(f"engine /info did not include a valid log_port: {value!r}") from exc
        if port <= 0:
            raise AutomationBridgeError(f"engine log service is unavailable: {value!r}")
        return port

    def log_stream(
        self,
        timeout: float = 2.0,
        read_timeout: Optional[float] = None,
        host: str = "127.0.0.1",
    ) -> EngineLogStream:
        """Open Defold's TCP log stream. Use as `with bridge.log_stream() as logs:`."""
        return EngineLogStream(host, self.engine_log_port(), timeout=timeout, read_timeout=read_timeout)

    def read_logs(
        self,
        duration: float = 1.0,
        limit: Optional[int] = None,
        idle_timeout: float = 0.1,
    ) -> List[str]:
        """Collect future engine log lines for `duration` seconds or until `limit` lines are read."""
        lines: List[str] = []
        deadline = time.monotonic() + max(0.0, duration)
        with self.log_stream(timeout=self.timeout) as logs:
            while time.monotonic() < deadline:
                if limit is not None and len(lines) >= limit:
                    break
                remaining = deadline - time.monotonic()
                line = logs.readline(timeout=max(0.0, min(idle_timeout, remaining)))
                if line is not None:
                    lines.append(line)
        return lines

    @property
    def profiler(self) -> "ProfilerClient":
        """Return a client for Defold's built-in engine profiler endpoints."""
        from .profiler import ProfilerClient

        return ProfilerClient(self.port, timeout=self.timeout, profiler_url=self._remotery_url)

    @property
    def gestures(self) -> "GestureGenerator":
        """Return deterministic, dependency-free generated gesture helpers."""
        from .gestures import GestureGenerator

        return GestureGenerator(self)

    @property
    def visual(self) -> "VisualClient":
        """Return explicit pixel comparison and visual wait helpers."""
        from .visual import VisualClient

        return VisualClient(self)

    @property
    def video_recording(self) -> "VideoRecordingClient":
        """Return optional video recording helpers."""
        from .recording import VideoRecordingClient

        return VideoRecordingClient(self)

    def trace(
        self,
        path: Union[str, Path],
        *,
        screenshots: str = "on_error",
        screenshot_directory: Optional[Union[str, Path]] = None,
        prerequisites: Optional[Mapping[str, Any]] = None,
    ) -> "TraceSession":
        """Create a diagnostic trace context with explicitly best-effort replay."""
        from .trace import TraceSession

        return TraceSession(
            self,
            path,
            screenshots=screenshots,
            screenshot_directory=screenshot_directory,
            prerequisites=prerequisites,
        )

    @property
    def profiler_url(self) -> Optional[str]:
        """Return the profiler stream URL discovered while bootstrapping, if known."""
        return self._remotery_url

    @property
    def last_window_size(self) -> Optional[Tuple[int, int]]:
        """Return the last known `(width, height)` from `screen()`, `health()`, or `resize()`."""
        return self._last_window_size

    def resize(self, width: int, height: int, wait: float = 0.25) -> JsonDict:
        """Request a resize and return requested, window, viewport, and outcome data."""
        _validate_screen_size(width, height)
        capabilities = self.health().get("capabilities", [])
        if not isinstance(capabilities, (list, tuple, set)) or "screen.resize" not in capabilities:
            raise AutomationBridgeError("Automation Bridge endpoint does not advertise the screen.resize capability")

        response = self._request("PUT", "/screen", json_body={"width": width, "height": height})
        screen_value = response.get("screen") if isinstance(response, Mapping) else None
        screen = dict(screen_value) if isinstance(screen_value, Mapping) else dict(response)
        self._remember_window_size(screen)
        if wait:
            deadline = time.monotonic() + wait
            while time.monotonic() < deadline:
                screen = self.screen()
                window = screen.get("window")
                if isinstance(window, Mapping) and window.get("width") == width and window.get("height") == height:
                    break
                time.sleep(min(0.02, max(0.0, deadline - time.monotonic())))
        window = screen.get("window") if isinstance(screen, Mapping) else None
        observed_width = None
        observed_height = None
        if isinstance(window, Mapping):
            observed_width = window.get("width")
            observed_height = window.get("height")
        window_matches = observed_width == width and observed_height == height
        backbuffer = screen.get("backbuffer") if isinstance(screen, Mapping) else None
        backbuffer_matches = isinstance(backbuffer, Mapping) and backbuffer.get("width") == width and backbuffer.get("height") == height
        result = dict(response)
        result.update(
            {
                "width": observed_width if isinstance(observed_width, int) else width,
                "height": observed_height if isinstance(observed_height, int) else height,
                "requested": {"width": width, "height": height},
                "screen": screen,
                "window_matches": window_matches,
                "backbuffer_matches": backbuffer_matches,
            }
        )
        if not window_matches:
            result["outcome"] = "requested_not_observed_before_timeout"
        elif result.get("outcome") == "requested_not_observed":
            result["outcome"] = "resized"
        elif "outcome" not in result:
            result["outcome"] = "resized"
        return result

    def set_portrait(self, wait: float = 0.25) -> JsonDict:
        """Resize to portrait by swapping the last known width and height when needed."""
        width, height = self._known_window_size()
        if width > height:
            return self.resize(height, width, wait=wait)
        return self._unchanged_resize_result(width, height)

    def set_landscape(self, wait: float = 0.25) -> JsonDict:
        """Resize to landscape by swapping the last known width and height when needed."""
        width, height = self._known_window_size()
        if width < height:
            return self.resize(height, width, wait=wait)
        return self._unchanged_resize_result(width, height)

    def _unchanged_resize_result(self, width: int, height: int) -> JsonDict:
        screen = self.screen()
        return {
            "width": width,
            "height": height,
            "requested": {"width": width, "height": height},
            "outcome": "already_correct",
            "window_matches": True,
            "backbuffer_matches": (
                isinstance(screen.get("backbuffer"), Mapping)
                and screen["backbuffer"].get("width") == width
                and screen["backbuffer"].get("height") == height
            ),
            "screen": screen,
        }

    def reboot(self, *args: str, wait: bool = True, timeout: Optional[float] = None) -> None:
        """Reboot the engine through `/post/@system/reboot` with up to six command-line args."""
        payload = _encode_system_reboot(args)
        self._post_engine_message("/post/@system/reboot", payload, timeout=timeout)
        self._last_window_size = None
        if wait:
            wait_timeout = self.timeout if timeout is None else timeout
            self._wait_ready_after_reboot(wait_timeout)

    def close_engine(self, timeout: float = 2.0) -> None:
        """Ask the running Defold engine to exit, falling back to the local listener PID."""
        self._logs.close()
        url = f"http://127.0.0.1:{self.port}/post/@system/exit"
        try:
            self._post_engine_message("/post/@system/exit", b"\010\000", timeout=timeout)
        except AutomationBridgeError:
            if self._terminate_process_on_port(timeout):
                return
            raise

        if self._wait_until_unavailable(timeout):
            return
        if self._terminate_process_on_port(timeout):
            return
        raise HttpError("POST", url, "engine did not stop after exit request")

    @classmethod
    def _close_candidate_engine_ports(cls, editor: Any) -> None:
        service_ports = editor._engine_service_ports()
        for service_port in service_ports:
            if not service_port:
                continue
            editor._record_lifecycle("previous_engine_closing", port=service_port)
            try:
                cls(service_port, timeout=1.0).close_engine(timeout=1.0)
            except AutomationBridgeError as exc:
                editor._record_lifecycle("previous_engine_close_failed", port=service_port, error=str(exc))
                continue
            editor._record_lifecycle("previous_engine_closed", port=service_port)

    def _wait_until_unavailable(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.health()
            except AutomationBridgeError:
                return True
            time.sleep(0.05)
        return False

    def _wait_ready_after_reboot(self, timeout: float) -> JsonDict:
        deadline = time.monotonic() + timeout
        accept_ready_after = time.monotonic() + min(0.25, max(0.0, timeout) / 2.0)
        saw_unavailable = False
        last_error: Optional[BaseException] = None

        while time.monotonic() <= deadline:
            try:
                data = self.health()
            except AutomationBridgeError as exc:
                saw_unavailable = True
                last_error = exc
            else:
                if saw_unavailable or time.monotonic() >= accept_ready_after:
                    return data

            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                break
            time.sleep(min(0.05, remaining))

        try:
            data = self.health()
        except AutomationBridgeError as exc:
            last_error = exc
        else:
            if saw_unavailable or time.monotonic() >= accept_ready_after:
                return data

        message = "Automation Bridge endpoint did not become ready after reboot"
        if last_error:
            raise AssertionError(f"{message}: {last_error}") from last_error
        raise AssertionError(message)

    def _terminate_process_on_port(self, timeout: float) -> bool:
        for pid in self._listening_pids(self.port):
            if pid == os.getpid():
                continue
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                continue
        return self._wait_until_unavailable(timeout)

    @staticmethod
    def _listening_pids(port: int) -> List[int]:
        if sys.platform.startswith("win"):
            return []
        try:
            output = subprocess.check_output(
                ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=1.0,
            )
        except (OSError, subprocess.SubprocessError):
            return []

        pids: List[int] = []
        for line in output.splitlines():
            try:
                pid = int(line.strip())
            except ValueError:
                continue
            if pid not in pids:
                pids.append(pid)
        return pids

    def dump_scene(
        self,
        path: Optional[Union[str, Path]] = None,
        visible: Optional[bool] = None,
        include: Optional[Union[str, Iterable[str]]] = None,
    ) -> Union[str, Path]:
        """Return scene JSON text or write it to `path`."""
        text = json.dumps(self.scene(visible=visible, include=include), indent=2, sort_keys=True)
        if path is None:
            return text
        output_path = Path(path)
        output_path.write_text(text + "\n", encoding="utf-8")
        return output_path

    def format_elements(self, elements: Optional[Iterable[Element]] = None, **selector: Any) -> str:
        """Format elements as compact one-line diagnostics."""
        selected = list(elements if elements is not None else self.elements(**selector))
        return "\n".join(element.compact() for element in selected)

    def wait_for_element(
        self,
        timeout: float = 10.0,
        interval: float = 0.1,
        after_scene_sequence: Optional[int] = None,
        retry_exceptions: RetryExceptions = (SelectorError,),
        **selector: Any,
    ) -> Element:
        """Wait for one element, optionally requiring a newer scene sequence."""
        def one_element() -> Optional[Element]:
            elements, _, _ = self._select_nodes(selector)
            if len(elements) != 1:
                return None
            element = elements[0]
            if after_scene_sequence is not None and element.scene_sequence <= after_scene_sequence:
                return None
            return element

        return wait_until(
            one_element,
            timeout=timeout,
            interval=interval,
            message=f"element did not appear after scene sequence {after_scene_sequence}: {selector}",
            retry_exceptions=retry_exceptions,
            scene_sequence=lambda: getattr(self, "_last_scene_sequence", None),
        )

    def wait_for_count(
        self,
        expected: int,
        timeout: float = 10.0,
        interval: float = 0.1,
        retry_exceptions: RetryExceptions = (),
        **selector: Any,
    ) -> int:
        """Wait for an exact count, retaining each observed count in diagnostics."""
        return wait_until(
            lambda: self.count(**selector),
            timeout=timeout,
            interval=interval,
            message=f"element count did not become {expected}: {selector}",
            retry_exceptions=retry_exceptions,
            scene_sequence=lambda: getattr(self, "_last_scene_sequence", None),
            predicate=lambda count: count == expected,
        )

    def wait_frames(
        self,
        count: int,
        timeout: Optional[float] = None,
        interval: float = 0.005,
    ) -> JsonDict:
        """Wait for ``count`` native engine updates and return the final frame receipt.

        When ``timeout`` is omitted, allow the greater of five seconds or the
        requested frame count at 30 FPS. Explicit timeouts are used unchanged.
        A timeout reports the initial and last observed frames, whether frames
        advanced, and a best-effort runtime health/lifecycle snapshot.
        """
        if not isinstance(count, int) or count < 0:
            raise ValueError("frame count must be a non-negative integer")
        if timeout is not None and timeout < 0:
            raise ValueError("timeout must be non-negative")
        effective_timeout = max(5.0, count / 30.0) if timeout is None else float(timeout)
        initial = self._request("GET", "/frame")
        initial_frame = int(initial.get("engine_frame", 0))
        target = initial_frame + count
        if count == 0:
            return initial

        try:
            return wait_until(
                lambda: self._request("GET", "/frame"),
                timeout=effective_timeout,
                interval=interval,
                message=f"engine frame did not reach {target}",
                predicate=lambda data: int(data.get("engine_frame", 0)) >= target,
            )
        except WaitTimeoutError as error:
            last = error.last_value if isinstance(error.last_value, Mapping) else {}
            last_frame = int(last.get("engine_frame", initial_frame))
            lifecycle_stage: Any = "unknown"
            engine_health: Any = "unavailable"
            try:
                health = self.health()
                engine_health = "reachable"
                lifecycle = health.get("lifecycle")
                if isinstance(lifecycle, Mapping):
                    lifecycle_stage = lifecycle.get("current_stage", "unknown")
            except Exception as health_error:
                engine_health = f"unavailable ({type(health_error).__name__}: {health_error})"

            diagnostics = (
                f"initial_frame={initial_frame}, last_observed_frame={last_frame}, "
                f"frames_advancing={last_frame > initial_frame}, "
                f"lifecycle_stage={lifecycle_stage!r}, engine_health={engine_health}"
            )
            enriched = WaitTimeoutError(
                f"engine frame did not reach {target}; {diagnostics}",
                last_value=error.last_value,
                elapsed=error.elapsed,
                attempts=error.attempts,
                scene_sequence=error.scene_sequence,
                last_exception=error.last_exception,
            )
            raise enriched from error

    def observe_element(
        self,
        minimum_frames: int = 3,
        timeout: float = 10.0,
        interval: float = 0.01,
        identity: str = "logical",
        **selector: Any,
    ) -> ObservationReceipt:
        """Require one element identity across distinct frames and return its observation span."""
        if minimum_frames <= 0:
            raise ValueError("minimum_frames must be positive")
        if identity not in {"logical", "snapshot", "instance"}:
            raise ValueError("identity must be logical, snapshot, or instance")
        observation: Dict[str, Any] = {}

        def sample() -> Optional[ObservationReceipt]:
            node = self.maybe_element(**selector)
            if node is None:
                observation.clear()
                return None
            node_identity = {
                "logical": node.logical_id or node.snapshot_id,
                "snapshot": node.snapshot_id,
                "instance": node.instance_id or node.snapshot_id,
            }[identity]
            if observation.get("identity") != node_identity:
                observation.update(
                    identity=node_identity,
                    node=node,
                    first_frame=node.engine_frame,
                    last_frame=node.engine_frame,
                    first_sequence=node.scene_sequence,
                    last_sequence=node.scene_sequence,
                    frames=1,
                )
            elif node.engine_frame != observation["last_frame"]:
                observation["node"] = node
                observation["last_frame"] = node.engine_frame
                observation["last_sequence"] = node.scene_sequence
                observation["frames"] += 1
            if observation["frames"] < minimum_frames:
                return None
            return ObservationReceipt(
                element=observation["node"],
                identity=observation["identity"],
                first_frame=observation["first_frame"],
                last_frame=observation["last_frame"],
                first_scene_sequence=observation["first_sequence"],
                last_scene_sequence=observation["last_sequence"],
                observed_frames=observation["frames"],
            )

        return wait_until(
            sample,
            timeout=timeout,
            interval=interval,
            message=f"element was not observed for {minimum_frames} distinct frames: {selector}",
        )

    def wait_for_disappearance(
        self,
        element_id: str,
        timeout: float = 10.0,
        interval: float = 0.02,
    ) -> ObservationReceipt:
        """Wait until a snapshot id is absent and return its last and disappearance receipts."""
        first = self.maybe_element(id=element_id)
        last = first
        first_frame = first.engine_frame if first else 0
        first_sequence = first.scene_sequence if first else 0
        observed = 1 if first else 0

        def disappeared() -> Optional[ObservationReceipt]:
            nonlocal last, observed
            node = self.maybe_element(id=element_id)
            if node is not None:
                if last is None or node.engine_frame != last.engine_frame:
                    observed += 1
                last = node
                return None
            current = self._request("GET", "/frame")
            return ObservationReceipt(
                element=last,
                identity=(last.logical_id or last.snapshot_id) if last else element_id,
                first_frame=first_frame,
                last_frame=last.engine_frame if last else first_frame,
                first_scene_sequence=first_sequence,
                last_scene_sequence=last.scene_sequence if last else first_sequence,
                observed_frames=observed,
                disappeared_frame=int(current.get("engine_frame", 0)),
                disappeared_scene_sequence=int(current.get("scene_sequence", 0)),
            )

        return wait_until(
            disappeared,
            timeout=timeout,
            interval=interval,
            message=f"element did not disappear: {element_id}",
        )

    def _request(
        self,
        method: str,
        path: str,
        params: Optional[Mapping[str, Any]] = None,
        json_body: Optional[Mapping[str, Any]] = None,
    ) -> JsonDict:
        url = self.base_url + path
        encoded_params = self._encoded_params(params)
        if encoded_params:
            url += "?" + urllib.parse.urlencode(encoded_params)

        request_trace = {"method": method, "path": path, "params": dict(params or {})}
        if json_body is not None:
            request_trace["json_body"] = dict(json_body)
        try:
            status, response = request_json(url, method=method, timeout=self.timeout, json_body=json_body)
        except BaseException as error:
            request_trace["error"] = repr(error)
            self._trace_record("action" if path.startswith("/input/") else "request_error", request_trace)
            raise
        if not response.get("ok"):
            error = response.get("error", {})
            code = str(error.get("code", "unknown"))
            message = str(error.get("message", response))
            request_trace.update({"status": status, "error": response})
            self._trace_record("action" if path.startswith("/input/") else "request_error", request_trace)
            raise AutomationBridgeApiError(code, message, status, response)
        data = response.get("data", {})
        self._remember_scene_sequence(data)
        request_trace.update({"status": status, "response": data})
        self._trace_record("action" if path.startswith("/input/") else "request", request_trace)
        return data

    def _request_json(self, method: str, path: str, payload: Mapping[str, Any]) -> JsonDict:
        url = self.base_url + path
        compact_payload = {key: value for key, value in payload.items() if value is not None}
        data = json.dumps(compact_payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        status, response = request_json(
            url,
            method=method,
            timeout=self.timeout,
            data=data,
            headers={"Content-Type": "application/json"},
        )
        if not response.get("ok"):
            error = response.get("error", {})
            code = str(error.get("code", "unknown"))
            message = str(error.get("message", response))
            raise AutomationBridgeApiError(code, message, status, response)
        data = response.get("data", {})
        self._remember_scene_sequence(data)
        return data

    def _input_params(self, lease: float = 5.0) -> Dict[str, Any]:
        """Return ownership and per-request correlation fields for mutating input calls."""
        params: Dict[str, Any] = {
            "client_id": self.client_id,
            "session_id": self.session_id,
            "request_id": f"r-{uuid.uuid4().hex[:12]}",
        }
        if lease != 5.0:
            params["lease"] = lease
        return params

    def _wait_input_compat(
        self,
        receipt: InputReceipt,
        wait: Union[str, bool, float, None],
        timeout: float,
        cancel_on_interrupt: bool = True,
        flush_on_interrupt: bool = False,
    ) -> InputReceipt:
        if wait is False or wait == 0:
            return receipt
        if wait is None or wait is True:
            state = "released"
        elif isinstance(wait, str):
            state = wait
        elif isinstance(wait, (int, float)):
            state = "released"
            timeout = float(wait)
        else:
            raise TypeError("wait must be a lifecycle state, bool, numeric timeout, or None")
        return self.input.wait(
            receipt,
            state=state,
            timeout=timeout,
            cancel_on_interrupt=cancel_on_interrupt,
            flush_on_interrupt=flush_on_interrupt,
        )

    @staticmethod
    def _input_duration(value: Any, name: str) -> float:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise TypeError(f"{name} must be a number")
        result = float(value)
        if result < 0.0 or result > 60.0 or result != result or result in {float("inf"), float("-inf")}:
            raise ValueError(f"{name} must be finite and between 0 and 60 seconds")
        return result

    def _trace_record(self, kind: str, payload: Any) -> None:
        for trace in tuple(self._active_traces):
            trace.record(kind, payload)

    def _post_engine_message(self, path: str, payload: bytes, timeout: Optional[float] = None) -> bytes:
        url = f"http://127.0.0.1:{self.port}{path}"
        status, body = request_bytes(url, payload, timeout=self.timeout if timeout is None else timeout)
        if status < 200 or status >= 300:
            preview = body[:200].decode("utf-8", "replace")
            raise HttpError("POST", url, f"unexpected status {status}: {preview}", status=status)
        return body

    def _remember_window_size(self, screen: Mapping[str, Any]) -> None:
        window = screen.get("window")
        if not isinstance(window, Mapping):
            return
        width = window.get("width")
        height = window.get("height")
        if isinstance(width, int) and isinstance(height, int) and width > 0 and height > 0:
            self._last_window_size = (width, height)

    def _remember_scene_sequence(self, data: Any) -> None:
        if not isinstance(data, Mapping):
            return
        sequence = data.get("scene_sequence")
        if isinstance(sequence, int) and not isinstance(sequence, bool):
            self._last_scene_sequence = sequence

    def _known_window_size(self) -> Tuple[int, int]:
        if self._last_window_size is None:
            self.screen()
        if self._last_window_size is None:
            raise AutomationBridgeError("window size is unavailable")
        return self._last_window_size

    @staticmethod
    def _encoded_params(params: Optional[Mapping[str, Any]]) -> Dict[str, str]:
        if not params:
            return {}
        return {key: _encode_param(value) for key, value in params.items() if value is not None}

    def _select_nodes(self, selector: Mapping[str, Any]) -> Tuple[List[Element], JsonDict, str]:
        self._validate_selector(selector)
        limit = selector.get("limit", 50)
        params = self._server_params(selector, limit=limit)
        data = self._request("GET", "/nodes", params)
        server_nodes = [Element(node) for node in data.get("nodes", []) if isinstance(node, dict)]
        filtered = [node for node in server_nodes if self._matches_client_filters(node, selector)]
        data = dict(data)
        data["_nodes"] = server_nodes
        return filtered, data, self._selector_text(selector)

    def _server_params(self, selector: Mapping[str, Any], limit: Any) -> Dict[str, Any]:
        params: Dict[str, Any] = {}
        for key in self._SERVER_FILTERS:
            value = selector.get(key)
            if value is not None:
                params[key] = value

        include = selector.get("include")
        if include is None and selector.get("has_bounds") is not None:
            include = "basic,bounds"
        normalized_include = _normalize_include(include)
        if normalized_include:
            params["include"] = normalized_include
        if limit is not None:
            params["limit"] = limit
        for key in ("offset", "cursor"):
            if selector.get(key) is not None:
                params[key] = selector[key]
        return params

    def _matches_client_filters(self, node: Element, selector: Mapping[str, Any]) -> bool:
        if selector.get("enabled") is not None and node.enabled != bool(selector["enabled"]):
            return False
        if selector.get("kind") is not None and node.kind != selector["kind"]:
            return False
        if selector.get("path") is not None and node.path != selector["path"]:
            return False
        if selector.get("name_exact") is not None and node.name != selector["name_exact"]:
            return False
        if selector.get("text_exact") is not None and node.text != selector["text_exact"]:
            return False
        if selector.get("has_bounds") is not None and (node.bounds is not None) != bool(selector["has_bounds"]):
            return False
        visible_and_enabled = selector.get("visible_and_enabled")
        if visible_and_enabled is not None and (node.visible and node.enabled) != bool(visible_and_enabled):
            return False
        return True

    def _validate_selector(self, selector: Mapping[str, Any]) -> None:
        unknown = set(selector) - self._SELECTOR_KEYS
        if unknown:
            raise TypeError(f"unknown element selector keys: {', '.join(sorted(unknown))}")

    def _has_client_filters(self, selector: Mapping[str, Any]) -> bool:
        return any(selector.get(key) is not None for key in self._CLIENT_FILTERS)

    @staticmethod
    def _selector_text(selector: Mapping[str, Any]) -> str:
        parts = [f"{key}={value!r}" for key, value in sorted(selector.items()) if value is not None]
        return ", ".join(parts) if parts else "<all elements>"

    def _selector_error(
        self,
        prefix: str,
        selector: Mapping[str, Any],
        selector_text: str,
        nodes: Sequence[Element],
        metadata: Mapping[str, Any],
    ) -> str:
        candidates = metadata.get("_nodes", [])
        shown = nodes if nodes else candidates
        matched = int(metadata.get("matched", len(nodes)))
        truncated = bool(metadata.get("truncated", False))
        lines = [
            f"{prefix}; selector: {selector_text}; returned {len(nodes)}; "
            f"server matched {matched}; truncated={truncated}; "
            f"scene_sequence={metadata.get('scene_sequence')}; engine_frame={metadata.get('engine_frame')}"
        ]
        active_collections = metadata.get("active_collections")
        if active_collections:
            lines.append(f"active collections: {active_collections}")
        excluded = metadata.get("excluded")
        if isinstance(excluded, Mapping) and any(excluded.values()):
            lines.append(
                "matching elements excluded by state: "
                f"visibility={excluded.get('visibility', 0)}, "
                f"enabled={excluded.get('enabled', 0)}, bounds={excluded.get('bounds', 0)}"
            )
        if not shown:
            shown = self._nearest_selector_nodes(selector)
        if shown:
            lines.append("candidates:")
            lines.extend(f"  {node.compact()}" for node in list(shown)[:10])
            requested_values = [
                str(value)
                for key, value in (
                    (part.split("=", 1)[0], part.split("=", 1)[1] if "=" in part else "")
                    for part in selector_text.split(", ")
                )
                if key in {"name", "name_exact", "text", "text_exact", "path"}
            ]
            choices = [value for node in shown for value in (node.name, node.text or "", node.path) if value]
            nearest = []
            for value in requested_values:
                nearest.extend(difflib.get_close_matches(value.strip("'\""), choices, n=3, cutoff=0.2))
            if nearest:
                lines.append(f"nearest name/text/path values: {list(dict.fromkeys(nearest))[:5]}")
        if matched > 1:
            lines.append("suggestion: add name_exact, text_exact, path, logical_id, or instance_id")
        return "\n".join(lines)

    def _nearest_selector_nodes(self, selector: Mapping[str, Any], maximum: int = 5000) -> List[Element]:
        """Fetch bounded complete pages only while formatting a failed direct selector."""
        candidates: List[Element] = []
        cursor: Optional[str] = None
        while len(candidates) < maximum:
            params: Dict[str, Any] = {"limit": min(500, maximum - len(candidates)), "include": "basic"}
            if cursor is not None:
                params["cursor"] = cursor
            data = self._request("GET", "/nodes", params)
            candidates.extend(Element(raw) for raw in data.get("nodes", []) if isinstance(raw, dict))
            cursor = data.get("next_cursor")
            if not cursor:
                break
        requested = [
            str(selector[key])
            for key in ("name_exact", "name", "text_exact", "text", "path")
            if selector.get(key) is not None
        ]
        if not requested:
            return candidates[:10]
        scored = []
        for node in candidates:
            choices = [node.name, node.text or "", node.path]
            ratios = [
                difflib.SequenceMatcher(None, expected, choice).ratio()
                for expected in requested
                for choice in choices
                if choice
            ]
            score = max(ratios) if ratios else 0.0
            scored.append((score, node))
        scored.sort(key=lambda item: item[0], reverse=True)
        return [node for _, node in scored[:10]]

    @staticmethod
    def _is_node_ref(target: Target) -> bool:
        return isinstance(target, (Element, str))

    @staticmethod
    def _node_id(target: Target) -> str:
        if isinstance(target, Element):
            return target.id
        if isinstance(target, str):
            return target
        raise TypeError(f"target is not an element reference: {target!r}")

    def _point(self, target: Union[Target, float, int], y: Optional[float] = None) -> Tuple[Any, Any]:
        if isinstance(target, Element):
            center = target.center or self.element_by_id(target.id, include="basic,bounds").center
            if center:
                return center["x"], center["y"]
        elif isinstance(target, str):
            center = self.element_by_id(target, include="basic,bounds").center
            if center:
                return center["x"], center["y"]
        elif isinstance(target, Mapping):
            if "x" in target and "y" in target:
                return target["x"], target["y"]
            center = target.get("center")
            if isinstance(center, Mapping) and "x" in center and "y" in center:
                return center["x"], center["y"]
        elif isinstance(target, Sequence) and not isinstance(target, (bytes, bytearray, str)) and len(target) == 2:
            return target[0], target[1]
        elif isinstance(target, (float, int)) and y is not None:
            return target, y

        raise TypeError(f"target does not provide coordinates: {target!r}")
