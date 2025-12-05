#include "res_spine_json.h"
#include <memory.h>
#include <string.h>
#include <stdio.h>

#ifdef __MACH__
    #include <malloc/_malloc.h>
#else
    #include <malloc.h>
#endif

#include <dmsdk/dlib/log.h>
#include <dmsdk/resource/resource.h>

namespace dmSpine
{
    static SpineJsonResource* CreateResource(const void* buffer, uint32_t buffer_size)
    {
        SpineJsonResource* resource = new SpineJsonResource;
        resource->m_Json = (char*)malloc(buffer_size + 1);
        if (!resource->m_Json)
        {
            delete resource;
            return 0;
        }

        memcpy((void*)resource->m_Json, buffer, buffer_size);
        resource->m_Json[buffer_size] = 0;
        resource->m_Length = buffer_size;

        return resource;
    }

    static void DestroyResource(SpineJsonResource* resource)
    {
        free((void*)resource->m_Json);
        delete resource;
    }

    static dmResource::Result ResourceTypeJson_Create(const dmResource::ResourceCreateParams* params)
    {
        SpineJsonResource* resource = CreateResource(params->m_Buffer, params->m_BufferSize);
        if (!resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        dmResource::SetResource(params->m_Resource, resource);
        dmResource::SetResourceSize(params->m_Resource, resource->m_Length);
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeJson_Destroy(const dmResource::ResourceDestroyParams* params)
    {
        SpineJsonResource* resource = (SpineJsonResource*)dmResource::GetResource(params->m_Resource);
        DestroyResource(resource);
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeJson_Recreate(const dmResource::ResourceRecreateParams* params)
    {
        SpineJsonResource* new_resource = CreateResource(params->m_Buffer, params->m_BufferSize);
        if (!new_resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        SpineJsonResource* old_resource = (SpineJsonResource*) dmResource::GetResource(params->m_Resource);

        // swap the internals
        // we wish to keep the "old" resource, since that pointer might be shared in the system
        char* tmp = old_resource->m_Json;
        old_resource->m_Json = new_resource->m_Json;
        old_resource->m_Length = new_resource->m_Length;

        new_resource->m_Json = tmp;
        DestroyResource(new_resource);

        dmResource::SetResourceSize(params->m_Resource, old_resource->m_Length);
        return dmResource::RESULT_OK;
    }

    static ResourceResult ResourceTypeJson_Register(HResourceTypeContext ctx, HResourceType type)
    {
        return (ResourceResult)dmResource::SetupType(ctx,
                                                       type,
                                                       0, // context
                                                       0, // preload
                                                       ResourceTypeJson_Create,
                                                       0, // post create
                                                       ResourceTypeJson_Destroy,
                                                       ResourceTypeJson_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineJsonExt, "spinejsonc", dmSpine::ResourceTypeJson_Register, 0);
