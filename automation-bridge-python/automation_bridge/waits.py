"""Polling helpers with timeout diagnostics that do not hide programming errors."""

import time
from typing import Any, Callable, Mapping, Optional, Tuple, Type, TypeVar, Union


T = TypeVar("T")
RetryExceptions = Union[Type[BaseException], Tuple[Type[BaseException], ...]]
SceneSequence = Union[int, Callable[[], Optional[int]], None]


class WaitTimeoutError(AssertionError):
    """A polling timeout with the observations needed to diagnose the wait."""

    def __init__(
        self,
        message: str,
        *,
        last_value: Any,
        elapsed: float,
        attempts: int,
        scene_sequence: Optional[int],
        last_exception: Optional[BaseException],
    ):
        self.message = message
        self.last_value = last_value
        self.elapsed = elapsed
        self.attempts = attempts
        self.scene_sequence = scene_sequence
        self.last_exception = last_exception
        details = [
            f"elapsed={elapsed:.3f}s",
            f"attempts={attempts}",
            f"last_value={last_value!r}",
            f"scene_sequence={scene_sequence!r}",
        ]
        if last_exception is not None:
            details.append(
                f"last_retryable_exception={type(last_exception).__name__}: {last_exception}"
            )
        super().__init__(f"{message} ({', '.join(details)})")


def wait_until(
    fn: Callable[[], T],
    timeout: float = 10.0,
    interval: float = 0.1,
    message: str = "condition not met before timeout",
    retry_exceptions: RetryExceptions = (),
    scene_sequence: SceneSequence = None,
    predicate: Optional[Callable[[T], bool]] = None,
) -> T:
    """Poll ``fn`` until it returns a truthy value or ``timeout`` expires.

    Only exceptions explicitly listed in ``retry_exceptions`` are retried.
    Other exceptions, including ``KeyboardInterrupt`` and programming errors,
    escape immediately.  A timeout raises :class:`WaitTimeoutError` with the
    last value, elapsed time, attempt count, scene sequence, and last retryable
    exception. ``scene_sequence`` may be a known integer or a zero-argument
    callback for clients that track it from native responses. ``predicate``
    can define success independently of truthiness so waits for values such as
    zero still retain the actual observation in timeout diagnostics.
    """
    retry_types = _normalize_retry_exceptions(retry_exceptions)
    if timeout < 0:
        raise ValueError("timeout must be non-negative")
    if interval < 0:
        raise ValueError("interval must be non-negative")

    started = time.monotonic()
    deadline = started + timeout
    last_value: Any = None
    last_error: Optional[BaseException] = None
    last_scene_sequence = _read_scene_sequence(scene_sequence)
    attempts = 0

    while True:
        attempts += 1
        try:
            last_value = fn()
            inferred_sequence = _sequence_from_value(last_value)
            last_scene_sequence = (
                inferred_sequence
                if inferred_sequence is not None
                else _read_scene_sequence(scene_sequence, last_scene_sequence)
            )
            succeeded = predicate(last_value) if predicate is not None else bool(last_value)
            if succeeded:
                return last_value
        except retry_types as exc:
            last_error = exc
            last_scene_sequence = _read_scene_sequence(scene_sequence, last_scene_sequence)

        now = time.monotonic()
        if now >= deadline:
            elapsed = now - started
            error = WaitTimeoutError(
                message,
                last_value=last_value,
                elapsed=elapsed,
                attempts=attempts,
                scene_sequence=last_scene_sequence,
                last_exception=last_error,
            )
            if last_error is not None:
                raise error from last_error
            raise error
        time.sleep(min(interval, max(0.0, deadline - now)))


def _normalize_retry_exceptions(value: RetryExceptions) -> Tuple[Type[BaseException], ...]:
    values = value if isinstance(value, tuple) else (value,)
    for exception_type in values:
        if not isinstance(exception_type, type) or not issubclass(exception_type, BaseException):
            raise TypeError("retry_exceptions must contain exception classes")
    return values


def _read_scene_sequence(value: SceneSequence, previous: Optional[int] = None) -> Optional[int]:
    observed = value() if callable(value) else value
    return observed if isinstance(observed, int) and not isinstance(observed, bool) else previous


def _sequence_from_value(value: Any) -> Optional[int]:
    if isinstance(value, Mapping):
        sequence = value.get("scene_sequence")
        if isinstance(sequence, int) and not isinstance(sequence, bool):
            return sequence
    sequence = getattr(value, "scene_sequence", None)
    return sequence if isinstance(sequence, int) and not isinstance(sequence, bool) else None
