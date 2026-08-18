#!/usr/bin/env python3
"""Generate the self-contained manual SkeletonBinary corruption fixtures."""

from __future__ import annotations

import argparse
import base64
import math
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


CORRUPTED_ROOT = Path(__file__).resolve().parents[2]
ASSETS_ROOT = CORRUPTED_ROOT / "_assets"
CASES_ROOT = ASSETS_ROOT / "cases"
SHARED_ROOT = ASSETS_ROOT / "shared"


class BinaryWriter:
    def __init__(self) -> None:
        self.data = bytearray()

    def byte(self, value: int) -> None:
        self.data.append(value & 0xFF)

    def bytes(self, value: bytes) -> None:
        self.data.extend(value)

    def int32(self, value: int) -> None:
        self.bytes(struct.pack(">I", value & 0xFFFFFFFF))

    def float32(self, value: float) -> None:
        self.bytes(struct.pack(">f", value))

    def varint(self, value: int) -> None:
        value &= 0xFFFFFFFF
        while True:
            byte = value & 0x7F
            value >>= 7
            if value:
                self.byte(byte | 0x80)
            else:
                self.byte(byte)
                return

    def string(self, value: str | None) -> None:
        if value is None:
            self.varint(0)
            return
        encoded = value.encode("utf-8")
        self.varint(len(encoded) + 1)
        self.bytes(encoded)

    def finish(self) -> bytes:
        return bytes(self.data)


@dataclass(frozen=True)
class Case:
    key: str
    description: str
    expected: str
    branch: str
    default_animation: str
    build: Callable[[], bytes]


def string_ref(strings: list[str], value: str | None) -> int:
    return 0 if value is None else strings.index(value) + 1


def write_header(writer: BinaryWriter, version: str | None = "4.2.99") -> None:
    writer.int32(0x43525250)
    writer.int32(0x54454421)
    writer.string(version)


def write_skeleton_prefix(
    writer: BinaryWriter,
    strings: list[str],
    setup_attachment: str | None,
    *,
    bone_x: float = 0.0,
    with_slot: bool = True,
) -> None:
    write_header(writer)

    # Skeleton bounds and reference scale.
    for value in (0.0, 0.0, 20.0, 20.0, 1.0):
        writer.float32(value)
    writer.byte(0)  # nonessential

    writer.varint(len(strings))
    for value in strings:
        writer.string(value)

    # One root bone.
    writer.varint(1)
    writer.string("root")
    for value in (0.0, bone_x, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0):
        writer.float32(value)
    writer.varint(0)  # inherit: normal
    writer.byte(0)  # skinRequired

    writer.varint(1 if with_slot else 0)
    if with_slot:
        writer.string("slot")
        writer.varint(0)  # bone index
        writer.bytes(bytes((255, 255, 255, 255)))  # light color
        writer.bytes(bytes((255, 255, 255, 255)))  # no dark color
        writer.varint(string_ref(strings, setup_attachment))
        writer.varint(0)  # blend mode: normal

    # IK, transform, path, and physics constraints.
    for _ in range(4):
        writer.varint(0)


def write_default_skin(
    writer: BinaryWriter,
    strings: list[str],
    attachments: list[tuple[str, Callable[[BinaryWriter], None]]],
) -> None:
    writer.varint(1 if attachments else 0)
    if not attachments:
        return
    writer.varint(0)  # slot index
    writer.varint(len(attachments))
    for name, write_attachment in attachments:
        writer.varint(string_ref(strings, name))
        write_attachment(writer)


def write_region(
    writer: BinaryWriter,
    strings: list[str],
    *,
    path: str | None = None,
    sequence: bool = False,
) -> None:
    flags = 0  # SP_ATTACHMENT_REGION
    if path is not None:
        flags |= 16
    if sequence:
        flags |= 64
    writer.byte(flags)
    if path is not None:
        writer.varint(string_ref(strings, path))
    if sequence:
        writer.varint(2)  # number of regions
        writer.varint(0)  # start
        writer.varint(1)  # digits
        writer.varint(0)  # setup index
    for value in (0.0, 0.0, 1.0, 1.0, 20.0, 20.0):
        writer.float32(value)


