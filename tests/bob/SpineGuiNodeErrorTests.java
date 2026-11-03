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

package com.dynamo.bob.pipeline;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;

import com.defold.extension.pipeline.SpineGuiNode;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.Project;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.DefaultFileSystem;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.util.MurmurHash;
import com.dynamo.gamesys.proto.Gui.NodeDesc;
import com.dynamo.gamesys.proto.Gui.Property;

/** Checks that GUI context does not replace the location of an invalid Spine descriptor. */
public final class SpineGuiNodeErrorTests {
    // Descriptor parse errors keep their resource, line and cause alongside the resolved node and layout.
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        Files.writeString(root.resolve("invalid.spinescene"),
                "spine_json: \"/data.spinejson\"\n\natlas: 42\n");
        Project project = new Project(new DefaultFileSystem(), root.toString(), "build/default");
        try {
            IResource scene = project.getResource("/invalid.spinescene");
            GuiBuildContext context = new GuiBuildContext(project, project.getResource("/owner.gui"), Map.of("scene", scene));
            NodeDesc node = NodeDesc.newBuilder().setType(NodeDesc.Type.TYPE_CUSTOM).setId("template/spine")
                    .addCustomProperties(Property.newBuilder().setIdHash(MurmurHash.hash64("spine_scene"))
                            .setType(Property.PropertyType.TYPE_STRING).setString("scene"))
                    .build();
            for (String layout : new String[] {"", "Landscape"}) {
                try {
                    new SpineGuiNode().create(context, Task.newBuilder(null), node, layout);
                    throw new AssertionError("Malformed descriptor must fail");
                } catch (CompileExceptionError error) {
                    String prefix = "Node 'template/spine'" + (layout.isEmpty() ? "" : ", layout 'Landscape'") + ": ";
                    if (!error.getResource().equals(scene) || error.getLineNumber() != 3
                            || !(error.getCause() instanceof CompileExceptionError)
                            || !error.getMessage().startsWith(prefix)
                            || !error.getMessage().endsWith(error.getCause().getMessage())) {
                        throw new AssertionError("Lost descriptor location or GUI context: "
                                + error.getResource().getPath() + ":" + error.getLineNumber() + ": " + error.getMessage(), error);
                    }
                }
            }
        } finally {
            project.dispose();
        }
    }
}
