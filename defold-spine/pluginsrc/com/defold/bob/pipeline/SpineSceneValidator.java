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

import java.io.IOException;
import java.util.Arrays;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.ConcurrentHashMap;

import org.apache.commons.lang3.StringUtils;

import com.dynamo.bob.CompileExceptionError;
import com.dynamo.bob.Project;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.fs.ResourceUtil;
import com.dynamo.bob.plugin.IPlugin;
import com.dynamo.spine.proto.Spine.SpineSceneDesc;

/** Shared source and skeleton validation for Spine scenes, models and GUI nodes. */
public final class SpineSceneValidator implements IPlugin {
    public static final String CACHE_KEY = "spine-validation-v1";
    private static final Map<Project, Map<String, SceneData>> metadataByProject = new WeakHashMap<>();

    public SpineSceneValidator() {
    }

    @Override
    public void init(Project project) {
        synchronized (metadataByProject) {
            // Bob can reuse a Project after a failed build, without calling exit.
            metadataByProject.put(project, new ConcurrentHashMap<>());
        }
    }

    @Override
    public void exit(Project project) {
        synchronized (metadataByProject) {
            metadataByProject.remove(project);
        }
    }

    private static Map<String, SceneData> metadataCache(Project project) {
        synchronized (metadataByProject) {
            // Also support callers that create builders directly, outside Bob's plugin lifecycle.
            return metadataByProject.computeIfAbsent(project, key -> new ConcurrentHashMap<>());
        }
    }

    public static IResource checkResource(Project project, IResource owner, String field,
            String path, String... extensions) throws CompileExceptionError {
        IResource resource = BuilderUtil.checkResource(project, owner, field, path);
        for (String extension : extensions) {
            if (StringUtils.endsWithIgnoreCase(path, extension)) {
                return resource;
            }
        }
        throw new CompileExceptionError(owner, 0, field + " file '" + path + "' must end with '"
                + String.join("' or '", extensions) + "'");
    }

    public static IResource getSpineData(Project project, IResource scene) throws IOException, CompileExceptionError {
        SpineSceneDesc.Builder descriptor = SpineSceneDesc.newBuilder();
        ProtoUtil.merge(scene, descriptor);
        return checkResource(project, scene, "spine_json", descriptor.getSpineJson(), ".spinejson", ".skel");
    }

    public static final class SceneData {
        private final Set<String> animations;
        private final Set<String> skins;

        private SceneData(String[] animations, String[] skins) {
            this.animations = new HashSet<>(Arrays.asList(animations));
            this.skins = new HashSet<>(Arrays.asList(skins));
            // The editor uses the empty selection for the default skin.
            this.skins.remove("default");
        }

        public String getValidationError(String animation, String skin) {
            if (!animation.isEmpty() && !animations.contains(animation)) {
                return "animation '" + animation + "' could not be found in the specified spine scene";
            }
            if (!skin.isEmpty() && !skins.contains(skin)) {
                return "skin '" + skin + "' could not be found in the specified spine scene";
            }
            return null;
        }
    }

    public static SceneData load(Project project, IResource spineData, IResource atlas) throws IOException, Spine.SpineException {
        // The native loader selects binary input using a case-sensitive suffix.
        boolean binary = StringUtils.endsWithIgnoreCase(spineData.getPath(), ".skel")
                || StringUtils.endsWithIgnoreCase(spineData.getPath(), ".skelc");
        String dataPath = binary ? ResourceUtil.changeExt(spineData.getPath(), ".skel") : spineData.getPath();
        // Source and compiled skeletons have identical bytes. Share their immutable
        // metadata across all scene, model and GUI tasks in this build, including aliases.
        String key = (binary ? "binary:" : "json:") + HexFormat.of().formatHex(spineData.sha1());
        Map<String, SceneData> cache = metadataCache(project);
        SceneData cached = cache.get(key);
        if (atlas == null && cached != null) {
            return cached;
        }

        // Deduplicate concurrent metadata misses within this project. The Spine
        // wrapper owns native synchronization and handle cleanup.
        synchronized (cache) {
            cached = cache.get(key);
            if (atlas == null && cached != null) {
                return cached;
            }
            byte[] data = spineData.getContent();
            byte[] atlasData = atlas == null ? null : atlas.getContent();
            if (data == null) {
                throw new Spine.SpineException("Could not read Spine data '" + spineData.getPath() + "'");
            }
            if (atlas != null && atlasData == null) {
                throw new Spine.SpineException("Could not read Spine atlas '" + atlas.getPath() + "'");
            }
            // Atlas validation is scene-specific and must run even on a metadata hit.
            try (Spine.SpinePointer handle = atlas == null
                    ? Spine.SPINE_LoadFileFromBuffer(data, dataPath)
                    : Spine.SPINE_LoadFileFromBuffer(data, dataPath, atlasData, atlas.getPath())) {
                SceneData result = new SceneData(Spine.SPINE_GetAnimations(handle), Spine.SPINE_GetSkins(handle));
                cache.putIfAbsent(key, result);
                return cache.get(key);
            }
        }
    }
}
