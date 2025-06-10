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
#include <dmsdk/dlib/transform.h>
#include <dmsdk/dlib/hashtable.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/render_constants.h>
#include <dmsdk/render/render.h>
#include <dmsdk/gamesys/script.h>
// The engine ddf formats aren't stored in the "dmsdk" folder (yet)
#include <gamesys/gamesys_ddf.h>

#include "res_spine_model.h"

struct spAnimationState;
struct spBone;
struct spSkeleton;
struct spTrackEntry;
struct spIkConstraint;
struct lua_State;

namespace dmSpine
{
    const int32_t ALL_TRACKS = -1;

    struct SpineAnimationTrack {
        spTrackEntry*                           m_AnimationInstance;
        dmhash_t                                m_AnimationId;
        dmGameObject::Playback                  m_Playback;
        dmMessage::URL                          m_Listener;
        lua_State*                              m_Context;

        dmScript::LuaCallbackInfo*              m_CallbackInfo;
        uint32_t                                m_CallbackId;
    };

    struct IKTarget
    {
        dmhash_t                                m_ConstraintHash;
        spIkConstraint*                         m_Constraint;
        dmGameObject::HInstance                 m_Target;
        dmVMath::Point3                         m_Position;
    };

    struct SpineModelComponent
    {
        dmGameObject::HInstance                 m_Instance;
        dmTransform::Transform                  m_Transform;
        dmVMath::Matrix4                        m_World;
        SpineModelResource*                     m_Resource;
        spSkeleton*                             m_SkeletonInstance;
        spAnimationState*                       m_AnimationStateInstance;
        dmArray<dmSpine::SpineAnimationTrack>   m_AnimationTracks;
        dmGameSystem::HComponentRenderConstants m_RenderConstants;
        dmGameSystem::MaterialResource*         m_Material;
        /// Node instances corresponding to the bones
        dmArray<dmGameObject::HInstance>        m_BoneInstances;
        dmArray<spBone*>                        m_Bones;                        // We shouldn't really have to have a duplicate array of these
        dmHashTable64<uint32_t>                 m_BoneNameToNodeInstanceIndex;  // should really be in the spine_scene

        dmArray<dmSpine::IKTarget>              m_IKTargets;
        dmArray<dmSpine::IKTarget>              m_IKTargetPositions;
        uint32_t                                m_MixedHash;
        uint16_t                                m_ComponentIndex;
        uint8_t                                 m_Enabled : 1;
        uint8_t                                 m_DoRender : 1;
        uint8_t                                 m_AddedToUpdate : 1;
        uint8_t                                 m_ReHash : 1;
    };

    // For scripting
    bool CompSpineModelPlayAnimation(SpineModelComponent* component, dmGameSystemDDF::SpinePlayAnimation* message, dmMessage::URL* sender, dmScript::LuaCallbackInfo* callback_info, lua_State* L);
    bool CompSpineModelCancelAnimation(SpineModelComponent* component, dmGameSystemDDF::SpineCancelAnimation* message);

    bool CompSpineModelSetConstant(SpineModelComponent* component, dmGameSystemDDF::SetConstant* message);
    bool CompSpineModelResetConstant(SpineModelComponent* component, dmGameSystemDDF::ResetConstant* message);

    bool CompSpineModelSetIKTargetInstance(SpineModelComponent* component, dmhash_t constraint_id, float mix, dmhash_t instance_id);
    bool CompSpineModelSetIKTargetPosition(SpineModelComponent* component, dmhash_t constraint_id, float mix, Vectormath::Aos::Point3 position);
    bool CompSpineModelResetIKTarget(SpineModelComponent* component, dmhash_t constraint_id);

    bool CompSpineModelClear(SpineModelComponent* component, dmhash_t skin_id);
    bool CompSpineModelAddSkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b);
    bool CompSpineModelCopySkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b);
    bool CompSpineModelSetSkin(SpineModelComponent* component, dmhash_t skin_id);
    bool CompSpineModelSetAttachment(SpineModelComponent* component, dmhash_t slot_id, dmhash_t attachment_id);

    bool CompSpineModelGetBone(SpineModelComponent* component, dmhash_t bone_name, dmhash_t* instance_id);

    void RunTrackCallback(dmScript::LuaCallbackInfo* callback_data, const dmDDF::Descriptor* desc, const char* data, const dmMessage::URL* sender);

}

#endif // DM_GAMESYS_COMP_SPINE_MODEL_H
