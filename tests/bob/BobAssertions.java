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
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;

import com.dynamo.bob.util.MurmurHash;
import com.dynamo.gamesys.proto.Gui.NodeDesc;
import com.dynamo.gamesys.proto.Gui.Property;
import com.dynamo.gamesys.proto.Gui.SceneDesc;
import com.dynamo.spine.proto.Spine.SpineModelDesc;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;
import com.google.protobuf.Message;

/** Assertions for the compiled output produced by test_bob_validation.py. */
public class BobAssertions {
    private static final Map<Long, Property.PropertyType> SPINE_PROPERTIES = Map.of(
            MurmurHash.hash64("spine_scene"), Property.PropertyType.TYPE_STRING,
            MurmurHash.hash64("spine_default_animation"), Property.PropertyType.TYPE_STRING,
            MurmurHash.hash64("spine_skin"), Property.PropertyType.TYPE_STRING,
            MurmurHash.hash64("spine_create_bones"), Property.PropertyType.TYPE_BOOLEAN);

    static void equal(Object expected, Object actual) {
        if (!Objects.equals(expected, actual)) {
            throw new AssertionError("Expected " + expected + ", got " + actual);
        }
    }

    static void checkNodes(List<NodeDesc> nodes) {
        for (NodeDesc node : nodes) {
            equal(false, node.hasSpineScene() || node.hasSpineDefaultAnimation() || node.hasSpineSkin()
                    || node.hasSpineNodeChild() || node.hasSpineCreateBones());
            equal(false, node.hasCustomTypeName());
            for (int field : node.getOverriddenFieldsList()) {
                equal(false, field == NodeDesc.SPINE_SCENE_FIELD_NUMBER || field == NodeDesc.SPINE_DEFAULT_ANIMATION_FIELD_NUMBER
                        || field == NodeDesc.SPINE_SKIN_FIELD_NUMBER || field == NodeDesc.SPINE_CREATE_BONES_FIELD_NUMBER
                        || field == NodeDesc.SPINE_NODE_CHILD_FIELD_NUMBER);
            }
            for (Property property : node.getCustomPropertiesList()) {
                equal(false, property.hasId());
                equal(true, property.hasIdHash());
            }
            if (node.getType() == NodeDesc.Type.TYPE_CUSTOM && node.getCustomType() == MurmurHash.hash32("Spine")) {
                equal(4, node.getCustomPropertiesCount());
                var seen = new HashSet<Long>();
                for (Property property : node.getCustomPropertiesList()) {
                    equal(true, seen.add(property.getIdHash()));
                    equal(SPINE_PROPERTIES.get(property.getIdHash()), property.getType());
                }
                equal(SPINE_PROPERTIES.keySet(), seen);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("--descriptor")) {
            byte[] bytes = Files.readAllBytes(Path.of(args[1]));
            Message descriptor = args[1].endsWith(".spinemodelc")
                    ? SpineModelDesc.parseFrom(bytes) : SpineSceneDesc.parseFrom(bytes);
            for (int i = 2; i < args.length; i += 2) {
                equal(args[i + 1], descriptor.getField(descriptor.getDescriptorForType().findFieldByName(args[i])));
            }
            return;
        }
        if (args[0].equals("--hashes")) {
            for (int i = 1; i < args.length; ++i) {
                System.out.println(Long.toUnsignedString(MurmurHash.hash64(args[i])));
            }
            return;
        }
        SceneDesc scene = SceneDesc.parseFrom(Files.readAllBytes(Path.of(args[0])));
        equal(0, scene.getSpineScenesCount());
        checkNodes(scene.getNodesList());
        for (SceneDesc.LayoutDesc layout : scene.getLayoutsList()) {
            checkNodes(layout.getNodesList());
        }
        List<NodeDesc> nodes = args[1].isEmpty() ? scene.getNodesList()
                : scene.getLayoutsList().stream().filter(layout -> layout.getName().equals(args[1])).findFirst().orElseThrow().getNodesList();
        NodeDesc node = args[2].isEmpty() ? null
                : nodes.stream().filter(candidate -> candidate.getId().equals(args[2])).findFirst().orElseThrow();
        if (node != null) {
            equal(NodeDesc.Type.TYPE_CUSTOM, node.getType());
            equal(MurmurHash.hash32("Spine"), node.getCustomType());
        }
        for (int i = 3; i < args.length; i += 2) {
            String name = args[i];
            String expected = args[i + 1];
            if (name.startsWith("@resource:")) {
                String resourceName = name.substring("@resource:".length());
                equal(expected, scene.getResourcesList().stream().filter(resource -> resource.getName().equals(resourceName))
                        .findFirst().orElseThrow().getPath());
            } else {
                long hash = MurmurHash.hash64(name);
                Property property = node.getCustomPropertiesList().stream().filter(value -> value.getIdHash() == hash).findFirst().orElse(null);
                String actual = property == null ? "<absent>" : switch (property.getType()) {
                    case TYPE_BOOLEAN -> Boolean.toString(property.getBoolean());
                    case TYPE_STRING -> property.getString();
                    default -> throw new AssertionError("Unexpected property type: " + property.getType());
                };
                equal(expected, actual);
            }
        }
    }
}
