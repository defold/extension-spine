# Automation Bridge Python Helpers

Dependency-free Python helpers for driving a Defold debug build through the
Automation Bridge HTTP API.

## Quick start

When running from this repository, add the wrapper directory to `PYTHONPATH`:

```sh
PYTHONPATH=automation_bridge/automation-bridge-python python3 your_script.py
```

Build, connect, query the scene, and send input:

```python
from automation_bridge import editor

project = editor.open_project(".")
game = project.build_and_run()

spawner = game.element(type="goc", name_exact="/spawner", visible=True)
game.click(spawner)
label = game.wait_for_element(type="labelc", text_exact="L1")
game.drag(game.parent(label), (500, 300), duration=0.16)
shot = game.screenshot(wait=True, resolution_multiplier=0.5)
print(shot.path, shot.frame, shot.scene_sequence, shot.sha256)
```

Use `game.close_engine()` only when the script intentionally owns engine cleanup.

## Public API

The package root exposes only `editor` and `engine`.

- Bootstrap: `editor.open_project(...)`, `project.build_and_run()`,
  `project.clean_build_and_run()`, `project.connect_engine()`, and
  `engine.connect(port)`.
- Editor operations: `project.commands`, `project.debugger`, `project.console`,
  `project.preferences`, `project.reference`, `project.preview`, and
  `project.build_and_run_html5()`.
- Runtime state: `health()`, `screen()`, `scene(...)`, capabilities, and lifecycle.
- Elements: `elements(...)`, `element(...)`, `maybe_element(...)`,
  `element_by_id(...)`, `parent(...)`, `count(...)`, compact formatting, and
  scene dumps.
- Input: `click(...)`, `drag(...)`, `drag_path(...)`, `pointer(...)`,
  `type_text(...)`, `key(...)`, and `game.input`.
- Synchronization: events, states, application commands, full input
  acknowledgements, timeline markers, frame/count waits, and element
  observation.
- Runtime control and geometry: `resize(...)`, `set_portrait()`,
  `set_landscape()`, `convert_point(...)`, `reboot(...)`, and
  `close_engine()`.
- Capture and diagnostics: screenshots, historical `game.logs`, live engine
  logs, profiling, visual comparison, tracing, and `game.video_recording`.
- Raw engine escape hatch: `game.request(method, path, params=..., json=...)`.

## Bootstrap

```python
from automation_bridge import editor, engine

project = editor.open_project(".")
game = project.build_and_run()
clean_game = project.clean_build_and_run()
existing_game = project.connect_engine()
direct_game = engine.connect(51337)
```

Choose the operation by ownership and lifecycle intent:

| Operation | Editor required | Builds | Engine intent |
| --- | --- | --- | --- |
| `editor.open_project(".")` | No; starts or reuses it | No | Return an editor project client |
| `project.build_and_run()` | Yes | Incremental | Replace the previous engine and connect |
| `project.clean_build_and_run()` | Yes | Clean | Replace the previous engine and connect |
| `project.connect_engine()` | Yes | No | Connect to the healthy engine already registered by this project |
| `engine.connect(port)` | No | No | Connect directly to a known engine service port |
| `game.close_engine()` | No | No | Close the connected engine; use only when the script owns cleanup |

There is intentionally no `project.connect()`: a project is already the editor
client, while `connect_engine()` makes the second client and its lifecycle
explicit.

The editor bootstrap discovers `.internal/editor.port`, launches Defold when
needed, rejects stale engine ports, and waits for Automation Bridge health.
Pass `start_if_needed=False` to require an already-running editor.

## Reliable game test loop

Resize before coordinate-based input or visual assertions, then synchronize on
application-published state instead of estimating completion from elapsed time:

If a `.go`, `.gui`, or `.collection` resource was created or edited manually,
render it through the editor preview before building. This makes the editor
parse its references and produces a quick visual artifact, so malformed files,
missing resources, and obvious layout mistakes are reported close to the edit:

```python
from pathlib import Path
from automation_bridge import editor

project = editor.open_project(".")
preview_directory = Path("/tmp/defold-previews")
preview_directory.mkdir(parents=True, exist_ok=True)

for resource in (
    "main/player.go",
    "main/hud.gui",
    "main/main.collection",
):
    png = project.preview.render(resource, resolution_multiplier=0.5)
    output = preview_directory / f"{Path(resource).name}.png"
    output.write_bytes(png)
    print(f"Rendered {resource} to {output}")
```

