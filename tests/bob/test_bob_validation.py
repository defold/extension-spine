#!/usr/bin/env python3
# Copyright 2020-2026 The Defold Foundation
# Copyright 2014-2020 King
# Copyright 2009-2014 Ragnar Svensson, Christian Murray
# Licensed under the Defold License version 1.0 (the "License"); you may not use
# this file except in compliance with the License.
#
# You may obtain a copy of the License, together with FAQs at
# https://www.defold.com/license
#
# Unless required by applicable law or agreed to in writing, software distributed
# under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
# CONDITIONS OF ANY KIND, either express or implied. See the License for the
# specific language governing permissions and limitations under the License.

"""Exercise Spine validation through Bob using temporary, self-contained projects."""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest
import zipfile


TESTS = Path(__file__).resolve().parent
REPO = TESTS.parents[1]
ATLAS = "/corrupted/_assets/shared/test.atlas"
MATERIAL = "/corrupted/_assets/shared/spine.material"
CONFIG = None


def fields(values):
    return "".join(f"{name}: {json.dumps(value)}\n" for name, value in values.items() if value is not None)


def properties(values):
    return "".join(
        f"custom_properties {{ id: {json.dumps(name)} type: TYPE_STRING string: {json.dumps(value)} }}\n"
        for name, value in values.items() if value is not None
    )


def gui_node(scene="scene", animation="idle", skin="", node_id="spine", template_child=False):
    return (
        'nodes { type: TYPE_CUSTOM custom_type_name: "Spine"\n'
        + fields({"id": node_id})
        + ("template_node_child: true\n" if template_child else "")
        + properties({"spine_scene": scene, "spine_default_animation": animation, "spine_skin": skin})
        + "}\n"
    )


def legacy_node(scene="scene", animation="idle", skin="alternate", bones=True, node_id="spine",
                type_fields="type: TYPE_SPINE", template_child=False, overrides=()):
    return (
        f"nodes {{ {type_fields}\n"
        + fields({"id": node_id, "spine_scene": scene, "spine_default_animation": animation,
                  "spine_skin": skin, "spine_create_bones": bones})
        + ("template_node_child: true\n" if template_child else "")
        + "".join(f"overridden_fields: {number}\n" for number in overrides)
        + "spine_node_child: true\n}\n"
    )


def boolean_property(name, value):
    return f'custom_properties {{ id: {json.dumps(name)} type: TYPE_BOOLEAN boolean: {json.dumps(value)} }}\n'


def legacy_resources(source):
    return source.replace('resources { name: "scene" path:', 'spine_scenes { name: "scene" spine_scene:')


def gui(node=None, scene="/data.spinescene"):
    return (
        'material: "/builtins/materials/gui.material"\n'
        + (gui_node() if node is None else node)
        + f'resources {{ name: "scene" path: {json.dumps(scene)} }}\n'
    )


def model(scene="/data.spinescene", animation="idle", skin="", material=MATERIAL):
    return fields({"spine_scene": scene, "default_animation": animation, "skin": skin, "material": material})


class BobValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        host = (platform.system(), platform.machine().lower())
        cls.platform, native_platform = {
            ("Darwin", "arm64"): ("arm64-macos", "arm64-osx"),
            ("Darwin", "x86_64"): ("x86_64-macos", "x86_64-osx"),
            ("Linux", "x86_64"): ("x86_64-linux", "x86_64-linux"),
            ("Linux", "aarch64"): ("arm64-linux", "arm64-linux"),
            ("Windows", "amd64"): ("x86_64-win32", "x86_64-win32"),
        }[host]
        cls.root = Path(tempfile.mkdtemp(prefix="spine-bob-validation-"))
        if not CONFIG.keep_temp:
            cls.addClassCleanup(shutil.rmtree, cls.root)
        print(f"Validation project: {cls.root}", flush=True)
        (cls.root / "game.project").write_text("[project]\ntitle = Spine validation tests\n")
        plugin_root = cls.root / "build/plugins/defold-spine/plugins"
        (plugin_root / "share").mkdir(parents=True)
        shutil.copy2(CONFIG.plugin, plugin_root / "share/pluginSpineExt.jar")
        shutil.copytree(REPO / "defold-spine/plugins/lib" / native_platform, plugin_root / "lib" / native_platform)
        shutil.copytree(REPO / "corrupted/_assets/shared", cls.root / "corrupted/_assets/shared")
        # Keep the skeleton corruption fixtures, but use a PNG Bob can decode.
        shutil.copy2(REPO / "assets/spineboy/images/crosshair.png", cls.root / "corrupted/_assets/shared/bone.png")
        for name in ("00_valid_control", "02_missing_version", "04_missing_region"):
            shutil.copytree(REPO / "corrupted/_assets/cases" / name, cls.root / "corrupted/_assets/cases" / name)
        default_material = cls.root / "defold-spine/assets/spine.material"
        default_material.parent.mkdir(parents=True)
        shutil.copy2(cls.root / MATERIAL.lstrip("/"), default_material)
        version_header = (REPO / "defold-spine/include/spine/Version.h").read_text()
        cls.version = re.search(r'SPINE_VERSION_STRING "([^"]+)"', version_header).group(1) + ".00"
        cls.write_json("/data.spinejson")
        cls.write_scene("/data.spinescene", "/data.spinejson")
        cls.classpath = os.pathsep.join(str(path.resolve()) for path in [*CONFIG.extra_classpath, CONFIG.bob])
        assertions = cls.root / "test-tools"
        assertions.mkdir()
        assertion_classpath = os.pathsep.join([cls.classpath, str(CONFIG.plugin.resolve())])
        compiled = subprocess.run(["javac", "-cp", assertion_classpath, "-d", str(assertions),
                                   *map(str, sorted(TESTS.glob("*.java")))], capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            raise RuntimeError("Could not compile the output assertions:\n" + compiled.stdout + compiled.stderr)
        cls.assertion_command = ["java", "-cp", os.pathsep.join([str(assertions), assertion_classpath]), "BobAssertions"]
        cls.editor_test_command = [*cls.assertion_command[:-1], "EditorSpineTests"]
        names = ("spine_scene", "spine_default_animation", "spine_skin", "spine_create_bones")
        hashes = subprocess.run([*cls.assertion_command, "--hashes", *names], check=True, capture_output=True, text=True)
        cls.property_hashes = dict(zip(names, hashes.stdout.splitlines()))
        cls.build_count = 0

    @classmethod
    def write_json(cls, path, animations=("idle", "walk"), skins=("default", "alternate")):
        data = {
            "skeleton": {"spine": cls.version},
            "bones": [{"name": "root"}],
            "skins": [{"name": name, "attachments": {}} for name in skins],
            "animations": {name: {} for name in animations},
        }
        (cls.root / path.lstrip("/")).write_text(json.dumps(data))

    @classmethod
    def write_scene(cls, path, data, atlas=ATLAS):
        (cls.root / path.lstrip("/")).write_text(fields({"spine_json": data, "atlas": atlas}))

    def build(self, name, extension, source, error=None, options=()):
        path = self.root / f"{name}.{extension}"
        # Preserve the file on incremental builds when the caller has not changed it.
        if not path.exists() or path.read_text() != source:
            path.write_text(source)
        return self.build_inputs(["/" + path.name], error=error, options=options)

    def build_inputs(self, inputs, error=None, options=(), expected_results=None):
        sources = {path: path.read_bytes() for path in self.root.rglob("*.gui")}
        input_args = [arg for path in inputs for arg in ("--build-input", path)]
        command = ["java", "-cp", self.classpath, "com.dynamo.bob.Bob"]
        if expected_results is not None:
            command = [*self.version_test_command(), expected_results]
        result = subprocess.run(
            [*command, "--root", str(self.root),
             "--platform", self.platform, "--build-input", "/builtins/graphics/default.texture_profiles",
             *input_args, *options, "build"],
            cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60,
        )
        type(self).build_count += 1
        log = self.root / f"bob-{self.build_count:03d}.log"
        log.write_text(result.stdout)
        for path, content in sources.items():
            self.assertEqual(content, path.read_bytes(), f"Bob rewrote GUI source: {path}")
        if error is None:
            self.assertEqual(0, result.returncode, result.stdout)
        else:
            self.assertEqual(1, result.returncode, result.stdout)
            self.assertIn(error, result.stdout)
            self.assertNotIn("NullPointerException", result.stdout)
        return result.stdout

    def version_test_command(self):
        plugin = self.root / "build/plugins/defold-spine/plugins/share/pluginSpineExt.jar"
        classpath = os.pathsep.join([str(self.root / "test-tools"), self.classpath, str(plugin)])
        return ["java", "-cp", classpath, "SpineValidationVersionTests"]

    def assert_descriptor(self, name, extension, **expected):
        args = ["--descriptor", str(self.root / f"build/default/{name}.{extension}c")]
        for key, value in expected.items():
            args.extend((key, value))
        result = subprocess.run([*self.assertion_command, *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def assert_gui(self, name, node_id="spine", layout="", **expected):
        args = [str(self.root / f"build/default/{name}.guic"), layout, node_id]
        for key, value in expected.items():
            args.extend((key, str(value).lower() if isinstance(value, bool) else value))
        result = subprocess.run([*self.assertion_command, *args], capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def editor_case(self, name, error=None):
        fixtures = subprocess.run([*self.editor_test_command, "write", name, str(self.root)],
                                  capture_output=True, text=True, timeout=30)
        self.assertEqual(0, fixtures.returncode, fixtures.stdout + fixtures.stderr)
        output = self.build_inputs(fixtures.stdout.splitlines(), error=error)
        assertions = subprocess.run([*self.editor_test_command, "check", name, str(self.root)],
                                    capture_output=True, text=True, timeout=30)
        self.assertEqual(0, assertions.returncode, assertions.stdout + assertions.stderr)
        return output

    # Port the editor's legacy template scene override and preserve aliases pointing at the same resource.
    def test_editor_template_scene_override(self):
        self.editor_case("template_scene_override")

    # Port the editor's layout-specific template scene selection without changing the default selection.
    def test_editor_template_layout_scene_override(self):
        self.editor_case("template_layout_scene_override")

    # Port the editor's runtime node-field checks, including auto size and regular fields inherited by sparse layouts.
    def test_editor_layout_node_fields(self):
        self.editor_case("layout_node_fields")

    # Port collection usage and embedded-model migration checks through a complete collection dependency graph.
    def test_editor_collection_usage(self):
        self.editor_case("collection_usage")

    # Port the editor migration error for old .json data referenced by an embedded Spine model.
    def test_editor_legacy_collection_data_extension(self):
        output = self.editor_case("legacy_collection_data_extension", "must end with '.spinejson' or '.skel'")
        self.assertIn("editor_unmigrated.spinescene", output)

    # Required scene selections fail, optional empty names pass, and unknown names fail against native metadata.
    def test_gui_validation(self):
        cases = [
            ("valid", gui(), None),
            ("empty_names", gui(gui_node(animation="", skin="")), None),
            ("omitted_names", gui(gui_node(animation=None, skin=None)), None),
            ("known_skin", gui(gui_node(skin="alternate")), None),
            ("empty_scene", gui(gui_node(scene="")), "Node 'spine': Spine scene must not be empty"),
            ("omitted_scene", gui(gui_node(scene=None)), "Node 'spine': Spine scene must not be empty"),
            ("missing_scene", gui(gui_node(scene="missing")), "missing"),
            ("wrong_scene_type", gui(scene=ATLAS), "must refer to a .spinescene file"),
            ("unknown_animation", gui(gui_node(animation="missing")), "animation 'missing' could not be found"),
            ("unknown_skin", gui(gui_node(skin="missing")), "skin 'missing' could not be found"),
            ("literal_default_skin", gui(gui_node(skin="default")), "skin 'default' could not be found"),
        ]
        for name, source, error in cases:
            with self.subTest(name=name):
                self.build("gui_" + name, "gui", source, error)
                if error is None:
                    self.assert_gui("gui_" + name)

    # Bob checks all registered types before Spine callbacks, including the boolean that Spine does not read.
    def test_gui_property_types(self):
        for name in ("spine_scene", "spine_default_animation", "spine_skin"):
            with self.subTest(property=name):
                source = gui(gui_node(scene=None if name == "spine_scene" else "scene",
                                      animation=None if name == "spine_default_animation" else "idle", skin=None))
                source = source.replace("}\nresources", boolean_property(name, True) + "}\nresources")
                self.build("wrong_type_" + name, "gui", source, f"Node 'spine': Property '{name}' must have type TYPE_STRING")
        source = gui(gui_node()).replace("}\nresources", properties({"spine_create_bones": "true"}) + "}\nresources")
        self.build("wrong_type_spine_create_bones", "gui", source,
                   "Node 'spine': Property 'spine_create_bones' must have type TYPE_BOOLEAN")

    # Empty resource names must fail independently of node callbacks, including migrated and inherited entries.
    def test_gui_resource_names(self):
        for resource_type, path_field in (("resources", "path"), ("spine_scenes", "spine_scene")):
            for location in ("unused", "with_node", "template"):
                with self.subTest(resource_type=resource_type, location=location):
                    name = f"empty_{resource_type}_{location}"
                    resource = f'{resource_type} {{ name: "" {path_field}: "/data.spinescene" }}\n'
                    source = gui(gui_node() if location == "with_node" else "") + resource
                    owner = name + ".gui"
                    if location == "template":
                        owner = name + "_template.gui"
                        (self.root / owner).write_text(source)
                        source = gui(f'nodes {{ type: TYPE_TEMPLATE id: "template" template: "/{owner}" }}\n')
                    output = self.build(name, "gui", source, "GUI resource name must not be empty")
                    self.assertIn(owner, output)
                    self.assertIn("/data.spinescene", output)
                    self.assertFalse((self.root / f"build/default/{name}.guic").exists())

    # Every node must be validated even when an earlier node populated the skeleton metadata cache.
    def test_shared_skeleton_validates_every_node(self):
        source = gui(gui_node(node_id="first") + gui_node(animation="missing", node_id="second"))
        self.build("shared_skeleton", "gui", source, "Node 'second': animation 'missing' could not be found")

    # Models use the same name checks as GUIs, and reject empty or incorrectly typed required resources.
    def test_model_validation(self):
        cases = [
            ("valid", model(), None),
            ("empty_names", model(animation="", skin=""), None),
            ("known_skin", model(skin="alternate"), None),
            ("default_material", model(material=None), None),
            ("empty_scene", model(scene=""), "spine_scene"),
            ("wrong_scene_type", model(scene=ATLAS), "must end with '.spinescene'"),
            ("empty_material", model(material=""), "material"),
            ("wrong_material_type", model(material=ATLAS), "must end with '.material'"),
            ("unknown_animation", model(animation="missing"), "animation 'missing' could not be found"),
            ("unknown_skin", model(skin="missing"), "skin 'missing' could not be found"),
            ("literal_default_skin", model(skin="default"), "skin 'default' could not be found"),
        ]
        for name, source, error in cases:
            with self.subTest(name=name):
                self.build("model_" + name, "spinemodel", source, error)
        self.assert_descriptor("model_valid", "spinemodel", spine_scene="/data.spinescenec",
                               material=MATERIAL.replace(".material", ".materialc"), default_animation="idle", skin="")

    # Missing files for every required resource must fail at the owning descriptor, without Java exceptions.
    def test_missing_resource_files(self):
        cases = [
            ("gui_scene", "gui", gui(scene="/missing.spinescene"), "/missing.spinescene"),
            ("model_scene", "spinemodel", model(scene="/missing.spinescene"), "/missing.spinescene"),
            ("model_material", "spinemodel", model(material="/missing.material"), "/missing.material"),
            ("scene_data", "spinescene", fields({"spine_json": "/missing.spinejson", "atlas": ATLAS}), "/missing.spinejson"),
            ("scene_atlas", "spinescene", fields({"spine_json": "/data.spinejson", "atlas": "/missing.atlas"}), "/missing.atlas"),
        ]
        for name, extension, source, missing in cases:
            with self.subTest(resource=name):
                self.build("missing_" + name, extension, source, missing)

    # Scene type errors are build errors, and native failures must not leave a compiled scene behind.
    def test_scene_validation(self):
        cases = [
            ("empty_data", "", ATLAS, "spine_json"),
            ("wrong_data_type", ATLAS, ATLAS, "must end with '.spinejson' or '.skel'"),
            ("empty_atlas", "/data.spinejson", "", "atlas"),
            ("wrong_atlas_type", "/data.spinejson", "/data.spinejson", "must end with '.atlas'"),
            ("invalid_binary", "/corrupted/_assets/cases/02_missing_version/case.skel", ATLAS, "Failed to load spine scene"),
            ("missing_region", "/corrupted/_assets/cases/04_missing_region/case.skel", ATLAS, "Failed to load spine scene"),
        ]
        for name, data, atlas, error in cases:
            with self.subTest(name=name):
                self.build("scene_" + name, "spinescene", fields({"spine_json": data, "atlas": atlas}), error)
                self.assertFalse((self.root / f"build/default/scene_{name}.spinescenec").exists())

    # Malformed JSON and a missing version must fail native validation before a scene output is written.
    def test_invalid_json(self):
        for name, source in (
            ("syntax", "{ broken"),
            ("version", '{"skeleton": {}, "bones": [{"name": "root"}]}'),
            ("version_mismatch", '{"skeleton": {"spine": "0.0.0"}, "bones": [{"name": "root"}]}'),
        ):
            with self.subTest(case=name):
                data = f"/invalid_{name}.spinejson"
                (self.root / data.lstrip("/")).write_text(source)
                self.build("invalid_" + name, "spinescene", fields({"spine_json": data, "atlas": ATLAS}),
                           "Failed to load spine scene")
                self.assertFalse((self.root / f"build/default/invalid_{name}.spinescenec").exists())

    # Sparse layout overrides inherit the scene and report the layout when an animation or skin is invalid.
    def test_layout_validation(self):
        for name, animation, skin, error in (
            ("valid", "walk", "alternate", None),
            ("animation", "missing", None, "animation 'missing' could not be found"),
            ("skin", None, "missing", "skin 'missing' could not be found"),
        ):
            with self.subTest(name=name):
                layout = 'layouts { name: "Landscape"\n' + gui_node(scene=None, animation=animation, skin=skin) + "}\n"
                output = self.build("layout_" + name, "gui", gui() + layout, error)
                if error:
                    self.assertIn("Node 'spine', layout 'Landscape'", output)
                else:
                    self.assert_gui("layout_" + name, layout="Landscape", spine_scene="scene",
                                    spine_default_animation="walk", spine_skin="alternate")

    # A parent may fill a template's required scene and replace an invalid default animation before validation.
    def test_template_overrides(self):
        (self.root / "template.gui").write_text(gui(gui_node(scene="", animation="missing")))
        template_node = 'nodes { type: TYPE_TEMPLATE id: "template" template: "/template.gui" }\n'
        for name, animation, error in (
            ("valid", "idle", None),
            ("invalid", "missing", "Node 'template/spine': animation 'missing' could not be found"),
        ):
            with self.subTest(name=name):
                override = gui_node(animation=animation, node_id="template/spine", template_child=True)
                self.build("template_" + name, "gui", gui(template_node + override), error)

    # Both legacy node types and custom nodes with legacy fields migrate to the same compiled representation.
    def test_legacy_gui_properties(self):
        for name, type_fields in (
            ("legacy", "type: TYPE_SPINE"),
            ("custom_hash", "type: TYPE_CUSTOM custom_type: 405028931"),
            ("custom_name", 'type: TYPE_CUSTOM custom_type_name: "Spine"'),
        ):
            with self.subTest(name=name):
                self.build(name, "gui", gui(legacy_node(type_fields=type_fields)))
                self.assert_gui(name, spine_scene="scene", spine_default_animation="idle", spine_skin="alternate",
                                spine_create_bones=True, spine_node_child="<absent>")

    # Explicit modern properties, including hashed IDs and empty/false values, take precedence over legacy fields.
    def test_modern_properties_override_legacy(self):
        for hashed in (False, True):
            with self.subTest(hashed=hashed):
                name = "mixed_hashed" if hashed else "mixed_named"
                modern = properties({"spine_scene": "scene", "spine_default_animation": "idle", "spine_skin": ""})
                modern += boolean_property("spine_create_bones", False)
                if hashed:
                    for prop, hash_value in self.property_hashes.items():
                        modern = modern.replace(f'id: "{prop}"', "id_hash: " + hash_value)
                node = legacy_node(scene="missing", animation="missing", skin="missing")[:-2] + modern + "}\n"
                self.build(name, "gui", gui(node))
                self.assert_gui(name, spine_scene="scene", spine_default_animation="idle", spine_skin="", spine_create_bones=False)

    # Legacy resource lists become ordinary dependencies, including lists with no Spine nodes and inherited lists.
    def test_legacy_resource_lists(self):
        self.build("legacy_resources", "gui", legacy_resources(gui(legacy_node())))
        self.assert_gui("legacy_resources", **{"@resource:scene": "/data.spinescenec"})
        self.build("legacy_unused_resources", "gui", legacy_resources(gui("")))
        self.assert_gui("legacy_unused_resources", node_id="", **{"@resource:scene": "/data.spinescenec"})

        (self.root / "resource-template.gui").write_text(legacy_resources(gui(legacy_node())))
        template = 'nodes { type: TYPE_TEMPLATE id: "template" template: "/resource-template.gui" }\n'
        source = 'material: "/builtins/materials/gui.material"\n' + template
        self.build("legacy_inherited_resource", "gui", source)
        self.assert_gui("legacy_inherited_resource", node_id="template/spine", **{"@resource:scene": "/data.spinescenec"})

        self.write_json("/override.spinejson", animations=("run",))
        self.write_scene("/override.spinescene", "/override.spinejson")
        override = gui_node(scene=None, animation="run", skin=None, node_id="template/spine", template_child=True)
        self.build("legacy_resource_override", "gui", gui(template + override, "/override.spinescene"))
        self.assert_gui("legacy_resource_override", node_id="template/spine", spine_default_animation="run",
                        **{"@resource:scene": "/override.spinescenec"})

        duplicate = legacy_resources(gui(legacy_node())) + 'resources { name: "scene" path: "/data.spinescene" }\n'
        self.build("legacy_duplicate_resource", "gui", duplicate, "duplicated")

    # Sparse legacy and modern overrides work across mixed nested templates without replacing inherited defaults.
    def test_mixed_template_migration(self):
        for old_base in (False, True):
            for old_override in (False, True):
                with self.subTest(old_base=old_base, old_override=old_override):
                    name = f"mixed_template_{old_base}_{old_override}"
                    base = legacy_node() if old_base else (
                        gui_node(skin="alternate")[:-2] + boolean_property("spine_create_bones", True) + "}\n")
                    (self.root / "migration-template.gui").write_text(gui(base))
                    inner = 'nodes { type: TYPE_TEMPLATE id: "inner" template: "/migration-template.gui" }\n'
                    (self.root / "migration-middle.gui").write_text(gui(inner))
                    outer = 'nodes { type: TYPE_TEMPLATE id: "outer" template: "/migration-middle.gui" }\n'
                    node_id = "outer/inner/spine"
                    # Unmarked legacy defaults must not erase the inherited skin or create-bones value.
                    override = (legacy_node(scene=None, animation="walk", skin="", bones=False, node_id=node_id,
                                            type_fields="type: TYPE_CUSTOM", template_child=True, overrides=(40,))
                                if old_override else gui_node(scene=None, animation="walk", skin=None,
                                                              node_id=node_id, template_child=True))
                    # Markers alone explicitly reset skin and create-bones to their protobuf defaults.
                    layout = 'layouts { name: "Landscape"\n' + legacy_node(
                        scene=None, animation=None, skin=None, bones=None, node_id=node_id,
                        type_fields="type: TYPE_CUSTOM", template_child=True, overrides=(41, 48)) + "}\n"
                    self.build(name, "gui", gui(outer + override) + layout)
                    self.assert_gui(name, node_id=node_id, spine_scene="scene", spine_default_animation="walk",
                                    spine_skin="alternate", spine_create_bones=True)
                    self.assert_gui(name, node_id=node_id, layout="Landscape", spine_scene="scene",
                                    spine_default_animation="walk", spine_skin="", spine_create_bones=False)

    # Direct layout overrides preserve the scene and can explicitly select empty strings and false.
    def test_legacy_layout_defaults(self):
        for omit_values in (False, True):
            with self.subTest(omit_values=omit_values):
                name = "layout_legacy_omitted" if omit_values else "layout_legacy_explicit"
                layout = 'layouts { name: "Landscape"\n' + legacy_node(
                    scene=None, animation=None, skin=None if omit_values else "", bones=None if omit_values else False,
                    type_fields="type: TYPE_SPINE", overrides=(41, 48)) + "}\n"
                self.build(name, "gui", gui(legacy_node()) + layout)
                self.assert_gui(name, layout="Landscape", spine_scene="scene", spine_default_animation="idle",
                                spine_skin="", spine_create_bones=False)

    # Preserve the removed Bob regression: a legacy animation field overrides only the selected layout.
    def test_legacy_layout_animation_overrides(self):
        for name, type_fields in (("legacy", "type: TYPE_SPINE"), ("custom", "type: TYPE_CUSTOM custom_type: 405028931")):
            with self.subTest(type=name):
                layout = 'layouts { name: "Landscape"\n' + legacy_node(
                    scene=None, animation="walk", skin=None, bones=None,
                    type_fields=type_fields, overrides=(40,)) + "}\n"
                self.build("legacy_animation_" + name, "gui", gui(legacy_node()) + layout)
                self.assert_gui("legacy_animation_" + name, spine_default_animation="idle")
                self.assert_gui("legacy_animation_" + name, layout="Landscape", spine_scene="scene",
                                spine_default_animation="walk", spine_skin="alternate", spine_create_bones=True)

    # Reusing a migrated template must not leak the first instance's overrides into the second instance.
    def test_repeated_template_instances(self):
        (self.root / "repeated-template.gui").write_text(legacy_resources(gui(legacy_node())))
        nodes = "".join(f'nodes {{ type: TYPE_TEMPLATE id: "{name}" template: "/repeated-template.gui" }}\n'
                        for name in ("first", "second"))
        nodes += legacy_node(scene=None, animation="walk", skin=None, bones=None, node_id="first/spine",
                             type_fields="type: TYPE_CUSTOM", template_child=True, overrides=(40,))
        layout = 'layouts { name: "Landscape"\n' + legacy_node(
            scene=None, animation=None, skin=None, bones=None, node_id="first/spine",
            type_fields="type: TYPE_CUSTOM", template_child=True, overrides=(40, 41, 48)) + "}\n"
        self.build("repeated", "gui", gui(nodes) + layout)
        self.assert_gui("repeated", node_id="first/spine", spine_default_animation="walk", spine_skin="alternate", spine_create_bones=True)
        self.assert_gui("repeated", node_id="second/spine", spine_default_animation="idle", spine_skin="alternate", spine_create_bones=True)
        self.assert_gui("repeated", node_id="first/spine", layout="Landscape", spine_default_animation="", spine_skin="", spine_create_bones=False)
        self.assert_gui("repeated", node_id="second/spine", layout="Landscape", spine_default_animation="idle", spine_skin="alternate", spine_create_bones=True)

    # Legacy scene overrides must select the parent's resource; empty animation/skin and false bones remain explicit.
    def test_template_scene_override_and_resets(self):
        (self.root / "scene-template.gui").write_text(gui(legacy_node()))
        self.write_json("/other.spinejson", animations=("run",), skins=("default",))
        self.write_scene("/other.spinescene", "/other.spinejson")
        template = 'nodes { type: TYPE_TEMPLATE id: "template" template: "/scene-template.gui" }\n'
        override = legacy_node(scene="other", animation="", skin="", bones=False, node_id="template/spine",
                               type_fields="type: TYPE_CUSTOM", template_child=True, overrides=(39, 40, 41, 48))
        source = gui(template + override) + 'resources { name: "other" path: "/other.spinescene" }\n'
        self.build("scene_override", "gui", source)
        self.assert_gui("scene_override", node_id="template/spine", spine_scene="other", spine_default_animation="",
                        spine_skin="", spine_create_bones=False, **{"@resource:other": "/other.spinescenec"})

    # Parent-supplied scene names and resources are checked after inheritance, even for an invalid template default.
    def test_parent_overrides_missing_resource(self):
        template = 'nodes { type: TYPE_TEMPLATE id: "template" template: "/missing-resource-template.gui" }\n'
        for name, selected in (("overridden", "missing"), ("parent_supplied", "scene")):
            with self.subTest(name=name):
                (self.root / "missing-resource-template.gui").write_text(
                    'material: "/builtins/materials/gui.material"\n' + legacy_node(scene=selected))
                override = gui_node(animation=None, skin=None, node_id="template/spine", template_child=True)
                self.build("resource_" + name, "gui", gui(template + override))
                self.assert_gui("resource_" + name, node_id="template/spine", spine_scene="scene")

    # A missing plugin must reject legacy types and named custom types instead of writing unusable GUI output.
    def test_missing_plugin(self):
        plugin = self.root / "build/plugins/defold-spine/plugins/share/pluginSpineExt.jar"
        backup = self.root / "disabled-plugin.jar"
        plugin.rename(backup)
        try:
            material = 'material: "/builtins/materials/gui.material"\n'
            self.build("no_plugin_legacy", "gui", material + 'nodes { type: TYPE_SPINE id: "spine" }\n',
                       "unsupported GUI node type 'TYPE_SPINE'")
            self.build("no_plugin_custom", "gui", material + gui_node(), "Unknown GUI custom node type 'Spine'")
            self.build("no_plugin_hash", "gui", material + gui_node().replace('custom_type_name: "Spine"', "custom_type: 405028931"),
                       "Unknown GUI custom node type hash '405028931'")
        finally:
            backup.rename(plugin)

    # Binary skeleton metadata is validated through the same native loader as JSON metadata.
    def test_binary_metadata(self):
        scene = "/corrupted/_assets/cases/00_valid_control/case.spinescene"
        self.build("binary_valid", "gui", gui(scene=scene))
        self.assert_gui("binary_valid", spine_default_animation="idle")
        self.build("binary_valid_model", "spinemodel", model(scene=scene))
        self.build("binary_invalid_gui", "gui", gui(gui_node(animation="missing"), scene), "animation 'missing' could not be found")
        self.build("binary_invalid_skin", "gui", gui(gui_node(skin="missing"), scene), "skin 'missing' could not be found")
        self.build("binary_invalid_model", "spinemodel", model(scene=scene, skin="missing"), "skin 'missing' could not be found")

    # Mixed-case suffixes must retain Bob's resource support and select the native binary parser correctly.
    def test_case_insensitive_extensions(self):
        atlas = "/corrupted/_assets/shared/mixed.AtLaS"
        material = "/corrupted/_assets/shared/mixed.MATERIAL"
        shutil.copy2(self.root / ATLAS.lstrip("/"), self.root / atlas.lstrip("/"))
        shutil.copy2(self.root / MATERIAL.lstrip("/"), self.root / material.lstrip("/"))
        for name, source_data, extension in (
            ("json", "/data.spinejson", "SPINEJSON"),
            ("binary", "/corrupted/_assets/cases/00_valid_control/case.skel", "SkEl"),
        ):
            with self.subTest(data=name):
                data = f"/mixed_{name}.{extension}"
                scene = f"/mixed_{name}.SPINESCENE"
                shutil.copy2(self.root / source_data.lstrip("/"), self.root / data.lstrip("/"))
                self.write_scene(scene, data, atlas)
                self.build("mixed_" + name + "_gui", "gui", gui(scene=scene))
                self.assert_gui("mixed_" + name + "_gui", **{"@resource:scene": f"/mixed_{name}.spinescenec"})
                self.build("mixed_" + name + "_model", "spinemodel", model(scene=scene, material=material))
                self.assert_descriptor("mixed_" + name + "_model", "spinemodel", spine_scene=f"/mixed_{name}.spinescenec",
                                       material="/corrupted/_assets/shared/mixed.materialc")
                self.assert_descriptor("mixed_" + name, "spinescene", spine_json=f"/mixed_{name}.{'skelc' if name == 'binary' else 'spinejsonc'}",
                                       atlas="/corrupted/_assets/shared/mixed.a.texturesetc")

    # Parallel GUI tasks can reuse resource names while resolving different skeletons and retaining their own plugin state.
    def test_multiple_gui_tasks(self):
        inputs = []
        for index in range(8):
            name = f"parallel_{index}"
            self.write_json(f"/{name}.spinejson", animations=(name,))
            self.write_scene(f"/{name}.spinescene", f"/{name}.spinejson")
            (self.root / f"{name}.gui").write_text(gui(gui_node(animation=name), f"/{name}.spinescene"))
            inputs.append(f"/{name}.gui")
        self.build_inputs(inputs, options=("--max-cpu-threads", "4"))
        for index in range(8):
            name = f"parallel_{index}"
            self.assert_gui(name, spine_default_animation=name, **{"@resource:scene": f"/{name}.spinescenec"})

    # Changing only skeleton data must revalidate both consumers even when the compiled scene is unchanged.
    def test_dependency_changes_revalidate_consumers(self):
        for kind in ("gui", "model"):
            for removed in ("animation", "skin"):
                with self.subTest(kind=kind, removed=removed):
                    name = f"changed_{kind}_{removed}"
                    data, scene = f"/{name}.spinejson", f"/{name}.spinescene"
                    self.write_json(data)
                    self.write_scene(scene, data)
                    extension = "gui" if kind == "gui" else "spinemodel"
                    source = gui(gui_node(skin="alternate"), scene) if kind == "gui" else model(scene=scene, skin="alternate")
                    self.build(name, extension, source)
                    compiled_scene = self.root / f"build/default/{name}.spinescenec"
                    before = compiled_scene.read_bytes()
                    if removed == "animation":
                        self.write_json(data, animations=("walk",))
                    else:
                        self.write_json(data, skins=("default",))
                    self.build(name, extension, source, f"{removed} '")
                    self.assertEqual(before, compiled_scene.read_bytes())

    # Changing a template source invalidates an unchanged parent, and fixing the template recovers without a clean build.
    def test_template_changes_revalidate_parent(self):
        template = self.root / "changing-template.gui"
        source = gui('nodes { type: TYPE_TEMPLATE id: "template" template: "/changing-template.gui" }\n')
        template.write_text(gui(legacy_node()))
        self.build("changing_parent", "gui", source)
        template.write_text(gui(legacy_node(animation="missing")))
        self.build("changing_parent", "gui", source, "Node 'template/spine': animation 'missing' could not be found")
        template.write_text(gui(legacy_node(animation="walk")))
        self.build("changing_parent", "gui", source)
        self.assert_gui("changing_parent", node_id="template/spine", spine_default_animation="walk")

    # Retargeting a scene descriptor must revalidate its consumers and track the newly selected data file.
    def test_scene_changes_revalidate_consumers(self):
        for kind in ("gui", "model"):
            with self.subTest(kind=kind):
                name = "retarget_" + kind
                scene, data = f"/{name}.spinescene", f"/{name}.spinejson"
                self.write_scene(scene, "/data.spinejson")
                self.write_json(data, animations=("walk",))
                extension = "gui" if kind == "gui" else "spinemodel"
                source = gui(scene=scene) if kind == "gui" else model(scene=scene)
                self.build(name, extension, source)
                self.write_scene(scene, data)
                self.build(name, extension, source, "animation 'idle' could not be found")
                source = gui(gui_node(animation="walk"), scene) if kind == "gui" else model(scene=scene, animation="walk")
                self.build(name, extension, source)
                self.write_json(data, animations=("run",))
                self.build(name, extension, source, "animation 'walk' could not be found")

    # Restore an atlas dependency from the local cache, then revalidate consumers when their skeleton changes.
    def test_local_resource_cache(self):
        for kind in ("gui", "model"):
            with self.subTest(kind=kind):
                name = "cached_" + kind
                data, scene = f"/{name}.spinejson", f"/{name}.spinescene"
                self.write_json(data)
                # GUI/model tasks use incremental signatures; atlas tasks also use the resource cache.
                # Give each case a fresh atlas so an earlier test cannot have already built it without caching.
                atlas = f"/{name}.atlas"
                shutil.copy2(self.root / ATLAS.lstrip("/"), self.root / atlas.lstrip("/"))
                self.write_scene(scene, data, atlas)
                extension = "gui" if kind == "gui" else "spinemodel"
                source = gui(scene=scene) if kind == "gui" else model(scene=scene)
                cache = self.root / ("cache_" + kind)
                options = ("--resource-cache-local", str(cache), "--verbose")
                self.build(name, extension, source, options=options)
                compiled = self.root / f"build/default/{name}.{extension}c"
                expected = compiled.read_bytes()
                self.assertTrue(any(path.is_file() for path in cache.rglob("*")))
                compiled_atlas = self.root / f"build/default/{name}.a.texturesetc"
                expected_atlas = compiled_atlas.read_bytes()
                compiled_atlas.unlink()
                output = self.build(name, extension, source, options=options)
                self.assertIn("loaded from the local cache", output)
                self.assertEqual(expected_atlas, compiled_atlas.read_bytes())
                self.assertEqual(expected, compiled.read_bytes())
                self.write_json(data, animations=("walk",))
                self.build(name, extension, source, "animation 'idle' could not be found", options=options)

    # Source resource lookup and native atlas validation must also work with minified output paths.
    def test_minified_resources(self):
        self.build("minified_gui", "gui", gui(), options=("--experimental-path-minification",))
        self.build("minified_model", "spinemodel", model(), options=("--experimental-path-minification",))
        self.build("minified_legacy_gui", "gui", legacy_resources(gui(legacy_node())), options=("--experimental-path-minification",))

    # A validation version change must revalidate scenes as well as their consumers with identical source inputs.
    def test_validation_version_invalidates_all_tasks(self):
        self.write_scene("/validation_version.spinescene", "/data.spinejson")
        (self.root / "validation_version.gui").write_text(gui(scene="/validation_version.spinescene"))
        (self.root / "validation_version.spinemodel").write_text(model(scene="/validation_version.spinescene"))
        inputs = ["/validation_version.gui", "/validation_version.spinemodel"]
        plugin = self.root / "build/plugins/defold-spine/plugins/share/pluginSpineExt.jar"
        original = plugin.read_bytes()
        key = subprocess.run([*self.version_test_command(), "--cache-key"], check=True,
                             capture_output=True, text=True, timeout=30).stdout.strip().encode()
        replacement = b"X" * len(key)
        self.assertNotEqual(key, replacement)
        try:
            for changed in (False, True):
                if changed:
                    # Java inlines String constants into consumers. Replace the same-length UTF-8
                    # constant in every class, preserving all class-file lengths and resource bytes.
                    with zipfile.ZipFile(plugin) as jar:
                        entries = [(info, jar.read(info)) for info in jar.infolist()]
                    with zipfile.ZipFile(plugin, "w") as jar:
                        for info, data in entries:
                            jar.writestr(info, data.replace(key, replacement) if info.filename.endswith(".class") else data)
                self.build_inputs(inputs, expected_results="SUCCESS")
                self.build_inputs(inputs, expected_results="SKIPPED")
        finally:
            plugin.write_bytes(original)

    # Shared native metadata is deduplicated across concurrent tasks, scoped to a build and invalidated by content.
    def test_metadata_cache(self):
        self.build("metadata_cache", "spinescene", fields({"spine_json": "/data.spinejson", "atlas": ATLAS}))
        result = subprocess.run([*self.assertion_command[:-1], "SpineMetadataCacheTests", str(self.root)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("one skeleton load", result.stdout)

    @classmethod
    def tearDownClass(cls):
        print(f"Verified {cls.build_count} Bob build invocations", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bob", type=Path, required=True, help="Bob JAR with GUI custom-node lifecycle support")
    parser.add_argument("--plugin", type=Path, default=REPO / "defold-spine/plugins/share/pluginSpineExt.jar")
    parser.add_argument("--extra-classpath", type=Path, action="append", default=[], help="Optional local Bob class overlay")
    parser.add_argument("--keep-temp", action="store_true", help="Keep the temporary project, compiled outputs and Bob logs")
    CONFIG, remaining = parser.parse_known_args()
    for path in (CONFIG.bob, CONFIG.plugin):
        if not path.is_file():
            parser.error(f"JAR does not exist: {path}")
    if not CONFIG.extra_classpath:
        with zipfile.ZipFile(CONFIG.bob) as bob:
            if "com/dynamo/bob/pipeline/GuiBuildContext.class" not in bob.namelist():
                parser.error("The selected Bob predates the GUI callbacks. Rebuild Bob from the matching Defold branch and set BOB to that JAR.")
    unittest.main(argv=[__file__, *remaining], verbosity=2)
