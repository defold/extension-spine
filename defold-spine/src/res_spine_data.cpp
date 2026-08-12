#include "res_spine_data.h"
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
    static SpineDataResource* CreateResource(const void* buffer, uint32_t buffer_size)
    {
        SpineDataResource* resource = new SpineDataResource;
        resource->m_Data = (char*)malloc(buffer_size + 1);
        if (!resource->m_Data)
        {
            delete resource;
            return 0;
        }

        memcpy((void*)resource->m_Data, buffer, buffer_size);
        resource->m_Data[buffer_size] = 0;
        resource->m_Length = buffer_size;

        return resource;
    }

    static void DestroyResource(SpineDataResource* resource)
    {
        free((void*)resource->m_Data);
        delete resource;
    }

    static dmResource::Result ResourceTypeData_Create(const dmResource::ResourceCreateParams* params)
    {
        SpineDataResource* resource = CreateResource(params->m_Buffer, params->m_BufferSize);
        if (!resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        dmResource::SetResource(params->m_Resource, resource);
        dmResource::SetResourceSize(params->m_Resource, resource->m_Length);
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeData_Destroy(const dmResource::ResourceDestroyParams* params)
    {
        SpineDataResource* resource = (SpineDataResource*)dmResource::GetResource(params->m_Resource);
        DestroyResource(resource);
        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceTypeData_Recreate(const dmResource::ResourceRecreateParams* params)
    {
        SpineDataResource* new_resource = CreateResource(params->m_Buffer, params->m_BufferSize);
        if (!new_resource)
        {
            return dmResource::RESULT_OUT_OF_RESOURCES;
        }

        SpineDataResource* old_resource = (SpineDataResource*) dmResource::GetResource(params->m_Resource);

        // swap the internals
        // we wish to keep the "old" resource, since that pointer might be shared in the system
        char* tmp = old_resource->m_Data;
        old_resource->m_Data = new_resource->m_Data;
        old_resource->m_Length = new_resource->m_Length;

        new_resource->m_Data = tmp;
        DestroyResource(new_resource);

        dmResource::SetResourceSize(params->m_Resource, old_resource->m_Length);
        return dmResource::RESULT_OK;
    }

    static ResourceResult ResourceTypeData_Register(HResourceTypeContext ctx, HResourceType type)
    {
        return (ResourceResult)dmResource::SetupType(ctx,
                                                       type,
                                                       0, // context
                                                       0, // preload
                                                       ResourceTypeData_Create,
                                                       0, // post create
                                                       ResourceTypeData_Destroy,
                                                       ResourceTypeData_Recreate);

    }
}

DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineJsonExt, "spinejsonc", dmSpine::ResourceTypeData_Register, 0);
DM_DECLARE_RESOURCE_TYPE(ResourceTypeSpineSkelExt, "skelc", dmSpine::ResourceTypeData_Register, 0);
