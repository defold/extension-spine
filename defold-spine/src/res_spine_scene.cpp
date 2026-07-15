#include "res_spine_scene.h"
#include "res_spine_json.h"
#include "spine_ddf.h" // generated from the spine_ddf.proto

#include <string.h>

#include <common/spine_loader.h>

#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/resource/resource.h>

#include <spine/SkeletonJson.h>
#include <spine/Animation.h>
#include <spine/AnimationStateData.h>
#include <spine/ConstraintData.h>
#include <spine/IkConstraintData.h>
#include <spine/Skin.h>
#include <spine/SlotData.h>
#include <dmsdk/gamesys/resources/res_textureset.h>

// Also see the guide http://esotericsoftware.com/spine-c#Loading-skeleton-data

#if 0
#define DEBUGLOG(...) dmLogWarning("DEBUG: " __VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif

namespace dmSpine
{
    SpineSceneData::SpineSceneData()
    : m_Factory(0)
    , m_RefCount(1)
    , m_Generation(1)
    , m_Ddf(0)
    , m_TextureSet(0)
    , m_Regions(0)
    , m_Skeleton(0)
    , m_AnimationStateData(0)
    , m_AttachmentLoader(0)
    {
    }

    template <typename T>
    static void PrepareHashTable(dmHashTable64<T>& table, uint32_t required_capacity)
    {
        uint32_t current_capacity = table.Capacity();
        if (current_capacity > 0)
        {
            table.Clear();
        }
        if (current_capacity < required_capacity)
        {
            table.SetCapacity(dmMath::Max(1U, required_capacity / 3), required_capacity);
        }
    }

    template <typename T>
    static void ClearHashTable(dmHashTable64<T>& table)
    {
        if (table.Capacity() > 0)
        {
            table.Clear();
        }
    }

    static void AddAttachmentName(SpineSceneData* data, const char* attachment_name)
    {
        dmhash_t name_hash = dmHashString64(attachment_name);
        if (data->m_AttachmentHashToName.Get(name_hash))
        {
            return;
        }

        if (data->m_AttachmentHashToName.Full())
        {
            uint32_t capacity = data->m_AttachmentHashToName.Capacity() + 16;
            data->m_AttachmentHashToName.SetCapacity(capacity / 2 + 1, capacity);
        }

        size_t name_length = strlen(attachment_name);
        char* name_copy = new char[name_length + 1];
        memcpy(name_copy, attachment_name, name_length + 1);

        if (data->m_AttachmentNames.Full())
        {
            data->m_AttachmentNames.OffsetCapacity(16);
        }
        data->m_AttachmentNames.Push(name_copy);
        data->m_AttachmentHashToName.Put(name_hash, name_copy);
    }

    static dmResource::Result AcquireResources(dmResource::HFactory factory, SpineSceneData* data, const char* filename)
    {
        data->m_Factory = factory;
        dmResource::Result result = dmResource::Get(factory, data->m_Ddf->m_Atlas, (void**) &data->m_TextureSet); // .atlas -> .texturesetc
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }

