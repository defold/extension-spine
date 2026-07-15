#!/usr/bin/env python3
"""Run the extension-spine example suite through Automation Bridge.

The validator intentionally uses the project-owned Python wrapper and the
Defold editor HTTP API.  It performs a clean build by default, switches examples
through semantic application commands, exercises shared GO/GUI skin ownership,
and writes an incremental JSON report plus atomic screenshots for every example.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import sys
import time
import traceback
from typing import Any, Iterable, Mapping, Optional, Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[1]
WRAPPER_ROOT = PROJECT_ROOT / "automation-bridge-python"
NAVIGATION_STATE = "extension_spine.examples"
LOAD_EXAMPLE_COMMAND = "extension_spine.load_example"
EXERCISE_CURRENT_COMMAND = "extension_spine.exercise_current"

REQUIRED_CAPABILITIES = (
    "runtime.health",
    "runtime.lifecycle",
    "diagnostics.metadata",
    "scene",
    "nodes",
    "node",
    "screen",
    "screen.resize",
    "coordinates.convert",
    "frame.wait",
    "input.click",
    "input.drag",
    "input.key",
    "input.receipts",
    "input.device.mouse",
    "screenshot",
    "timeline.markers",
    "application.state",
    "application.commands",
)

UNEXPECTED_LOG_PATTERNS = tuple(
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        r"(?:^|\s)(?:ERROR|FATAL):",
        r"\bSCRIPT ERROR\b",
        r"\bstack traceback:\b",
        r"\bassertion failed\b",
        r"\bPANIC\b",
    )
)


@dataclass(frozen=True)
class Example:
    index: int
    name: str
    proxy_id: str
    spinemodel_count: int
    label_exact: Optional[str] = None
    label_contains: Optional[str] = None
    observe_seconds: float = 0.25
    exercise: Optional[str] = None


EXAMPLES = (
    Example(1, "Basic Animation", "basic_animation_proxy", 2, "Basic Animation Example", observe_seconds=0.15, exercise="basic"),
    Example(2, "Small Walk Animation", "small_walk_animation_proxy", 1, "Small Walk Animation Example"),
    Example(3, "Constants Demo", "constants_demo_proxy", 1, "Constants Demo Example", observe_seconds=1.1),
    Example(4, "Slow Jump", "slow_jump_proxy", 1, "Slow Jump Example"),
    Example(5, "Sequence Animation", "sequence_animation_proxy", 1, "Sequence Animation Example"),
    Example(6, "Coin Blend and Mask", "coin_blend_and_mask_proxy", 2, "Coin Blend and Mask Example"),
    Example(7, "Mix Skins", "mix_skins_proxy", 1, "Mix Skins Example", observe_seconds=1.1),
    Example(8, "GUI Mix Skins", "gui_mix_skins_proxy", 0, "GUI Mix Skins Example", observe_seconds=1.1, exercise="gui_shared_skin"),
    Example(9, "Color Slots", "slot_color_proxy", 1, "Slot Coloring Example", observe_seconds=1.1),
    Example(10, "Physics", "physics_proxy", 1, "Physics Example", observe_seconds=0.4, exercise="physics"),
    Example(11, "GUI Spine Demo", "gui_spine_demo_proxy", 0, "GUI Spine Demo Example", observe_seconds=0.35),
    Example(12, "MixBlend 'Add' Demo", "owl_mix_blend_add_proxy", 1, observe_seconds=0.45, exercise="owl"),
    Example(13, "Swap Spine Scenes", "swap_spinescene_proxy", 3, observe_seconds=3.25, exercise="swap"),
    Example(14, "Instances", "instances", 6),
    Example(15, "Bones", "bones", 2, observe_seconds=1.1, exercise="bones"),
    Example(16, "Performance Test", "performance_test_proxy", 1800, label_contains="Spineboy models: 1800", observe_seconds=0.4),
)


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    parser = argparse.ArgumentParser(
        description="Build extension-spine and validate all examples through Automation Bridge.",
    )
    parser.add_argument(
        "--project",
        type=Path,
        default=PROJECT_ROOT,
        help="Defold project root (default: repository root).",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=PROJECT_ROOT / "build" / "automation-results" / timestamp,
        help="Artifact directory for report.json and screenshots.",
    )
    parser.add_argument(
        "--incremental",
        action="store_true",
        help="Use the editor's incremental build instead of the default clean build.",
    )
    parser.add_argument(
        "--no-start-editor",
        action="store_true",
        help="Require the Defold editor to be running instead of launching it.",
    )
    parser.add_argument(
        "--build-timeout",
        type=float,
        default=300.0,
        help="Seconds allowed for editor startup and build/run (default: 300).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Seconds allowed for each semantic wait (default: 30).",
    )
    parser.add_argument(
        "--require-visual-change",
        action="store_true",
        help="Fail an example whose adjacent captures do not change.",
    )
    parser.add_argument(
        "--visual-threshold",
        type=float,
        default=0.00001,
        help="Minimum normalized pixel difference for --require-visual-change.",
    )
    parser.add_argument(
        "--allow-error",
        action="append",
        default=[],
        metavar="REGEX",
        help="Explicitly allow a matching runtime/editor error line; repeat as needed.",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="Stop after the first failed example (final log scanning still runs).",
    )
    parser.add_argument(
        "--close-engine",
        action="store_true",
        help="Close the game after validation; by default it is left open for inspection.",
    )
    args = parser.parse_args(argv)
    if args.build_timeout <= 0 or args.timeout <= 0:
        parser.error("timeouts must be greater than zero")
    if args.visual_threshold < 0 or args.visual_threshold > 1:
        parser.error("--visual-threshold must be between 0 and 1")
    try:
        args.allow_error_patterns = tuple(re.compile(value) for value in args.allow_error)
    except re.error as error:
        parser.error(f"invalid --allow-error regular expression: {error}")
    return args


def json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, Mapping):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [json_safe(item) for item in value]
    raw = getattr(value, "raw", None)
    if isinstance(raw, Mapping):
        return json_safe(raw)
    return repr(value)


def write_report(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(json_safe(report), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def current_log_lines(game: Any) -> list[str]:
    return list(game.logs.tail(10000))


def console_delta(before: Sequence[str], after: Sequence[str]) -> list[str]:
    shared = 0
    maximum = min(len(before), len(after))
    while shared < maximum and before[shared] == after[shared]:
        shared += 1
    return list(after[shared:])


def unexpected_lines(
    source: str,
    lines: Iterable[str],
    allow_patterns: Sequence[re.Pattern[str]],
) -> list[dict[str, str]]:
    found = []
    for line in lines:
        if not any(pattern.search(line) for pattern in UNEXPECTED_LOG_PATTERNS):
            continue
        if any(pattern.search(line) for pattern in allow_patterns):
            continue
        found.append({"source": source, "line": line})
    return found


def wait_engine_seconds(game: Any, duration: float, timeout: float) -> int:
    """Advance rendered frames until a Defold timer has had `duration` wall seconds."""
    deadline = time.monotonic() + duration
    frames = 0
    while time.monotonic() < deadline:
        game.wait_frames(1, timeout=timeout)
        frames += 1
    return frames


def wait_for_log_tokens(
    game: Any,
    start_index: int,
    tokens: Sequence[str],
    timeout: float,
) -> list[str]:
    deadline = time.monotonic() + timeout
    last_lines: list[str] = []
    while True:
        all_lines = current_log_lines(game)
        last_lines = all_lines[start_index:] if start_index <= len(all_lines) else all_lines
        missing = [token for token in tokens if not any(token in line for line in last_lines)]
        if not missing:
            return last_lines
        if time.monotonic() >= deadline:
            raise AssertionError(f"runtime log tokens were not observed: {missing!r}")
        game.wait_frames(1, timeout=min(timeout, 5.0))


def navigation_revision(game: Any) -> int:
    return int(game.states().get("revision", 0))


def _matching_navigation_value(entry: Mapping[str, Any], expected_index: int) -> Optional[Mapping[str, Any]]:
    if entry.get("name") != NAVIGATION_STATE:
        return None
    value = entry.get("value")
    if not isinstance(value, Mapping) or int(value.get("index", 0)) != expected_index:
        return None
    return value


def wait_navigation_ready(
    game: Any,
    expected_index: int,
    timeout: float,
    after_revision: Optional[int] = None,
) -> Mapping[str, Any]:
    """Wait for the selected proxy's ready/failed publication without a sleep race."""
    deadline = time.monotonic() + timeout
    cursor = navigation_revision(game) if after_revision is None else int(after_revision)
    last_value: Any = "<unpublished>"

    if after_revision is None:
        try:
            snapshot = game.state(NAVIGATION_STATE)
        except Exception:
            snapshot = None
        if snapshot is not None and isinstance(snapshot.value, Mapping):
            last_value = snapshot.value
            if int(snapshot.value.get("index", 0)) == expected_index:
                if snapshot.value.get("status") == "failed":
                    raise AssertionError(f"example {expected_index} failed to load: {snapshot.value!r}")
                if snapshot.value.get("status") == "ready":
                    return snapshot.value

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                f"navigation did not publish ready for example {expected_index}; last={last_value!r}, revision={cursor}"
            )
        changed = game.request(
            "GET",
            "/state/wait",
            params={
                "after_revision": cursor,
                "timeout_ms": max(1, int(min(remaining, 1.0) * 1000)),
                "name": NAVIGATION_STATE,
            },
        )
        cursor = max(cursor, int(changed.get("revision", cursor)))
        for entry in changed.get("states", ()):
            if not isinstance(entry, Mapping):
                continue
            value = _matching_navigation_value(entry, expected_index)
            if value is None:
                continue
            last_value = value
            if value.get("status") == "failed":
                raise AssertionError(f"example {expected_index} failed to load: {value!r}")
            if value.get("status") == "ready" and int(entry.get("revision", 0)) > (after_revision or -1):
                return value


