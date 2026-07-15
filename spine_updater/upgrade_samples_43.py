#!/usr/bin/env python3
"""Rebuild the repository's Spine 4.3 sample JSON assets.

This script intentionally does not relax the runtime's skeleton-version check.
Official samples are copied from the upstream spine-runtimes 4.3 checkout, the
two custom mix-and-match samples retain their project-owned skins verbatim, and
the small constraint-free samples receive only the compatible 4.3 version tag.

Usage:
    python3 spine_updater/upgrade_samples_43.py \
        /path/to/spine-runtimes/examples
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import shutil
from pathlib import Path
from typing import Any


UPSTREAM_REPOSITORY = "https://github.com/EsotericSoftware/spine-runtimes"
UPSTREAM_BRANCH = "4.3"
UPSTREAM_COMMIT = "dc4a91bdb06ad83f90ecdf794f4fe47dd04812e5"

OFFICIAL_SAMPLES = {
    "assets/coins/coin-pro.spinejson": "coin/export/coin-pro.json",
    "assets/owl/owl.spinejson": "owl/export/owl-pro.json",
    "assets/spineboy/spineboy.spinejson": "spineboy/export/spineboy-pro.json",
    "assets/trophy_wingame_spine/celestial-circus-pro.spinejson": (
        "celestial-circus/export/celestial-circus-pro.json"
    ),
}

CUSTOM_SPINEBOY_SAMPLES = (
    "assets/mix_skins/spineboy/spineboy-pro_emptyskin.spinejson",
    "assets/mix_skins/spineboy/spineboy_mix.spinejson",
)

VERSION_ONLY_SAMPLES = (
    "assets/sequence_animation/sequence.spinejson",
    "assets/squirrel/squirrel.spinejson",
    "custom_res/squirrel.spinejson",
    "examples/instances/person_1/person_1.spinejson",
    "examples/instances/person_2/person_2.spinejson",
)

SPINEBOY_SOURCE = "spineboy/export/spineboy-pro.json"


ALL_TRANSFORM_PROPERTIES = {
    "rotate": {"to": {"rotate": {"max": 100}}},
    "x": {"to": {"x": {"max": 100}}},
    "y": {"to": {"y": {"max": 100}}},
    "scaleX": {"to": {"scaleX": {}}},
    "scaleY": {"to": {"scaleY": {}}},
    "shearY": {"to": {"shearY": {"max": 100}}},
}

CUSTOM_CONSTRAINTS = (
    (
        "front-leg-ik",
        {
            "type": "transform",
            "name": "short_legs_constraint_left",
            "skin": True,
            "source": "front-foot-target",
            "bones": ["front-thigh", "front-shin"],
            "scaleX": -0.6,
            "properties": copy.deepcopy(ALL_TRANSFORM_PROPERTIES),
            "mixRotate": 0,
            "mixX": 0,
            "mixScaleX": 0.4,
            "mixScaleY": 0,
            "mixShearY": 0,
        },
    ),
    (
        "rear-leg-ik",
        {
            "type": "transform",
            "name": "short_legs_constraint_right",
            "skin": True,
            "source": "rear-foot-target",
            "bones": ["rear-thigh", "rear-shin"],
            "scaleX": -0.6,
            "properties": copy.deepcopy(ALL_TRANSFORM_PROPERTIES),
            "mixRotate": 0,
            "mixX": 0,
            "mixScaleX": 0.4,
            "mixScaleY": 0,
            "mixShearY": 0,
        },
    ),
    (
        "aim-torso-ik",
        {
            "type": "transform",
            "name": "hip_position_constraint",
            "skin": True,
            "source": "root",
            "bones": ["hip"],
            "properties": copy.deepcopy(ALL_TRANSFORM_PROPERTIES),
            "mixRotate": 0,
            "mixX": 0,
            "mixY": 0.1188,
            "mixScaleX": 0,
            "mixShearY": 0,
        },
    ),
)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object: {path}")
    return value


def matching_bracket(text: str, start: int) -> int:
    opening = text[start]
    closing = {"[": "]", "{": "}"}.get(opening)
    if closing is None:
        raise ValueError(f"Expected '[' or '{{' at offset {start}")

    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
        character = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"Unmatched {opening!r} at offset {start}")


def top_level_array_span(text: str, key: str) -> tuple[int, int]:
    match = re.search(rf'^"{re.escape(key)}"\s*:\s*\[', text, re.MULTILINE)
    if match is None:
        raise ValueError(f"Missing top-level array {key!r}")
    start = text.find("[", match.start())
    return start, matching_bracket(text, start) + 1


def replace_top_level_array(text: str, key: str, replacement: str) -> str:
    start, end = top_level_array_span(text, key)
    return text[:start] + replacement + text[end:]


def nested_object_span(
    text: str, key: str, indent: str, start: int, end: int
) -> tuple[int, int]:
    match = re.search(
        rf'^{re.escape(indent)}"{re.escape(key)}"\s*:\s*\{{',
        text[start:end],
        re.MULTILINE,
    )
    if match is None:
        raise ValueError(f"Missing object {key!r}")
    value_start = text.find("{", start + match.start())
    return value_start, matching_bracket(text, value_start) + 1


def replace_custom_hoverboard_timelines(text: str) -> tuple[str, dict[str, Any]]:
    data = json.loads(text)
    animations = data["animations"]
    attachments = animations["hoverboard"]["attachments"]
    default = attachments["default"]
    original_body: dict[str, Any] = {}
    for slot in ("front-foot", "front-shin", "rear-foot"):
        original_body[slot] = default.pop(slot)
    attachments["original_body"] = original_body

    root_start = text.find("{")
    root_end = matching_bracket(text, root_start) + 1
    animations_start, animations_end = nested_object_span(
        text, "animations", "", root_start, root_end
    )
    hoverboard_start, hoverboard_end = nested_object_span(
        text, "hoverboard", "\t", animations_start, animations_end
    )
    attachments_start, attachments_end = nested_object_span(
        text, "attachments", "\t\t", hoverboard_start, hoverboard_end
    )
    formatted_lines = json.dumps(
        attachments, ensure_ascii=False, indent="\t"
    ).splitlines()
    formatted = formatted_lines[0] + "\n" + "\n".join(
        "\t\t" + line for line in formatted_lines[1:]
    )
    output = text[:attachments_start] + formatted + text[attachments_end:]
    if json.loads(output)["animations"] != animations:
        raise ValueError("Custom hoverboard timeline rewrite was not lossless")
    return output, animations


def format_array_item(value: dict[str, Any]) -> str:
    formatted = json.dumps(value, ensure_ascii=False, indent="\t")
    return "\n".join("\t" + line for line in formatted.splitlines()) + ",\n"


def insert_constraint_before(
    source: str, target_name: str, constraint: dict[str, Any]
) -> str:
    constraints_start, constraints_end = top_level_array_span(source, "constraints")
    constraint_text = source[constraints_start:constraints_end]
    name_match = re.search(
        rf'^\t\{{\n\t\t"type":\s*"[^"]+",\n\t\t"name":\s*"{re.escape(target_name)}",',
        constraint_text,
        re.MULTILINE,
    )
    if name_match is None:
        raise ValueError(f"Constraint {target_name!r} not found in upstream spineboy")
    insertion = constraints_start + name_match.start()
    return source[:insertion] + format_array_item(constraint) + source[insertion:]


def attachment_paths(data: dict[str, Any]) -> set[str]:
    paths: set[str] = set()
    for skin in data.get("skins", []):
        for slot_attachments in skin.get("attachments", {}).values():
            for placeholder, attachment in slot_attachments.items():
                attachment_type = attachment.get("type", "region")
                if attachment_type in ("region", "mesh"):
                    paths.add(attachment.get("path", attachment.get("name", placeholder)))
    return paths


def migrate_custom_skins_43(
    skins_text: str, skins: list[dict[str, Any]]
) -> tuple[str, list[dict[str, Any]]]:
    """Rename the one linked-mesh field that changed between 4.2 and 4.3."""
    migrated = copy.deepcopy(skins)
    renamed = 0
    for skin in migrated:
        for slot_attachments in skin.get("attachments", {}).values():
            for attachment in slot_attachments.values():
                if "parent" not in attachment:
                    continue
                if attachment.get("type") != "linkedmesh" or "source" in attachment:
                    raise ValueError(
                        "Only 4.2 linked meshes may contain parent in custom skins"
                    )
                attachment["source"] = attachment.pop("parent")
                renamed += 1

    migrated_text, text_renames = re.subn(
        r'"parent"(\s*):', r'"source"\1:', skins_text
    )
    if text_renames != renamed:
        raise ValueError(
            "Custom skin linked-mesh migration changed an unexpected parent key"
        )
    if json.loads(migrated_text) != migrated:
        raise ValueError("Custom skin linked-mesh text migration is not lossless")
    return migrated_text, migrated


def names(items: list[dict[str, Any]]) -> list[str]:
    return [item["name"] for item in items]


def validate_common(path: Path, data: dict[str, Any]) -> None:
    version = data.get("skeleton", {}).get("spine", "")
    if not isinstance(version, str) or not version.startswith("4.3"):
        raise ValueError(f"{path}: expected a Spine 4.3 version, got {version!r}")
    if not data.get("bones"):
        raise ValueError(f"{path}: skeleton has no bones")
    if not data.get("animations"):
        raise ValueError(f"{path}: skeleton has no animations")
    if not data.get("skins"):
        raise ValueError(f"{path}: skeleton has no skins")


def validate_custom(
    path: Path,
    output: dict[str, Any],
    upstream: dict[str, Any],
    original_skins: list[dict[str, Any]],
    expected_animations: dict[str, Any],
) -> None:
    validate_common(path, output)
    if output["skins"] != original_skins:
        raise ValueError(f"{path}: custom skins changed")
    for key in ("bones", "slots", "events"):
        if output.get(key) != upstream.get(key):
            raise ValueError(f"{path}: {key} differs from upstream 4.3 spineboy")
    if output.get("animations") != expected_animations:
        raise ValueError(f"{path}: animations differ from the remapped 4.3 base")

    output_constraints = output["constraints"]
    output_names = names(output_constraints)
    expected_insertions = {
        target: constraint["name"] for target, constraint in CUSTOM_CONSTRAINTS
    }
    for target, inserted in expected_insertions.items():
        if output_names.index(inserted) + 1 != output_names.index(target):
            raise ValueError(f"{path}: {inserted} is not immediately before {target}")

    available_constraints = set(output_names)
    available_slots = set(names(output["slots"]))
    for skin in output["skins"]:
        for constraint_type in ("ik", "transform", "path", "physics", "slider"):
            missing = set(skin.get(constraint_type, [])) - available_constraints
            if missing:
                raise ValueError(
                    f"{path}: skin {skin['name']} references missing constraints {missing}"
                )
        missing_slots = set(skin.get("attachments", {})) - available_slots
        if missing_slots:
            raise ValueError(
                f"{path}: skin {skin['name']} references missing slots {missing_slots}"
            )


def upgrade_version_only(path: Path) -> None:
    before = path.read_bytes()
    data = json.loads(before)
    for legacy_key in ("ik", "transform", "path", "physics", "slider", "constraints"):
        if data.get(legacy_key):
            raise ValueError(
                f"{path}: version-only upgrade is allowed only for constraint-free data"
            )

    pattern = re.compile(rb'("spine"\s*:\s*")[^"]+(\")')
    after, replacements = pattern.subn(rb'\g<1>4.3\2', before, count=1)
    if replacements != 1:
        raise ValueError(f"{path}: could not replace skeleton.spine")
    upgraded = json.loads(after)
    validate_common(path, upgraded)

    comparison = copy.deepcopy(upgraded)
    comparison["skeleton"]["spine"] = data["skeleton"]["spine"]
    if comparison != data:
        raise ValueError(f"{path}: version-only upgrade changed more than the version")
    path.write_bytes(after)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "examples_root",
        type=Path,
        help="The examples directory from the upstream spine-runtimes 4.3 checkout",
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="extension-spine checkout (defaults to this script's parent checkout)",
    )
    arguments = parser.parse_args()
    examples_root = arguments.examples_root.resolve()
    project_root = arguments.project_root.resolve()

    upstream_data: dict[str, dict[str, Any]] = {}
    old_attachment_paths: dict[str, set[str]] = {}
    for destination, relative_source in OFFICIAL_SAMPLES.items():
        destination_path = project_root / destination
        source_path = examples_root / relative_source
        old_attachment_paths[destination] = attachment_paths(load_json(destination_path))
        source_data = load_json(source_path)
        validate_common(source_path, source_data)
        if attachment_paths(source_data) != old_attachment_paths[destination]:
            raise ValueError(
                f"{destination}: upstream 4.3 export changed the atlas attachment path set"
            )
        upstream_data[relative_source] = source_data
        shutil.copyfile(source_path, destination_path)

    spineboy_path = examples_root / SPINEBOY_SOURCE
    spineboy_text = spineboy_path.read_text(encoding="utf-8")
    spineboy_data = upstream_data.get(SPINEBOY_SOURCE, load_json(spineboy_path))
    for destination in CUSTOM_SPINEBOY_SAMPLES:
        destination_path = project_root / destination
        custom_text = destination_path.read_text(encoding="utf-8")
        skins_start, skins_end = top_level_array_span(custom_text, "skins")
        custom_skins_text = custom_text[skins_start:skins_end]
        custom_skins = load_json(destination_path)["skins"]
        custom_skins_text, custom_skins = migrate_custom_skins_43(
            custom_skins_text, custom_skins
        )

        output_text = replace_top_level_array(
            spineboy_text, "skins", custom_skins_text
        )
        for target_name, constraint in CUSTOM_CONSTRAINTS:
            output_text = insert_constraint_before(
                output_text, target_name, constraint
            )
        output_text, expected_animations = replace_custom_hoverboard_timelines(
            output_text
        )
        output = json.loads(output_text)
        validate_custom(
            destination_path,
            output,
            spineboy_data,
            custom_skins,
            expected_animations,
        )
        destination_path.write_text(output_text, encoding="utf-8")

    for destination in VERSION_ONLY_SAMPLES:
        upgrade_version_only(project_root / destination)

    expected = set(OFFICIAL_SAMPLES) | set(CUSTOM_SPINEBOY_SAMPLES) | set(
        VERSION_ONLY_SAMPLES
    )
    source_roots = ("assets", "custom_res", "examples", "main")
    actual = {
        str(path.relative_to(project_root))
        for source_root in source_roots
        for path in (project_root / source_root).rglob("*.spinejson")
    }
    if actual != expected:
        raise ValueError(
            "The project-owned .spinejson set changed; update this script's manifest: "
            f"missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}"
        )

    for relative_path in sorted(expected):
        validate_common(project_root / relative_path, load_json(project_root / relative_path))
        print(relative_path)


if __name__ == "__main__":
    main()