def write_mesh(
    writer: BinaryWriter,
    strings: list[str],
    *,
    path: str | None = None,
    sequence: bool = False,
) -> None:
    flags = 2  # SP_ATTACHMENT_MESH
    if path is not None:
        flags |= 16
    if sequence:
        flags |= 64
    writer.byte(flags)
    if path is not None:
        writer.varint(string_ref(strings, path))
    if sequence:
        writer.varint(2)
        writer.varint(0)
        writer.varint(1)
        writer.varint(0)

    writer.varint(4)  # hull length
    writer.varint(4)  # vertex count
    for value in (-10.0, -10.0, -10.0, 10.0, 10.0, 10.0, 10.0, -10.0):
        writer.float32(value)
    for value in (0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0):
        writer.float32(value)
    for index in (0, 1, 2, 2, 3, 0):
        writer.varint(index)


def write_linked_mesh(
    writer: BinaryWriter,
    strings: list[str],
    *,
    path: str,
    parent: str,
    skin_index: int = 0,
) -> None:
    writer.byte(3 | 16)  # linked mesh with an explicit path
    writer.varint(string_ref(strings, path))
    writer.varint(skin_index)
    writer.varint(string_ref(strings, parent))


def write_empty_animation(writer: BinaryWriter) -> None:
    writer.varint(0)  # declared timeline count (unused by the runtime)
    for _ in range(9):
        writer.varint(0)


def finish_skeleton(
    writer: BinaryWriter,
    *,
    animations: list[tuple[str, Callable[[BinaryWriter], None]]] | None = None,
) -> bytes:
    writer.varint(0)  # additional skins
    writer.varint(0)  # events
    animations = animations or []
    writer.varint(len(animations))
    for name, write_animation in animations:
        writer.string(name)
        write_animation(writer)
    return writer.finish()


def build_valid_control(*, bone_x: float = 0.0) -> bytes:
    strings = ["bone"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "bone", bone_x=bone_x)
    write_default_skin(
        writer,
        strings,
        [("bone", lambda out: write_region(out, strings))],
    )
    return finish_skeleton(writer, animations=[("idle", write_empty_animation)])


def build_truncated_header() -> bytes:
    return b"CORRUPT!"  # Eight bytes: rejected before SkeletonBinary reads it.


def build_missing_version() -> bytes:
    writer = BinaryWriter()
    write_header(writer, None)
    return writer.finish()


def build_version_mismatch() -> bytes:
    writer = BinaryWriter()
    write_header(writer, "3.8.99")
    return writer.finish()


def build_missing_region() -> bytes:
    strings = ["missing-region"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "missing-region")
    write_default_skin(
        writer,
        strings,
        [("missing-region", lambda out: write_region(out, strings))],
    )
    return finish_skeleton(writer)


def build_missing_mesh_region() -> bytes:
    strings = ["mesh", "missing-region"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "mesh")
    write_default_skin(
        writer,
        strings,
        [("mesh", lambda out: write_mesh(out, strings, path="missing-region"))],
    )
    return finish_skeleton(writer)


def build_missing_region_sequence() -> bytes:
    strings = ["missing-sequence"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "missing-sequence")
    write_default_skin(
        writer,
        strings,
        [("missing-sequence", lambda out: write_region(out, strings, sequence=True))],
    )
    return finish_skeleton(writer)


def build_missing_mesh_sequence() -> bytes:
    strings = ["missing-sequence"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "missing-sequence")
    write_default_skin(
        writer,
        strings,
        [("missing-sequence", lambda out: write_mesh(out, strings, sequence=True))],
    )
    return finish_skeleton(writer)


def build_missing_linked_mesh_region() -> bytes:
    strings = ["parent-mesh", "bone", "linked", "missing-region"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "linked")
    write_default_skin(
        writer,
        strings,
        [
            ("parent-mesh", lambda out: write_mesh(out, strings, path="bone")),
            (
                "linked",
                lambda out: write_linked_mesh(
                    out,
                    strings,
                    path="missing-region",
                    parent="parent-mesh",
                ),
            ),
        ],
    )
    return finish_skeleton(writer)


def build_missing_linked_mesh_parent() -> bytes:
    strings = ["linked", "bone", "missing-parent"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "linked")
    write_default_skin(
        writer,
        strings,
        [
            (
                "linked",
                lambda out: write_linked_mesh(
                    out,
                    strings,
                    path="bone",
                    parent="missing-parent",
                ),
            )
        ],
    )
    return finish_skeleton(writer)


