//
// License: MIT
//

package com.defold.extension.pipeline;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.pipeline.GuiBuildContext;
import com.dynamo.bob.pipeline.GuiCustomNode;
import com.dynamo.bob.pipeline.GuiSceneMigrationContext;
import com.dynamo.bob.pipeline.IGuiCustomNode;
import com.dynamo.bob.pipeline.IGuiCustomType;
import com.dynamo.bob.pipeline.Spine;
import com.dynamo.bob.pipeline.SpineSceneValidator;
import com.dynamo.bob.util.MurmurHash;
import com.dynamo.gamesys.proto.Gui.NodeDesc;
import com.dynamo.gamesys.proto.Gui.NodeDesc.Type;
import com.dynamo.gamesys.proto.Gui.NodeDescOrBuilder;
import com.dynamo.gamesys.proto.Gui.Property;
import com.dynamo.gamesys.proto.Gui.Property.PropertyType;
import com.dynamo.gamesys.proto.Gui.SceneDesc;
import com.dynamo.gamesys.proto.Gui.SceneDesc.LayoutDesc;
import com.dynamo.gamesys.proto.Gui.SceneDesc.ResourceDesc;
import com.google.protobuf.Descriptors.FieldDescriptor;

@GuiCustomNode(type = "Spine")
public class SpineGuiNode implements IGuiCustomNode {
    private static final int TYPE_HASH = MurmurHash.hash32("Spine");
    private static final Set<Integer> LEGACY_FIELDS = Set.of(
            NodeDesc.SPINE_SCENE_FIELD_NUMBER, NodeDesc.SPINE_DEFAULT_ANIMATION_FIELD_NUMBER,
            NodeDesc.SPINE_SKIN_FIELD_NUMBER, NodeDesc.SPINE_NODE_CHILD_FIELD_NUMBER,
            NodeDesc.SPINE_CREATE_BONES_FIELD_NUMBER);
    private final Map<String, IResource> dataByScene = new HashMap<>();

    public static void registerProperties(IGuiCustomType type) {
        type.addProperty("spine_scene", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_RESOURCE);
        type.addProperty("spine_default_animation", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_DEFAULT);
        type.addProperty("spine_skin", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_DEFAULT);
        type.addProperty("spine_create_bones", false, PropertyType.TYPE_BOOLEAN, IGuiCustomType.EDIT_TYPE_DEFAULT);
    }

    private static void migrateProperty(NodeDesc.Builder node, Set<Long> propertyIds, Set<Integer> overrides,
            boolean sparse, int fieldNumber, String name, PropertyType type) {
        long id = MurmurHash.hash64(name);
        if (propertyIds.contains(id)) {
            return;
        }
        FieldDescriptor field = NodeDesc.getDescriptor().findFieldByNumber(fieldNumber);
        Object value = node.getField(field);
        // An override marker can explicitly select a protobuf default even when
        // the field is omitted. Unmarked defaults in sparse nodes are inherited.
        if (!overrides.contains(fieldNumber)
                && !(node.hasField(field) && (!sparse || !value.equals(field.getDefaultValue())))) {
            return;
        }
        Property.Builder property = Property.newBuilder().setId(name).setType(type);
        if (type == PropertyType.TYPE_BOOLEAN) {
            property.setBoolean((Boolean) value);
        } else {
            property.setString((String) value);
        }
        node.addCustomProperties(property);
        propertyIds.add(id);
    }

    private static void migrateNode(NodeDesc.Builder node, boolean layout) {
        boolean legacyType = node.getType() == Type.TYPE_SPINE;
        boolean spineType = legacyType || (node.getType() == Type.TYPE_CUSTOM
                && (node.getCustomTypeName().equals("Spine")
                    || (node.getCustomTypeName().isEmpty() && node.getCustomType() == TYPE_HASH)));
        boolean sparse = layout || node.getTemplateNodeChild();
        boolean anonymousOverride = sparse && node.getCustomTypeName().isEmpty() && node.getCustomType() == 0;
        if (!spineType && !anonymousOverride) {
            return;
        }

        Set<Integer> overrides = new HashSet<>(node.getOverriddenFieldsList());
        Set<Long> propertyIds = new HashSet<>();
        for (Property property : node.getCustomPropertiesList()) {
            if (property.hasId() && !property.getId().isEmpty()) {
                propertyIds.add(MurmurHash.hash64(property.getId()));
            } else if (property.hasIdHash()) {
                propertyIds.add(property.getIdHash());
            }
        }
        migrateProperty(node, propertyIds, overrides, sparse, NodeDesc.SPINE_SCENE_FIELD_NUMBER, "spine_scene", PropertyType.TYPE_STRING);
        migrateProperty(node, propertyIds, overrides, sparse, NodeDesc.SPINE_DEFAULT_ANIMATION_FIELD_NUMBER, "spine_default_animation", PropertyType.TYPE_STRING);
        migrateProperty(node, propertyIds, overrides, sparse, NodeDesc.SPINE_SKIN_FIELD_NUMBER, "spine_skin", PropertyType.TYPE_STRING);
        migrateProperty(node, propertyIds, overrides, sparse, NodeDesc.SPINE_CREATE_BONES_FIELD_NUMBER, "spine_create_bones", PropertyType.TYPE_BOOLEAN);

        var retainedOverrides = new ArrayList<Integer>();
        for (int field : node.getOverriddenFieldsList()) {
            if (!LEGACY_FIELDS.contains(field)) {
                retainedOverrides.add(field);
            }
        }
        node.clearOverriddenFields().addAllOverriddenFields(retainedOverrides);
        node.clearSpineScene().clearSpineDefaultAnimation().clearSpineSkin()
                .clearSpineCreateBones().clearSpineNodeChild();
        if (legacyType) {
            node.setType(Type.TYPE_CUSTOM).setCustomTypeName("Spine").setCustomType(TYPE_HASH);
        }
    }

