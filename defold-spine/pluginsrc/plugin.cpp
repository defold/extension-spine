


// // Fix for "error: undefined symbol: __declspec(dllimport) longjmp" from libtess2
// #if defined(_MSC_VER)
// #include <setjmp.h>
// static jmp_buf jmp_buffer;
// __declspec(dllexport) int dummyFunc()
// {
//     int r = setjmp(jmp_buffer);
//     if (r == 0) longjmp(jmp_buffer, 1);
//     return r;
// }
// #endif

#include <stdio.h>
#include <stdint.h>

#include <dmsdk/sdk.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/dlib/dstrings.h>
#include <dmsdk/dlib/shared_library.h>
#include <dmsdk/dlib/static_assert.h>
#include <dmsdk/ddf/ddf.h>
#include <dmsdk/gamesys/resources/res_textureset.h>
#include <gamesys/texture_set_ddf.h>

#include <common/spine_loader.h>
#include <common/vertices.h>
#include "renderobject.h"

#include <spine/AnimationStateData.h>
#include <spine/AnimationState.h>
#include <spine/SkeletonData.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonClipping.h>

static const dmhash_t UNIFORM_TINT = dmHashString64("tint");

struct AABB
{
    float minX, minY, maxX, maxY;
};

struct SpineBone // a copy of the info from the spBone
{
    const char* name;
    int   parent;
    float posX, posY, rotation, scaleX, scaleY, length;
};

struct SpineFile
{
    const char*                             m_Path;
    // Base data
    spAtlasRegion*                          m_AtlasRegions;
    spSkeletonData*                         m_SkeletonData;
    spAnimationStateData*                   m_AnimationStateData;
    dmSpine::spDefoldAtlasAttachmentLoader* m_AttachmentLoader;
    dmArray<const char*>                    m_AnimationNames;
    dmArray<const char*>                    m_SkinNames;
    // Instance data
    spSkeleton*                             m_SkeletonInstance;
    spAnimationState*                       m_AnimationStateInstance;
    dmArray<SpineBone>                      m_Bones;
    // Render data
    dmArray<dmSpine::SpineVertex>           m_VertexBuffer;
    dmArray<dmSpinePlugin::RenderObject>    m_RenderObjects;
    dmhash_t                                m_CurrentSkin;
    dmhash_t                                m_CurrentAnimation;

    const char*                             m_Error;

    SpineFile()
    : m_Path(0)
    , m_AtlasRegions(0)
    , m_SkeletonData(0)
    , m_AnimationStateData(0)
    , m_AttachmentLoader(0)
    , m_SkeletonInstance(0)
    , m_AnimationStateInstance(0)
    , m_CurrentSkin(0)
    , m_CurrentAnimation(0)
    , m_Error(0)
    {
    }
};

char g_LastSpineError[1024] = {0};

typedef SpineFile* HSpineFile;

static void UpdateRenderData(SpineFile* file);
static void UpdateVertices(SpineFile* file, float dt);