def build_invalid_slot_timeline() -> bytes:
    strings = ["bone"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "bone")
    write_default_skin(
        writer,
        strings,
        [("bone", lambda out: write_region(out, strings))],
    )
    writer.varint(0)  # additional skins
    writer.varint(0)  # events
    writer.varint(1)  # animations
    writer.string("invalid-slot-timeline")
    writer.varint(1)  # declared timeline count
    writer.varint(1)  # slot timeline groups
    writer.varint(0)  # slot index
    writer.varint(1)  # timelines for the slot
    writer.byte(255)  # invalid slot timeline type
    writer.varint(1)  # frame count
    return writer.finish()


def build_invalid_bone_timeline() -> bytes:
    strings = ["bone"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "bone")
    write_default_skin(
        writer,
        strings,
        [("bone", lambda out: write_region(out, strings))],
    )
    writer.varint(0)
    writer.varint(0)
    writer.varint(1)
    writer.string("invalid-bone-timeline")
    writer.varint(1)  # declared timeline count
    writer.varint(0)  # slot timeline groups
    writer.varint(1)  # bone timeline groups
    writer.varint(0)  # bone index
    writer.varint(1)  # timelines for the bone
    writer.byte(255)  # invalid bone timeline type
    writer.varint(1)  # frame count
    writer.varint(0)  # bezier count
    return writer.finish()


def build_missing_deform_attachment() -> bytes:
    strings = ["bone", "missing-deform-attachment"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "bone")
    write_default_skin(
        writer,
        strings,
        [("bone", lambda out: write_region(out, strings))],
    )
    writer.varint(0)
    writer.varint(0)
    writer.varint(1)
    writer.string("missing-deform-attachment")
    writer.varint(1)  # declared timeline count
    for _ in range(6):  # slot, bone, IK, transform, path, physics timelines
        writer.varint(0)
    writer.varint(1)  # attachment timeline skin groups
    writer.varint(0)  # default skin
    writer.varint(1)  # slot groups
    writer.varint(0)  # slot index
    writer.varint(1)  # attachment timelines
    writer.varint(string_ref(strings, "missing-deform-attachment"))
    return writer.finish()


def build_unknown_attachment_type() -> bytes:
    strings = ["unknown-attachment"]
    writer = BinaryWriter()
    write_skeleton_prefix(writer, strings, "unknown-attachment")
    writer.varint(1)  # default skin slot count
    writer.varint(0)  # slot index
    writer.varint(1)  # attachment count
    writer.varint(string_ref(strings, "unknown-attachment"))
    writer.byte(7)  # no SP_ATTACHMENT_* value maps to 7

    # readSkin returns NULL without setting an error. These bytes are then read
    # as the normal top-level counts, demonstrating the silent acceptance path.
    writer.varint(0)  # additional skins
    writer.varint(0)  # events
    writer.varint(0)  # animations
    return writer.finish()