    @Override
    public void migrateScene(GuiSceneMigrationContext context) {
        context.addExtraCacheKey("spine-gui-migration-v1");
        SceneDesc.Builder scene = context.getSceneBuilder();
        if (scene.getSpineScenesCount() != 0) {
            var resources = new ArrayList<ResourceDesc>();
            for (SceneDesc.SpineSceneDesc resource : scene.getSpineScenesList()) {
                resources.add(ResourceDesc.newBuilder().setName(resource.getName()).setPath(resource.getSpineScene()).build());
            }
            resources.addAll(scene.getResourcesList());
            scene.clearResources().addAllResources(resources).clearSpineScenes();
        }
        for (NodeDesc.Builder node : scene.getNodesBuilderList()) {
            migrateNode(node, false);
        }
        for (LayoutDesc.Builder layout : scene.getLayoutsBuilderList()) {
            for (NodeDesc.Builder node : layout.getNodesBuilderList()) {
                migrateNode(node, true);
            }
        }
    }

    private static String getString(NodeDescOrBuilder node, String name) {
        Property property = GuiBuildContext.getProperty(node, name);
        return property == null ? "" : property.getString();
    }

    private static IResource getScene(GuiBuildContext context, NodeDescOrBuilder node, String layoutName) throws CompileExceptionError {
        String name = getString(node, "spine_scene");
        if (name.isEmpty()) {
            throw context.error(node, layoutName, "Spine scene must not be empty");
        }
        IResource scene = context.getResources().get(name);
        if (scene == null) {
            throw context.error(node, layoutName, "spine scene '" + name + "' does not exist in the scene");
        }
        if (!scene.getPath().endsWith(".spinescene")) {
            throw context.error(node, layoutName, "Spine scene '" + name + "' must refer to a .spinescene file");
        }
        return scene;
    }

    @Override
    public void create(GuiBuildContext context, Task.TaskBuilder<?> taskBuilder, NodeDesc node, String layoutName) throws IOException, CompileExceptionError {
        IResource scene = getScene(context, node, layoutName);
        IResource data = dataByScene.get(scene.getPath());
        if (data == null) {
            try {
                data = SpineSceneValidator.getSpineData(context.getProject(), scene);
            } catch (CompileExceptionError e) {
                throw new CompileExceptionError(e.getResource(), e.getLineNumber(),
                        context.error(node, layoutName, e.getMessage()).getMessage(), e);
            }
            dataByScene.put(scene.getPath(), data);
        }
        // The compiled scene descriptor can stay identical when animation or skin
        // names change. Track both source files so those changes revalidate the GUI.
        taskBuilder.addInput(scene).addInput(data);
        taskBuilder.addExtraCacheKey(SpineSceneValidator.CACHE_KEY);
        taskBuilder.addExtraCacheKey("spine-gui-build-v1");
    }

    @Override
    public void build(GuiBuildContext context, Task task, NodeDesc.Builder node, String layoutName) throws IOException, CompileExceptionError {
        IResource scene = getScene(context, node, layoutName);
        IResource data = dataByScene.get(scene.getPath());
        SpineSceneValidator.SceneData info;
        try {
            info = SpineSceneValidator.load(data, null);
        } catch (Spine.SpineException e) {
            throw context.error(node, layoutName, e.getMessage());
        }
        String error = info.getValidationError(getString(node, "spine_default_animation"), getString(node, "spine_skin"));
        if (error != null) {
            throw context.error(node, layoutName, error);
        }
        // Match the editor: Spine controls node dimensions, including in layout overrides.
        node.setSizeMode(NodeDesc.SizeMode.SIZE_MODE_AUTO);
    }
}
