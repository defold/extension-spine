


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
#include <stdlib.h>
#include <string.h>

#include <dmsdk/sdk.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/dlib/dstrings.h>
#include <dmsdk/dlib/shared_library.h>
#include <dmsdk/ddf/ddf.h>
#include <dmsdk/gamesys/resources/res_textureset.h>
#include <gamesys/texture_set_ddf.h>

#include <common/spine_loader.h>
#include <common/vertices.h>

#include <spine/AnimationStateData.h>
#include <spine/AnimationState.h>
#include <spine/Animation.h>
#include <spine/Atlas.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/Physics.h>
#include <spine/SkeletonData.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonRenderer.h>
#include <spine/Skin.h>

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
    spine::AtlasRegion*                     m_AtlasRegions;
    spine::SkeletonData*                    m_SkeletonData;
    spine::AnimationStateData*              m_AnimationStateData;
    dmSpine::DefoldAtlasAttachmentLoader*   m_AttachmentLoader;
    dmArray<const char*>                    m_AnimationNames;
    dmArray<const char*>                    m_SkinNames;
    // Instance data
    spine::Skeleton*                        m_SkeletonInstance;
    spine::AnimationState*                  m_AnimationStateInstance;
    spine::SkeletonRenderer*                m_SkeletonRenderer;
    dmArray<SpineBone>                      m_Bones;
    // Render data
    dmArray<dmSpine::SpineVertex>           m_VertexBuffer;
    dmArray<uint32_t>                       m_IndexBuffer;
    dmArray<dmSpine::SpineIndexedDrawDesc>  m_DrawDescs;
    dmArray<dmSpine::SpineIndexedDrawDesc>  m_DrawDescScratch;
    dmArray<dmSpine::SpineIndexedDrawDesc>  m_MergedDrawDescScratch;
    dmArray<float>                           m_GeometryScratch;
    uint32_t                                m_VertexBufferVersion;
    uint32_t                                m_IndexBufferVersion;
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
    , m_SkeletonRenderer(0)
    , m_VertexBufferVersion(0)
    , m_IndexBufferVersion(0)
    , m_CurrentSkin(0)
    , m_CurrentAnimation(0)
    , m_Error(0)
    {
    }
};

char g_LastSpineError[1024] = {0};

typedef SpineFile* HSpineFile;

static void UpdateRenderData(SpineFile* file, const dmVMath::Matrix4& transform, const dmVMath::Vector4& color_tint, bool use_index_buffer);
static void UpdateVertices(SpineFile* file, float dt, const dmVMath::Matrix4& transform, const dmVMath::Vector4& color_tint, bool use_index_buffer);

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

static dmVMath::Matrix4 ToMatrix4(const float* m)
{
    if (!m)
    {
        return dmVMath::Matrix4::identity();
    }

    return dmVMath::Matrix4(dmVMath::Vector4(m[0], m[1], m[2], m[3]),
                            dmVMath::Vector4(m[4], m[5], m[6], m[7]),
                            dmVMath::Vector4(m[8], m[9], m[10], m[11]),
                            dmVMath::Vector4(m[12], m[13], m[14], m[15]));
}