// Need to free() the buffer
static uint8_t* ReadFile(const char* path, size_t* file_size) {
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        dmLogError("Failed to read file '%s'", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long _file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buffer = (uint8_t*)malloc(_file_size);
    if (fread(buffer, 1, _file_size, f) != (size_t) _file_size)
    {
        fclose(f);
        free(buffer);
        return 0;
    }
    fclose(f);

    if (file_size)
        *file_size = _file_size;
    return buffer;
}

static SpineFile* ToSpineFile(void* _file, const char* fnname)
{
    if (!_file) {
        dmLogError("%s: File handle is null", fnname);
    }
    return (SpineFile*)_file;
}

#define TO_SPINE_FILE(_P_) ToSpineFile(_P_, __FUNCTION__);

#define CHECK_FILE_RETURN(_P_) \
    if (!(_P_) || !(_P_)->m_AnimationStateInstance) { \
        return 0; \
    }

#define CHECK_FILE_RETURN_VALUE(_P_, _VALUE_) \
    if (!(_P_) || !(_P_)->m_AnimationStateInstance) { \
        return (_VALUE_); \
    }

#define CHECK_FILE_RETURN_VOID(_P_) \
    if (!(_P_) || !(_P_)->m_AnimationStateInstance) { \
        return; \
    }


static int FindBoneIndex(spBoneData** const array, int num_bones, const spBoneData* bone)
{
    for (int i = 0; i < num_bones; ++i)
    {
        if (array[i] == bone)
            return i;
    }
    return -1;
}

static void SetupBones(SpineFile* file)
{
    file->m_Bones.SetSize(0);
    if (!file || !file->m_SkeletonData)
        return;

    int num_bones = file->m_SkeletonData->bonesCount;
    spBoneData** const bones = file->m_SkeletonData->bones;
    file->m_Bones.SetCapacity(num_bones);
    file->m_Bones.SetSize(num_bones);

    for (int i = 0; i < num_bones; ++i)
    {
        const spBoneData* bone = bones[i];
        int parent_index = FindBoneIndex(bones, num_bones, bone->parent);

        SpineBone& out = file->m_Bones[i];
        out.name = bone->name;
        out.parent = parent_index;
        out.posX = bone->x;
        out.posY = bone->y;
        out.rotation = bone->rotation;
        out.scaleX = bone->scaleX;
        out.scaleY = bone->scaleY;
        out.length = 1.0f; // TODO: Calculate the length to the parent
    }
}

extern "C" DM_DLLEXPORT void SPINE_Destroy(void* _file);


extern "C" DM_DLLEXPORT const char* SPINE_GetLastError()
{
    return g_LastSpineError;
}

static void SPINE_SetLastError(const char* str)
{
    dmSnPrintf(g_LastSpineError, sizeof(g_LastSpineError), "%s", str);
    dmLogError("%s", g_LastSpineError);
}

static void SPINE_SetLastError(spAttachmentLoader* loader)
{
    dmSnPrintf(g_LastSpineError, sizeof(g_LastSpineError), "%s %s", loader->error1?loader->error1:"", loader->error2?loader->error2:"");
    dmLogError("%s", g_LastSpineError);
}

static dmGameSystemDDF::TextureSet* LoadAtlasFromBuffer(void* buffer, size_t buffer_size, const char* path)
{
    dmGameSystemDDF::TextureSet* texture_set_ddf;
    dmDDF::Result e  = dmDDF::LoadMessage(buffer, (uint32_t)buffer_size, &texture_set_ddf);
    if ( e != dmDDF::RESULT_OK )
    {
        dmLogError("Failed to load atlas from '%s' (result: %d). Has the dmGameSystemDDF::TextureSet file format changed?", path, (int)e);
        return 0;
    }
    return texture_set_ddf;
}

static void DestroyAtlas(dmGameSystemDDF::TextureSet* texture_set_ddf)
{
    dmDDF::FreeMessage(texture_set_ddf);
}

extern "C" DM_DLLEXPORT void* SPINE_LoadFromBuffer(void* json, size_t json_size, const char* path, void* atlas_buffer, size_t atlas_size, const char* atlas_path)
{
    // We map these to ints on the java side (See Spine.java)
    DM_STATIC_ASSERT(sizeof(dmGraphics::CompareFunc) == sizeof(int), Invalid_struct_size);
    DM_STATIC_ASSERT(sizeof(dmGraphics::StencilOp) == sizeof(int), Invalid_struct_size);
    // Making sure the size is the same for both C++ and Java
    DM_STATIC_ASSERT(sizeof(dmSpinePlugin::RenderObject) == 288, Invalid_struct_size);


    dmGameSystemDDF::TextureSet* texture_set_ddf = 0;
    if (atlas_buffer)
    {
        texture_set_ddf = LoadAtlasFromBuffer(atlas_buffer, atlas_size, atlas_path);
        if (!texture_set_ddf)
        {
            dmLogError("Couldn't load atlas '%s' for spine scene '%s'", atlas_path, path);
            return 0;
        }
    }

    SpineFile* file = new SpineFile;

    // Create a 1:1 mapping between animation frames and regions in a format that is spine friendly
    if (texture_set_ddf)
    {
        file->m_AtlasRegions = dmSpine::CreateRegions(texture_set_ddf);
        file->m_AttachmentLoader = dmSpine::CreateAttachmentLoader(texture_set_ddf, file->m_AtlasRegions);
        DestroyAtlas(texture_set_ddf);
    }
    else {
        file->m_AtlasRegions = 0;
        file->m_AttachmentLoader = dmSpine::CreateAttachmentLoader();
    }

    // Create the spine resource
    spAttachmentLoader* attachment_loader = (spAttachmentLoader*)file->m_AttachmentLoader;
    file->m_SkeletonData = dmSpine::ReadSkeletonJsonData(attachment_loader, path, json);
    if (!file->m_SkeletonData)
    {
        if (attachment_loader->error1 || attachment_loader->error2)
            SPINE_SetLastError(attachment_loader);

        dmLogError("Failed to load Spine skeleton from json file %s", path);
        SPINE_Destroy(file);
        return 0;
    }

    file->m_AnimationStateData = spAnimationStateData_create(file->m_SkeletonData);

    file->m_SkeletonInstance = spSkeleton_create(file->m_SkeletonData);
    if (!file->m_SkeletonInstance)
    {
        dmLogError("Failed to create skeleton instance");
        SPINE_Destroy(file);
        return 0;
    }
    spSkeleton_setSkin(file->m_SkeletonInstance, file->m_SkeletonData->defaultSkin);

    file->m_AnimationStateInstance = spAnimationState_create(file->m_AnimationStateData);
    if (!file->m_AnimationStateInstance)
    {
        dmLogError("Failed to create animation state instance");
        SPINE_Destroy(file);
        return 0;
    }

    spSkeleton_setToSetupPose(file->m_SkeletonInstance);
    spSkeleton_updateWorldTransform(file->m_SkeletonInstance, SP_PHYSICS_POSE);

    file->m_Path = strdup(path);

    file->m_AnimationNames.SetCapacity(file->m_SkeletonData->animationsCount);
    file->m_AnimationNames.SetSize(file->m_SkeletonData->animationsCount);
    for (int i = 0; i < file->m_SkeletonData->animationsCount; ++i)
    {
        file->m_AnimationNames[i] = strdup(file->m_SkeletonData->animations[i]->name);
    }

    file->m_SkinNames.SetCapacity(file->m_SkeletonData->skinsCount);
    file->m_SkinNames.SetSize(file->m_SkeletonData->skinsCount);
    for (int i = 0; i < file->m_SkeletonData->skinsCount; ++i)
    {
        file->m_SkinNames[i] = strdup(file->m_SkeletonData->skins[i]->name);
    }

    file->m_CurrentSkin = 0;
    file->m_CurrentAnimation = 0;

    UpdateVertices(file, 0.0f);
    SetupBones(file);

    return (void*)file;
}


extern "C" DM_DLLEXPORT void* SPINE_LoadFromPath(const char* path, const char* atlas_path) {
    size_t buffer_size = 0;
    uint8_t* buffer = ReadFile(path, &buffer_size);
    if (!buffer) {
        dmLogError("%s: Failed to read spine file into buffer", __FUNCTION__);
        return 0;
    }

    size_t atlas_buffer_size = 0;
    uint8_t* atlas_buffer = ReadFile(atlas_path, &atlas_buffer_size);
    if (!buffer) {
        dmLogError("%s: Failed to read atlas file %s", __FUNCTION__, atlas_path);
        return 0;
    }

    void* p = SPINE_LoadFromBuffer(buffer, buffer_size, path, atlas_buffer, atlas_buffer_size, atlas_path);
    free(buffer);
    free(atlas_buffer);
    return p;
}

extern "C" DM_DLLEXPORT void SPINE_Destroy(void* _file) {
    SpineFile* file = (SpineFile*)_file;
    if (file == 0)
    {
        return;
    }

    if (file->m_AnimationStateInstance)
        spAnimationState_dispose(file->m_AnimationStateInstance);
    if (file->m_SkeletonInstance)
        spSkeleton_dispose(file->m_SkeletonInstance);

    if (file->m_AnimationStateData)
        spAnimationStateData_dispose(file->m_AnimationStateData);
    if (file->m_SkeletonData)
        spSkeletonData_dispose(file->m_SkeletonData);
    dmSpine::Dispose(file->m_AttachmentLoader);

    free((void*)file->m_Path);

    delete file;
}

extern "C" DM_DLLEXPORT int32_t SPINE_GetNumAnimations(void* _file) {
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    return file->m_SkeletonInstance ? file->m_SkeletonData->animationsCount : 0;
}

extern "C" DM_DLLEXPORT const char* SPINE_GetAnimation(void* _file, int i) {
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    if (!file->m_SkeletonInstance)
        return 0;

    if (i < 0 || i >= file->m_SkeletonData->animationsCount) {
        dmLogError("%s: Animation index %d is not in range [0, %d]", __FUNCTION__, i, file->m_SkeletonData->animationsCount);
        return 0;
    }

    return file->m_SkeletonData->animations[i]->name;
}


extern "C" DM_DLLEXPORT int32_t SPINE_GetNumBones(void* _file) {
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    return file->m_Bones.Size();
}

extern "C" DM_DLLEXPORT void SPINE_GetBoneInternal(void* _file, int i, SpineBone* outbone) {
    SpineFile* file = TO_SPINE_FILE(_file);
    if (!file) {
        return;
    }

    if (i < 0 || i >= (int)file->m_Bones.Size()) {
        dmLogError("%s: Bone index %d is not in range [0, %u]", __FUNCTION__, i, (uint32_t)file->m_Bones.Size());
        return;
    }

    *outbone = file->m_Bones[i];
    (*outbone).name = strdup((*outbone).name); // Java will delete this
}

extern "C" DM_DLLEXPORT int SPINE_GetNumChildBones(void* _file, int bone_index)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    if (bone_index < 0 || bone_index >= (int)file->m_Bones.Size()) {
        dmLogError("%s: Bone index %d is not in range [0, %u]", __FUNCTION__, bone_index, (uint32_t)file->m_Bones.Size());
        return 0;
    }

    SpineBone* bone = &file->m_Bones[bone_index];
    uint32_t size = file->m_Bones.Size();
    uint32_t count = 0;
    for (int i = 0; i < size; ++i)
    {
        SpineBone* node = &file->m_Bones[i];
        if (node->parent == bone_index)
        {
            ++count;
        }
    }
    return count;
}

