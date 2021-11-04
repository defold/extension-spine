#include "res_spine_model.h"

#include <dmsdk/dlib/log.h>
#include <dmsdk/resource/resource.h>

namespace dmSpine
{
    static dmResource::Result AcquireResources(dmResource::HFactory factory, SpineModelResource* resource, const char* filename)
    {
        dmResource::Result result = dmResource::Get(factory, resource->m_Ddf->m_SpineScene, (void**) &resource->m_SpineScene);
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }

        result = dmResource::Get(factory, resource->m_Ddf->m_Material, (void**) &resource->m_Material);
        if (result != dmResource::RESULT_OK)
        {
            return result;
        }
        if(dmRender::GetMaterialVertexSpace(resource->m_Material) != dmRenderDDF::MaterialDesc::VERTEX_SPACE_WORLD)
        {
            dmLogError("Failed to create Spine Model component. This component only supports materials with the Vertex Space property set to 'vertex-space-world'");
            return dmResource::RESULT_NOT_SUPPORTED;
        }
        return dmResource::RESULT_OK;
    }

    static void ReleaseResources(dmResource::HFactory factory, SpineModelResource* resource)
    {
        if (resource->m_Ddf != 0x0)
            dmDDF::FreeMessage(resource->m_Ddf);
        if (resource->m_SpineScene != 0x0)
            dmResource::Release(factory, resource->m_SpineScene);
        if (resource->m_Material != 0x0)
            dmResource::Release(factory, resource->m_Material);
    }

    static dmResource::Result ResourceTypeModel_Preload(const dmResource::ResourcePreloadParams& params)
    {
        dmGameSystemDDF::SpineModelDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params.m_Buffer, params.m_BufferSize, &dmGameSystemDDF_SpineModelDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }

        dmResource::PreloadHint(params.m_HintInfo, ddf->m_SpineScene);
        dmResource::PreloadHint(params.m_HintInfo, ddf->m_Material);

        *params.m_PreloadData = ddf;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeModel_Create(const dmResource::ResourceCreateParams& params)
    {
        SpineModelResource* model_resource = new SpineModelResource();
        model_resource->m_Ddf = (dmGameSystemDDF::SpineModelDesc*) params.m_PreloadData;
        dmResource::Result r = AcquireResources(params.m_Factory, model_resource, params.m_Filename);
        if (r == dmResource::RESULT_OK)
        {
            params.m_Resource->m_Resource = (void*) model_resource;
        }
        else
        {
            ReleaseResources(params.m_Factory, model_resource);
            delete model_resource;
        }
        return r;
    }

    static dmResource::Result ResourceTypeModel_Destroy(const dmResource::ResourceDestroyParams& params)
    {
        SpineModelResource* model_resource = (SpineModelResource*)params.m_Resource->m_Resource;
        ReleaseResources(params.m_Factory, model_resource);
        delete model_resource;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeModel_Recreate(const dmResource::ResourceRecreateParams& params)
    {
        dmGameSystemDDF::SpineModelDesc* ddf;
        dmDDF::Result e = dmDDF::LoadMessage(params.m_Buffer, params.m_BufferSize, &dmGameSystemDDF_SpineModelDesc_DESCRIPTOR, (void**) &ddf);
        if (e != dmDDF::RESULT_OK)
        {
            return dmResource::RESULT_DDF_ERROR;
        }
        SpineModelResource* model_resource = (SpineModelResource*)params.m_Resource->m_Resource;
        ReleaseResources(params.m_Factory, model_resource);
        model_resource->m_Ddf = ddf;
        return AcquireResources(params.m_Factory, model_resource, params.m_Filename);
    }

    static dmResource::Result ResourceTypeModel_Register(dmResource::ResourceTypeRegisterContext& ctx)
    {
        return dmResource::RegisterType(ctx.m_Factory,
                                           ctx.m_Name,
                                           0, // context
                                           ResourceTypeModel_Preload,
                                           ResourceTypeModel_Create,
                                           0, // post create
                                           ResourceTypeModel_Destroy,
                                           ResourceTypeModel_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineModelExt, "spinemodelc", dmSpine::ResourceTypeModel_Register, 0);