Preview rendering validates editor loading and appearance; it does not replace
`build_and_run()` when runtime scripts, input, physics, or dynamically spawned
content must be tested.

After previewing manually authored resources, continue with the runtime loop:

```python
from automation_bridge import editor

project = editor.open_project(".")
game = project.build_and_run(required_capabilities=[
    "screen.resize",
    "input.pointer",
    "screenshot",
    "application.state",
])

game.resize(1080, 1920)
game.set_portrait()
game.wait_for_state("tutorial.phase", "ready")

before = game.screenshot(after_frames=1)
game.drag((540, 1500), (540, 500), duration=0.25)
game.wait_for_state("tutorial.phase", "waiting_for_pickup")
after = game.screenshot(after_frames=1)
```

The state names and values in this example belong to the game. Publish them
from Lua together with semantic scene annotations:

```lua
automation_bridge.publish("tutorial", {
    phase = "waiting_for_pickup",
    figure = figure_num,
})

automation_bridge.annotate("main:/ghost#sprite", {
    automation_id = "tutorial_target_ghost",
    role = "tutorial_target",
})
```

Tests can then use
`game.element(automation_id="tutorial_target_ghost")` instead of inferring a
runtime object from size, scale, depth, or changing instance ids.

On macOS, run a bootstrap that may launch Defold outside restricted agent
sandboxes. A GUI process inherits its Python parent's sandbox and cannot
register with WindowServer or LaunchServices. The wrapper refuses to launch
Defold when Codex marks the process as sandboxed and raises `LaunchError` with
instructions to start Defold manually or rerun with escalated execution. An
already-running healthy editor can still be reused without escalation.

Installation discovery and preview rendering are explicit:

```python
for installation in editor.installations():
    print(installation.launcher_path, installation.last_launched_at)

preview_png = project.preview.render(
    "main/main.collection",
    resolution_multiplier=0.5,
)
```

Built-in preferences are discoverable constants with metadata, while custom
editor-script paths remain strings:

```python
size = project.preferences.get(project.preferences.CODE_FONT_SIZE)
project.preferences.set(project.preferences.CODE_FONT_SIZE, 16)
project.preferences.set("my-extension/preview/quality", "high")
for preference in project.preferences.list(prefix="code"):
    print(preference.path, preference.description)
```

## Capabilities

Declare mandatory capabilities during bootstrap or later with `require()`:

```python
game = project.build_and_run(
    required_capabilities=["runtime.lifecycle", "application.events>=1"],
)

game.require("scene", "input.drag")
```

Check nonessential functionality without mutating the client:

```python
available = {
    name: game.supports(name)
    for name in ("scene", "screenshot", "input.drag")
}
```

Capability declarations may use `name>=N`. Incompatible API versions raise
`IncompatibleApiVersionError`; missing required features raise
`UnsupportedCapabilityError`.

## Raw requests

Named helpers are preferred. For endpoint-level debugging, use the single raw
escape hatch:

```python
health = game.request("GET", "/health")
result = game.request("POST", "/coordinates/convert", json={
    "point": {"x": 0.5, "y": 0.5},
    "from_space": "normalized_viewport",
    "to_space": "window",
})
```

Raw native endpoint documentation is in [`../README.md`](../README.md).

## Elements and selectors

`elements()` returns snapshots. Re-query after input, collection changes, or UI
updates.

```python
labels = game.elements(type="labelc", text="L", limit=100)
restart = game.element(name_exact="restart", enabled=True)
maybe_popup = game.maybe_element(automation_id="popup")
parent = game.parent(labels[0])
total = game.count(type="labelc")
```

Substring filters are `type`, `name`, `text`, and `url`. Exact filters include
`type_exact`, `name_exact`, `text_exact`, `url_exact`, `path`, `kind`,
`instance_id`, `logical_id`, `automation_id`, `localization_key`, and `role`.
Boolean filters include `visible`, `enabled`, `has_bounds`, and
`visible_and_enabled`.

`Element` exposes `id`, `snapshot_id`, `instance_id`, `instance_generation`,
`logical_id`, `created_scene_sequence`, `scene_sequence`, `engine_frame`,
`name`, `type`, `kind`, `path`, `parent_id`, `text`, `url`, semantic metadata,
visibility, bounds, children, and `raw`.

