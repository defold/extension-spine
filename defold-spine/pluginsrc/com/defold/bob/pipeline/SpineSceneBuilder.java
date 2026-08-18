package com.dynamo.bob.pipeline;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.ProtoBuilder;
import com.dynamo.bob.ProtoParams;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.pipeline.BuilderUtil;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;
import com.dynamo.bob.pipeline.Spine;

import java.io.IOException;
import java.nio.Buffer;

@ProtoParams(srcClass = SpineSceneDesc.class, messageClass = SpineSceneDesc.class)
@BuilderParams(name="SpineScene", inExts=".spinescene", outExt=".spinescenec")
public class SpineSceneBuilder extends ProtoBuilder<SpineSceneDesc.Builder> {

    @Override
    protected SpineSceneDesc.Builder transform(Task task, IResource resource, SpineSceneDesc.Builder builder) throws CompileExceptionError {

        String path;
        path = builder.getSpineJson();
        if (!path.equals("")) {
            BuilderUtil.checkResource(this.project, resource, "spine_json", path);
        }
        path = BuilderUtil.replaceExt(path, ".skel", ".skelc");
        builder.setSpineJson(BuilderUtil.replaceExt(path, ".spinejson", ".spinejsonc"));


        path = builder.getAtlas();
        if (!path.equals("")) {
            BuilderUtil.checkResource(this.project, resource, "atlas", path);
        }
        builder.setAtlas(BuilderUtil.replaceExt(path, ".atlas", ".a.texturesetc"));

        return builder;
    }

    @Override
    public void build(Task task) throws CompileExceptionError, IOException {
        super.build(task);

        IResource texturec = null;
        IResource spineData = null;
        for (IResource input: task.getInputs()) {
            String path = input.getPath();
            if (path.endsWith("texturesetc")) {
                texturec = input;
            }
            else if (path.endsWith("spinejsonc") || path.endsWith("skelc")) {
                spineData = input;
            }
        }
        try {
            Spine.SPINE_LoadFileFromBuffer(spineData.getContent(), spineData.getPath(), texturec.getContent(), texturec.getPath());
        }
        catch (Spine.SpineException e) {
            throw new CompileExceptionError(task.getInputs().get(0), -1, e.getMessage());
        }
    }
}