extern "C" DM_DLLEXPORT int SPINE_GetChildBone(void* _file, int bone_index, int child_index)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    if (bone_index < 0 || bone_index >= (int)file->m_Bones.Size()) {
        dmLogError("%s: Bone index %d is not in range [0, %u)", __FUNCTION__, bone_index, (uint32_t)file->m_Bones.Size());
        return -1;
    }

    SpineBone* bone = &file->m_Bones[bone_index];

    uint32_t size = file->m_Bones.Size();
    uint32_t count = 0;
    for (int i = 0; i < size; ++i)
    {
        SpineBone* node = &file->m_Bones[i];
        if (node->parent != bone_index)
            continue;
        if (count == child_index)
            return i;
        ++count;
    }

    dmLogError("%s: Bone child index %d is not in range [0, %u)", __FUNCTION__, child_index, count);
    return -1;
}

///////////////////////////////////////////////////////////////////////////////

static void CreateAABB(SpineFile* file);

extern "C" DM_DLLEXPORT int SPINE_GetVertexSize() {
    return sizeof(dmSpine::SpineVertex);
}

extern "C" DM_DLLEXPORT void SPINE_UpdateVertices(void* _file, float dt) {
    SpineFile* file = TO_SPINE_FILE(_file);
    UpdateVertices(file, dt);
}

