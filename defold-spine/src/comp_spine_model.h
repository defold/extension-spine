// Copyright 2020 The Defold Foundation
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#ifndef DM_GAMESYS_COMP_SPINE_MODEL_H
#define DM_GAMESYS_COMP_SPINE_MODEL_H

#include <stdint.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/hash.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/render_constants.h>
#include <dmsdk/render/render.h>

#include "res_spine_model.h"

struct spSkeleton;
struct spAnimationState;
struct spTrackEntry;

namespace dmSpine
{
    struct SpineModelComponent
    {
        dmGameObject::HInstance                 m_Instance;
        dmTransform::Transform                  m_Transform;
        dmVMath::Matrix4                        m_World;
        SpineModelResource*                     m_Resource;
        spSkeleton*                             m_SkeletonInstance;
        spAnimationState*                       m_AnimationStateInstance;
        spTrackEntry*                           m_AnimationInstance;
        dmMessage::URL                          m_Listener;
        dmGameSystem::HComponentRenderConstants m_RenderConstants;
        dmRender::HMaterial                     m_Material;
        /// Node instances corresponding to the bones
        dmArray<dmGameObject::HInstance>        m_NodeInstances;
        dmGameObject::Playback                  m_Playback;
        int                                     m_AnimationCallbackRef;
        float                                   m_PlaybackRate;
        uint32_t                                m_MixedHash;
        uint16_t                                m_ComponentIndex;
        uint8_t                                 m_Enabled : 1;
        uint8_t                                 m_DoRender : 1;
        uint8_t                                 m_AddedToUpdate : 1;
        uint8_t                                 m_ReHash : 1;
    };

    // For scripting
    bool CompSpineModelSetIKTargetInstance(SpineModelComponent* component, dmhash_t constraint_id, float mix, dmhash_t instance_id);
    bool CompSpineModelSetIKTargetPosition(SpineModelComponent* component, dmhash_t constraint_id, float mix, Vectormath::Aos::Point3 position);
    bool CompSpineModelResetIKTarget(SpineModelComponent* component, dmhash_t constraint_id);

    bool CompSpineModelSetSkin(SpineModelComponent* component, dmhash_t skin_id);
    bool CompSpineModelSetSkinSlot(SpineModelComponent* component, dmhash_t skin_id, dmhash_t slot_id);
}

#endif // DM_GAMESYS_COMP_SPINE_MODEL_H
