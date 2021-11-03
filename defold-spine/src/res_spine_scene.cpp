#include "res_spine_scene.h"
#include "res_spine_json.h"
#include "spine_loader.h"
#include "spine_ddf.h" // generated from the spine_ddf.proto

#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/resource/resource.h>

#include <spine/SkeletonJson.h>
#include <spine/AnimationStateData.h>

// Also see the guide http://esotericsoftware.com/spine-c#Loading-skeleton-data

namespace dmSpine
{
    static spSkeletonData* ReadSkeletonJsonData(spAttachmentLoader* loader, const char* path, void* json_data)
    {
        spSkeletonJson* skeleton_json = spSkeletonJson_createWithLoader(loader);
        if (!skeleton_json) {
            dmLogError("Failed to create spine skeleton for %s", path);
            return 0;
        }

        dmLogWarning("%s: %p   json: %p", __FUNCTION__, skeleton_json, json_data);

        spSkeletonData* skeletonData = spSkeletonJson_readSkeletonData(skeleton_json, (const char *)json_data);
        if (!skeletonData)
        {
            dmLogError("Failed to read spine skeleton for %s: %s", path, skeleton_json->error);
            return 0;
        }
        spSkeletonJson_dispose(skeleton_json);
        return skeletonData;
    }

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
        resource->m_Regions = CreateRegions(resource->m_TextureSet);
        resource->m_AttachmentLoader = dmSpine::CreateAttachmentLoader(resource->m_TextureSet, resource->m_Regions);

        // Create the spine resource
        resource->m_Skeleton = ReadSkeletonJsonData((spAttachmentLoader*)resource->m_AttachmentLoader, filename, spine_json_resource->m_Json);
        if (!resource->m_Skeleton)
        {
            return dmResource::RESULT_INVALID_DATA;
        }

        resource->m_AnimationStateData = spAnimationStateData_create(resource->m_Skeleton);
        //spAnimationStateData_setDefaultMix(resource->m_AnimationStateData, 0.1f); // There's currently no such function!
        resource->m_AnimationStateData->defaultMix = 0.1f; // force mixing

        // We can release this json data now
        dmResource::Release(factory, spine_json_resource);

        resource->m_AnimationNameToIndex.SetCapacity(dmMath::Max(1, resource->m_Skeleton->animationsCount/3), resource->m_Skeleton->animationsCount);
        for (int n = 0; n < resource->m_Skeleton->animationsCount; ++n)
        {
            dmhash_t name_hash = dmHashString64(resource->m_Skeleton->animations[n]->name);
            resource->m_AnimationNameToIndex.Put(name_hash, n);
        }


        // if(dmRender::GetMaterialVertexSpace(resource->m_Material) != dmRenderDDF::MaterialDesc::VERTEX_SPACE_WORLD)
        // {
        //     dmLogError("Failed to create Spine Model component. This component only supports materials with the Vertex Space property set to 'vertex-space-world'");
        //     return dmResource::RESULT_NOT_SUPPORTED;
        // }
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

    static dmResource::Result ResourceTypeScene_Preload(const dmResource::ResourcePreloadParams& params)
    {
        dmGameSystemDDF::SpineSceneDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params.m_Buffer, params.m_BufferSize, &dmGameSystemDDF_SpineSceneDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }

        dmResource::PreloadHint(params.m_HintInfo, ddf->m_SpineJson);
        dmResource::PreloadHint(params.m_HintInfo, ddf->m_Atlas);

        *params.m_PreloadData = ddf;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeScene_Create(const dmResource::ResourceCreateParams& params)
    {
        SpineSceneResource* scene_resource = new SpineSceneResource();
        scene_resource->m_Ddf = (dmGameSystemDDF::SpineSceneDesc*) params.m_PreloadData;
        dmResource::Result r = AcquireResources(params.m_Factory, scene_resource, params.m_Filename);
        if (r == dmResource::RESULT_OK)
        {
            params.m_Resource->m_Resource = (void*) scene_resource;
        }
        else
        {
            ReleaseResources(params.m_Factory, scene_resource);
            delete scene_resource;
        }
        return r;
    }

    static dmResource::Result ResourceTypeScene_Destroy(const dmResource::ResourceDestroyParams& params)
    {
        SpineSceneResource* scene_resource = (SpineSceneResource*)params.m_Resource->m_Resource;
        ReleaseResources(params.m_Factory, scene_resource);
        delete scene_resource;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeScene_Recreate(const dmResource::ResourceRecreateParams& params)
    {
        dmGameSystemDDF::SpineSceneDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params.m_Buffer, params.m_BufferSize, &dmGameSystemDDF_SpineSceneDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }
        SpineSceneResource* resource = (SpineSceneResource*)params.m_Resource->m_Resource;
        ReleaseResources(params.m_Factory, resource);
        resource->m_Ddf = ddf;
        return AcquireResources(params.m_Factory, resource, params.m_Filename);
    }

    static dmResource::Result ResourceTypeScene_Register(dmResource::ResourceTypeRegisterContext& ctx)
    {
        return dmResource::RegisterType(ctx.m_Factory,
                                           ctx.m_Name,
                                           0, // context
                                           ResourceTypeScene_Preload,
                                           ResourceTypeScene_Create,
                                           0, // post create
                                           ResourceTypeScene_Destroy,
                                           ResourceTypeScene_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineSceneExt, "spinescenec", dmSpine::ResourceTypeScene_Register, 0);
