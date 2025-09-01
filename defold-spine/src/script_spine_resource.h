#ifndef DM_SPINE_SCRIPT_RESOURCE_H
#define DM_SPINE_SCRIPT_RESOURCE_H

#include <dmsdk/resource/resource.h>

namespace dmSpine
{
    void ScriptSpineResourceInitialize(dmResource::HFactory factory);
    void ScriptSpineResourceRegister(struct lua_State* L);
}

#endif // DM_SPINE_SCRIPT_RESOURCE_H