static dmVMath::Vector4 ToVector4(const float* v)
{
    if (!v)
    {
        return dmVMath::Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    return dmVMath::Vector4(v[0], v[1], v[2], v[3]);
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


static int FindBoneIndex(const spine::Array<spine::BoneData*>& array, const spine::BoneData* bone)
{
    for (uint32_t i = 0; i < array.size(); ++i)
    {
        if (array[i] == bone)
            return (int)i;
    }
    return -1;
}

static void SetupBones(SpineFile* file)
{
    file->m_Bones.SetSize(0);
    if (!file || !file->m_SkeletonData)
        return;

    spine::Array<spine::BoneData*>& bones = file->m_SkeletonData->getBones();
    uint32_t num_bones = (uint32_t)bones.size();
    file->m_Bones.SetCapacity(num_bones);
    file->m_Bones.SetSize(num_bones);

    for (uint32_t i = 0; i < num_bones; ++i)
    {
        spine::BoneData* bone = bones[i];
        spine::BonePose& setup_pose = bone->getSetupPose();
        int parent_index = FindBoneIndex(bones, bone->getParent());

        SpineBone& out = file->m_Bones[i];
        out.name = bone->getName().buffer();
        out.parent = parent_index;
        out.posX = setup_pose.getX();
        out.posY = setup_pose.getY();
        out.rotation = setup_pose.getRotation();
        out.scaleX = setup_pose.getScaleX();
        out.scaleY = setup_pose.getScaleY();
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
    if (!json)
    {
        SPINE_SetLastError("Spine JSON buffer is null");
        return 0;
    }

    // Spine's C++ runtime defaults to Y-down; Defold uses Y-up coordinates.
    spine::Bone::setYDown(false);

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

    // The Java/editor bridge supplies a sized byte buffer, which is not guaranteed
    // to have the NUL terminator expected by SkeletonJson::readSkeletonData().
    char* json_text = (char*)malloc(json_size + 1);
    if (!json_text)
    {
        SPINE_SetLastError("Failed to allocate Spine JSON buffer");
        SPINE_Destroy(file);
        return 0;
    }
    memcpy(json_text, json, json_size);
    json_text[json_size] = '\0';

    // Create the spine resource.
    file->m_SkeletonData = dmSpine::ReadSkeletonJsonData(file->m_AttachmentLoader, path, json_text);
    free(json_text);
    if (!file->m_SkeletonData)
    {
        const char* loader_error = file->m_AttachmentLoader->GetError();
        if (loader_error && loader_error[0])
            SPINE_SetLastError(loader_error);

        dmLogError("Failed to load Spine skeleton from json file %s", path);
        SPINE_Destroy(file);
        return 0;
    }

    file->m_AnimationStateData = new spine::AnimationStateData(*file->m_SkeletonData);

    file->m_SkeletonInstance = new spine::Skeleton(*file->m_SkeletonData);
    if (!file->m_SkeletonInstance)
    {
        dmLogError("Failed to create skeleton instance");
        SPINE_Destroy(file);
        return 0;
    }
    file->m_SkeletonInstance->setSkin(file->m_SkeletonData->getDefaultSkin());

    file->m_AnimationStateInstance = new spine::AnimationState(*file->m_AnimationStateData);
    if (!file->m_AnimationStateInstance)
    {
        dmLogError("Failed to create animation state instance");
        SPINE_Destroy(file);
        return 0;
    }

    file->m_SkeletonRenderer = new spine::SkeletonRenderer();
    file->m_SkeletonInstance->setupPose();
    file->m_SkeletonInstance->updateWorldTransform(spine::Physics_Pose);

    file->m_Path = strdup(path);

    spine::Array<spine::Animation*>& animations = file->m_SkeletonData->getAnimations();
    file->m_AnimationNames.SetCapacity((uint32_t)animations.size());
    file->m_AnimationNames.SetSize((uint32_t)animations.size());
    for (uint32_t i = 0; i < animations.size(); ++i)
    {
        file->m_AnimationNames[i] = strdup(animations[i]->getName().buffer());
    }

    spine::Array<spine::Skin*>& skins = file->m_SkeletonData->getSkins();
    file->m_SkinNames.SetCapacity((uint32_t)skins.size());
    file->m_SkinNames.SetSize((uint32_t)skins.size());
    for (uint32_t i = 0; i < skins.size(); ++i)
    {
        file->m_SkinNames[i] = strdup(skins[i]->getName().buffer());
    }

    file->m_CurrentSkin = 0;
    file->m_CurrentAnimation = 0;

    UpdateVertices(file, 0.0f, dmVMath::Matrix4::identity(), dmVMath::Vector4(1.0f, 1.0f, 1.0f, 1.0f), true);
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
    uint8_t* atlas_buffer = 0;
    if (atlas_path)
    {
        atlas_buffer = ReadFile(atlas_path, &atlas_buffer_size);
        if (!atlas_buffer) {
            dmLogError("%s: Failed to read atlas file %s", __FUNCTION__, atlas_path);
            free(buffer);
            return 0;
        }
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

    for (uint32_t i = 0; i < file->m_AnimationNames.Size(); ++i)
        free((void*)file->m_AnimationNames[i]);
    for (uint32_t i = 0; i < file->m_SkinNames.Size(); ++i)
        free((void*)file->m_SkinNames[i]);

    delete file->m_AnimationStateInstance;
    delete file->m_SkeletonRenderer;
    delete file->m_SkeletonInstance;
    delete file->m_AnimationStateData;
    delete file->m_SkeletonData;
    dmSpine::Dispose(file->m_AttachmentLoader);
    delete[] file->m_AtlasRegions;

    free((void*)file->m_Path);

    delete file;
}

extern "C" DM_DLLEXPORT int32_t SPINE_GetNumAnimations(void* _file) {
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    return file->m_SkeletonInstance ? (int32_t)file->m_SkeletonData->getAnimations().size() : 0;
}

extern "C" DM_DLLEXPORT const char* SPINE_GetAnimation(void* _file, int i) {
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);

    if (!file->m_SkeletonInstance)
        return 0;

    spine::Array<spine::Animation*>& animations = file->m_SkeletonData->getAnimations();
    if (i < 0 || i >= (int)animations.size()) {
        dmLogError("%s: Animation index %d is not in range [0, %u)", __FUNCTION__, i, (uint32_t)animations.size());
        return 0;
    }

    return animations[i]->getName().buffer();
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

extern "C" DM_DLLEXPORT void SPINE_UpdateVertices(void* _file, float dt, const float* world_transform, const float* color_tint, int use_index_buffer) {
    SpineFile* file = TO_SPINE_FILE(_file);
    UpdateVertices(file, dt, ToMatrix4(world_transform), ToVector4(color_tint), use_index_buffer != 0);
}

static void UpdateVertices(SpineFile* file, float dt, const dmVMath::Matrix4& transform, const dmVMath::Vector4& color_tint, bool use_index_buffer)
{
    if (!file)
        return;

    if (!file->m_AnimationStateInstance) {
        if (file->m_VertexBuffer.Empty()) {
            CreateAABB(file);
        }
        return;
    }

    file->m_AnimationStateInstance->update(dt);
    file->m_AnimationStateInstance->apply(*file->m_SkeletonInstance);
    file->m_SkeletonInstance->update(dt);
    file->m_SkeletonInstance->updateWorldTransform(spine::Physics_Update);

    UpdateRenderData(file, transform, color_tint, use_index_buffer); // Update the draw call list
}

extern "C" DM_DLLEXPORT dmSpine::SpineVertex* SPINE_GetVertexBufferData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_VertexBuffer.Size();
    return file->m_VertexBuffer.Begin();
}

extern "C" DM_DLLEXPORT void* SPINE_GetVertexBufferPointer(void* _file, int* pcount)
{
    return SPINE_GetVertexBufferData(_file, pcount);
}

extern "C" DM_DLLEXPORT uint32_t SPINE_GetVertexBufferVersion(void* _file)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    return file->m_VertexBufferVersion;
}

extern "C" DM_DLLEXPORT uint32_t* SPINE_GetIndexBufferData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_IndexBuffer.Size();
    return file->m_IndexBuffer.Begin();
}

extern "C" DM_DLLEXPORT uint32_t SPINE_GetIndexBufferVersion(void* _file)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    return file->m_IndexBufferVersion;
}

