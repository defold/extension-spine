// Copyright 2020-2026 The Defold Foundation
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

import org.apache.commons.lang3.StringUtils;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.ProtoBuilder;
import com.dynamo.bob.ProtoParams;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.IResource;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;

import java.io.IOException;

@ProtoParams(srcClass = SpineSceneDesc.class, messageClass = SpineSceneDesc.class)
@BuilderParams(name="SpineScene", inExts=".spinescene", outExt=".spinescenec")
public class SpineSceneBuilder extends ProtoBuilder<SpineSceneDesc.Builder> {
    private IResource compiledData;
    private IResource compiledAtlas;

    @Override
    public Task create(IResource input) throws IOException, CompileExceptionError {
        SpineSceneDesc.Builder source = getSrcBuilder(input);
        IResource data = SpineSceneValidator.checkResource(project, input, "spine_json", source.getSpineJson(), ".spinejson", ".skel");
        IResource atlas = SpineSceneValidator.checkResource(project, input, "atlas", source.getAtlas(), ".atlas");
        compiledData = data.changeExt(StringUtils.endsWithIgnoreCase(data.getPath(), ".skel") ? ".skelc" : ".spinejsonc");
        compiledAtlas = atlas.changeExt(".a.texturesetc");
        return super.create(input);
    }

    @Override
    protected SpineSceneDesc.Builder transform(Task task, IResource resource, SpineSceneDesc.Builder builder) {
        String path = BuilderUtil.replaceExt(builder.getSpineJson(), ".skel", ".skelc");
        builder.setSpineJson(BuilderUtil.replaceExt(path, ".spinejson", ".spinejsonc"));
        builder.setAtlas(BuilderUtil.replaceExt(builder.getAtlas(), ".atlas", ".a.texturesetc"));
        return builder;
    }

    @Override
    public void build(Task task) throws CompileExceptionError, IOException {
        try {
            SpineSceneValidator.load(compiledData, compiledAtlas);
        } catch (Spine.SpineException e) {
            throw new CompileExceptionError(task.firstInput(), 0, e.getMessage(), e);
        }
        // Do not write a compiled scene until native validation succeeds.
        super.build(task);
    }
}
