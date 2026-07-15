"""Reusable finalization hooks for recordings, traces, and diagnostic artifacts."""

from typing import Any, Callable, Optional


class FinalizationHooks:
    """Callbacks for successful finalization and interruption-driven abort.

    Recording and trace implementations can expose the same small contract:
    ``finalize(artifact)`` after a complete artifact is ready and
    ``abort(cause, artifact)`` after best-effort salvage. Callback failures are
    normally visible. Pass ``suppress=True`` only while preserving an original
    exception that is already unwinding.
    """

    def __init__(
        self,
        on_finalize: Optional[Callable[[Any], None]] = None,
        on_abort: Optional[Callable[[BaseException, Any], None]] = None,
    ):
        self.on_finalize = on_finalize
        self.on_abort = on_abort

    def finalize(self, artifact: Any) -> None:
        """Notify that ``artifact`` was finalized successfully."""
        if self.on_finalize is not None:
            self.on_finalize(artifact)

    def abort(
        self,
        cause: BaseException,
        artifact: Any,
        *,
        suppress: bool = False,
    ) -> None:
        """Notify that ``artifact`` was salvaged after ``cause``."""
        if self.on_abort is None:
            return
        if not suppress:
            self.on_abort(cause, artifact)
            return
        try:
            self.on_abort(cause, artifact)
        except BaseException:
            pass
