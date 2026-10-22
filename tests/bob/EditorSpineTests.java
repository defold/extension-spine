// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import com.dynamo.bob.util.MurmurHash;
import com.dynamo.gameobject.proto.GameObject.CollectionDesc;
import com.dynamo.gameobject.proto.GameObject.ComponentDesc;
import com.dynamo.gameobject.proto.GameObject.EmbeddedComponentDesc;
import com.dynamo.gameobject.proto.GameObject.EmbeddedInstanceDesc;
import com.dynamo.gameobject.proto.GameObject.PrototypeDesc;
import com.dynamo.gamesys.proto.Gui.NodeDesc;
import com.dynamo.gamesys.proto.Gui.Property;
import com.dynamo.gamesys.proto.Gui.SceneDesc;
import com.dynamo.gamesys.proto.Gui.SceneDesc.LayoutDesc;
import com.dynamo.gamesys.proto.Gui.SceneDesc.ResourceDesc;
import com.dynamo.spine.proto.Spine.SpineModelDesc;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;
import com.google.protobuf.Message;
import com.google.protobuf.TextFormat;

/** Build regressions ported from editor/test/integration/extension_spine_test.clj.
 * The Python runner invokes write, builds the printed input through Bob, then invokes check.
 */
public final class EditorSpineTests {
    private static final String MATERIAL = "/corrupted/_assets/shared/spine.material";

    private static void equal(Object expected, Object actual) {
        BobAssertions.equal(expected, actual);
    }

    private static Property.Builder stringProperty(String name, String value) {
        return Property.newBuilder().setId(name).setType(Property.PropertyType.TYPE_STRING).setString(value);
    }

    private static NodeDesc.Builder spineNode(String id) {
        NodeDesc.Builder node = NodeDesc.newBuilder().setId(id).setType(NodeDesc.Type.TYPE_CUSTOM)
                .setCustomTypeName("Spine").setInheritAlpha(true)
                .addCustomProperties(stringProperty("spine_scene", "spineboy"))
                .addCustomProperties(stringProperty("spine_default_animation", "idle"));
        node.getPositionBuilder().setX(200);
        return node;
    }

    private static SceneDesc.Builder scene() {
        return SceneDesc.newBuilder().setMaterial("/builtins/materials/gui.material")
                .setAdjustReference(SceneDesc.AdjustReference.ADJUST_REFERENCE_PARENT);
    }

    private static ResourceDesc.Builder resource(String name) {
        return ResourceDesc.newBuilder().setName(name).setPath("/data.spinescene");
    }

    private static void write(Path root, String name, Message message) throws Exception {
        Files.writeString(root.resolve(name), TextFormat.printer().printToString(message));
    }

    private static NodeDesc.Builder templateChild() {
        return NodeDesc.newBuilder().setId("template/spine").setParent("template")
                .setType(NodeDesc.Type.TYPE_CUSTOM).setCustomType(MurmurHash.hash32("Spine"))
                .setTemplateNodeChild(true);
    }

    private static SceneDesc.Builder templateScene(Path root) throws Exception {
        write(root, "editor_template.gui", scene().addResources(resource("spineboy"))
                .addNodes(spineNode("spine").setSizeMode(NodeDesc.SizeMode.SIZE_MODE_AUTO)).build());
        return scene().addResources(resource("flag"))
                .addNodes(NodeDesc.newBuilder().setType(NodeDesc.Type.TYPE_TEMPLATE).setId("template")
                        .setTemplate("/editor_template.gui"));
    }

