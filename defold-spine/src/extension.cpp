
#include <dmsdk/sdk.h>
#include "script_spine.h"
#include "script_spine_resource.h"
#include "gui_spine.h"

static dmExtension::Result AppInitializeSpine(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result InitializeSpine(dmExtension::Params* params)
{
    dmSpine::ScriptSpineGoRegister(params->m_L);
    dmSpine::ScriptSpineResourceInitialize(params->m_ResourceFactory);
    dmSpine::ScriptSpineResourceRegister(params->m_L);
    dmSpine::GuiSpineInitialize(params->m_ResourceFactory);
    dmLogInfo("Registered spine extension\n");
    return dmExtension::RESULT_OK;
}

static dmExtension::Result AppFinalizeSpine(dmExtension::AppParams* params)
{
    return dmExtension::RESULT_OK;
}

static dmExtension::Result FinalizeSpine(dmExtension::Params* params)
{
    dmSpine::GuiSpineFinalize();
    return dmExtension::RESULT_OK;
}


// DM_DECLARE_EXTENSION(symbol, name, app_init, app_final, init, update, on_event, final)
DM_DECLARE_EXTENSION(SpineExt, "SpineExt", AppInitializeSpine, AppFinalizeSpine, InitializeSpine, 0, 0, FinalizeSpine);