## Input

Input targets can be elements, element ids, point mappings, `(x, y)` pairs, or raw
coordinates:

```python
game.click(element)
game.click(480, 320)
game.drag(first, second, duration=0.2, easing="ease_in_out")
game.type_text("Hello")
game.key("SPACE")
```

`click()`, `drag()`, and `drag_path()` wait for native release by default.
`type_text()` and `key()` return after the request is accepted unless a `wait`
state is supplied. These five helpers accept `wait="accepted"`,
`wait="started"`, `wait="released"`, or `wait=False` as appropriate.

Low-level queue and interruption control lives under `game.input`:

```python
with game.input.interruption_scope():
    receipt = game.click(element, wait=False)
    game.input.wait(receipt, "released")
```

For a continuous held pointer:

```python
with game.pointer((100, 100)) as pointer:
    pointer.move((200, 150), duration=0.2)
    pointer.hold(0.1)
```

The pointer object exposes its configured `lease`, `input_id`, and `closed`
state. `game.input.status(pointer.input_id)` returns its current native receipt.
To capture a rendered pressed state, request a frame-relative screenshot while
the pointer context is still open and choose a lease that comfortably covers
the capture:

```python
with game.pointer((100, 100), lease=10) as pointer:
    pressed = game.screenshot(after_frames=1)
    assert game.input.status(pointer.input_id).state == "started"
```

## Synchronization and observation

Use application events and published state instead of sleeps:

```python
with game.events("now") as events:
    game.click(button)
    completed = events.wait("operation.complete", timeout=5)

revision = game.state("ui").revision
game.click(button)
state = game.wait_for_state("ui.busy", False, after_revision=revision)
```

`wait_frames(count)` is useful when rendered-frame evidence is specifically
required. Its default timeout is `max(5, count / 30)` seconds; an explicit
`timeout` overrides that calculation. Timeout errors include the initial and
last observed frame, whether frames advanced, the current lifecycle stage, and
a best-effort engine health result. Frame waits do not replace semantic state or
event waits.

Run application commands:

```python
result = game.command("reset_fixture", {"seed": 42})
```

Add a timeline marker. `recording_timestamp_us` is optional; when omitted, the
wrapper records the host monotonic clock in microseconds:

```python
marker = game.mark("workflow_started", {"fixture": "menu"})
```

Native input completion and application acknowledgement are distinct. An
application can call `automation_bridge.acknowledge_input(input_id, result)`
from Lua; Python waits for that semantic result explicitly:

```python
with game.events("now") as events:
    receipt = game.click(button)
    acknowledgement = game.wait_for_input_acknowledgement(
        receipt.input_id,
        events=events,
    )
```

Wait for scene evidence:

```python
element = game.wait_for_element(
    automation_id="result",
    after_scene_sequence=previous_sequence,
)
stable = game.observe_element(logical_id=element.logical_id, minimum_frames=3)
gone = game.wait_for_disappearance(element.id)
```

## Screenshots and visual checks

`screenshot(wait=True)` returns an atomic `ScreenshotReceipt` containing the
capture path, engine frame, scene sequence, dimensions, and SHA-256. Runtime
screenshots also accept `resolution_multiplier` from `0.01` through `1.0`:

```python
shot = game.screenshot(resolution_multiplier=0.5)
print(shot.path, shot.width, shot.height, shot.sha256)
```

Screenshot pixels and pointer input both use top-left window orientation.
Defold's graphics adapters already normalize readback rows to display order, so
the native capture preserves their order when publishing the PNG and pixel
coordinates in the receipt correspond to input coordinates. Defold world
coordinates are a different space: a generic `height - world_y` conversion is
not reliable with cameras, projections, viewports, scaling, or letterboxing.
`game.convert_point(...)` converts among the advertised top-left window,
client, display-pixel, backbuffer, viewport, and normalized-viewport spaces;
world transforms must be published by the application when needed.

Large black regions are not automatically classified as capture corruption
because they may be legitimate game content. When a capture looks suspicious,
retain its receipt and PNG, capture an adjacent frame with
`screenshot(after_frames=1)`, inspect live scene state, and compare the two with
`game.visual.difference(...)`. The receipt's frame, scene sequence, dimensions,
and SHA-256 make the evidence reproducible without silently replacing it.

