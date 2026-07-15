"""Public API for interacting with an already-running Defold engine."""

from .client import (
    AutomationBridgeApiError,
    AutomationBridgeError as Error,
    Client,
    EngineLogStream,
    HttpError,
    IncompatibleApiVersionError,
    InputController,
    InputExecutionError,
    InputInterruptionScope,
    InputReceipt,
    PointerSession,
    RuntimeLogs,
    SelectorError,
    UnsupportedCapabilityError,
)
from .events import CommandTimeout, Event, EventBufferOverflow, EventStream, StateSnapshot
from .nodes import Bounds, Element
from .profiler import *  # Re-export the focused profiler result and control types.
from .recording import (
    VideoRecordingCapabilities,
    VideoRecordingClient,
    VideoRecordingError,
    VideoRecordingMetadata,
    VideoRecordingSession,
)
from .receipts import ObservationReceipt, ScreenshotReceipt
from .trace import TraceSession
from .visual import VisualObservation
from .waits import WaitTimeoutError, wait_until


def connect(
    port: int,
    *,
    timeout: float = 10.0,
    profiler_url=None,
    client_id=None,
    session_id=None,
    required_capabilities=(),
) -> Client:
    """Return a client for an already-running engine service."""
    client = Client(
        port,
        timeout=timeout,
        profiler_url=profiler_url,
        client_id=client_id,
        session_id=session_id,
        required_capabilities=required_capabilities,
    )
    logs = getattr(client, "logs", None)
    if logs is not None:
        logs.start()
    return client


__all__ = [name for name in globals() if not name.startswith("_")]