        SpineJsonResource* spine_json_resource = 0;
        result = dmResource::Get(factory, data->m_Ddf->m_SpineJson, (void**) &spine_json_resource);
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }

        // Create a 1:1 mapping between animation frames and regions in a format that is spine friendly
        data->m_Regions = dmSpine::CreateRegions(data->m_TextureSet->m_TextureSet);
        data->m_AttachmentLoader = dmSpine::CreateAttachmentLoader(data->m_TextureSet->m_TextureSet, data->m_Regions);

        // Create the spine resource
        data->m_Skeleton = dmSpine::ReadSkeletonJsonData(data->m_AttachmentLoader, filename, spine_json_resource->m_Json);
        dmResource::Release(factory, spine_json_resource);
        if (!data->m_Skeleton)
        {
            return dmResource::RESULT_INVALID_DATA;
        }

        data->m_AnimationStateData = new spine::AnimationStateData(*data->m_Skeleton);
        data->m_AnimationStateData->setDefaultMix(0.1f);

        {
            spine::Array<spine::Animation*>& animations = data->m_Skeleton->getAnimations();
            uint32_t count = (uint32_t)animations.size();
            PrepareHashTable(data->m_AnimationNameToIndex, count);
            for (uint32_t n = 0; n < count; ++n)
            {
                const char* name = animations[n]->getName().buffer();
                dmhash_t name_hash = dmHashString64(name);
                data->m_AnimationNameToIndex.Put(name_hash, n);
                DEBUGLOG("anim: %d %s", n, name);
            }
        }

        {
            spine::Array<spine::Skin*>& skins = data->m_Skeleton->getSkins();
            uint32_t count = (uint32_t)skins.size();
            PrepareHashTable(data->m_SkinNameToIndex, count);
            PrepareHashTable(data->m_AttachmentHashToName, 32);
            for (uint32_t n = 0; n < count; ++n)
            {
                spine::Skin* skin = skins[n];
                const char* skin_name = skin->getName().buffer();
                dmhash_t name_hash = dmHashString64(skin_name);
                data->m_SkinNameToIndex.Put(name_hash, n);

                DEBUGLOG("skin: %d %s", n, skin_name);
                spine::Skin::AttachmentMap::Entries entries = skin->getAttachments();
                while(entries.hasNext())
                {
                    spine::Skin::AttachmentMap::Entry& entry = entries.next();
                    const char* attachment_name = entry._placeholder.buffer();
                    DEBUGLOG("attachment: %s  slot: %d", attachment_name, entry._slotIndex);
                    AddAttachmentName(data, attachment_name);
                }
            }
        }

        {
            spine::Array<spine::SlotData*>& slots = data->m_Skeleton->getSlots();
            uint32_t count = (uint32_t)slots.size();
            PrepareHashTable(data->m_SlotNameToIndex, count);
            for (uint32_t n = 0; n < count; ++n)
            {
                const char* name = slots[n]->getName().buffer();
                dmhash_t name_hash = dmHashString64(name);
                data->m_SlotNameToIndex.Put(name_hash, n);
                DEBUGLOG("slot: %d %s", n, name);
            }
        }

        {
            spine::Array<spine::ConstraintData*>& constraints = data->m_Skeleton->getConstraints();
            uint32_t ik_count = 0;
            for (uint32_t n = 0; n < constraints.size(); ++n)
                ik_count += constraints[n]->getRTTI().instanceOf(spine::IkConstraintData::rtti) ? 1 : 0;

            PrepareHashTable(data->m_IKNameToIndex, ik_count);
            for (uint32_t n = 0; n < constraints.size(); ++n)
            {
                spine::ConstraintData* constraint = constraints[n];
                if (!constraint->getRTTI().instanceOf(spine::IkConstraintData::rtti))
                    continue;
                const char* name = constraint->getName().buffer();
                dmhash_t name_hash = dmHashString64(name);
                data->m_IKNameToIndex.Put(name_hash, n);
                DEBUGLOG("ik: %d %s", n, name);
            }
        }

        return dmResource::RESULT_OK;
    }

    static void DeleteSceneData(SpineSceneData* data)
    {
        ClearHashTable(data->m_AnimationNameToIndex);
        ClearHashTable(data->m_SkinNameToIndex);
        ClearHashTable(data->m_SlotNameToIndex);
        ClearHashTable(data->m_IKNameToIndex);
        ClearHashTable(data->m_AttachmentHashToName);
        for (uint32_t i = 0; i < data->m_AttachmentNames.Size(); ++i)
        {
            delete[] data->m_AttachmentNames[i];
        }
        data->m_AttachmentNames.SetSize(0);

        if (data->m_AnimationStateData)
            delete data->m_AnimationStateData;
        if (data->m_Skeleton)
            delete data->m_Skeleton;

        // SkeletonData owns the normal attachment references. Detached
        // attachments retained by clear_skin must be released only afterward.
        for (uint32_t i = 0; i < data->m_RetainedAttachments.Size(); ++i)
        {
            spine::Attachment* attachment = data->m_RetainedAttachments[i];
            attachment->dereference();
            if (attachment->getRefCount() == 0)
                delete attachment;
        }
        data->m_RetainedAttachments.SetSize(0);

        if (data->m_AttachmentLoader)
            dmSpine::Dispose(data->m_AttachmentLoader);
        delete[] data->m_Regions;
        if (data->m_TextureSet)
            dmResource::Release(data->m_Factory, data->m_TextureSet);
        if (data->m_Ddf)
            dmDDF::FreeMessage(data->m_Ddf);

        delete data;
    }

    SpineSceneData* RetainSceneData(SpineSceneResource* resource)
    {
        SpineSceneData* data = resource ? resource->m_Data : 0;
        RetainSceneData(data);
        return data;
    }

    void RetainSceneData(SpineSceneData* data)
    {
        if (data)
            ++data->m_RefCount;
    }

    void ReleaseSceneData(SpineSceneData* data)
    {
        if (data && --data->m_RefCount == 0)
            DeleteSceneData(data);
    }

    void RetainSceneAttachment(SpineSceneData* data, spine::Attachment* attachment)
    {
        if (!data || !attachment)
            return;
        if (data->m_RetainedAttachments.Full())
            data->m_RetainedAttachments.OffsetCapacity(8);
        attachment->reference();
        data->m_RetainedAttachments.Push(attachment);
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
        SpineSceneData* data = new SpineSceneData();
        data->m_Ddf = (dmGameSystemDDF::SpineSceneDesc*) params->m_PreloadData;
        dmResource::Result r = AcquireResources(params->m_Factory, data, params->m_Filename);
        if (r == dmResource::RESULT_OK)
        {
            scene_resource->m_Data = data;
            dmResource::SetResource(params->m_Resource, scene_resource);
        }
        else
        {
            ReleaseSceneData(data);
            delete scene_resource;
        }
        return r;
    }

    static dmResource::Result ResourceTypeScene_Destroy(const dmResource::ResourceDestroyParams* params)
    {
        SpineSceneResource* scene_resource = (SpineSceneResource*)dmResource::GetResource(params->m_Resource);
        ReleaseSceneData(scene_resource->m_Data);
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
        SpineSceneData* data = new SpineSceneData();
        data->m_Generation = resource->m_Data->m_Generation + 1;
        if (data->m_Generation == 0)
            data->m_Generation = 1;
        data->m_Ddf = ddf;

        dmResource::Result result = AcquireResources(params->m_Factory, data, params->m_Filename);
        if (result != dmResource::RESULT_OK)
        {
            // Recreate is transactional: keep the published generation intact.
            ReleaseSceneData(data);
            return result;
        }

        SpineSceneData* old_data = resource->m_Data;
        resource->m_Data = data;
        // Live model/GUI instances hold their own references to old_data and
        // release them only after destroying their Skeleton/AnimationState.
        ReleaseSceneData(old_data);
        return dmResource::RESULT_OK;
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
