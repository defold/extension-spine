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
import java.nio.FloatBuffer;
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

    public static native void SPINE_UpdateVertices(SpinePointer spine, float dt);
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

    // The enums come straight from https://github.com/defold/defold/blob/dev/engine/graphics/src/dmsdk/graphics/graphics.h
    public enum CompareFunc
    {
        COMPARE_FUNC_NEVER    (0),
        COMPARE_FUNC_LESS     (1),
        COMPARE_FUNC_LEQUAL   (2),
        COMPARE_FUNC_GREATER  (3),
        COMPARE_FUNC_GEQUAL   (4),
        COMPARE_FUNC_EQUAL    (5),
        COMPARE_FUNC_NOTEQUAL (6),
        COMPARE_FUNC_ALWAYS   (7);

        private final int value;
        private CompareFunc(int v) { this.value = v; }
        public int getValue() { return this.value; }
    };

    public enum FaceWinding
    {
        FACE_WINDING_CCW (0),
        FACE_WINDING_CW  (1);

        private final int value;
        private FaceWinding(int v) { this.value = v; }
        public int getValue() { return this.value; }
    };

    public enum StencilOp
    {
        STENCIL_OP_KEEP      (0),
        STENCIL_OP_ZERO      (1),
        STENCIL_OP_REPLACE   (2),
        STENCIL_OP_INCR      (3),
        STENCIL_OP_INCR_WRAP (4),
        STENCIL_OP_DECR      (5),
        STENCIL_OP_DECR_WRAP (6),
        STENCIL_OP_INVERT    (7);

        private final int value;
        private StencilOp(int v) { this.value = v; }
        public int getValue() { return this.value; }
    };

    static public class StencilTestFunc extends Structure {
        public int m_Func;      // dmGraphics::CompareFunc
        public int m_OpSFail;   // dmGraphics::StencilOp
        public int m_OpDPFail;  // dmGraphics::StencilOp
        public int m_OpDPPass;  // dmGraphics::StencilOp
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"m_Func", "m_OpSFail", "m_OpDPFail", "m_OpDPPass"});
        }
    }

    static public class StencilTestParams extends Structure {
        public StencilTestFunc m_Front;
        public StencilTestFunc m_Back;
        public byte    m_Ref;
        public byte    m_RefMask;
        public byte    m_BufferMask;
        public byte    m_ColorBufferMask;
        public byte    m_ClearBuffer;      // bool
        public byte    m_SeparateFaceStates; // bool
        public byte[]  pad = new byte[32 - 6];
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {
                "m_Front", "m_Back",
                "m_Ref", "m_RefMask", "m_BufferMask",
                "m_ColorBufferMask", "m_ClearBuffer", "m_SeparateFaceStates", "pad"});
        }
    }

    static public class Matrix4 extends Structure {
        public float[] m = new float[16];
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"m"});
        }
    }

    static public class Vector4 extends Structure {
        public float x, y, z, w;
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"x","y","z","w"});
        }
    }

    static public class ShaderConstant extends Structure {
        public Vector4 m_Value;
        public long m_NameHash;
        public long pad1;
        protected List getFieldOrder() {
            return Arrays.asList(new String[] {"m_Value", "m_NameHash", "pad1"});
        }
    }

    // Matching the layout 1:1 with the struct in vertices.h
    static public class RenderObject extends Structure {
        public StencilTestParams    m_StencilTestParams;
        public Matrix4              m_WorldTransform; // 16 byte alignment for simd
        public ShaderConstant[]     m_Constants = new ShaderConstant[4];
        public int                  m_NumConstants;
        public int                  m_VertexStart;
        public int                  m_VertexCount;
        public int                  m_BlendFactor;
        public byte                 m_SetBlendFactors;
        public byte                 m_SetStencilTest;
        public byte                 m_SetFaceWinding;
        public byte                 m_FaceWindingCCW;
        public byte                 m_UseIndexBuffer;
        public byte                 m_IsTriangleStrip;
        public byte[]               pad2 = new byte[(4*4) - 6];

        protected List getFieldOrder() {
            return Arrays.asList(new String[] {
                "m_StencilTestParams", "m_WorldTransform", "m_Constants",
                "m_NumConstants", "m_VertexStart", "m_VertexCount", "m_BlendFactor",
                "m_SetBlendFactors", "m_SetStencilTest", "m_SetFaceWinding", "m_FaceWindingCCW", "m_UseIndexBuffer", "m_IsTriangleStrip", "pad2"});
        }

        public int getOffset(String name) {
            return super.fieldOffset(name);
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
    public static native RenderObject SPINE_GetRenderObjectData(SpinePointer spine, IntByReference objectCount);
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
        if (first == null)
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

    // public static int[] SPINE_GetIndexBuffer(SpinePointer spine) {
    //     IntByReference pcount = new IntByReference();
    //     IntByReference p = SPINE_GetIndexBufferData(spine, pcount);
    //     if (pcount == null || p == null)
    //     {
    //         System.out.printf("Index buffer is empty!");
    //         return new int[0];
    //     }
    //     return p.getPointer().getIntArray(0, pcount.getValue());
    // }

    public static RenderObject[] SPINE_GetRenderObjects(SpinePointer spine) {
        IntByReference pcount = new IntByReference();
        RenderObject first = SPINE_GetRenderObjectData(spine, pcount);
        if (first == null)
        {
            System.out.printf("Render object buffer is empty!");
            return new RenderObject[0];
        }

        int ro_size = 288;
        if (first.size() != ro_size) {
            System.out.printf("RenderObject size is not %d, it was %d\n", ro_size, first.size());
            return new RenderObject[0];
        }

        return (RenderObject[])first.toArray(pcount.getValue());
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

        SPINE_UpdateVertices(p, 0.0f);

        int count = 0;
        SpineVertex[] vertices = SPINE_GetVertexBuffer(p);

        System.out.printf("Vertices: count: %d  size: %d bytes\n", vertices.length, vertices.length>0 ? vertices.length * vertices[0].size() : 0);

        for (SpineVertex vertex : vertices) {
            if (count > 10) {
                System.out.printf(" ...\n");
                break;
            }
            System.out.printf(" vertex %d: %.4f, %.4f\n", count++, vertex.x, vertex.y);
        }

        count = 0;
        RenderObject[] ros = SPINE_GetRenderObjects(p);

        System.out.printf("Render Objects: count %d\n", ros.length);
        for (RenderObject ro : ros) {
            if (count > 10) {
                System.out.printf(" ...\n");
                break;
            }

            System.out.printf(" ro %d: fw(ccw): %b  offset: %d  count: %d  constants: %d\n", count++, ro.m_FaceWindingCCW, ro.m_VertexStart, ro.m_VertexCount, ro.m_NumConstants);

            for (int i = 0; i < ro.m_NumConstants && i < 2; ++i)
            {
                System.out.printf("    var %d: %s %.3f, %.3f, %.3f, %.3f\n", i, Long.toUnsignedString(ro.m_Constants[i].m_NameHash), ro.m_Constants[i].m_Value.x, ro.m_Constants[i].m_Value.y, ro.m_Constants[i].m_Value.z, ro.m_Constants[i].m_Value.w);
            }
        }
    }
}