The engine capture remains available at `shot.raw["source_path"]`; the returned
receipt points to a bilinear-downscaled PNG with updated dimensions and hash.
Downscaling requires `wait=True`. Agents should normally start with `0.5` and
use full resolution only when fine detail matters.

## Runtime logs

Public project and engine connection helpers start a bounded background
collector against Defold's engine log service as soon as the Automation Bridge
client is available. Recent output and errors can then be queried without
depending on the editor console:

```python
recent = game.logs.tail(100)
errors = game.logs.tail(100, contains="ERROR:")
for line in errors:
    print(line)
```

The filter is a case-sensitive substring match. Filtering happens before the
limit is applied, so the second example returns up to 100 matching errors, not
only errors among the last 100 unfiltered lines. The collector retains the most
recent 10,000 lines and is stopped by `game.close_engine()`.

Collection begins at the earliest successful bridge connection. Messages
emitted before the wrapper discovers the engine service port cannot be recovered
from Defold's forward-only log service; use `project.console` only when those
earliest build or startup messages are required.

`game.log_stream()` opens Defold's live TCP log stream. Start it before an
operation when every subsequent line matters; `game.read_logs()` is also
forward-only:

```python
with game.log_stream(read_timeout=0.1) as logs:
    game.click(button)
    game.wait_for_state("tutorial.phase", "waiting_for_pickup")
    while line := logs.readline(timeout=0.1):
        print(line)
```

`project.console` remains available when the full editor snapshot or structured
console regions are needed.

Pixel comparisons are explicitly namespaced:

```python
before = game.screenshot()
game.click(button)
changed = game.visual.wait_for_region_change(before, region=(0, 0, 300, 200))

stable = game.visual.wait_for_stable_frame(consecutive_frames=3)
error = game.visual.difference(before, stable.screenshot)
```

## Generated gestures

Gesture synthesis has one public entry path:

```python
gesture = game.gestures.generate_drag(
    (100, 100),
    (500, 300),
    seed=42,
    duration=(0.7, 0.9),
    control_points=4,
)
game.drag_path(**gesture)
```

## Recording and traces

Video recording is native and namespaced. It records the running Defold
process's largest on-screen window to H.264 MP4 using ScreenCaptureKit on
macOS 15+ or Windows Graphics Capture and Media Foundation on Windows 10
version 1903+:

```python
capabilities = game.video_recording.capabilities()

with game.video_recording.start("capture.mp4", size=(960, 540), fps=30):
    game.click(button)
```

The recorder runs in the game process, automatically selects that process's
window, crops it to the undecorated game content area, and finalizes the file
when the context exits. Omitted `audio` uses the platform default: enabled on
macOS and disabled on Windows. The current Windows backend is video-only and
rejects `audio=True`. The first macOS capture may trigger Screen Recording
permission. Check `capabilities().available` before treating recording as an
optional feature. No FFmpeg installation or external recorder process is used.

Diagnostic traces remain a client-bound context because they intercept client
operations:

```python
with game.trace("session.trace.json", screenshots="on_error") as trace:
    game.click(button)
    trace.record("checkpoint", {"name": "after click"})
```

Replay is best effort and cannot restore application state, random seeds,
timing mode, or external services.

## Profiling

All profiling is presented through `game.profiler`; the underlying stream
protocol is an implementation detail.

```python
resources = game.profiler.resources()

capture = game.profiler.capture(frames=120, warmup_frames=10)
for scope in capture.scopes(contains="Update"):
    print(scope.path, scope.self.p95_ms)

for counter in capture.counters(contains="Texture"):
    print(counter.path, counter.values.last)
```

Record until the automation script decides the interesting interval is over:

```python
recording = game.profiler.start_recording(warmup_frames=10)
try:
    game.click(button)
finally:
    capture = recording.stop()
```

Advanced profiler-facing type names live in `automation_bridge.profiler`, such
as `ProfilerCapture`, `ProfilerRecording`, `ProfilerConnection`,
`ProfilerScopeStats`, and `ProfilerCounterStats`.

## Tests

Run the wrapper tests with:

```sh
PYTHONPATH=automation_bridge/automation-bridge-python \
python3 -m unittest tests.test_automation_bridge_api tests.test_tooling
```