    private static String writeFixtures(Path root, String test) throws Exception {
        String input = "editor_" + test;
        switch (test) {
            case "template_scene_override" -> {
                write(root, input + ".gui", templateScene(root).addNodes(templateChild()
                        .setSpineScene("flag").addOverriddenFields(NodeDesc.SPINE_SCENE_FIELD_NUMBER)).build());
            }
            case "template_layout_scene_override" -> {
                write(root, input + ".gui", templateScene(root).addNodes(templateChild())
                        .addLayouts(LayoutDesc.newBuilder().setName("Portrait").addNodes(templateChild()
                                .addCustomProperties(stringProperty("spine_scene", "flag"))))
                        .addLayouts(LayoutDesc.newBuilder().setName("LegacyPortrait").addNodes(templateChild()
                                .setSpineScene("flag").addOverriddenFields(NodeDesc.SPINE_SCENE_FIELD_NUMBER))).build());
            }
            case "layout_node_fields" -> {
                NodeDesc.Builder legacy = NodeDesc.newBuilder().setId("legacy").setType(NodeDesc.Type.TYPE_SPINE)
                        .setSpineScene("spineboy").setSpineDefaultAnimation("idle").setInheritAlpha(true);
                legacy.getPositionBuilder().setX(300);
                write(root, input + ".gui", scene().addResources(resource("spineboy"))
                        .addNodes(spineNode("spine"))
                        .addNodes(legacy)
                        .addNodes(spineNode("manual").setSizeMode(NodeDesc.SizeMode.SIZE_MODE_MANUAL))
                        .addNodes(NodeDesc.newBuilder().setId("box").setType(NodeDesc.Type.TYPE_BOX)
                                .setSizeMode(NodeDesc.SizeMode.SIZE_MODE_MANUAL))
                        .addLayouts(LayoutDesc.newBuilder().setName("Empty"))
                        .addLayouts(LayoutDesc.newBuilder().setName("Portrait")
                                .addNodes(NodeDesc.newBuilder().setId("spine").setType(NodeDesc.Type.TYPE_CUSTOM)
                                        .setCustomTypeName("Spine").addCustomProperties(stringProperty("spine_default_animation", "walk")))
                                .addNodes(NodeDesc.newBuilder().setId("legacy").setType(NodeDesc.Type.TYPE_SPINE)
                                        .setSpineDefaultAnimation("walk").addOverriddenFields(NodeDesc.SPINE_DEFAULT_ANIMATION_FIELD_NUMBER)))
                        .build());
            }
            case "collection_usage", "legacy_collection_data_extension" -> {
                String scenePath = "/data.spinescene";
                if (test.equals("legacy_collection_data_extension")) {
                    Files.copy(root.resolve("data.spinejson"), root.resolve("editor_unmigrated.json"));
                    scenePath = "/editor_unmigrated.spinescene";
                    write(root, "editor_unmigrated.spinescene", SpineSceneDesc.newBuilder()
                            .setSpineJson("/editor_unmigrated.json").setAtlas("/corrupted/_assets/shared/test.atlas").build());
                }
                SpineModelDesc model = SpineModelDesc.newBuilder().setSpineScene(scenePath)
                        .setMaterial(MATERIAL).setSkin("").setDefaultAnimation("idle").build();
                write(root, "editor_referenced.spinemodel", model);
                write(root, "editor_collection.gui", scene().addResources(resource("spineboy"))
                        .addNodes(spineNode("spine").setSizeMode(NodeDesc.SizeMode.SIZE_MODE_AUTO)).build());
                PrototypeDesc prototype = PrototypeDesc.newBuilder()
                        .addComponents(ComponentDesc.newBuilder().setId("referenced").setComponent("/editor_referenced.spinemodel"))
                        .addComponents(ComponentDesc.newBuilder().setId("gui").setComponent("/editor_collection.gui"))
                        .addEmbeddedComponents(EmbeddedComponentDesc.newBuilder().setId("embedded").setType("spinemodel")
                                .setData(TextFormat.printer().printToString(model.toBuilder().setDefaultAnimation("walk").build())))
                        .build();
                write(root, input + ".collection", CollectionDesc.newBuilder().setName("editor")
                        .addEmbeddedInstances(EmbeddedInstanceDesc.newBuilder().setId("actor")
                                .setData(TextFormat.printer().printToString(prototype))).build());
                return "/" + input + ".collection";
            }
            default -> throw new IllegalArgumentException("Unknown editor regression: " + test);
        }
        return "/" + input + ".gui";
    }

    private static NodeDesc node(SceneDesc scene, String layout, String id) {
        List<NodeDesc> nodes = layout.isEmpty() ? scene.getNodesList() : scene.getLayoutsList().stream()
                .filter(value -> value.getName().equals(layout)).findFirst().orElseThrow().getNodesList();
        return nodes.stream().filter(value -> value.getId().equals(id)).findFirst().orElseThrow();
    }

    private static String property(NodeDesc node, String name) {
        return node.getCustomPropertiesList().stream().filter(value -> value.getIdHash() == MurmurHash.hash64(name))
                .findFirst().orElseThrow().getString();
    }

    private static SceneDesc readGui(Path root, String name) throws Exception {
        SceneDesc scene = SceneDesc.parseFrom(Files.readAllBytes(root.resolve("build/default/" + name + ".guic")));
        equal(0, scene.getSpineScenesCount());
        BobAssertions.checkNodes(scene.getNodesList());
        for (LayoutDesc layout : scene.getLayoutsList()) {
            BobAssertions.checkNodes(layout.getNodesList());
        }
        return scene;
    }

    private static void resourceAliases(SceneDesc scene) {
        equal(Map.of("spineboy", "/data.spinescenec", "flag", "/data.spinescenec"),
                scene.getResourcesList().stream().collect(Collectors.toMap(ResourceDesc::getName, ResourceDesc::getPath)));
    }