def load_example(game: Any, example: Example, timeout: float) -> dict[str, Any]:
    revision = navigation_revision(game)
    command = game.command(LOAD_EXAMPLE_COMMAND, {"index": example.index}, timeout=timeout)
    state = wait_navigation_ready(game, example.index, timeout=timeout, after_revision=revision)
    return {"command": json_safe(command), "state": json_safe(state)}


def wait_for_label(game: Any, example: Example, timeout: float) -> Optional[dict[str, Any]]:
    if example.label_exact:
        element = game.wait_for_element(timeout=timeout, type_exact="labelc", text_exact=example.label_exact)
    elif example.label_contains:
        element = game.wait_for_element(timeout=timeout, type_exact="labelc", text=example.label_contains)
    else:
        return None
    return {
        "id": element.id,
        "text": element.text,
        "path": element.path,
        "scene_sequence": element.scene_sequence,
        "engine_frame": element.engine_frame,
    }


def _walk_scene_nodes(root: Any) -> Iterable[Mapping[str, Any]]:
    if isinstance(root, Mapping):
        if "type" in root or "kind" in root:
            yield root
        children = root.get("children", ())
        if isinstance(children, (list, tuple)):
            for child in children:
                yield from _walk_scene_nodes(child)
    elif isinstance(root, (list, tuple)):
        for child in root:
            yield from _walk_scene_nodes(child)