extern "C" DM_DLLEXPORT dmSpine::SpineIndexedDrawDesc* SPINE_GetDrawDescData(void* _file, int* pcount)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN(file);
    *pcount = (int)file->m_DrawDescs.Size();
    return file->m_DrawDescs.Begin();
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
    if (!skin)
        return;

    dmhash_t name_hash = dmHashString64(skin);
    if (name_hash == file->m_CurrentSkin)
        return;

    spine::Skin* new_skin = file->m_SkeletonData->findSkin(skin);
    if (!new_skin)
    {
        dmLogError("Failed to set skin '%s' to spine instance '%s'", skin, file->m_Path);
        return;
    }

    file->m_CurrentSkin = name_hash;
    file->m_SkeletonInstance->setSkin(new_skin);
    file->m_SkeletonInstance->setupPoseSlots();
}

extern "C" DM_DLLEXPORT void SPINE_SetAnimation(void* _file, const char* animation)
{
    SpineFile* file = TO_SPINE_FILE(_file);
    CHECK_FILE_RETURN_VOID(file);
    if (!animation)
        return;

    dmhash_t name_hash = dmHashString64(animation);
    if (name_hash == file->m_CurrentAnimation)
        return;

    spine::Animation* new_animation = file->m_SkeletonData->findAnimation(animation);
    if (!new_animation)
    {
        dmLogError("Failed to set animation '%s' to spine instance '%s'", animation, file->m_Path);
        return;
    }

    file->m_CurrentAnimation = name_hash;
    bool loop = true; // we're in the editor, so we want it to loop
    uint32_t track = 0; // In the editor, we're only playing a single animation
    file->m_AnimationStateInstance->setAnimation(track, *new_animation, loop);
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

    uint32_t size = file->m_VertexBuffer.Size();
    if (size == 0 && file->m_SkeletonInstance)
    {
        dmSpine::SpineModelBounds bounds;
        dmSpine::GetSkeletonBounds(file->m_SkeletonInstance, bounds, file->m_GeometryScratch);
        if (bounds.minX <= bounds.maxX && bounds.minY <= bounds.maxY)
        {
            aabb.minX = bounds.minX;
            aabb.minY = bounds.minY;
            aabb.maxX = bounds.maxX;
            aabb.maxY = bounds.maxY;
            return aabb;
        }

        aabb.minX = 0.0f;
        aabb.minY = 0.0f;
        aabb.maxX = 0.0f;
        aabb.maxY = 0.0f;
        return aabb;
    }

    float minx = 100000.0f;
    float maxx = -100000.0f;
    float miny = 100000.0f;
    float maxy = -100000.0f;

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
    file->m_VertexBufferVersion++;

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


static void UpdateRenderData(SpineFile* file, const dmVMath::Matrix4& transform, const dmVMath::Vector4& color_tint, bool use_index_buffer)
{
    if (!file || !file->m_AnimationStateInstance)
        return;

    // reset the buffers (corresponding to the dmRender::RENDER_LIST_OPERATION_BEGIN)
    // we don't need to share the index/vertex buffer with another instance so
    file->m_DrawDescs.SetSize(0);
    file->m_VertexBuffer.SetSize(0);
    file->m_IndexBuffer.SetSize(0);

    uint32_t draw_desc_count = dmSpine::CalcDrawDescCount(file->m_SkeletonInstance);

    if (use_index_buffer)
    {
        file->m_DrawDescScratch.SetSize(0);
        if (file->m_DrawDescScratch.Capacity() < draw_desc_count)
        {
            uint32_t new_capacity = dmMath::Max(draw_desc_count, dmMath::Max(16U, file->m_DrawDescScratch.Capacity() * 2));
            file->m_DrawDescScratch.SetCapacity(new_capacity);
        }

        dmSpine::GenerateIndexedVertexData(file->m_VertexBuffer, file->m_IndexBuffer, file->m_SkeletonInstance, file->m_SkeletonRenderer, transform, color_tint, &file->m_DrawDescScratch, file->m_GeometryScratch);

        MergeIndexedDrawDescs(file->m_DrawDescScratch, file->m_MergedDrawDescScratch);

        uint32_t merged_draw_desc_count = file->m_MergedDrawDescScratch.Size();
        if (file->m_DrawDescs.Capacity() < merged_draw_desc_count)
        {
            uint32_t new_capacity = dmMath::Max(merged_draw_desc_count, dmMath::Max(16U, file->m_DrawDescs.Capacity() * 2));
            file->m_DrawDescs.SetCapacity(new_capacity);
        }
        file->m_DrawDescs.SetSize(merged_draw_desc_count);
        if (merged_draw_desc_count > 0)
        {
            memcpy(file->m_DrawDescs.Begin(), file->m_MergedDrawDescScratch.Begin(), merged_draw_desc_count * sizeof(dmSpine::SpineIndexedDrawDesc));
        }
    }
    else
    {
        dmSpine::GenerateVertexData(file->m_VertexBuffer, file->m_SkeletonInstance, file->m_SkeletonRenderer, transform, color_tint, 0);
    }

    file->m_VertexBufferVersion++;
    file->m_IndexBufferVersion++;

}
