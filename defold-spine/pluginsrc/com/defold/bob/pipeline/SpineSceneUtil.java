// Copyright 2020 The Defold Foundation
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
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

import org.codehaus.jackson.JsonNode;
import org.codehaus.jackson.JsonParseException;
import org.codehaus.jackson.map.JsonMappingException;
import org.codehaus.jackson.map.ObjectMapper;

import com.dynamo.bob.util.RigUtil.Bone;
import com.dynamo.bob.util.RigUtil.Transform;

/**
 * Convenience class for loading spine json data.
 *
 * Should preferably have been an extension to Bob rather than located inside it.
 */
public class SpineSceneUtil {
    @SuppressWarnings("serial")
    public static class LoadException extends Exception {
        public LoadException(String msg) {
            super(msg);
        }
    }

    public List<Bone> bones = new ArrayList<Bone>();
    public Map<String, Bone> nameToBones = new HashMap<String, Bone>();

    public Bone getBone(String name) {
        return nameToBones.get(name);
    }

    public Bone getBone(int index) {
        return bones.get(index);
    }

    public List<Bone> getBones() {
        return this.bones;
    }

    private static void loadTransform(JsonNode node, Transform t) {
        t.position.set(JsonUtil.get(node, "x", 0.0), JsonUtil.get(node, "y", 0.0), 0.0);
        t.setZAngleDeg(JsonUtil.get(node, "rotation", 0.0));
        t.scale.set(JsonUtil.get(node, "scaleX", 1.0), JsonUtil.get(node, "scaleY", 1.0), 1.0);
    }

    private void loadBone(JsonNode boneNode) throws LoadException {
        Bone bone = new Bone();
        bone.name = boneNode.get("name").asText();
        bone.index = this.bones.size();
        if (boneNode.has("inheritScale")) {
            bone.inheritScale = boneNode.get("inheritScale").asBoolean();
        }
        if (boneNode.has("length")) {
            bone.length = boneNode.get("length").asDouble();
        }
        loadTransform(boneNode, bone.localT);
        if (boneNode.has("parent")) {
            String parentName = boneNode.get("parent").asText();
            bone.parent = getBone(parentName);
            if (bone.parent == null) {
                throw new LoadException(String.format("The parent bone '%s' does not exist.", parentName));
            }
            bone.worldT.set(bone.parent.worldT);
            bone.worldT.mul(bone.localT);
            // Restore scale to local when it shouldn't be inherited
            if (!bone.inheritScale) {
                bone.worldT.scale.set(bone.localT.scale);
            }
        } else {
            bone.worldT.set(bone.localT);
        }
        bone.invWorldT.set(bone.worldT);
        bone.invWorldT.inverse();
        this.bones.add(bone);
        this.nameToBones.put(bone.name, bone);
    }

    // Used by the GUI builder!!
    public static List<Bone> getBones(InputStream is) throws LoadException {
        SpineSceneUtil scene = new SpineSceneUtil();
        ObjectMapper m = new ObjectMapper();
        try {
            JsonNode node = m.readValue(new InputStreamReader(is, "UTF-8"), JsonNode.class);
            Iterator<JsonNode> boneIt = node.get("bones").getElements();
            while (boneIt.hasNext()) {
                JsonNode boneNode = boneIt.next();
                scene.loadBone(boneNode);
            }
            return scene.bones;
        } catch (JsonParseException e) {
            throw new LoadException(e.getMessage());
        } catch (JsonMappingException e) {
            throw new LoadException(e.getMessage());
        } catch (UnsupportedEncodingException e) {
            throw new LoadException(e.getMessage());
        } catch (IOException e) {
            throw new LoadException(e.getMessage());
        }
    }

    public static class JsonUtil {

        public static double get(JsonNode n, String name, double defaultVal) {
            return n.has(name) ? n.get(name).asDouble() : defaultVal;
        }

        public static float get(JsonNode n, String name, float defaultVal) {
            return n.has(name) ? (float)n.get(name).asDouble() : defaultVal;
        }

        public static String get(JsonNode n, String name, String defaultVal) {
            return n.has(name) ? (!n.get(name).isNull() ? n.get(name).asText() : defaultVal) : defaultVal;
        }

        public static int get(JsonNode n, String name, int defaultVal) {
            return n.has(name) ? n.get(name).asInt() : defaultVal;
        }

        public static boolean get(JsonNode n, String name, boolean defaultVal) {
            return n.has(name) ? n.get(name).asBoolean() : defaultVal;
        }

        public static JsonNode get(JsonNode n, String name) {
            return n.has(name) ? n.get(name) : null;
        }

        public static void hexToRGBA(String hex, float[] value) {
            for (int i = 0; i < 4; ++i) {
                int offset = i*2;
                value[i] = Integer.valueOf(hex.substring(0 + offset, 2 + offset), 16) / 255.0f;
            }
        }
    }
}