def scene_summary(game: Any) -> dict[str, Any]:
    metadata = game.request("GET", "/nodes", params={"limit": 0})
    scene = game.scene(include="basic")
    type_counts: Counter[str] = Counter()
    kind_counts: Counter[str] = Counter()
    for node in _walk_scene_nodes(scene.get("root")):
        node_type = node.get("type")
        node_kind = node.get("kind")
        if node_type:
            type_counts[str(node_type)] += 1
        if node_kind:
            kind_counts[str(node_kind)] += 1

    representative = None
    first_page = game.elements(limit=1, include="basic")
    if first_page:
        detail = game.element_by_id(first_page[0].id, include="basic")
        representative = {
            key: detail.raw.get(key)
            for key in ("id", "name", "type", "kind", "path", "text", "visible", "enabled")
        }

    return {
        "count": scene.get("count"),
        "scene_sequence": scene.get("scene_sequence"),
        "engine_frame": scene.get("engine_frame"),
        "matched": metadata.get("matched"),
        "total": metadata.get("total"),
        "active_collections": json_safe(metadata.get("active_collections", ())),
        "type_counts": dict(sorted(type_counts.items())),
        "kind_counts": dict(sorted(kind_counts.items())),
        "representative_node": representative,
    }


def capture(game: Any, destination: Path) -> dict[str, Any]:
    receipt = game.screenshot(wait=True, after_frames=1)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(receipt.path, destination)
    return {
        "artifact": str(destination),
        "receipt": json_safe(receipt.raw),
        "width": receipt.width,
        "height": receipt.height,
    }