CASES = [
    Case(
        "00_valid_control",
        "Valid binary control with a region and empty idle animation.",
        "Opens, renders a tiny region, and updates without an error.",
        "Control (not corrupt)",
        "idle",
        build_valid_control,
    ),
    Case(
        "01_truncated_header",
        "Eight-byte file rejected by ReadSkeletonBinaryData before parsing.",
        "Fatal resource error; editor remains responsive.",
        "spine_loader.cpp: binary_data_size < 9",
        "",
        build_truncated_header,
    ),
    Case(
        "02_missing_version",
        "Header contains a null version string.",
        "Fatal 'Skeleton version is missing' resource error.",
        "SkeletonBinary.c: missing version",
        "",
        build_missing_version,
    ),
    Case(
        "03_version_mismatch",
        "Header declares Spine 3.8 for a 4.2 runtime.",
        "Fatal version mismatch resource error.",
        "SkeletonBinary.c: version prefix mismatch",
        "",
        build_version_mismatch,
    ),
    Case(
        "04_missing_region",
        "Region attachment names a region absent from the local atlas.",
        "Fatal atlas/region error when the root GO is opened.",
        "SkeletonBinary.c: region attachment loader returns NULL",
        "",
        build_missing_region,
    ),
    Case(
        "05_missing_mesh_region",
        "Mesh attachment names a region absent from the local atlas.",
        "Fatal atlas/region error; allocated mesh arrays are cleaned up.",
        "SkeletonBinary.c: mesh attachment loader returns NULL",
        "",
        build_missing_mesh_region,
    ),
    Case(
        "06_missing_region_sequence",
        "Region sequence resolves to atlas regions that do not exist.",
        "Fatal sequence-loading error when the root GO is opened.",
        "SkeletonBinary.c: region sequence attachment returns NULL",
        "",
        build_missing_region_sequence,
    ),
    Case(
        "07_missing_mesh_sequence",
        "Mesh sequence resolves to atlas regions that do not exist.",
        "Fatal sequence-loading error; mesh arrays and sequence are cleaned up.",
        "SkeletonBinary.c: mesh sequence attachment returns NULL",
        "",
        build_missing_mesh_sequence,
    ),
    Case(
        "08_missing_linked_mesh_region",
        "Linked mesh uses an atlas path that does not exist.",
        "Fatal atlas/region error while creating the linked mesh.",
        "SkeletonBinary.c: linked-mesh attachment loader returns NULL",
        "",
        build_missing_linked_mesh_region,
    ),
    Case(
        "09_missing_linked_mesh_parent",
        "Linked mesh names a parent attachment that is absent.",
        "Fatal 'Parent mesh not found' resource error.",
        "SkeletonBinary.c: linked-mesh parent lookup",
        "",
        build_missing_linked_mesh_parent,
    ),
    Case(
        "10_invalid_slot_timeline",
        "Animation contains an unknown slot timeline type.",
        "Fatal 'Animation corrupted' resource error.",
        "SkeletonBinary.c: slot timeline default branch",
        "",
        build_invalid_slot_timeline,
    ),
    Case(
        "11_invalid_bone_timeline",
        "Animation contains an unknown bone timeline type.",
        "Fatal 'Animation corrupted' resource error.",
        "SkeletonBinary.c: bone timeline default branch",
        "",
        build_invalid_bone_timeline,
    ),
    Case(
        "12_missing_deform_attachment",
        "Animation deform timeline references an absent attachment.",
        "Fatal 'Animation corrupted' resource error.",
        "SkeletonBinary.c: attachment timeline lookup",
        "",
        build_missing_deform_attachment,
    ),
    Case(
        "13_unknown_attachment_type",
        "Skin attachment has the unused type value 7.",
        "Must not crash. Current runtime silently drops it and opens empty.",
        "SkeletonBinary.c: attachment switch default / readSkin NULL",
        "",
        build_unknown_attachment_type,
    ),
    Case(
        "14_non_finite_transform",
        "Valid structure contains NaN as the root bone X position.",
        "Must not crash while creating, updating, or previewing the skeleton.",
        "Operation path after a successful binary load",
        "idle",
        lambda: build_valid_control(bone_x=math.nan),
    ),
]


PNG_1X1_RGBA = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4"
    "z8DwHwAFgAI/ScLxVQAAAABJRU5ErkJggg=="
)


ATLAS = """\
images {
  image: "/corrupted/_assets/shared/bone.png"
}
"""


MATERIAL = """\
name: "corrupted_spine"
tags: "tile"
vertex_program: "/corrupted/_assets/shared/spine.vp"
fragment_program: "/corrupted/_assets/shared/spine.fp"
vertex_constants {
  name: "world_view_proj"
  type: CONSTANT_TYPE_WORLDVIEWPROJ
}
fragment_constants {
  name: "tint"
  type: CONSTANT_TYPE_USER
  value {
    x: 1.0
    y: 1.0
    z: 1.0
    w: 1.0
  }
}
"""


VERTEX_SHADER = """\
#version 140

in highp vec4 position;
in mediump vec2 texcoord0;
in lowp vec4 color;

out mediump vec2 var_texcoord0;
out lowp vec4 var_color;

uniform vs_uniforms
{
    highp mat4 world_view_proj;
};

void main()
{
    gl_Position = world_view_proj * vec4(position.xyz, 1.0);
    var_texcoord0 = texcoord0;
    var_color = vec4(color.rgb * color.a, color.a);
}
"""


FRAGMENT_SHADER = """\
#version 140

in mediump vec2 var_texcoord0;
in lowp vec4 var_color;

uniform lowp sampler2D texture_sampler;

uniform fs_uniforms
{
    mediump vec4 tint;
};

out vec4 out_fragColor;

void main()
{
    lowp vec4 tint_pm = vec4(tint.xyz * tint.w, tint.w);
    out_fragColor = texture(texture_sampler, var_texcoord0.xy) * var_color * tint_pm;
}
"""


def make_scene(case: Case) -> str:
    return (
        f'spine_json: "/corrupted/_assets/cases/{case.key}/case.skel"\n'
        'atlas: "/corrupted/_assets/shared/test.atlas"\n'
    )


def make_go(case: Case) -> str:
    return f'''\
embedded_components {{
  id: "corruption_case"
  type: "spinemodel"
  data: "spine_scene: \\"/corrupted/_assets/cases/{case.key}/case.spinescene\\"\\n"
  "default_animation: \\"{case.default_animation}\\"\\n"
  "skin: \\"\\"\\n"
  "material: \\"/corrupted/_assets/shared/spine.material\\"\\n"
  ""
}}
'''