    // Port legacy-template-child-spine-scene-override-builds-test: both aliases survive when an inherited selection changes.
    private static void checkTemplateSceneOverride(SceneDesc scene) {
        resourceAliases(scene);
        equal("flag", property(node(scene, "", "template/spine"), "spine_scene"));
    }

    // Port legacy-template-child-spine-scene-layout-override-builds-test for both saved modern and legacy layout overrides.
    private static void checkTemplateLayoutSceneOverride(SceneDesc scene) {
        resourceAliases(scene);
        equal("spineboy", property(node(scene, "", "template/spine"), "spine_scene"));
        for (String layout : List.of("Portrait", "LegacyPortrait")) {
            NodeDesc node = node(scene, layout, "template/spine");
            equal("flag", property(node, "spine_scene"));
            equal("idle", property(node, "spine_default_animation"));
            equal(200.0f, node.getPosition().getX());
            equal(true, node.getInheritAlpha());
        }
    }

    // Port layout-node-desc-includes-size-mode-test: effective custom values and regular fields must coexist in runtime layouts.
    private static void checkLayoutNodeFields(SceneDesc scene) {
        for (String id : List.of("spine", "legacy", "manual")) {
            NodeDesc node = node(scene, "", id);
            equal(NodeDesc.SizeMode.SIZE_MODE_AUTO, node.getSizeMode());
            equal("idle", property(node, "spine_default_animation"));
        }
        equal(NodeDesc.SizeMode.SIZE_MODE_MANUAL, node(scene, "", "box").getSizeMode());
        for (String id : List.of("spine", "legacy")) {
            NodeDesc node = node(scene, "Portrait", id);
            equal(NodeDesc.SizeMode.SIZE_MODE_AUTO, node.getSizeMode());
            equal(id.equals("spine") ? 200.0f : 300.0f, node.getPosition().getX());
            equal(true, node.getInheritAlpha());
            equal("walk", property(node, "spine_default_animation"));
            equal("spineboy", property(node, "spine_scene"));
            equal("", property(node, "spine_skin"));
        }
        // Bob may materialize unchanged nodes in empty layouts; compare their effective values.
        LayoutDesc empty = scene.getLayoutsList().stream().filter(value -> value.getName().equals("Empty")).findFirst().orElseThrow();
        for (NodeDesc node : empty.getNodesList()) {
            equal(node(scene, "", node.getId()), node);
        }
    }

    private static Path compiledResource(Path root, String path) {
        return root.resolve("build/default").resolve(path.startsWith("/") ? path.substring(1) : path);
    }

    // Port collection-usage-test and the embedded-model part of legacy-spine-project-user-migration-test through real Bob outputs.
    private static void checkCollectionUsage(Path root) throws Exception {
        CollectionDesc collection = CollectionDesc.parseFrom(Files.readAllBytes(compiledResource(root, "/editor_collection_usage.collectionc")));
        equal(0, collection.getEmbeddedInstancesCount());
        equal(1, collection.getInstancesCount());
        PrototypeDesc prototype = PrototypeDesc.parseFrom(Files.readAllBytes(compiledResource(root, collection.getInstances(0).getPrototype())));
        equal(0, prototype.getEmbeddedComponentsCount());
        equal(3, prototype.getComponentsCount());
        for (ComponentDesc component : prototype.getComponentsList()) {
            if (component.getId().equals("gui")) {
                equal("/editor_collection.guic", component.getComponent());
                continue;
            }
            equal(true, component.getComponent().endsWith(".spinemodelc"));
            SpineModelDesc model = SpineModelDesc.parseFrom(Files.readAllBytes(compiledResource(root, component.getComponent())));
            equal("/data.spinescenec", model.getSpineScene());
            equal(MATERIAL + "c", model.getMaterial());
            equal(component.getId().equals("embedded") ? "walk" : "idle", model.getDefaultAnimation());
            equal("", model.getSkin());
        }
        equal("spineboy", property(node(readGui(root, "editor_collection"), "", "spine"), "spine_scene"));
    }

    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[2]);
        String test = args[1];
        if (args[0].equals("write")) {
            System.out.println(writeFixtures(root, test));
            return;
        }
        switch (test) {
            case "template_scene_override" -> checkTemplateSceneOverride(readGui(root, "editor_" + test));
            case "template_layout_scene_override" -> checkTemplateLayoutSceneOverride(readGui(root, "editor_" + test));
            case "layout_node_fields" -> checkLayoutNodeFields(readGui(root, "editor_" + test));
            case "collection_usage" -> checkCollectionUsage(root);
            case "legacy_collection_data_extension" -> equal(false,
                    Files.exists(compiledResource(root, "/editor_legacy_collection_data_extension.collectionc")));
            default -> throw new IllegalArgumentException("Unknown editor regression: " + test);
        }
    }
}
