//
// License: MIT
//

package com.dynamo.bob.pipeline;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.Arrays;

import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.PointerType;
import com.sun.jna.Structure;
import com.sun.jna.ptr.IntByReference;

public class Spine {

    static {
        try {
            Native.register("SpineExt");
        } catch (Exception e) {
            System.out.println("FATAL: " + e.getMessage());
        }
    }

    public static class SpinePointer extends PointerType {
        public SpinePointer() { super(); }
        public SpinePointer(Pointer p) { super(p); }
        @Override
        public void finalize() {
            SPINE_Destroy(this);
        }
    }

    public static class SpineException extends Exception {
        public SpineException(String errorMessage) {
            super(errorMessage);
        }
    }

    // Type Mapping in JNA
    // https://java-native-access.github.io/jna/3.5.1/javadoc/overview-summary.html#marshalling

    public static native Pointer SPINE_LoadFromPath(String path, String atlas_path);
    public static native Pointer SPINE_LoadFromBuffer(Buffer buffer, int bufferSize, String path, Buffer atlas_buffer, int atlas_bufferSize, String atlas_path);
    public static native void SPINE_Destroy(SpinePointer spine);

    // TODO: Create a jna Structure for this
    // Structures in JNA
    // https://java-native-access.github.io/jna/3.5.1/javadoc/overview-summary.html#structures

