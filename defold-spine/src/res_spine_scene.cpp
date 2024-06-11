#include "res_spine_scene.h"
#include "res_spine_json.h"
#include "spine_ddf.h" // generated from the spine_ddf.proto

#include <common/spine_loader.h>

#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/resource/resource.h>

#include <spine/SkeletonJson.h>
#include <spine/AnimationStateData.h>
#include <dmsdk/gamesys/resources/res_textureset.h>

// Also see the guide http://esotericsoftware.com/spine-c#Loading-skeleton-data

#if 0
#define DEBUGLOG(...) dmLogWarning("DEBUG: " __VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif

namespace dmSpine
{

    static dmResource::Result AcquireResources(dmResource::HFactory factory, SpineSceneResource* resource, const char* filename)
    {
        dmResource::Result result = dmResource::Get(factory, resource->m_Ddf->m_Atlas, (void**) &resource->m_TextureSet); // .atlas -> .texturesetc
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }

        SpineJsonResource* spine_json_resource = 0;
        result = dmResource::Get(factory, resource->m_Ddf->m_SpineJson, (void**) &spine_json_resource);
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }

        // Create a 1:1 mapping between animation frames and regions in a format that is spine friendly
        resource->m_Regions = dmSpine::CreateRegions(resource->m_TextureSet->m_TextureSet);
        resource->m_AttachmentLoader = dmSpine::CreateAttachmentLoader(resource->m_TextureSet->m_TextureSet, resource->m_Regions);

        // Create the spine resource
        resource->m_Skeleton = dmSpine::ReadSkeletonJsonData((spAttachmentLoader*)resource->m_AttachmentLoader, filename, spine_json_resource->m_Json);
        if (!resource->m_Skeleton)
        {
            return dmResource::RESULT_INVALID_DATA;
        }

        resource->m_AnimationStateData = spAnimationStateData_create(resource->m_Skeleton);
        //spAnimationStateData_setDefaultMix(resource->m_AnimationStateData, 0.1f); // There's currently no such function!
        resource->m_AnimationStateData->defaultMix = 0.1f; // force mixing

        // We can release this json data now
        dmResource::Release(factory, spine_json_resource);

        {
            uint32_t count = resource->m_Skeleton->animationsCount;
            resource->m_AnimationNameToIndex.SetCapacity(dmMath::Max(1U, count/3), count);
            for (int n = 0; n < count; ++n)
            {
                dmhash_t name_hash = dmHashString64(resource->m_Skeleton->animations[n]->name);
                resource->m_AnimationNameToIndex.Put(name_hash, n);
                DEBUGLOG("anim: %d %s", n, resource->m_Skeleton->animations[n]->name);
            }
        }

        {
            uint32_t count = resource->m_Skeleton->skinsCount;
            resource->m_SkinNameToIndex.SetCapacity(dmMath::Max(1U, count/3), count);
            resource->m_AttachmentHashToName.SetCapacity(17,32);
            for (int n = 0; n < count; ++n)
            {
                spSkin* skin = resource->m_Skeleton->skins[n];
                dmhash_t name_hash = dmHashString64(skin->name);
                resource->m_SkinNameToIndex.Put(name_hash, n);

                DEBUGLOG("skin: %d %s", n, skin->name);
                spSkinEntry* entry = spSkin_getAttachments(skin);
                while(entry)
                {
                    DEBUGLOG("attachment: %s  slot: %d", entry->name, entry->slotIndex);
                    if (resource->m_AttachmentHashToName.Full())
                    {
                        uint32_t capacity = resource->m_AttachmentHashToName.Capacity() + 16;
                        resource->m_AttachmentHashToName.SetCapacity(capacity/2+1, capacity);
                    }
                    resource->m_AttachmentHashToName.Put(dmHashString64(entry->name), entry->name);
                    entry = entry->next;
                }
            }
        }

        {
            uint32_t count = resource->m_Skeleton->slotsCount;
            resource->m_SlotNameToIndex.SetCapacity(dmMath::Max(1U, count/3), count);
            for (int n = 0; n < count; ++n)
            {
                dmhash_t name_hash = dmHashString64(resource->m_Skeleton->slots[n]->name);
                resource->m_SlotNameToIndex.Put(name_hash, n);
                DEBUGLOG("slot: %d %s", n, resource->m_Skeleton->slots[n]->name);
            }
        }

        {
            uint32_t count = resource->m_Skeleton->ikConstraintsCount;
            resource->m_IKNameToIndex.SetCapacity(dmMath::Max(1U, count/3), count);
            for (int n = 0; n < count; ++n)
            {
                dmhash_t name_hash = dmHashString64(resource->m_Skeleton->ikConstraints[n]->name);
                resource->m_IKNameToIndex.Put(name_hash, n);
                DEBUGLOG("ik: %d %s", n, resource->m_Skeleton->ikConstraints[n]->name);
            }
        }

        return dmResource::RESULT_OK;
    }

    static void ReleaseResources(dmResource::HFactory factory, SpineSceneResource* resource)
    {
        if (resource->m_Ddf)
            dmDDF::FreeMessage(resource->m_Ddf);
        if (resource->m_TextureSet)
            dmResource::Release(factory, resource->m_TextureSet);

        if (resource->m_AnimationStateData)
            spAnimationStateData_dispose(resource->m_AnimationStateData);
        if (resource->m_Skeleton)
            spSkeletonData_dispose(resource->m_Skeleton);
        dmSpine::Dispose(resource->m_AttachmentLoader);
        delete[] resource->m_Regions;
    }

    static dmResource::Result ResourceTypeScene_Preload(const dmResource::ResourcePreloadParams* params)
    {
        dmGameSystemDDF::SpineSceneDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params->m_Buffer, params->m_BufferSize, &dmGameSystemDDF_SpineSceneDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }

        dmResource::PreloadHint(params->m_HintInfo, ddf->m_SpineJson);
        dmResource::PreloadHint(params->m_HintInfo, ddf->m_Atlas);

        *params->m_PreloadData = ddf;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeScene_Create(const dmResource::ResourceCreateParams* params)
    {
        SpineSceneResource* scene_resource = new SpineSceneResource();
        scene_resource->m_Ddf = (dmGameSystemDDF::SpineSceneDesc*) params->m_PreloadData;
        dmResource::Result r = AcquireResources(params->m_Factory, scene_resource, params->m_Filename);
        if (r == dmResource::RESULT_OK)
        {
            dmResource::SetResource(params->m_Resource, scene_resource);
        }
        else
        {
            ReleaseResources(params->m_Factory, scene_resource);
            delete scene_resource;
        }
        return r;
    }

    static dmResource::Result ResourceTypeScene_Destroy(const dmResource::ResourceDestroyParams* params)
    {
        SpineSceneResource* scene_resource = (SpineSceneResource*)dmResource::GetResource(params->m_Resource);
        ReleaseResources(params->m_Factory, scene_resource);
        delete scene_resource;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeScene_Recreate(const dmResource::ResourceRecreateParams* params)
    {
        dmGameSystemDDF::SpineSceneDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params->m_Buffer, params->m_BufferSize, &dmGameSystemDDF_SpineSceneDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }
        SpineSceneResource* resource = (SpineSceneResource*)dmResource::GetResource(params->m_Resource);
        ReleaseResources(params->m_Factory, resource);
        resource->m_Ddf = ddf;
        return AcquireResources(params->m_Factory, resource, params->m_Filename);
    }

    static ResourceResult ResourceTypeScene_Register(HResourceTypeContext ctx, HResourceType type)
    {
        return (ResourceResult)dmResource::SetupType(ctx,
                                                   type,
                                                   0, // context
                                                   ResourceTypeScene_Preload,
                                                   ResourceTypeScene_Create,
                                                   0, // post create
                                                   ResourceTypeScene_Destroy,
                                                   ResourceTypeScene_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineSceneExt, "spinescenec", dmSpine::ResourceTypeScene_Register, 0);