def compare_captures(game: Any, before: Mapping[str, Any], after: Mapping[str, Any]) -> dict[str, float]:
    before_path = Path(str(before["artifact"]))
    after_path = Path(str(after["artifact"]))
    width = int(before.get("width", 0))
    height = int(before.get("height", 0))
    full = float(game.visual.difference(before_path, after_path))
    left = float(
        game.visual.difference(
            before_path,
            after_path,
            region=(0, 0, max(1, width // 2), max(1, height)),
        )
    )
    return {"full": full, "left_half": left, "maximum": max(full, left)}


def normalized_window_point(game: Any, x: float, y: float) -> tuple[float, float]:
    point = game.convert_point(
        {"x": x, "y": y},
        from_space="normalized_viewport",
        to_space="window",
    )
    return float(point["x"]), float(point["y"])


def exercise_basic(game: Any, log_start: int, timeout: float) -> dict[str, Any]:
    receipts = []
    receipts.append(dict(game.click(normalized_window_point(game, 0.5, 0.5), wait="released", device="mouse")))
    for key in ("KEY_SPACE", "KEY_SPACE", "KEY_C", "KEY_P", "KEY_R"):
        receipts.append(dict(game.key(key, wait="released")))

    started = False
    try:
        receipts.append(dict(game.key("KEY_B", wait="released")))
        started = True
        wait_for_log_tokens(game, log_start, ("GO callback", "GUI callback"), timeout=min(timeout, 12.0))
    finally:
        if started:
            receipts.append(dict(game.key("KEY_B", wait="released")))

    expected_logs = (
        "GO IK mode: fixed",
        "GO IK mode: follow",
        "IK mode: fixed",
        "IK mode: follow",
        "GO: Testing cursor control",
        "GUI: Testing cursor control",
        "GO: Testing playback rate control",
        "GUI: Testing playback rate control",
        "Reset IK target",
        "GO callback",
        "GUI callback",
        "GO animation cancelled on track 1",
        "GUI animation cancelled on track 1",
    )
    observed = wait_for_log_tokens(game, log_start, expected_logs, timeout=min(timeout, 12.0))
    return {
        "input_receipts": json_safe(receipts),
        "expected_log_tokens": list(expected_logs),
        "observed_log_lines": observed,
    }


def _element_center(element: Any) -> Optional[tuple[float, float]]:
    center = element.center
    if not isinstance(center, Mapping) or "x" not in center or "y" not in center:
        return None
    return float(center["x"]), float(center["y"])


def exercise_physics(game: Any) -> dict[str, Any]:
    attempts = []
    targets = []
    targets.extend(game.elements(name_exact="click_zone", kind="gui_node", has_bounds=True, limit=10, include="basic,bounds"))
    targets.extend(game.elements(type_exact="spinemodelc", has_bounds=True, limit=10, include="basic,bounds"))
    seen = set()
    successful = 0
    for target in targets:
        if target.id in seen:
            continue
        seen.add(target.id)
        center = _element_center(target)
        if center is None:
            continue
        destination = (center[0] + 36.0, center[1] + 12.0)
        attempt = {"target": target.compact(), "from": center, "to": destination}
        try:
            attempt["receipt"] = json_safe(
                dict(game.drag(center, destination, duration=0.25, wait="released", device="mouse"))
            )
            successful += 1
        except Exception as error:
            attempt["error"] = f"{type(error).__name__}: {error}"
        attempts.append(attempt)
    if successful == 0:
        raise AssertionError(f"no physics drag target could be exercised: {attempts!r}")
    return {"drag_attempts": attempts, "successful_drag_count": successful}


def exercise_owl(game: Any) -> dict[str, Any]:
    start = normalized_window_point(game, 0.25, 0.30)
    end = normalized_window_point(game, 0.75, 0.70)
    receipt = game.drag(start, end, duration=0.35, wait="released", device="mouse")
    return {"drag": {"from": start, "to": end, "receipt": json_safe(dict(receipt))}}


def exercise_shared_skin(
    game: Any,
    log_start: int,
    timeout: float,
    implementation: str,
    instance_count: int,
    restoration: str,
    artifact_prefix: Path,
) -> dict[str, Any]:
    cleared_token = f"{implementation} shared skin last-reference clear completed"
    attachment_token = f"{implementation} shared skin attachment restored: {restoration}"
    passed_token = f"{implementation} shared skin regression passed: instances={instance_count}"
    render_before = capture(game, artifact_prefix.with_name(f"{artifact_prefix.name}-before.png"))
    command = game.command(EXERCISE_CURRENT_COMMAND, {}, timeout=timeout)
    observed = wait_for_log_tokens(
        game,
        log_start,
        (cleared_token, attachment_token, passed_token),
        timeout=min(timeout, 12.0),
    )
    render_after = capture(game, artifact_prefix.with_name(f"{artifact_prefix.name}-after.png"))
    render_difference = compare_captures(game, render_before, render_after)
    if render_difference["maximum"] <= 0.00001:
        raise AssertionError(
            f"{implementation} shared-skin exercise produced no visible render change: "
            f"{render_difference['maximum']:.8f}"
        )
    return {
        "command": json_safe(command),
        "implementation": implementation,
        "shared_instance_count": instance_count,
        "expected_log_tokens": [cleared_token, attachment_token, passed_token],
        "observed_log_lines": observed,
        "render_difference": render_difference,
        "render_captures": {
            "before": render_before,
            "after": render_after,
        },
    }


def pre_wait_exercise(
    game: Any,
    example: Example,
    log_start: int,
    timeout: float,
    artifact_prefix: Path,
) -> dict[str, Any]:
    evidence: dict[str, Any] = {}
    if example.exercise == "basic":
        evidence.update(exercise_basic(game, log_start, timeout))
    elif example.exercise == "physics":
        evidence.update(exercise_physics(game))
    elif example.exercise == "owl":
        evidence.update(exercise_owl(game))
    elif example.exercise == "gui_shared_skin":
        evidence["shared_skin"] = exercise_shared_skin(
            game, log_start, timeout, "GUI", 2, "mouth-smile", artifact_prefix
        )
    elif example.exercise == "swap":
        evidence["gui_node_count_before"] = game.count(kind="gui_node")
        evidence["shared_skin"] = exercise_shared_skin(
            game, log_start, timeout, "GO", 2, "scene-reset", artifact_prefix
        )
    elif example.exercise == "bones":
        evidence["game_object_count_before"] = game.count(type_exact="goc")
    return evidence


def post_wait_assertions(game: Any, example: Example, evidence: dict[str, Any], timeout: float) -> None:
    if example.exercise == "swap":
        expected = int(evidence["gui_node_count_before"]) + 2
        deadline = time.monotonic() + timeout
        observed = game.count(kind="gui_node")
        while observed < expected and time.monotonic() < deadline:
            game.wait_frames(1, timeout=min(timeout, 5.0))
            observed = game.count(kind="gui_node")
        evidence["gui_node_count_after"] = observed
        evidence["minimum_expected_gui_nodes"] = expected
        if observed < expected:
            raise AssertionError(f"dynamic GUI spine nodes did not appear: expected >= {expected}, got {observed}")
    elif example.exercise == "bones":
        before = int(evidence["game_object_count_before"])
        deadline = time.monotonic() + timeout
        observed = game.count(type_exact="goc")
        while observed == before and time.monotonic() < deadline:
            game.wait_frames(1, timeout=min(timeout, 5.0))
            observed = game.count(type_exact="goc")
        evidence["game_object_count_after"] = observed
        if observed == before:
            raise AssertionError(f"bone game-object count did not change after scene swap: {before}")


def validate_example(
    game: Any,
    example: Example,
    output: Path,
    timeout: float,
    require_visual_change: bool,
    visual_threshold: float,
    allow_patterns: Sequence[re.Pattern[str]],
) -> dict[str, Any]:
    slug = f"{example.index:02d}-{re.sub(r'[^a-z0-9]+', '-', example.name.lower()).strip('-')}"
    result: dict[str, Any] = {
        "index": example.index,
        "name": example.name,
        "proxy_id": example.proxy_id,
        "status": "running",
        "started_at": utc_now(),
    }
    log_start = len(current_log_lines(game))
    started = time.monotonic()
    # Native marker metadata currently has a compact payload limit. The full
    # example name remains in report.json; the marker only needs a stable key.
    game.mark("extension_spine_example_started", {"index": example.index})
    try:
        result["navigation"] = load_example(game, example, timeout)
        count_timeout = max(timeout, 60.0) if example.index == 16 else timeout
        observed_count = game.wait_for_count(
            example.spinemodel_count,
            timeout=count_timeout,
            type_exact="spinemodelc",
        )
        result["spinemodel_count"] = {
            "expected": example.spinemodel_count,
            "observed": observed_count,
        }
        result["label"] = wait_for_label(game, example, timeout)
        result["scene_before"] = scene_summary(game)
        result["screenshot_before"] = capture(game, output / f"{slug}-before.png")

        evidence = pre_wait_exercise(
            game,
            example,
            log_start,
            timeout,
            output / f"{slug}-shared-skin-render",
        )
        evidence["frames_waited"] = wait_engine_seconds(game, example.observe_seconds, timeout)
        post_wait_assertions(game, example, evidence, timeout)
        result["exercise"] = evidence

        result["screenshot_after"] = capture(game, output / f"{slug}-after.png")
        result["scene_after"] = scene_summary(game)
        result["visual_difference"] = compare_captures(
            game,
            result["screenshot_before"],
            result["screenshot_after"],
        )
        if require_visual_change and result["visual_difference"]["maximum"] <= visual_threshold:
            raise AssertionError(
                f"visual difference {result['visual_difference']['maximum']:.8f} did not exceed {visual_threshold:.8f}"
            )

        game.wait_frames(2, timeout=min(timeout, 5.0))
        logs = current_log_lines(game)
        result["runtime_logs"] = logs[log_start:] if log_start <= len(logs) else logs
        result["unexpected_runtime_logs"] = unexpected_lines(
            f"runtime:{example.name}", result["runtime_logs"], allow_patterns
        )
        if result["unexpected_runtime_logs"]:
            raise AssertionError(f"unexpected runtime errors: {result['unexpected_runtime_logs']!r}")
        result["status"] = "passed"
    except Exception as error:
        result["status"] = "failed"
        result["failure"] = {
            "type": type(error).__name__,
            "message": str(error),
            "traceback": traceback.format_exc(),
        }
        try:
            if "screenshot_after" not in result:
                result["screenshot_after"] = capture(game, output / f"{slug}-failure.png")
        except Exception as capture_error:
            result["failure_capture_error"] = f"{type(capture_error).__name__}: {capture_error}"
        try:
            logs = current_log_lines(game)
            result["runtime_logs"] = logs[log_start:] if log_start <= len(logs) else logs
            result["unexpected_runtime_logs"] = unexpected_lines(
                f"runtime:{example.name}", result["runtime_logs"], allow_patterns
            )
        except Exception as log_error:
            result["runtime_log_error"] = f"{type(log_error).__name__}: {log_error}"
    finally:
        result["duration_seconds"] = time.monotonic() - started
        result["finished_at"] = utc_now()
        try:
            game.mark(
                "extension_spine_example_finished",
                {"index": example.index},
            )
        except Exception as marker_error:
            result["finish_marker_error"] = f"{type(marker_error).__name__}: {marker_error}"
    return result


def run(args: argparse.Namespace) -> int:
    project_root = args.project.expanduser().resolve()
    output = args.output.expanduser().resolve()
    report_path = output / "report.json"
    output.mkdir(parents=True, exist_ok=True)
    report: dict[str, Any] = {
        "schema_version": 1,
        "status": "running",
        "started_at": utc_now(),
        "project": str(project_root),
        "output": str(output),
        "build": "incremental" if args.incremental else "clean",
        "required_capabilities": list(REQUIRED_CAPABILITIES),
        "examples": [],
        "failures": [],
    }
    write_report(report_path, report)

    project = None
    game = None
    console_before: list[str] = []
    all_runtime_logs: list[str] = []
    try:
        wrapper_root = project_root / "automation-bridge-python"
        if not wrapper_root.is_dir():
            raise FileNotFoundError(f"Automation Bridge Python wrapper is missing: {wrapper_root}")
        sys.path.insert(0, str(wrapper_root))
        from automation_bridge import editor

        project = editor.open_project(
            project_root,
            start_if_needed=not args.no_start_editor,
            timeout=args.build_timeout,
        )
        console_before = list(project.console.read().lines)
        build = project.build_and_run if args.incremental else project.clean_build_and_run
        game = build(
            timeout=args.build_timeout,
            required_capabilities=REQUIRED_CAPABILITIES,
        )
        report["health"] = json_safe(game.health())
        report["metadata"] = json_safe(game.trace_metadata())
        report["lifecycle"] = json_safe(game.lifecycle())
        report["screen_before_resize"] = json_safe(game.screen())
        report["resize"] = json_safe(game.resize(960, 640))
        report["initial_navigation"] = json_safe(
            wait_navigation_ready(game, expected_index=1, timeout=args.timeout)
        )
        write_report(report_path, report)

        for example in EXAMPLES:
            print(f"[{example.index:02d}/{len(EXAMPLES)}] {example.name}", flush=True)
            result = validate_example(
                game,
                example,
                output,
                timeout=args.timeout,
                require_visual_change=args.require_visual_change,
                visual_threshold=args.visual_threshold,
                allow_patterns=args.allow_error_patterns,
            )
            report["examples"].append(result)
            if result["status"] != "passed":
                report["failures"].append(
                    {"example": example.name, "failure": result.get("failure", {"message": "unknown failure"})}
                )
            write_report(report_path, report)
            if args.fail_fast and result["status"] != "passed":
                break
    except Exception as error:
        report["fatal_error"] = {
            "type": type(error).__name__,
            "message": str(error),
            "traceback": traceback.format_exc(),
        }
        report["failures"].append({"fatal": report["fatal_error"]})
    finally:
        if game is not None:
            try:
                game.wait_frames(2, timeout=min(args.timeout, 5.0))
            except Exception:
                pass
            try:
                all_runtime_logs = current_log_lines(game)
                report["runtime_logs"] = all_runtime_logs
            except Exception as error:
                report["runtime_log_error"] = f"{type(error).__name__}: {error}"

        if project is not None:
            try:
                console_after = list(project.console.read().lines)
                report["editor_console_delta"] = console_delta(console_before, console_after)
            except Exception as error:
                report["editor_console_error"] = f"{type(error).__name__}: {error}"

        combined_unexpected = []
        combined_unexpected.extend(
            unexpected_lines("runtime", all_runtime_logs, args.allow_error_patterns)
        )
        combined_unexpected.extend(
            unexpected_lines(
                "editor",
                report.get("editor_console_delta", ()),
                args.allow_error_patterns,
            )
        )
        deduplicated = []
        seen = set()
        for item in combined_unexpected:
            identity = (item["source"], item["line"])
            if identity not in seen:
                seen.add(identity)
                deduplicated.append(item)
        report["unexpected_logs"] = deduplicated
        if deduplicated:
            report["failures"].append({"unexpected_logs": deduplicated})

        report["status"] = "passed" if not report["failures"] else "failed"
        report["finished_at"] = utc_now()
        write_report(report_path, report)

        if game is not None:
            if args.close_engine:
                try:
                    game.close_engine()
                except Exception as error:
                    print(f"warning: could not close engine: {error}", file=sys.stderr)
            else:
                game.logs.close()

    passed = sum(1 for result in report["examples"] if result.get("status") == "passed")
    total = len(report["examples"])
    print(f"{report['status'].upper()}: {passed}/{total} examples passed; report: {report_path}")
    return 0 if report["status"] == "passed" else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