    ////////////////////////////////////////////////////////////////////////////////
    static public class AABB extends Structure {
        public float minX, minY, maxX, maxY;
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"minX", "minY", "maxX", "maxY"});
        }
        public AABB() {
            this.minX = this.minY = this.maxX = this.maxY = 0;
        }
        public static class ByValue extends AABB implements Structure.ByValue { }
    }

    public static native AABB.ByValue SPINE_GetAABB(SpinePointer spine);

    ////////////////////////////////////////////////////////////////////////////////

    static public class SpineBone extends Structure {
        public String name;
        public int parent;
        public float posX, posY, rotation, scaleX, scaleY, length;

        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"name", "parent", "posX", "posY", "rotation", "scaleX", "scaleY", "length"});
        }
        public SpineBone() {
            this.name = "<empty>";
            this.parent = -1;
            this.posX = this.posY = 0.0f;
            this.scaleX = this.scaleY = 1.0f;
            this.rotation = 0.0f;
            this.length = 100.0f;
        }
    }

    static public class Bone {
        public String name;
        public int   index;
        public int   parent;
        public int[] children;
        public float posX, posY, rotation, scaleX, scaleY, length;
    }

    public static native int SPINE_GetNumBones(SpinePointer spine);
    public static native String SPINE_GetBoneInternal(SpinePointer spine, int index, SpineBone bone);
    public static native int SPINE_GetNumChildBones(SpinePointer spine, int bone);
    public static native int SPINE_GetChildBone(SpinePointer spine, int bone, int index);

    protected static Bone SPINE_GetBone(SpinePointer spine, int index) {
        Bone bone = new Bone();
        SpineBone internal = new SpineBone();
        SPINE_GetBoneInternal(spine, index, internal);
        bone.index = index;
        bone.name = internal.name;
        bone.posX = internal.posX;
        bone.posY = internal.posY;
        bone.scaleX = internal.scaleX;
        bone.scaleY = internal.scaleY;
        bone.rotation = internal.rotation;
        bone.length = internal.length;
        bone.parent = internal.parent;

        int num_children = SPINE_GetNumChildBones(spine, index);
        bone.children = new int[num_children];

        for (int i = 0; i < num_children; ++i) {
            bone.children[i] = SPINE_GetChildBone(spine, index, i);
        }
        return bone;
    }

    public static Bone[] SPINE_GetBones(SpinePointer spine) {
        int num_bones = SPINE_GetNumBones(spine);
        Bone[] bones = new Bone[num_bones];
        for (int i = 0; i < num_bones; ++i) {
            bones[i] = SPINE_GetBone(spine, i);
        }
        return bones;
    }

    ////////////////////////////////////////////////////////////////////////////////

    private static final float[] IDENTITY_TRANSFORM = new float[] {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    private static final float[] IDENTITY_COLOR = new float[] { 1.0f, 1.0f, 1.0f, 1.0f };

    public static native void SPINE_UpdateVertices(SpinePointer spine, float dt, float[] worldTransform, float[] colorTint, int useIndexBuffer);

    public static native int SPINE_GetVertexSize(); // size in bytes per vertex


    ////////////////////////////////////////////////////////////////////////////////
    // Render data

    // Matching the struct in vertices.h
    static public class SpineVertex extends Structure {
        public float x, y, z, u, v, r, g, b, a, page_index;
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"x", "y", "z", "u", "v", "r", "g", "b", "a", "page_index"});
        }
    }

    // Matching the layout 1:1 with dmSpine::SpineIndexedDrawDesc in vertices.h
    static public class DrawDesc extends Structure {
        public int m_IndexStart;
        public int m_IndexCount;
        public int m_BlendMode;

        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"m_IndexStart", "m_IndexCount", "m_BlendMode"});
        }
    }

    static public class NativeString extends Structure {
        public String    string;
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"string"});
        }
        public int getOffset(String name) {
            return super.fieldOffset(name);
        }
    }

    public static native String SPINE_GetLastError();
    public static native SpineVertex SPINE_GetVertexBufferData(SpinePointer spine, IntByReference objectCount);
    public static native Pointer SPINE_GetVertexBufferPointer(SpinePointer spine, IntByReference objectCount);
    public static native int SPINE_GetVertexBufferVersion(SpinePointer spine);
    public static native Pointer SPINE_GetIndexBufferData(SpinePointer spine, IntByReference objectCount);
    public static native int SPINE_GetIndexBufferVersion(SpinePointer spine);
    public static native DrawDesc SPINE_GetDrawDescData(SpinePointer spine, IntByReference objectCount);
    public static native NativeString SPINE_GetAnimationData(SpinePointer spine, IntByReference objectCount);
    public static native NativeString SPINE_GetSkinData(SpinePointer spine, IntByReference objectCount);

    public static native void SPINE_SetSkin(SpinePointer spine, String skin);
    public static native void SPINE_SetAnimation(SpinePointer spine, String animation);

    private static String[] nativeToStringArray(NativeString first, IntByReference pcount)
    {
        if (first == null)
        {
            // System.out.printf("String buffer is empty!\n");
            return new String[0];
        }
        int count = pcount.getValue();
        NativeString[] arr = (NativeString[])first.toArray(count);
        String[] result = new String[count];
        for (int i = 0; i < count; ++i)
        {
            result[i] = arr[i].string;
        }
        return result;
    }

    public static String[] SPINE_GetAnimations(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        NativeString first = SPINE_GetAnimationData(spine, pcount);
        return nativeToStringArray(first, pcount);
    }

    public static String[] SPINE_GetSkins(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        NativeString first = SPINE_GetSkinData(spine, pcount);
        return nativeToStringArray(first, pcount);
    }

    // idea from https://stackoverflow.com/a/15431595/468516
    public static SpineVertex[] SPINE_GetVertexBuffer(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        SpineVertex first = SPINE_GetVertexBufferData(spine, pcount);
        if (first == null || pcount.getValue() == 0)
        {
            SpineVertex[] arr = new SpineVertex[1];
            SpineVertex v = new SpineVertex();
            v.x = v.y = v.z = 0;
            v.u = v.v = 0;
            v.r = v.g = v.b = v.a = 0;
            arr[0] = v;
            return arr;
        }
        return (SpineVertex[])first.toArray(pcount.getValue());
    }

    public static ByteBuffer SPINE_GetVertexBufferByteBuffer(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        Pointer first = SPINE_GetVertexBufferPointer(spine, pcount);
        if (first == null || pcount.getValue() == 0)
        {
            return ByteBuffer.allocateDirect(0).order(ByteOrder.nativeOrder());
        }
        return first.getByteBuffer(0, (long)pcount.getValue() * SPINE_GetVertexSize()).order(ByteOrder.nativeOrder());
    }

    public static int[] SPINE_GetIndexBuffer(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        Pointer first = SPINE_GetIndexBufferData(spine, pcount);
        if (first == null || pcount.getValue() == 0)
        {
            return new int[0];
        }
        return first.getIntArray(0, pcount.getValue());
    }

    public static ByteBuffer SPINE_GetIndexBufferByteBuffer(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        Pointer first = SPINE_GetIndexBufferData(spine, pcount);
        if (first == null || pcount.getValue() == 0)
        {
            return ByteBuffer.allocateDirect(0).order(ByteOrder.nativeOrder());
        }
        return first.getByteBuffer(0, (long)pcount.getValue() * Integer.BYTES).order(ByteOrder.nativeOrder());
    }

    public static DrawDesc[] SPINE_GetDrawDescs(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        DrawDesc first = SPINE_GetDrawDescData(spine, pcount);
        if (first == null || pcount.getValue() == 0)
        {
            return new DrawDesc[0];
        }

        int draw_desc_size = 12;
        if (first.size() != draw_desc_size) {
            System.out.printf("DrawDesc size is not %d, it was %d\n", draw_desc_size, first.size());
            return new DrawDesc[0];
        }

        return (DrawDesc[])first.toArray(pcount.getValue());
    }

    ////////////////////////////////////////////////////////////////////////////////

    public static SpinePointer SPINE_LoadFileFromBuffer(byte[] json_buffer, String path, byte[] atlas_buffer, String atlas_path) throws SpineException {
        if (json_buffer == null || atlas_buffer == null)
        {
            System.out.printf("One of the buffers is null (%s)\n", json_buffer == null ? "json_buffer":"atlas_buffer");
            return null;
        }
        Buffer b = ByteBuffer.wrap(json_buffer);
        Buffer a = ByteBuffer.wrap(atlas_buffer);
        Pointer p = SPINE_LoadFromBuffer(b, b.capacity(), path, a, a.capacity(), atlas_path);
        if (p == null) {
            throw new SpineException(String.format("Failed to load spine scene '%s' with atlas '%s': %s", path, atlas_path, SPINE_GetLastError()));
        }
        return new SpinePointer(p);
    }

    public static SpinePointer SPINE_LoadFileFromBuffer(byte[] json_buffer, String path) throws SpineException {
        if (json_buffer == null)
        {
            System.out.printf("The json buffer is null\n");
            return null;
        }
        Buffer b = ByteBuffer.wrap(json_buffer);
        Pointer p = SPINE_LoadFromBuffer(b, b.capacity(), path, null, 0, null);
        if (p == null) {
            throw new SpineException(String.format("Failed to load spine scene '%s': %s", path, SPINE_GetLastError()));
        }
        return new SpinePointer(p);
    }

    // public static void SPINE_GetVertices(SpinePointer spine, float[] buffer){
    //     Buffer b = FloatBuffer.wrap(buffer);
    //     SPINE_GetVertices(spine, b, b.capacity()*4);
    // }

    private static void Usage() {
        System.out.printf("Usage: pluginSpineExt.jar <.spinejson> <.texturesetc>\n");
        System.out.printf("\n");
    }

    private static void DebugPrintBone(Bone bone, Bone[] bones, int indent) {
        String tab = " ".repeat(indent * 4);
        System.out.printf("Bone:%s %s: idx: %d parent = %d, pos: %f, %f  scale: %f, %f  rot: %f  length: %f\n",
                tab, bone.name, bone.index, bone.parent, bone.posX, bone.posY, bone.scaleX, bone.scaleY, bone.rotation, bone.length);

        int num_children = bone.children.length;
        for (int i = 0; i < num_children; ++i){
            int child_index = bone.children[i];
            Bone child = bones[child_index];
            DebugPrintBone(child, bones, indent+1);
        }
    }

    private static void DebugPrintBones(Bone[] bones) {
        for (Bone bone : bones) {
            if (bone.parent == -1) {
                DebugPrintBone(bone, bones, 0);
            }
        }
    }

    // Used for testing functions
    // See ./utils/test_plugin.sh
    public static void main(String[] args) throws IOException {
        System.setProperty("java.awt.headless", "true");

        if (args.length < 2) {
            Usage();
            return;
        }

        String path = args[0];       // .spinejson
        String atlas_path = args[1]; // .texturesetc
        Pointer spine_file = SPINE_LoadFromPath(path, atlas_path);

        if (spine_file != null) {
            System.out.printf("Loaded %s\n", path);
        } else {
            System.err.printf("Failed to load %s\n", path);
            return;
        }

        SpinePointer p = new SpinePointer(spine_file);

        {
            int i = 0;
            for (String name : SPINE_GetAnimations(p)) {
                System.out.printf("Animation %d: %s\n", i++, name);
            }
        }

        {
            int i = 0;
            for (String name : SPINE_GetSkins(p)) {
                System.out.printf("Skin %d: %s\n", i++, name);
            }
        }

        Bone[] bones = SPINE_GetBones(p);
        DebugPrintBones(bones);

        SPINE_UpdateVertices(p, 0.0f, IDENTITY_TRANSFORM, IDENTITY_COLOR, 1);

        int count = 0;
        SpineVertex[] vertices = SPINE_GetVertexBuffer(p);
        int[] indices = SPINE_GetIndexBuffer(p);

        System.out.printf("Vertices: count: %d  size: %d bytes\n", vertices.length, vertices.length>0 ? vertices.length * vertices[0].size() : 0);
        System.out.printf("Indices: count: %d  size: %d bytes\n", indices.length, indices.length * Integer.BYTES);

        for (SpineVertex vertex : vertices) {
            if (count > 10) {
                System.out.printf(" ...\n");
                break;
            }
            System.out.printf(" vertex %d: %.4f, %.4f\n", count++, vertex.x, vertex.y);
        }

        count = 0;
        DrawDesc[] drawDescs = SPINE_GetDrawDescs(p);

        System.out.printf("Draw Descs: count %d\n", drawDescs.length);
        for (DrawDesc drawDesc : drawDescs) {
            if (count > 10) {
                System.out.printf(" ...\n");
                break;
            }

            System.out.printf(" draw desc %d: index start: %d  index count: %d  blend mode: %d\n", count++, drawDesc.m_IndexStart, drawDesc.m_IndexCount, drawDesc.m_BlendMode);
        }
    }
}
