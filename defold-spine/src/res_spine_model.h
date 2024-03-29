
#ifndef DM_RES_SPINE_MODEL_H
#define DM_RES_SPINE_MODEL_H

#include <dmsdk/render/render.h>
#include <dmsdk/gamesys/resources/res_rig_scene.h>
#include <dmsdk/gamesys/resources/res_material.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto

namespace dmSpine
{
    struct SpineSceneResource;

    struct SpineModelResource
    {
        dmGameSystemDDF::SpineModelDesc*    m_Ddf;
        SpineSceneResource*                 m_SpineScene;
        dmGameSystem::MaterialResource*     m_Material;
        uint8_t                             m_CreateGoBones:1;
    };
}

#endif // DM_RES_SPINE_MODEL_H
