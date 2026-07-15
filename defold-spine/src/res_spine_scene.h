#ifndef DM_RES_SPINE_SCENE_H
#define DM_RES_SPINE_SCENE_H

#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/hashtable.h>
#include <dmsdk/resource/resource.h>

#include <spine/Attachment.h>
#include <spine/AnimationStateData.h>
#include <spine/Atlas.h>
#include <spine/SkeletonData.h>

namespace dmGameSystemDDF
{
    struct SpineSceneDesc;
}

namespace dmGameSystem
{
    struct TextureSetResource;
}

namespace dmSpine
{
    class DefoldAtlasAttachmentLoader;

    // One immutable generation of a Spine scene. Runtime Skeleton and
    // AnimationState instances retain the generation they were created from,
    // so a resource reload can publish new data without invalidating them.
    struct SpineSceneData
    {
        dmResource::HFactory                m_Factory;
        uint32_t                            m_RefCount;
        uint32_t                            m_Generation;
        dmGameSystemDDF::SpineSceneDesc*    m_Ddf;
        dmGameSystem::TextureSetResource*   m_TextureSet;   // The atlas
        spine::AtlasRegion*                 m_Regions;      // Maps 1:1 with the atlas animations array
        spine::SkeletonData*                m_Skeleton;     // the .spinejson file
        spine::AnimationStateData*          m_AnimationStateData;
        DefoldAtlasAttachmentLoader*        m_AttachmentLoader;
        dmHashTable64<uint32_t>             m_AnimationNameToIndex;
        dmHashTable64<uint32_t>             m_SkinNameToIndex;
        dmHashTable64<uint32_t>             m_SlotNameToIndex;
        dmHashTable64<uint32_t>             m_IKNameToIndex;
        dmHashTable64<const char*>          m_AttachmentHashToName; // makes it easy for us to do a reverse hash for attachments
        dmArray<char*>                      m_AttachmentNames;      // stable copies; Spine skins can remove their attachment map entries at runtime
        dmArray<spine::Attachment*>         m_RetainedAttachments; // detached skin attachments kept alive for live Skeleton instances

        SpineSceneData();
    };

    // The resource pointer is stable across recreates. Only m_Data changes.
    struct SpineSceneResource
    {
        SpineSceneData* m_Data;

        SpineSceneResource() : m_Data(0) {}
    };

    SpineSceneData* RetainSceneData(SpineSceneResource* resource);
    void RetainSceneData(SpineSceneData* data);
    void ReleaseSceneData(SpineSceneData* data);

    // Keep detached attachments alive in the same generation retained by the
    // Skeleton instance that may still reference them.
    void RetainSceneAttachment(SpineSceneData* data, spine::Attachment* attachment);
}

#endif // DM_RES_SPINE_SCENE_H
