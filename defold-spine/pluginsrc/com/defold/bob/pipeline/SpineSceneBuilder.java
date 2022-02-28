package com.dynamo.bob.pipeline;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.ProtoBuilder;
import com.dynamo.bob.ProtoParams;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.pipeline.BuilderUtil;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;

@ProtoParams(srcClass = SpineSceneDesc.class, messageClass = SpineSceneDesc.class)
@BuilderParams(name="SpineScene", inExts=".spinescene", outExt=".spinescenec")
public class SpineSceneBuilder extends ProtoBuilder<SpineSceneDesc.Builder> {

    @Override
    protected SpineSceneDesc.Builder transform(Task<Void> task, IResource resource, SpineSceneDesc.Builder builder) throws CompileExceptionError {

        String path;
        path = builder.getSpineJson();
        if (!path.equals("")) {
            BuilderUtil.checkResource(this.project, resource, "spine_json", path);
        }
        builder.setSpineJson(BuilderUtil.replaceExt(path, ".spinejson", ".spinejsonc"));


        path = builder.getAtlas();
        if (!path.equals("")) {
            BuilderUtil.checkResource(this.project, resource, "atlas", path);
        }
        builder.setAtlas(BuilderUtil.replaceExt(path, ".atlas", ".texturesetc"));

        return builder;
    }
}
