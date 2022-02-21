#ifndef DM_RES_SPINE_JSON_H
#define DM_RES_SPINE_JSON_H

#include <stdint.h>

namespace dmSpine
{
    struct SpineJsonResource
    {
        char*       m_Json;
        uint32_t    m_Length;
    };
}

#endif // DM_RES_SPINE_JSON_H