static void UpdateVertices(SpineFile* file, float dt)
{
    if (!file || !file->m_AnimationStateInstance) {
        if (file->m_VertexBuffer.Empty()) {
            CreateAABB(file);
        }
        return;
    }

    spAnimationState_update(file->m_AnimationStateInstance, dt);
    spAnimationState_apply(file->m_AnimationStateInstance, file->m_SkeletonInstance);
    spSkeleton_update(file->m_SkeletonInstance, dt);
    spSkeleton_updateWorldTransform(file->m_SkeletonInstance, SP_PHYSICS_UPDATE);

    UpdateRenderData(file); // Update the draw call list
}

extern "C" DM_DLLEXPORT dmSpine::SpineVertex* SPINE_GetVertexBufferData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_VertexBuffer.Size();
    return file->m_VertexBuffer.Begin();
}

extern "C" DM_DLLEXPORT dmSpinePlugin::RenderObject* SPINE_GetRenderObjectData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_RenderObjects.Size();
    return file->m_RenderObjects.Begin();
}

extern "C" DM_DLLEXPORT const char** SPINE_GetAnimationData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_AnimationNames.Size();
    return file->m_AnimationNames.Begin();
}

extern "C" DM_DLLEXPORT const char** SPINE_GetSkinData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_SkinNames.Size();
    return file->m_SkinNames.Begin();
}

extern "C" DM_DLLEXPORT void SPINE_SetSkin(void* _file, const char* skin)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN_VOID(file);
    dmhash_t name_hash = dmHashString64(skin);
    if (name_hash == file->m_CurrentSkin)
        return;

    file->m_CurrentSkin = name_hash;
    if (!spSkeleton_setSkinByName(file->m_SkeletonInstance, skin))
    {
        dmLogError("Failed to set skin '%s' to spine instance '%s'", skin, file->m_Path);
        return;
    }
    spSkeleton_setSlotsToSetupPose(file->m_SkeletonInstance);
}

extern "C" DM_DLLEXPORT void SPINE_SetAnimation(void* _file, const char* animation)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN_VOID(file);
    dmhash_t name_hash = dmHashString64(animation);
    if (name_hash == file->m_CurrentAnimation)
        return;
    file->m_CurrentAnimation = name_hash;

    int loop = 1; // we're in the editor, so we want it to loop
    int track = 0; // In the editor, we're only playing a single animation
    spAnimationState_setAnimationByName(file->m_AnimationStateInstance, track, animation, loop);
}

