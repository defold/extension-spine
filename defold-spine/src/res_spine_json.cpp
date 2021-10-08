#include "res_spine_json.h"
#include <memory.h>
#include <string.h>
#include <stdio.h>

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
        dmLogWarning("MAWE %s:", __FUNCTION__);
        free((void*)resource->m_Json);
        delete resource;
    }

    static dmResource::Result ResourceTypeJson_Create(const dmResource::ResourceCreateParams& params)
    {
        dmLogWarning("MAWE %s: %s", __FUNCTION__, params.m_Filename);

        SpineJsonResource* resource = CreateResource(params.m_Buffer, params.m_BufferSize);
        if (!resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        printf("length: %u\n", resource->m_Length);

        for (int i = 0; i < 64 && i < resource->m_Length; ++i)
        {
            printf("%c", resource->m_Json[i]);
        }
        printf("\n");

        dmLogWarning("MAWE %s: %p length: %u", __FUNCTION__, resource->m_Json, resource->m_Length);

        params.m_Resource->m_Resource = (void*)resource;
        params.m_Resource->m_ResourceSize = resource->m_Length;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeJson_Destroy(const dmResource::ResourceDestroyParams& params)
    {
        dmLogWarning("MAWE %s", __FUNCTION__);
        SpineJsonResource* resource = (SpineJsonResource*)params.m_Resource->m_Resource;
        DestroyResource(resource);
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeJson_Recreate(const dmResource::ResourceRecreateParams& params)
    {
        SpineJsonResource* new_resource = CreateResource(params.m_Buffer, params.m_BufferSize);
        if (!new_resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        SpineJsonResource* old_resource = (SpineJsonResource*) params.m_Resource->m_Resource;

        // swap the internals
        // we wish to keep the "old" resource, since that pointer might be shared in the system
        char* tmp = old_resource->m_Json;
        old_resource->m_Json = new_resource->m_Json;
        old_resource->m_Length = new_resource->m_Length;

        new_resource->m_Json = tmp;
        DestroyResource(new_resource);

        params.m_Resource->m_ResourceSize = old_resource->m_Length;
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeJson_Register(dmResource::ResourceTypeRegisterContext& ctx)
    {
        return dmResource::RegisterType(ctx.m_Factory,
                                           ctx.m_Name,
                                           0, // context
                                           0, // preload
                                           ResourceTypeJson_Create,
                                           0, // post create
                                           ResourceTypeJson_Destroy,
                                           ResourceTypeJson_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineJsonExt, "spinejsonc", dmSpine::ResourceTypeJson_Register, 0);
