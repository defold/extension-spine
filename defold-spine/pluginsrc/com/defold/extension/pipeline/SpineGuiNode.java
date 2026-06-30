//
// License: MIT
//

package com.defold.extension.pipeline;

import java.util.Map;

import com.dynamo.bob.pipeline.GuiCustomNode;
import com.dynamo.bob.pipeline.IGuiCustomNode;
import com.dynamo.bob.pipeline.IGuiCustomType;
import com.dynamo.gamesys.proto.Gui.Property.PropertyType;

@GuiCustomNode(type = "Spine")
public class SpineGuiNode implements IGuiCustomNode {
    public static void registerProperties(IGuiCustomType type) {
        type.addProperty("spine_scene", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_RESOURCE);
        type.addProperty("spine_default_animation", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_DEFAULT);
        type.addProperty("spine_skin", "", PropertyType.TYPE_STRING, IGuiCustomType.EDIT_TYPE_DEFAULT);
        type.addProperty("spine_create_bones", false, PropertyType.TYPE_BOOLEAN, IGuiCustomType.EDIT_TYPE_DEFAULT);
    }

    public static void migrateProperties(Map<String, Object> properties) {
        properties.remove("spine_node_child");
    }
}