extern "C" DM_DLLEXPORT AABB SPINE_GetAABB(void* _file)
{
    AABB aabb;

    SpineFile* file = TO_SPINE_FILE(_file);
    if (!file) {
        aabb.minX = 0;
        aabb.minY = 0;
        aabb.maxX = 100;
        aabb.maxY = 100;
        return aabb;
    }

    float minx = 100000.0f;
    float maxx = -100000.0f;
    float miny = 100000.0f;
    float maxy = -100000.0f;

    uint32_t size = file->m_VertexBuffer.Size();
    for (uint32_t i = 0; i < size; ++i)
    {
        const dmSpine::SpineVertex& vtx = file->m_VertexBuffer[i];
        minx = dmMath::Min(minx, vtx.x);
        maxx = dmMath::Max(maxx, vtx.x);
        miny = dmMath::Min(miny, vtx.y);
        maxy = dmMath::Max(maxy, vtx.y);
    }

    aabb.minX = minx;
    aabb.minY = miny;
    aabb.maxX = maxx;
    aabb.maxY = maxy;
    return aabb;
}


static void CreateAABB(SpineFile* file)
{
    if (file->m_VertexBuffer.Capacity() < 6)
        file->m_VertexBuffer.SetCapacity(6);
    file->m_VertexBuffer.SetSize(6);

    float width = 200;
    float height = 200;

    float minx = -width;
    float miny = -height;
    float maxx = width;
    float maxy = height;

    dmSpine::SpineVertex* v = file->m_VertexBuffer.Begin();

    v->x = minx;
    v->y = miny;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;

    v->x = maxx;
    v->y = miny;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;

    v->x = maxx;
    v->y = maxy;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;


    v->x = maxx;
    v->y = maxy;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;

    v->x = minx;
    v->y = maxy;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;

    v->x = minx;
    v->y = miny;
    v->z = 0;
    v->u = v->v = 0;
    v->r = v->g = v->b = v->a = 1;
    v->page_index = 0;
    ++v;
}

// *******************************************************************************************************


template<typename T>
static T* AdjustArraySize(dmArray<T>& array, uint32_t count)
{
    if (array.Remaining() < count)
    {
        array.OffsetCapacity(count - array.Remaining());
    }

    T* p = array.End();
    array.SetSize(array.Size()+count); // no reallocation, since we've preallocated
    return p;
}

static void UpdateRenderData(SpineFile* file)
{
    if (!file || !file->m_AnimationStateInstance)
        return;

    // reset the buffers (corresponding to the dmRender::RENDER_LIST_OPERATION_BEGIN)
    // we don't need to share the index/vertex buffer with another instance so
    file->m_RenderObjects.SetSize(0);
    file->m_VertexBuffer.SetSize(0);

    spSkeletonClipping* clipper = spSkeletonClipping_create();

    uint32_t ro_count = dmSpine::CalcDrawDescCount(file->m_SkeletonInstance);
    AdjustArraySize(file->m_RenderObjects, ro_count);

    dmArray<dmSpine::SpineDrawDesc> draw_descs;
    draw_descs.SetCapacity(ro_count);

    dmVMath::Matrix4 transform = dmVMath::Matrix4::identity();
    dmSpine::GenerateVertexData(file->m_VertexBuffer, file->m_SkeletonInstance, clipper, transform, &draw_descs);

    dmArray<dmSpine::SpineDrawDesc> merged_draw_descs;
    MergeDrawDescs(draw_descs, merged_draw_descs);

    file->m_RenderObjects.SetSize(merged_draw_descs.Size());

    for (int i = 0; i < merged_draw_descs.Size(); ++i)
    {
        dmSpinePlugin::RenderObject& ro = file->m_RenderObjects[i];
        ro.Init();
        ro.m_VertexStart       = merged_draw_descs[i].m_VertexStart; // byte offset
        ro.m_VertexCount       = merged_draw_descs[i].m_VertexCount;
        ro.m_SetStencilTest    = 0;
        ro.m_UseIndexBuffer    = 0;
        ro.m_IsTriangleStrip   = 0; // 0 == GL_TRIANGLES, 1 == GL_TRIANGLE_STRIP
        ro.m_BlendFactor       = merged_draw_descs[i].m_BlendMode;
        ro.m_SetFaceWinding    = 0;
        ro.m_FaceWindingCCW    = dmGraphics::FACE_WINDING_CCW;

        //ro.AddConstant(UNIFORM_TINT, dmVMath::Vector4(1.0f, 1.0f, 1.0f, 1.0f));

        ro.m_WorldTransform = transform;
    }

    spSkeletonClipping_dispose(clipper);
}