def make_readme() -> str:
    rows = [
        "| Open this root file | Corruption | Branch exercised | Expected editor result |",
        "| --- | --- | --- | --- |",
    ]
    for case in CASES:
        rows.append(
            f"| `{case.key}.go` | {case.description} | {case.branch} | {case.expected} |"
        )

    return """\
# SkeletonBinary corruption fixtures

Open only the numbered `.go` files in `/corrupted`. Everything they reference is
under `/corrupted`; `_assets` is implementation detail, not an editor entry point.

The atlas-specific cases intentionally load successfully when their raw `.skel`
resource is inspected without an atlas. Opening the root `.go` supplies the local
atlas and exercises the intended attachment-loader failure.

""" + "\n".join(rows) + """

## Known limits

- `spSkeletonBinary_readSkeletonDataFile` is not used by the editor buffer path,
  so its missing-file branch cannot be reached by a linked editor asset.
- The `Skin not found` linked-mesh branch requires an invalid/out-of-range skin
  pointer before the check. Such a fixture invokes undefined memory access in the
  current runtime and would make this project unsafe to open, so it is not stored
  as an active `.skel` resource.
- Allocation-failure branches require fault-injected allocation, not file data.
- The eight-byte truncation case covers the loader's safe length guard. Deeper
  truncations are omitted until `SkeletonBinary.c` checks `input->end` on reads.

Regenerate or verify the deterministic assets with:

```sh
python3 corrupted/_assets/tools/generate.py
python3 corrupted/_assets/tools/generate.py --check
```
"""


def generated_files() -> dict[Path, bytes]:
    result: dict[Path, bytes] = {
        SHARED_ROOT / "bone.png": PNG_1X1_RGBA,
        SHARED_ROOT / "test.atlas": ATLAS.encode(),
        SHARED_ROOT / "spine.material": MATERIAL.encode(),
        SHARED_ROOT / "spine.vp": VERTEX_SHADER.encode(),
        SHARED_ROOT / "spine.fp": FRAGMENT_SHADER.encode(),
        ASSETS_ROOT / "README.md": make_readme().encode(),
    }

    for case in CASES:
        case_root = CASES_ROOT / case.key
        result[case_root / "case.skel"] = case.build()
        result[case_root / "case.spinescene"] = make_scene(case).encode()
        result[CORRUPTED_ROOT / f"{case.key}.go"] = make_go(case).encode()
    return result


def validate_self_contained(files: dict[Path, bytes]) -> list[str]:
    errors: list[str] = []
    resource_pattern = re.compile(r'(?<!\\)"(/[^"\\]+)"|\\"(/[^"\\]+)\\"')
    for path, content in files.items():
        if path.suffix not in {".go", ".spinescene", ".atlas", ".material"}:
            continue
        text = content.decode()
        for match in resource_pattern.finditer(text):
            resource_path = match.group(1) or match.group(2)
            if not resource_path.startswith("/corrupted/"):
                errors.append(f"{path}: external resource reference {resource_path}")
                continue
            target = CORRUPTED_ROOT.parent / resource_path.removeprefix("/")
            if target not in files and not target.is_file():
                errors.append(f"{path}: missing resource target {resource_path}")
    return errors


def check(files: dict[Path, bytes]) -> int:
    errors = validate_self_contained(files)
    for path, expected in files.items():
        if not path.is_file():
            errors.append(f"missing: {path.relative_to(CORRUPTED_ROOT)}")
        elif path.read_bytes() != expected:
            errors.append(f"out of date: {path.relative_to(CORRUPTED_ROOT)}")

    expected_root_files = {f"{case.key}.go" for case in CASES}
    actual_root_files = {path.name for path in CORRUPTED_ROOT.iterdir() if path.is_file()}
    for unexpected in sorted(actual_root_files - expected_root_files):
        errors.append(f"unexpected root file: {unexpected}")

    if errors:
        print("Corruption fixture verification failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print(f"Verified {len(CASES)} self-contained corruption entry points.")
    return 0


def write(files: dict[Path, bytes]) -> None:
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    print(f"Generated {len(CASES)} corruption entry points in {CORRUPTED_ROOT}.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()

    files = generated_files()
    errors = validate_self_contained(files)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    if args.check:
        return check(files)
    write(files)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
