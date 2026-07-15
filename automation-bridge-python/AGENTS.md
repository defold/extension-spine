# Python wrapper guidance

- This is the preferred, dependency-free interface for driving a Defold debug
  build. Add this directory to `PYTHONPATH`, then use the focused package-root
  namespaces: `from automation_bridge import editor, engine`.
- Bootstrap with `game = editor.open_project(".").build_and_run()`. The editor
  helper reuses a healthy project editor or starts the newest discovered Defold
  installation. Use `game.close_engine()` only when the script intentionally
  owns engine cleanup.
- On macOS, run a bootstrap that may start Defold with escalated/unsandboxed
  execution. Defold inherits a sandbox from its Python parent and will abort
  during AppKit registration when WindowServer and LaunchServices are denied.
  Reusing a healthy editor does not require escalation. When uncertain, probe
  with `editor.open_project(".", start_if_needed=False)` and rerun the normal
  bootstrap with escalation after `NotRunningError`. Do not rely on
  `start_new_session=True` or `/usr/bin/open` to escape the inherited sandbox.
- Declare mandatory features with `required_capabilities=[...]` or
  `game.require(...)`; probe optional features with `game.supports(...)`.
- Prefer named helpers. Use
  `game.request(method, path, params=..., json=...)` only as the raw native API
  escape hatch.
- Selectors support substring, exact, boolean, identity, and pagination filters.
  `count()` remains complete regardless of page limits. `Element` values are
  snapshots, so re-query after input or scene changes.
- A visible component may expose the useful label while its parent game object
  receives input; use `game.parent(component)` when appropriate. Input targets
  may be elements, ids, point mappings, `(x, y)` pairs, or raw coordinates.
- Prefer events, published state, commands, input acknowledgements, frame waits,
  `wait_for_element(...)`, `observe_element(...)`, and
  `wait_for_disappearance(...)` over sleeps.
- `screenshot(wait=True)` returns an atomic receipt. Start agent inspection with
  `resolution_multiplier=0.5`; the scaled receipt retains the engine file at
  `raw["source_path"]`. `project.preview.render(...)` provides the equivalent
  editor-side render without running the game.
- Optional diagnostics are intentionally namespaced under `game.visual`,
  `game.gestures`, `game.video_recording`, `game.profiler`, and
  `game.trace(...)`.
- Public docstrings are the exact API reference (`help(engine.Client.drag)`). See
  `README.md` for workflows and examples, and `../README.md` for raw endpoints.
- Run tests from the repository root with:
  `PYTHONPATH=automation_bridge/automation-bridge-python python3 -m unittest tests.test_automation_bridge_api tests.test_tooling`.
