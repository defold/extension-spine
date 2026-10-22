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

import java.io.IOException;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.ProtoBuilder;
import com.dynamo.bob.ProtoParams;
import com.dynamo.bob.Task;
import com.dynamo.bob.fs.IResource;
import com.dynamo.spine.proto.Spine.SpineModelDesc;

@ProtoParams(srcClass = SpineModelDesc.class, messageClass = SpineModelDesc.class)
@BuilderParams(name="SpineModel", inExts=".spinemodel", outExt=".spinemodelc")
public class SpineModelBuilder extends ProtoBuilder<SpineModelDesc.Builder> {
    private IResource spineData;

    @Override
    public Task create(IResource input) throws IOException, CompileExceptionError {
        SpineModelDesc.Builder source = getSrcBuilder(input);
        IResource scene = SpineSceneValidator.checkResource(project, input, "spine_scene", source.getSpineScene(), ".spinescene");
        SpineSceneValidator.checkResource(project, input, "material", source.getMaterial(), ".material");
        spineData = SpineSceneValidator.getSpineData(project, scene);

        Task.TaskBuilder<?> taskBuilder = Task.newBuilder(this)
                .setName(params.name())
                .addInput(input)
                .addInput(scene)
                .addInput(spineData)
                .addOutput(input.changeExt(params.outExt()))
                .addExtraCacheKey(SpineSceneValidator.CACHE_KEY);
        createSubTasks(source, taskBuilder);
        return taskBuilder.build();
    }

    @Override
    protected SpineModelDesc.Builder transform(Task task, IResource resource, SpineModelDesc.Builder builder) throws IOException, CompileExceptionError {
        try {
            SpineSceneValidator.SceneData info = SpineSceneValidator.load(spineData, null);
            String error = info.getValidationError(builder.getDefaultAnimation(), builder.getSkin());
            if (error != null) {
                throw new CompileExceptionError(resource, 0, error);
            }
        } catch (Spine.SpineException e) {
            throw new CompileExceptionError(resource, 0, e.getMessage(), e);
        }

        builder.setSpineScene(BuilderUtil.replaceExt(builder.getSpineScene(), ".spinescene", ".spinescenec"));
        builder.setMaterial(BuilderUtil.replaceExt(builder.getMaterial(), ".material", ".materialc"));
        return builder;
    }
}
