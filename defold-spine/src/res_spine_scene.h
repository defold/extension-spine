#ifndef DM_RES_SPINE_SCENE_H
#define DM_RES_SPINE_SCENE_H

#include <dmsdk/gamesys/resources/res_textureset.h>
#include <dmsdk/dlib/hashtable.h>

struct spAtlasRegion;
struct spSkeletonData;
struct spAnimationStateData;

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
    struct spDefoldAtlasAttachmentLoader;

    struct SpineSceneResource
    {
        dmGameSystemDDF::SpineSceneDesc*    m_Ddf;
        dmGameSystem::TextureSetResource*   m_TextureSet;   // The atlas
        spAtlasRegion*                      m_Regions;      // Maps 1:1 with the atlas animations array
        spSkeletonData*                     m_Skeleton;     // the .spinejson file
        spAnimationStateData*               m_AnimationStateData;
        spDefoldAtlasAttachmentLoader*      m_AttachmentLoader;
        dmHashTable64<uint32_t>             m_AnimationNameToIndex;
        dmHashTable64<uint32_t>             m_SkinNameToIndex;
        dmHashTable64<uint32_t>             m_SlotNameToIndex;
        dmHashTable64<uint32_t>             m_IKNameToIndex;
        dmHashTable64<const char*>          m_AttachmentHashToName; // makes it easy for us to do a reverse hash for attachments
    };
}

#endif // DM_RES_SPINE_SCENE_H
