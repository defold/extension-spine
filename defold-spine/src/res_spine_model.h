
#ifndef DM_RES_SPINE_MODEL_H
#define DM_RES_SPINE_MODEL_H

#include <dmsdk/render/render.h>
#include <dmsdk/gamesys/resources/res_rig_scene.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto

namespace dmSpine
{
    struct SpineSceneResource;

    struct SpineModelResource
    {
        dmGameSystemDDF::SpineModelDesc*    m_Ddf;
        SpineSceneResource*                 m_SpineScene;
        dmRender::HMaterial                 m_Material;
    };
}

#endif // DM_RES_SPINE_MODEL_H
