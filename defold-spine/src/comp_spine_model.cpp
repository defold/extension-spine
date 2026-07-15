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

#include "comp_spine_model.h"
#include "res_spine_scene.h"
#include "spine_skin_utils.h"

#include <spine/Animation.h>
#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/BlendMode.h>
#include <spine/Bone.h>
#include <spine/Event.h>
#include <spine/EventData.h>
#include <spine/IkConstraint.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonRenderer.h>
#include <spine/Skin.h>
#include <spine/Slot.h>

#include <string.h> // memset

#include <dmsdk/script.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/dlib/intersection.h>
#include <dmsdk/dlib/object_pool.h>
#include <dmsdk/dlib/profile.h>
#include <dmsdk/gameobject/component.h>
#include <dmsdk/gamesys/property.h>
#include <dmsdk/gamesys/resources/res_textureset.h>
#include <dmsdk/resource/resource.hpp>
#include <gameobject/gameobject_ddf.h>

#include <common/vertices.h>
#include "spine_gui_common.h"


#define _USE_MATH_DEFINES
#include <math.h> // M_PI

DM_PROPERTY_GROUP(rmtp_Spine, "Spine", 0);
DM_PROPERTY_U32(rmtp_SpineBones, 0, PROFILE_PROPERTY_FRAME_RESET, "# spine bones", &rmtp_Spine);
DM_PROPERTY_U32(rmtp_SpineComponents, 0, PROFILE_PROPERTY_FRAME_RESET, "# spine components", &rmtp_Spine);
DM_PROPERTY_U32(rmtp_SpineVertexCount, 0, PROFILE_PROPERTY_FRAME_RESET, "# vertices", &rmtp_Spine);
DM_PROPERTY_U32(rmtp_SpineVertexSize, 0, PROFILE_PROPERTY_FRAME_RESET, "size of vertices in bytes", &rmtp_Spine);
DM_PROPERTY_U32(rmtp_SpineIndexSize, 0, PROFILE_PROPERTY_FRAME_RESET, "size of indices in bytes", &rmtp_Spine);

namespace dmSpine
{
    using namespace dmVMath;

    static const dmhash_t PROP_SKIN = dmHashString64("skin");
    static const dmhash_t PROP_ANIMATION = dmHashString64("animation");
    static const dmhash_t PROP_CURSOR = dmHashString64("cursor");
    static const dmhash_t PROP_PLAYBACK_RATE = dmHashString64("playback_rate");
    static const dmhash_t PROP_MATERIAL = dmHashString64("material");
    static const dmhash_t MATERIAL_EXT_HASH = dmHashString64("materialc");

    static const uint32_t INVALID_ANIMATION_INDEX = 0xFFFFFFFF;

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params);
    static void DestroyComponent(struct SpineModelWorld* world, uint32_t index);
    static void SpineEventListener(spine::AnimationState* state, spine::EventType type, spine::TrackEntry* entry, spine::Event* event, void* user_data);
    static spine::IkConstraint* FindIKConstraint(SpineModelComponent* component, dmhash_t constraint_id);

    struct SpineModelWorld
    {
        dmObjectPool<SpineModelComponent*>      m_Components;
        dmArray<dmRender::RenderObject>         m_RenderObjects;
        dmArray<dmSpine::SpineModelBounds>      m_BoundingBoxes;
        dmArray<float>                           m_GeometryScratch;
        dmGraphics::HVertexDeclaration          m_VertexDeclaration;
        dmGraphics::HVertexBuffer               m_VertexBuffer;
        dmGraphics::HIndexBuffer                m_IndexBuffer;
        dmArray<dmSpine::SpineVertex>           m_VertexBufferData;
        dmArray<uint32_t>                       m_IndexBufferData;
        dmArray<uint8_t>                        m_PackedIndexBufferData;
        dmArray<SpineIndexedDrawDesc>           m_DrawDescBuffer;
        dmArray<SpineIndexedDrawDesc>           m_MergedDrawDescBuffer;
        dmResource::HFactory                    m_Factory;
        spine::SkeletonRenderer*                m_SkeletonRenderer;
        uint8_t                                 m_Is16BitIndex : 1;
    };

    struct SpineModelContext
    {
        SpineModelContext()
        {
            memset(this, 0, sizeof(*this));
        }
        dmResource::HFactory        m_Factory;
        dmRender::HRenderContext    m_RenderContext;
        dmGraphics::HContext        m_GraphicsContext;
        uint32_t                    m_MaxSpineModelCount;
    };

    dmGameObject::CreateResult CompSpineModelNewWorld(const dmGameObject::ComponentNewWorldParams& params)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        //dmRender::HRenderContext render_context = context->m_RenderContext;
        SpineModelWorld* world = new SpineModelWorld();
        world->m_Factory = context->m_Factory;

        uint32_t comp_count = dmMath::Min(params.m_MaxComponentInstances, context->m_MaxSpineModelCount);

        world->m_Components.SetCapacity(comp_count);
        world->m_RenderObjects.SetCapacity(comp_count);
        world->m_BoundingBoxes.SetCapacity(comp_count);
        world->m_BoundingBoxes.SetSize(comp_count);

        dmGraphics::HVertexStreamDeclaration stream_declaration = dmGraphics::NewVertexStreamDeclaration(context->m_GraphicsContext);
        dmGraphics::AddVertexStream(stream_declaration, "position", 3, dmGraphics::TYPE_FLOAT, false);
        dmGraphics::AddVertexStream(stream_declaration, "texcoord0", 2, dmGraphics::TYPE_FLOAT, true);
        dmGraphics::AddVertexStream(stream_declaration, "color", 4, dmGraphics::TYPE_FLOAT, true);
        dmGraphics::AddVertexStream(stream_declaration, "page_index", 1, dmGraphics::TYPE_FLOAT, false);

        world->m_VertexDeclaration = dmGraphics::NewVertexDeclaration(context->m_GraphicsContext, stream_declaration);
        world->m_VertexBuffer = dmGraphics::NewVertexBuffer(context->m_GraphicsContext, 0, 0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        world->m_IndexBuffer = dmGraphics::NewIndexBuffer(context->m_GraphicsContext, 0, 0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        world->m_Is16BitIndex = 1;

        dmGraphics::DeleteVertexStreamDeclaration(stream_declaration);

        *params.m_World = world;

        dmResource::RegisterResourceReloadedCallback(context->m_Factory, ResourceReloadedCallback, world);

        world->m_SkeletonRenderer = new spine::SkeletonRenderer();

        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompSpineModelDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        dmGraphics::DeleteVertexDeclaration(world->m_VertexDeclaration);
        dmGraphics::DeleteVertexBuffer(world->m_VertexBuffer);
        dmGraphics::DeleteIndexBuffer(world->m_IndexBuffer);

        dmResource::UnregisterResourceReloadedCallback(((SpineModelContext*)params.m_Context)->m_Factory, ResourceReloadedCallback, world);

        delete world->m_SkeletonRenderer;

        delete world;

        return dmGameObject::CREATE_RESULT_OK;
    }

    static inline dmGameSystem::MaterialResource* GetMaterialResource(const SpineModelComponent* component) {
        return component->m_Material ? component->m_Material : component->m_Resource->m_Material;
    }
    static inline dmRender::HMaterial GetMaterial(const SpineModelComponent* component) {
        return GetMaterialResource(component)->m_Material;
    }
    static inline SpineSceneResource* GetSpineScene(const SpineModelComponent* component) {
        return component->m_SpineScene ? component->m_SpineScene : component->m_Resource->m_SpineScene;
    }
    static inline SpineSceneData* GetSpineSceneData(const SpineModelComponent* component) {
        return component->m_SceneData;
    }

    static void ClearIKTargetBindings(SpineModelComponent* component)
    {
        // IK targets cache pointers owned by the current Skeleton instance.
        // They must never survive a Skeleton replacement.
        component->m_IKTargets.SetSize(0);
        component->m_IKTargetPositions.SetSize(0);
    }

    static void ReHash(SpineModelComponent* component)
    {
        // material, texture set, blend mode and render constants
        HashState32 state;
        bool reverse = false;
        SpineModelResource* resource = component->m_Resource;
        dmGameSystemDDF::SpineModelDesc* ddf = resource->m_Ddf;
        dmRender::HMaterial material = GetMaterial(component);

        dmGameSystem::TextureSetResource* texture_set = GetSpineSceneData(component)->m_TextureSet;
        dmHashInit32(&state, reverse);
        dmHashUpdateBuffer32(&state, &material, sizeof(material));
        dmHashUpdateBuffer32(&state, &texture_set, sizeof(texture_set));
        dmHashUpdateBuffer32(&state, &ddf->m_BlendMode, sizeof(ddf->m_BlendMode));
        if (component->m_RenderConstants)
            dmGameSystem::HashRenderConstants(component->m_RenderConstants, &state);
        component->m_MixedHash = dmHashFinal32(&state);
        component->m_ReHash = 0;
    }

    static void SetTransformFromBone(dmGameObject::HInstance instance, const dmTransform::Transform& parent, spine::Bone* bone)
    {
        spine::BonePose& pose = bone->getAppliedPose();
        float radians = pose.getWorldRotationX() * M_PI / 180.0f;
        float sx = pose.getWorldScaleX();
        float sy = pose.getWorldScaleY();

        dmTransform::Transform local = dmTransform::Transform(  dmVMath::Vector3(pose.getWorldX(), pose.getWorldY(), 0),
                                                                dmVMath::Quat::rotationZ(radians),
                                                                dmVMath::Vector3(sx, sy, 1));

        dmTransform::Transform transform = dmTransform::Mul(parent, local);

        dmGameObject::SetPosition(instance, Point3(transform.GetTranslation()));
        dmGameObject::SetRotation(instance, transform.GetRotation());
        dmGameObject::SetScale(instance, transform.GetScale());
    }

    static bool CreateGOBone(SpineModelComponent* component, dmGameObject::HCollection collection, dmGameObject::HInstance goparent, spine::Bone* parent, spine::Bone* bone, int indent)
    {
        dmGameObject::HInstance bone_instance = dmGameObject::New(collection, 0x0);
        if (!bone_instance)
        {
            dmLogError("Failed to create bone game object");
            return false;
        }

        dmGameObject::SetBone(bone_instance, true); // flag it as a bone instance
        dmGameObject::SetParent(bone_instance, goparent);

        uint32_t index = dmGameObject::AcquireInstanceIndex(collection);
        if (index == dmGameObject::INVALID_INSTANCE_POOL_INDEX)
        {
            dmLogError("Failed to acquire instance index for bone game object");
            return false;
        }

        dmhash_t id = dmGameObject::CreateInstanceId();
        dmGameObject::AssignInstanceIndex(index, bone_instance);

        dmGameObject::Result result = dmGameObject::SetIdentifier(collection, bone_instance, id);
        if (dmGameObject::RESULT_OK != result)
        {
            dmLogError("Failed to set identifier for bone game object");
            return false;
        }

        SetTransformFromBone(bone_instance, component->m_Transform, bone);

        dmhash_t name_hash = dmHashString64(bone->getData().getName().buffer());
        component->m_BoneNameToNodeInstanceIndex.Put(name_hash, component->m_BoneInstances.Size());

        component->m_BoneInstances.Push(bone_instance);
        component->m_Bones.Push(bone);

        // Create the children
        spine::Array<spine::Bone*>& children = bone->getChildren();
        for (uint32_t n = 0; n < children.size(); ++n)
        {
            // Since I haven't figured out how to update the local transforms, we do it in world space
            // However, that means all bones are parented to the top game object

            if (!CreateGOBone(component, collection, component->m_Instance, bone, children[n], indent + 2))
            //if (!CreateGOBone(component, collection, bone_instance, bone, bone->children[n], indent + 2))
                return false;
        }
        return true;
    }

    static bool CreateGOBones(SpineModelWorld* world, SpineModelComponent* component)
    {
        //dmGameObject::HCollection collection = dmGameObject::GetCollection(component->m_Instance);

        spine::Skeleton* skeleton = component->m_SkeletonInstance;

        uint32_t bone_count = (uint32_t)skeleton->getBones().size();
        component->m_Bones.SetCapacity(bone_count);
        component->m_BoneInstances.SetCapacity(bone_count);
        component->m_BoneNameToNodeInstanceIndex.OffsetCapacity(bone_count);
        if (!CreateGOBone(component, dmGameObject::GetCollection(component->m_Instance), component->m_Instance, 0, skeleton->getRootBone(), 0))
        {
            dmLogError("Failed to create bones");
            dmGameObject::DeleteBones(component->m_Instance); // iterates recursively and deletes the ones marked as a bone
            component->m_BoneInstances.SetSize(0);
            return false;
        }
        return true;
    }

    static void ScheduleBoneRebuild(SpineModelComponent* component)
    {
        component->m_Bones.SetSize(0);
        component->m_BoneInstances.SetSize(0);
        component->m_BoneNameToNodeInstanceIndex.Clear();
        dmGameObject::DeleteBones(component->m_Instance);
        // Bones are created in CompSpineModelPostUpdate because the previous ones are removed there
        component->m_RebuildBonesPending = component->m_Resource->m_CreateGoBones ? 1 : 0;
    }

    static inline SpineModelComponent* GetComponentFromIndex(SpineModelWorld* world, int index)
    {
        return world->m_Components.Get(index);
    }

    static void* CompSpineModelGetComponent(const dmGameObject::ComponentGetParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        uint32_t index = (uint32_t) params.m_UserData;
        return GetComponentFromIndex(world, index);
    }

    static inline uint32_t FindAnimationIndex(SpineModelComponent* component, dmhash_t animation)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);
        uint32_t* index = scene_data->m_AnimationNameToIndex.Get(animation);
        return index ? *index : INVALID_ANIMATION_INDEX;
    }

    static inline bool IsLooping(dmGameObject::Playback playback)
    {
        return  playback == dmGameObject::PLAYBACK_LOOP_BACKWARD ||
                playback == dmGameObject::PLAYBACK_LOOP_FORWARD ||
                playback == dmGameObject::PLAYBACK_LOOP_PINGPONG;
    }

    static inline bool IsReverse(dmGameObject::Playback playback)
    {
        return  playback == dmGameObject::PLAYBACK_LOOP_BACKWARD ||
                playback == dmGameObject::PLAYBACK_ONCE_BACKWARD;
    }

    static inline bool IsPingPong(dmGameObject::Playback playback)
    {
        return  playback == dmGameObject::PLAYBACK_LOOP_PINGPONG ||
                playback == dmGameObject::PLAYBACK_ONCE_PINGPONG;
    }

    static SpineAnimationTrack* GetTrackFromIndex(SpineModelComponent* component, int track_index)
    {
        if (track_index < 0 || track_index >= component->m_AnimationTracks.Size())
            return nullptr;
        return &component->m_AnimationTracks[track_index];
    }

    static void ClearCompletionCallback(SpineAnimationTrack* track)
    {
        if (track->m_CallbackInfo)
        {
            dmScript::DestroyCallback(track->m_CallbackInfo);
            track->m_CallbackInfo = 0x0;
        }
    }

    static void ClearAnimationTrackBindings(SpineModelComponent* component)
    {
        for (uint32_t i = 0; i < component->m_AnimationTracks.Size(); ++i)
        {
            ClearCompletionCallback(&component->m_AnimationTracks[i]);
            component->m_AnimationTracks[i].m_AnimationInstance = 0;
        }
        component->m_AnimationTracks.SetSize(0);
    }

    static bool PlayAnimation(SpineModelComponent* component, dmhash_t animation_id, dmGameObject::Playback playback,
        float blend_duration, float offset, float playback_rate, int track_index, dmGameSystemDDF::MixBlend mix_blend, float alpha)
    {
        uint32_t index = FindAnimationIndex(component, animation_id);
        if (index == INVALID_ANIMATION_INDEX)
        {
            dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
            return false;
        }

        SpineSceneData* scene_data = GetSpineSceneData(component);

        bool loop = IsLooping(playback);
        spine::Array<spine::Animation*>& animations = scene_data->m_Skeleton->getAnimations();
        if (index >= animations.size())
        {
            dmLogError("No animation index %u is too large. Number of animations are %u", index, (uint32_t)animations.size());
            return false;
        }

        spine::Animation* animation = animations[index];

        if (track_index < 0)
        {
            dmLogError("Invalid track index %d", track_index);
            return false;
        }

        // Spine 4.3 replaced mixBlend with an additive flag. MIX_BLEND_ADD maps
        // directly; the setup/first/replace modes all use normal replacement.
        bool additive = mix_blend == dmGameSystemDDF::MixBlend::MIX_BLEND_ADD;

        if (track_index >= component->m_AnimationTracks.Capacity())
        {
            component->m_AnimationTracks.SetCapacity(track_index + 4);
        }

        while (track_index >= component->m_AnimationTracks.Size())
        {
            SpineAnimationTrack track;
            track.m_AnimationInstance = nullptr;
            track.m_CallbackInfo = 0x0;
            component->m_AnimationTracks.Push(track);
        }
        SpineAnimationTrack& track = component->m_AnimationTracks[track_index];

        ClearCompletionCallback(&track);

        track.m_AnimationId = animation_id;
        track.m_AnimationInstance = &component->m_AnimationStateInstance->setAnimation(track_index, *animation, loop);

        track.m_Playback = playback;
        track.m_AnimationInstance->setTimeScale(playback_rate);
        track.m_AnimationInstance->setReverse(IsReverse(playback));
        track.m_AnimationInstance->setMixDuration(blend_duration);
        track.m_AnimationInstance->setTrackTime(dmMath::Clamp(offset,
            track.m_AnimationInstance->getAnimationStart(), track.m_AnimationInstance->getAnimationEnd()));
        track.m_AnimationInstance->setAdditive(additive);
        track.m_AnimationInstance->setAlpha(alpha);

        track.m_CallbackInfo = 0x0;
        dmMessage::ResetURL(&track.m_Listener);

        return true;
    }

    static bool SetupComponentFromScene(SpineModelWorld* world, SpineModelComponent* component, SpineSceneResource* spine_scene, bool create_bones, bool play_default_animation)
    {
        component->m_SceneData = RetainSceneData(spine_scene);
        SpineSceneData* scene_data = component->m_SceneData;
        component->m_SkeletonInstance = new spine::Skeleton(*scene_data->m_Skeleton);
        if (!component->m_SkeletonInstance)
        {
            dmLogError("Failed to create skeleton instance");
            ReleaseSceneData(component->m_SceneData);
            component->m_SceneData = 0;
            return false;
        }

        const char* skin_name = component->m_Resource->m_Ddf->m_Skin;
        spine::Skin* skin = skin_name && skin_name[0]
            ? scene_data->m_Skeleton->findSkin(skin_name)
            : 0;
        component->m_SkeletonInstance->setSkin(skin ? skin : scene_data->m_Skeleton->getDefaultSkin());
        component->m_SkeletonInstance->setupPoseSlots();

        component->m_AnimationStateInstance = new spine::AnimationState(*scene_data->m_AnimationStateData);
        if (!component->m_AnimationStateInstance)
        {
            dmLogError("Failed to create animation state instance");
            delete component->m_SkeletonInstance;
            component->m_SkeletonInstance = 0;
            ReleaseSceneData(component->m_SceneData);
            component->m_SceneData = 0;
            return false;
        }

        component->m_AnimationStateInstance->setListener(SpineEventListener, component);

        if (component->m_AnimationTracks.Capacity() < 8)
        {
            component->m_AnimationTracks.SetCapacity(8);
        }

        component->m_SkeletonInstance->setupPose();
        component->m_SkeletonInstance->updateWorldTransform(spine::Physics_Update);

        if (create_bones)
        {
            if (!CreateGOBones(world, component))
            {
                dmLogError("Failed to create game objects for bones in spine model. Consider increasing collection max instances (collection.max_instances).");
                return false;
            }
        }

        const char* default_animation = component->m_Resource->m_Ddf->m_DefaultAnimation;
        if (play_default_animation && default_animation && default_animation[0])
        {
            dmhash_t animation_id = dmHashString64(default_animation);
            PlayAnimation(component, animation_id, dmGameObject::PLAYBACK_LOOP_FORWARD, 0.0f,
                component->m_Resource->m_Ddf->m_Offset, component->m_Resource->m_Ddf->m_PlaybackRate, 0, dmGameSystemDDF::MixBlend::MIX_BLEND_FIRST, 1.0f);
                // TODO: Is the default playmode specified anywhere?
        }

        component->m_ReHash = 1;
        return true;
    }

    static void CancelTrackAnimation(SpineModelComponent* component, int32_t track_index)
    {
        SpineAnimationTrack* track = GetTrackFromIndex(component, track_index);
        if (!track || !track->m_AnimationInstance)
            return;

        component->m_AnimationStateInstance->clearTrack(track->m_AnimationInstance->getTrackIndex());

        ClearCompletionCallback(track);
        track->m_AnimationInstance = nullptr;
    }

    static void CancelAllAnimations(SpineModelComponent* component)
    {
        for (int32_t i = 0; i < component->m_AnimationTracks.Size(); i++) {
            CancelTrackAnimation(component, i);
        }
    }

    static bool GetSender(SpineModelComponent* component, dmMessage::URL* out_sender)
    {
        dmMessage::URL sender;
        sender.m_Socket = dmGameObject::GetMessageSocket(dmGameObject::GetCollection(component->m_Instance));
        if (dmMessage::IsSocketValid(sender.m_Socket))
        {
            dmGameObject::Result go_result = dmGameObject::GetComponentId(component->m_Instance, component->m_ComponentIndex, &sender.m_Fragment);
            if (go_result == dmGameObject::RESULT_OK)
            {
                sender.m_Path = dmGameObject::GetIdentifier(component->m_Instance);
                *out_sender = sender;
                return true;
            }
        }
        return false;
    }

    static void SendAnimationDone(SpineModelComponent* component, spine::AnimationState* state, spine::TrackEntry* entry, spine::Event* event)
    {
        SpineAnimationTrack& track = component->m_AnimationTracks[entry->getTrackIndex()];

        dmMessage::URL sender;
        dmMessage::URL receiver = track.m_Listener;

        if (!GetSender(component, &sender))
        {
            dmLogError("Could not send animation_done to listener because of incomplete component.");
            return;
        }

        dmGameSystemDDF::SpineAnimationDone message;
        message.m_AnimationId = dmHashString64(entry->getAnimation().getName().buffer());
        message.m_Playback    = track.m_Playback;
        message.m_Track       = entry->getTrackIndex() + 1;

        if (track.m_CallbackInfo)
        {
            uint32_t id = track.m_CallbackId;
            RunTrackCallback(track.m_CallbackInfo, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message, &sender);
            // If, in a Lua callback, the user calls spine.play_anim(),
            // it will destroy the current callback and create a new one (if specified).
            // Therefore, we need to check whether we are going to remove the same callback
            // that we are running. If not, it has already been removed.
            if (id == track.m_CallbackId)
            {
                ClearCompletionCallback(&track);
            }

        }
        else
        {
            dmGameObject::Result result = dmGameObject::PostDDF(&message, &sender, &receiver, 0, true);
            if (result != dmGameObject::RESULT_OK)
            {
                dmLogError("Could not send animation_done to listener: %d", result);
            }
        }
    }

    static void SendSpineEvent(SpineModelComponent* component, spine::AnimationState* state, spine::TrackEntry* entry, spine::Event* event)
    {
        SpineAnimationTrack& track = component->m_AnimationTracks[entry->getTrackIndex()];

        dmMessage::URL sender;
        dmMessage::URL receiver = track.m_Listener;

        if (!GetSender(component, &sender))
        {
            dmLogError("Could not send animation_done to listener because of incomplete component.");
            return;
        }

        if (!dmMessage::IsSocketValid(receiver.m_Socket))
        {
            receiver = sender;
            receiver.m_Fragment = 0;
        }

        dmGameSystemDDF::SpineEvent message;
        message.m_AnimationId = dmHashString64(entry->getAnimation().getName().buffer());
        message.m_EventId     = dmHashString64(event->getData().getName().buffer());
        message.m_BlendWeight = 0.0f;//keyframe_event->m_BlendWeight;
        message.m_T           = event->getTime();
        message.m_Integer     = event->getInt();
        message.m_Float       = event->getFloat();
        const char* event_string = event->getString().buffer();
        message.m_String      = dmHashString64(event_string ? event_string : "");
        message.m_Node.m_Ref  = 0;
        message.m_Node.m_ContextTableRef = 0;
        message.m_Track       = entry->getTrackIndex() + 1;

        if (track.m_CallbackInfo)
        {
            RunTrackCallback(track.m_CallbackInfo, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message, &sender);
        }
        else
        {
            dmGameObject::Result result = dmGameObject::PostDDF(&message, &sender, &receiver, 0, false);
            if (result != dmGameObject::RESULT_OK)
            {
                dmLogError("Could not send animation event '%s' from animation '%s' to listener: %d",
                    entry->getAnimation().getName().buffer(), event->getData().getName().buffer(), result);
            }
        }
    }

    static void SpineEventListener(spine::AnimationState* state, spine::EventType type, spine::TrackEntry* entry, spine::Event* event, void* user_data)
    {
        SpineModelComponent* component = (SpineModelComponent*)user_data;

        // Events are explained here: http://esotericsoftware.com/spine-api-reference#AnimationStateListener
        switch (type)
        {
            // case spine::EventType_Start:
            // {
            //     printf("Animation %s started on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            // case spine::EventType_Interrupt:
            // {
            //     printf("Animation %s interrupted on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            // case spine::EventType_End:
            // {
            //     printf("Animation %s ended on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            case spine::EventType_Complete:
            {
                if (entry->getMixingTo() != 0)
                {
                    // While mixing, the previous animation completed.
                    // The problem is that the track is already owned by the new animation, and we
                    // cannot call the previous callback (as we haven't stored it)
                    return;
                }

                SpineAnimationTrack& track = component->m_AnimationTracks[entry->getTrackIndex()];

                // Should we look at the looping state?
                if (!IsLooping(track.m_Playback))
                {
                    // We only send the event if it's not looping (same behavior as before)
                    SendAnimationDone(component, state, entry, event);
                }

                if (IsPingPong(track.m_Playback))
                {
                    track.m_AnimationInstance->setReverse(!track.m_AnimationInstance->getReverse());
                }
                break;
            }
            case spine::EventType_Dispose:
            {
                SpineAnimationTrack* track = GetTrackFromIndex(component, entry->getTrackIndex());
                if (track && track->m_AnimationInstance == entry)
                {
                    ClearCompletionCallback(track);
                    track->m_AnimationInstance = nullptr;
                }
                break;
            }
            case spine::EventType_Event:
                SendSpineEvent(component, state, entry, event);
                break;
            default:
                break;
        }
    }

    dmGameObject::CreateResult CompSpineModelCreate(const dmGameObject::ComponentCreateParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        if (world->m_Components.Full())
        {
            dmLogError("Spine Model could not be created since the buffer is full (%d).", world->m_Components.Capacity());
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }

        SpineModelResource* spine_model = (SpineModelResource*)params.m_Resource;

        uint32_t index = world->m_Components.Alloc();
        SpineModelComponent* component = new SpineModelComponent;
        memset(component, 0, sizeof(SpineModelComponent));
        world->m_Components.Set(index, component);
        component->m_Instance = params.m_Instance;
        component->m_Transform = dmTransform::Transform(Vector3(params.m_Position), params.m_Rotation, params.m_Scale);
        component->m_Resource = (SpineModelResource*)params.m_Resource;

        SpineSceneResource* spine_scene = GetSpineScene(component);
        component->m_ComponentIndex = params.m_ComponentIndex;
        component->m_Enabled = 1;
        component->m_World = Matrix4::identity();
        component->m_DoRender = 0;
        component->m_RenderConstants = 0;

        if (!SetupComponentFromScene(world, component, spine_scene, spine_model->m_CreateGoBones, true))
        {
            DestroyComponent(world, index);
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }

        *params.m_UserData = (uintptr_t)index;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static void DestroyComponent(SpineModelWorld* world, uint32_t index)
    {
        SpineModelComponent* component = world->m_Components.Get(index);
        dmGameObject::DeleteBones(component->m_Instance);
        // If we're going to use memset, then we should explicitly clear pose and instance arrays.
        component->m_BoneInstances.SetCapacity(0);
        ClearAnimationTrackBindings(component);
        component->m_AnimationTracks.SetCapacity(0);
        if (component->m_Material)
        {
            dmResource::Release(world->m_Factory, (void*)component->m_Material);
        }
        if (component->m_RenderConstants)
        {
            dmGameSystem::DestroyRenderConstants(component->m_RenderConstants);
        }

        ClearIKTargetBindings(component);
        if (component->m_AnimationStateInstance)
            delete component->m_AnimationStateInstance;
        if (component->m_SkeletonInstance)
            delete component->m_SkeletonInstance;
        ReleaseSceneData(component->m_SceneData);
        component->m_SceneData = 0;
        // Skeleton and AnimationState reference data in the scene resource, so
        // release an overridden scene only after destroying those instances.
        if (component->m_SpineScene)
        {
            dmResource::Release(world->m_Factory, (void*)component->m_SpineScene);
        }

        delete component;
        world->m_Components.Free(index, true);
    }

    dmGameObject::CreateResult CompSpineModelDestroy(const dmGameObject::ComponentDestroyParams& params)
    {
        //SpineModelContext* ctx = (SpineModelContext*)params.m_Context;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        uint32_t index = *params.m_UserData;
        DestroyComponent(world, index);
        return dmGameObject::CREATE_RESULT_OK;
    }

    static bool UpdateBones(SpineModelComponent* component)
    {
        if (component->m_BoneInstances.Empty())
            return false;

        uint32_t size = component->m_Bones.Size();
        DM_PROPERTY_ADD_U32(rmtp_SpineBones, size);
        for (uint32_t n = 0; n < size; ++n)
        {
            spine::Bone* bone = component->m_Bones[n];

            dmGameObject::HInstance bone_instance = component->m_BoneInstances[n];
            SetTransformFromBone(bone_instance, component->m_Transform, bone);
        }

        return true;
    }

    dmGameObject::CreateResult CompSpineModelAddToUpdate(const dmGameObject::ComponentAddToUpdateParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        uint32_t index = (uint32_t)*params.m_UserData;
        SpineModelComponent* component = GetComponentFromIndex(world, index);
        component->m_AddedToUpdate = true;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static void ApplyIKTargets(SpineModelComponent* component)
    {
        uint32_t count = component->m_IKTargetPositions.Size();
        uint32_t instance_count = component->m_IKTargets.Size();
        if (count == 0 && instance_count == 0)
            return;

        const dmTransform::Transform world_to_model =
            dmTransform::Inv(dmTransform::Mul(dmGameObject::GetWorldTransform(component->m_Instance), component->m_Transform));

        for (uint32_t i = 0; i < count; ++i)
        {
            const IKTarget& target = component->m_IKTargetPositions[i];
            dmVMath::Vector3 model_pos = (dmVMath::Vector3)dmTransform::Apply(world_to_model, target.m_Position);

            target.m_Constraint->getTarget().getPose().setPosition(model_pos.getX(), model_pos.getY());
        }
        component->m_IKTargetPositions.SetSize(0);

        for (uint32_t i = 0; i < instance_count; ++i)
        {
            const IKTarget& target = component->m_IKTargets[i];

            dmVMath::Vector3 model_pos = (dmVMath::Vector3)dmTransform::Apply(world_to_model, dmGameObject::GetWorldPosition(target.m_Target));

            target.m_Constraint->getTarget().getPose().setPosition(model_pos.getX(), model_pos.getY());
        }
    }


    dmGameObject::UpdateResult CompSpineModelUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        float dt = params.m_UpdateContext->m_DT;

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();
        DM_PROPERTY_ADD_U32(rmtp_SpineComponents, count);
        bool transforms_updated = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent& component = *components[i];
            component.m_DoRender = 0;

            if (!component.m_SkeletonInstance || !component.m_AnimationStateInstance)
            {
                component.m_Enabled = false;
            }

            if (!component.m_Enabled || !component.m_AddedToUpdate)
                continue;

            const Matrix4& go_world = dmGameObject::GetWorldMatrix(component.m_Instance);
            const Matrix4 local = dmTransform::ToMatrix4(component.m_Transform);
            component.m_World = go_world * local;

            // docs: http://esotericsoftware.com/spine-runtime-skeletons
            component.m_AnimationStateInstance->update(dt);
            component.m_AnimationStateInstance->apply(*component.m_SkeletonInstance);

            ApplyIKTargets(&component);

            component.m_SkeletonInstance->update(dt);
            component.m_SkeletonInstance->updateWorldTransform(spine::Physics_Update);

            // Update the game world objects
            transforms_updated |= UpdateBones(&component);

            if (component.m_ReHash || (component.m_RenderConstants && dmGameSystem::AreRenderConstantsUpdated(component.m_RenderConstants)))
            {
                ReHash(&component);
            }

            component.m_DoRender = 1;
        }

        // Since we've moved the child game objects (bones), we need to sync back the transforms
        update_result.m_TransformsUpdated = transforms_updated;
        return dmGameObject::UPDATE_RESULT_OK;
    }

    dmGameObject::UpdateResult CompSpineModelPostUpdate(const dmGameObject::ComponentsPostUpdateParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();
        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent* component = components[i];
            if (!component || !component->m_RebuildBonesPending)
                continue;

            component->m_RebuildBonesPending = 0;
            if (component->m_Resource->m_CreateGoBones)
            {
                if (!CreateGOBones(world, component))
                {
                    dmLogError("Failed to create game objects for bones in spine model. Consider increasing collection max instances (collection.max_instances).");
                }
            }
        }
        return dmGameObject::UPDATE_RESULT_OK;
    }

    static inline dmGameSystemDDF::SpineModelDesc::BlendMode SpineBlendModeToRenderBlendMode(spine::BlendMode spine_blend_mode)
    {
        switch(spine_blend_mode)
        {
            case spine::BlendMode_Normal:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA;
            case spine::BlendMode_Additive:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ADD;
            case spine::BlendMode_Multiply:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_MULT;
            case spine::BlendMode_Screen:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_SCREEN;
            default:break;
        }
        return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA;
    }

    static inline uint32_t GetIndexTypeSize(const SpineModelWorld* world)
    {
        return world->m_Is16BitIndex ? sizeof(uint16_t) : sizeof(uint32_t);
    }

    static inline dmGraphics::Type GetIndexType(const SpineModelWorld* world)
    {
        return world->m_Is16BitIndex ? dmGraphics::TYPE_UNSIGNED_SHORT : dmGraphics::TYPE_UNSIGNED_INT;
    }

    static void PackIndexBufferData(SpineModelWorld* world)
    {
        uint32_t index_count = world->m_IndexBufferData.Size();
        uint32_t index_data_size = index_count * GetIndexTypeSize(world);
        if (world->m_PackedIndexBufferData.Capacity() < index_data_size)
        {
            uint32_t new_capacity = dmMath::Max(index_data_size, dmMath::Max(64U, world->m_PackedIndexBufferData.Capacity() * 2));
            world->m_PackedIndexBufferData.SetCapacity(new_capacity);
        }
        world->m_PackedIndexBufferData.SetSize(index_data_size);

        if (index_data_size == 0)
        {
            return;
        }

        if (world->m_Is16BitIndex)
        {
            uint16_t* packed_indices = (uint16_t*)world->m_PackedIndexBufferData.Begin();
            for (uint32_t i = 0; i < index_count; ++i)
            {
                assert(world->m_IndexBufferData[i] <= 0xFFFF);
                packed_indices[i] = (uint16_t)world->m_IndexBufferData[i];
            }
        }
        else
        {
            memcpy(world->m_PackedIndexBufferData.Begin(), world->m_IndexBufferData.Begin(), index_data_size);
        }
    }

    static void FillRenderObject(SpineModelWorld*  world,
        dmRender::RenderObject&                    ro,
        dmGameSystem::HComponentRenderConstants    constants,
        dmGraphics::HTexture                       texture,
        dmRender::HMaterial                        material,
        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode,
        uint32_t                                   index_start,
        uint32_t                                   index_count)
    {
        ro.Init();
        ro.m_VertexDeclaration = world->m_VertexDeclaration;
        ro.m_VertexBuffer      = world->m_VertexBuffer;
        ro.m_IndexBuffer       = world->m_IndexBuffer;
        ro.m_PrimitiveType     = dmGraphics::PRIMITIVE_TRIANGLES;
        // Keep the logical index offset until all visible geometry has been generated
        // and the final 16/32-bit index type is known.
        ro.m_VertexStart       = index_start;
        ro.m_VertexCount       = index_count;
        ro.m_Textures[0]       = texture;
        ro.m_Material          = material;

        if (constants)
        {
            dmGameSystem::EnableRenderObjectConstants(&ro, constants);
        }

        ro.m_SetBlendFactors = 1;

        switch (blend_mode)
         {
            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ADD:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
            break;

            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_MULT:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_DST_COLOR;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_SCREEN:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
            break;

            default:
                dmLogError("Unknown blend mode: %d\n", blend_mode);
                assert(0);
            break;
        }
    }

    static void RenderBatch(SpineModelWorld* world, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        //DM_PROFILE(SpineModel, "RenderBatch");

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();

        uint32_t component_index           = (uint32_t)buf[*begin].m_UserData;
        const SpineModelComponent* first   = (const SpineModelComponent*) components[component_index];
        const SpineModelResource* resource = first->m_Resource;

        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode = resource->m_Ddf->m_BlendMode;
        bool use_inherit_blend = blend_mode == dmGameSystemDDF::SpineModelDesc::BLEND_MODE_INHERIT;

        uint32_t index_start            = world->m_IndexBufferData.Size();
        uint32_t draw_desc_buffer_count = 0;

        // This is a temporary scratch buffer just used for this batch call, so we make sure to reset it.
        world->m_DrawDescBuffer.SetSize(0);

        for (uint32_t *i = begin; i != end; ++i)
        {
            component_index = (uint32_t)buf[*i].m_UserData;
            if (use_inherit_blend)
            {
                const SpineModelComponent* component = (const SpineModelComponent*) components[component_index];
                draw_desc_buffer_count += dmSpine::CalcDrawDescCount(component->m_SkeletonInstance);
            }
        }

        if (draw_desc_buffer_count && draw_desc_buffer_count > world->m_DrawDescBuffer.Capacity())
        {
            uint32_t new_capacity = dmMath::Max(draw_desc_buffer_count, dmMath::Max(32U, world->m_DrawDescBuffer.Capacity() * 2));
            world->m_DrawDescBuffer.SetCapacity(new_capacity);
        }

        for (uint32_t *i = begin; i != end; ++i)
        {
            component_index = (uint32_t)buf[*i].m_UserData;
            const SpineModelComponent* component = (const SpineModelComponent*) components[component_index];
            dmSpine::GenerateIndexedVertexData(world->m_VertexBufferData, world->m_IndexBufferData, component->m_SkeletonInstance, world->m_SkeletonRenderer, component->m_World, Vector4(1.0f), use_inherit_blend ? &world->m_DrawDescBuffer : 0, world->m_GeometryScratch);
        }

        uint32_t index_count = world->m_IndexBufferData.Size() - index_start;
        if (index_count == 0)
        {
            return;
        }

        dmGraphics::HTexture texture = GetSpineSceneData(first)->m_TextureSet->m_Texture->m_Texture; // spine - texture set resource - texture resource - texture
        dmRender::HMaterial material = GetMaterial(first);

        if (use_inherit_blend)
        {
            uint32_t draw_desc_count = world->m_DrawDescBuffer.Size();
            if (draw_desc_count > 0)
            {
                MergeIndexedDrawDescs(world->m_DrawDescBuffer, world->m_MergedDrawDescBuffer);

                uint32_t merged_size = world->m_MergedDrawDescBuffer.Size();
                uint32_t ro_count_begin = world->m_RenderObjects.Size();
                uint32_t expected_size = world->m_RenderObjects.Size() + merged_size;
                if (world->m_RenderObjects.Capacity() < expected_size)
                {
                    uint32_t new_capacity = dmMath::Max(expected_size, dmMath::Max(32U, world->m_RenderObjects.Capacity() * 2));
                    world->m_RenderObjects.SetCapacity(new_capacity);
                }
                world->m_RenderObjects.SetSize(expected_size);

                for (int i = 0; i < merged_size; ++i)
                {
                    dmRender::RenderObject& ro = world->m_RenderObjects[ro_count_begin + i];
                    FillRenderObject(world, ro, first->m_RenderConstants, texture, material,
                        SpineBlendModeToRenderBlendMode((spine::BlendMode) world->m_MergedDrawDescBuffer[i].m_BlendMode),
                        world->m_MergedDrawDescBuffer[i].m_IndexStart,
                        world->m_MergedDrawDescBuffer[i].m_IndexCount);
                }
            }
        }
        else
        {
            uint32_t ro_index = world->m_RenderObjects.Size();
            world->m_RenderObjects.SetSize(ro_index + 1);
            dmRender::RenderObject& ro = world->m_RenderObjects[ro_index];
            FillRenderObject(world, ro, first->m_RenderConstants, texture, material, blend_mode, index_start, index_count);
        }
    }

    static void RenderListFrustumCulling(dmRender::RenderListVisibilityParams const &params)
    {
        DM_PROFILE("SpineModel");

        SpineModelWorld* world = (SpineModelWorld*)params.m_UserData;

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const dmIntersection::Frustum frustum = *params.m_Frustum;
        uint32_t num_entries = params.m_NumEntries;
        for (uint32_t i = 0; i < num_entries; ++i)
        {
            dmRender::RenderListEntry* entry = &params.m_Entries[i];
            int component_index = entry->m_UserData;

            SpineModelComponent* component_p = components[component_index];

            const SpineModelBounds& bounds = world->m_BoundingBoxes[component_index];
            if (bounds.minX > bounds.maxX || bounds.minY > bounds.maxY)
            {
                entry->m_Visibility = dmRender::VISIBILITY_NONE;
                continue;
            }

            // get center of bounding box in local coords
            float center_x = (bounds.maxX + bounds.minX)/2;
            float center_y = (bounds.maxY + bounds.minY)/2;
            dmVMath::Point3 center_local(center_x, center_y, 0);
            dmVMath::Point3 corner_local(bounds.maxX, bounds.maxY, 0);

            // transform to world coords
            dmVMath::Vector4 center_world = component_p->m_World * center_local;
            dmVMath::Vector4 corner_world = component_p->m_World * corner_local;

            float radius =  Vectormath::Aos::length(corner_world - center_world);

            bool intersect = dmIntersection::TestFrustumSphere(frustum, center_world, radius);
            entry->m_Visibility = intersect ? dmRender::VISIBILITY_FULL : dmRender::VISIBILITY_NONE;
        }
    }


    static void RenderListDispatch(dmRender::RenderListDispatchParams const &params)
    {
        SpineModelWorld *world = (SpineModelWorld *) params.m_UserData;

        switch (params.m_Operation)
        {
            case dmRender::RENDER_LIST_OPERATION_BEGIN:
            {
                world->m_RenderObjects.SetSize(0);
                world->m_VertexBufferData.SetSize(0);
                world->m_IndexBufferData.SetSize(0);
                world->m_PackedIndexBufferData.SetSize(0);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_BATCH:
            {
                RenderBatch(world, params.m_Buf, params.m_Begin, params.m_End);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_END:
            {
                world->m_Is16BitIndex = world->m_VertexBufferData.Size() <= 65536;
                uint32_t vertex_data_size = sizeof(dmSpine::SpineVertex) * world->m_VertexBufferData.Size();
                uint32_t index_data_size = GetIndexTypeSize(world) * world->m_IndexBufferData.Size();
                if (vertex_data_size && index_data_size)
                {
                    PackIndexBufferData(world);
                    dmGraphics::SetVertexBufferData(world->m_VertexBuffer, vertex_data_size,
                                                    world->m_VertexBufferData.Begin(), dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
                    dmGraphics::SetIndexBufferData(world->m_IndexBuffer, index_data_size,
                                                   world->m_PackedIndexBufferData.Begin(), dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);

                    uint32_t index_type_size = GetIndexTypeSize(world);
                    dmGraphics::Type index_type = GetIndexType(world);
                    for (uint32_t i = 0; i < world->m_RenderObjects.Size(); ++i)
                    {
                        dmRender::RenderObject& ro = world->m_RenderObjects[i];
                        ro.m_IndexType = index_type;
                        ro.m_VertexStart *= index_type_size;
                        dmRender::AddToRender(params.m_Context, &ro);
                    }

                    DM_PROPERTY_ADD_U32(rmtp_SpineVertexCount, world->m_VertexBufferData.Size());
                    DM_PROPERTY_ADD_U32(rmtp_SpineVertexSize, vertex_data_size);
                    DM_PROPERTY_ADD_U32(rmtp_SpineIndexSize, index_data_size);
                }
                break;
            }
            default:
                assert(false);
                break;
        }
    }

    dmGameObject::UpdateResult CompSpineModelRender(const dmGameObject::ComponentsRenderParams& params)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        dmRender::HRenderContext render_context = context->m_RenderContext;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        //UpdateTransforms(world);

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();

        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent& component = *components[i];
            if (!component.m_DoRender || !component.m_Enabled)
                continue;

            SpineModelBounds& bounds = world->m_BoundingBoxes[i];
            dmSpine::GetSkeletonBounds(component.m_SkeletonInstance, bounds, world->m_GeometryScratch);
        }

        // Prepare list submit
        dmRender::RenderListEntry* render_list = dmRender::RenderListAlloc(render_context, count);
        dmRender::HRenderListDispatch dispatch = dmRender::RenderListMakeDispatch(render_context, &RenderListDispatch, &RenderListFrustumCulling, world);
        dmRender::RenderListEntry* write_ptr = render_list;

        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent& component = *components[i];
            if (!component.m_DoRender || !component.m_Enabled)
                continue;

            const Vector4 trans = component.m_World.getCol(3);
            write_ptr->m_WorldPosition = Point3(trans.getX(), trans.getY(), trans.getZ());
            write_ptr->m_UserData = (uintptr_t) i;
            write_ptr->m_BatchKey = component.m_MixedHash;
            write_ptr->m_TagListKey = dmRender::GetMaterialTagListKey(GetMaterial(&component));
            write_ptr->m_Dispatch = dispatch;
            write_ptr->m_MinorOrder = 0;
            write_ptr->m_MajorOrder = dmRender::RENDER_ORDER_WORLD;
            ++write_ptr;
        }

        dmRender::RenderListSubmit(render_context, render_list, write_ptr);
        return dmGameObject::UPDATE_RESULT_OK;
    }

    static bool CompSpineModelGetConstantCallback(void* user_data, dmhash_t name_hash, dmRender::Constant** out_constant)
    {
        SpineModelComponent* component = (SpineModelComponent*)user_data;
        return component->m_RenderConstants && dmGameSystem::GetRenderConstant(component->m_RenderConstants, name_hash, out_constant);
    }

    static void CompSpineModelSetConstantCallback(void* user_data, dmhash_t name_hash, int32_t value_index, uint32_t* element_index, const dmGameObject::PropertyVar& var)
    {
        SpineModelComponent* component = (SpineModelComponent*)user_data;
        if (!component->m_RenderConstants)
            component->m_RenderConstants = dmGameSystem::CreateRenderConstants();
        dmGameSystem::SetRenderConstant(component->m_RenderConstants, GetMaterial(component), name_hash, value_index, element_index, var);
        component->m_ReHash = 1;
    }

    bool CompSpineModelPlayAnimation(SpineModelComponent* component, dmGameSystemDDF::SpinePlayAnimation* message, dmMessage::URL* sender, dmScript::LuaCallbackInfo* callback_info, lua_State* L)
    {
        bool result = PlayAnimation(component, message->m_AnimationId, (dmGameObject::Playback)message->m_Playback, message->m_BlendDuration,
                                                message->m_Offset, message->m_PlaybackRate, message->m_Track - 1, message->m_MixBlend, message->m_Alpha);
        if (result)
        {
            SpineAnimationTrack& track = component->m_AnimationTracks[message->m_Track - 1];
            track.m_Listener = *sender;
            track.m_Context = L;
            track.m_CallbackId++;
            track.m_CallbackInfo = callback_info;
        }
        return result;
    }

    bool CompSpineModelCancelAnimation(SpineModelComponent* component, dmGameSystemDDF::SpineCancelAnimation* message)
    {
        if (message->m_Track == ALL_TRACKS)
        {
            CancelAllAnimations(component);
        } else {
            CancelTrackAnimation(component, message->m_Track - 1);
        }
        return true;
    }

    bool CompSpineModelResetConstant(SpineModelComponent* component, dmGameSystemDDF::ResetConstant* message)
    {
        if (component->m_RenderConstants)
        {
            component->m_ReHash |= dmGameSystem::ClearRenderConstant(component->m_RenderConstants, message->m_NameHash);
        }
        return true;
    }

    dmGameObject::UpdateResult CompSpineModelOnMessage(const dmGameObject::ComponentOnMessageParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        SpineModelComponent* component = GetComponentFromIndex(world, *params.m_UserData);
        if (params.m_Message->m_Id == dmGameObjectDDF::Enable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 1;
        }
        else if (params.m_Message->m_Id == dmGameObjectDDF::Disable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 0;
        }
        else if (params.m_Message->m_Descriptor != 0x0)
        {
        }

        return dmGameObject::UPDATE_RESULT_OK;
    }

    struct ReloadTrackEntryState
    {
        dmhash_t m_AnimationId;
        float    m_AnimationDuration;
        float    m_AnimationStart;
        float    m_AnimationEnd;
        float    m_AnimationLast;
        float    m_TrackTime;
        float    m_TrackEnd;
        float    m_TimeScale;
        float    m_Delay;
        float    m_Alpha;
        float    m_MixTime;
        float    m_MixDuration;
        float    m_EventThreshold;
        float    m_MixAttachmentThreshold;
        float    m_AlphaAttachmentThreshold;
        float    m_MixDrawOrderThreshold;
        uint8_t  m_Loop;
        uint8_t  m_Additive;
        uint8_t  m_Reverse;
        uint8_t  m_ShortestRotation;
    };

    struct ReloadTrackState
    {
        uint32_t                   m_TrackIndex;
        uint32_t                   m_FirstEntry;
        uint32_t                   m_EntryCount;
        dmhash_t                   m_AnimationId;
        dmGameObject::Playback     m_Playback;
        dmMessage::URL             m_Listener;
        lua_State*                 m_Context;
        dmScript::LuaCallbackInfo* m_CallbackInfo;
        uint32_t                   m_CallbackId;
    };

    struct ReloadIKTargetState
    {
        dmhash_t                m_ConstraintHash;
        dmGameObject::HInstance m_Target;
        dmVMath::Point3         m_Position;
        float                   m_Mix;
        uint8_t                 m_IsPosition;
        uint8_t                 m_TargetWasGeneratedBone;
    };

    template <typename T>
    static void PushReloadValue(dmArray<T>& values, const T& value, uint32_t capacity_increment = 4)
    {
        if (values.Full())
            values.OffsetCapacity(capacity_increment);
        values.Push(value);
    }

    static ReloadTrackEntryState CaptureReloadTrackEntry(spine::TrackEntry* entry)
    {
        ReloadTrackEntryState state;
        state.m_AnimationId = dmHashString64(entry->getAnimation().getName().buffer());
        state.m_AnimationDuration = entry->getAnimation().getDuration();
        state.m_AnimationStart = entry->getAnimationStart();
        state.m_AnimationEnd = entry->getAnimationEnd();
        state.m_AnimationLast = entry->getAnimationLast();
        state.m_TrackTime = entry->getTrackTime();
        state.m_TrackEnd = entry->getTrackEnd();
        state.m_TimeScale = entry->getTimeScale();
        state.m_Delay = entry->getDelay();
        state.m_Alpha = entry->getAlpha();
        state.m_MixTime = entry->getMixTime();
        state.m_MixDuration = entry->getMixDuration();
        state.m_EventThreshold = entry->getEventThreshold();
        state.m_MixAttachmentThreshold = entry->getMixAttachmentThreshold();
        state.m_AlphaAttachmentThreshold = entry->getAlphaAttachmentThreshold();
        state.m_MixDrawOrderThreshold = entry->getMixDrawOrderThreshold();
        state.m_Loop = entry->getLoop() ? 1 : 0;
        state.m_Additive = entry->getAdditive() ? 1 : 0;
        state.m_Reverse = entry->getReverse() ? 1 : 0;
        state.m_ShortestRotation = entry->getShortestRotation() ? 1 : 0;
        return state;
    }

    static void CaptureReloadTracks(SpineModelComponent* component, dmArray<ReloadTrackState>& track_states,
                                    dmArray<ReloadTrackEntryState>& entry_states)
    {
        for (uint32_t track_index = 0; track_index < component->m_AnimationTracks.Size(); ++track_index)
        {
            SpineAnimationTrack& track = component->m_AnimationTracks[track_index];
            if (!track.m_AnimationInstance)
            {
                ClearCompletionCallback(&track);
                continue;
            }

            dmArray<spine::TrackEntry*> chain;
            for (spine::TrackEntry* entry = track.m_AnimationInstance; entry; entry = entry->getMixingFrom())
            {
                PushReloadValue(chain, entry);
            }

            ReloadTrackState track_state;
            memset(&track_state, 0, sizeof(track_state));
            track_state.m_TrackIndex = track_index;
            track_state.m_FirstEntry = entry_states.Size();
            track_state.m_AnimationId = track.m_AnimationId;
            track_state.m_Playback = track.m_Playback;
            track_state.m_Listener = track.m_Listener;
            track_state.m_Context = track.m_Context;
            track_state.m_CallbackInfo = track.m_CallbackInfo;
            track_state.m_CallbackId = track.m_CallbackId;

            // Recreate mixing chains from oldest to current.
            for (int32_t chain_index = (int32_t)chain.Size() - 1; chain_index >= 0; --chain_index)
            {
                ReloadTrackEntryState entry_state = CaptureReloadTrackEntry(chain[chain_index]);
                PushReloadValue(entry_states, entry_state, 8);
            }
            track_state.m_EntryCount = entry_states.Size() - track_state.m_FirstEntry;
            PushReloadValue(track_states, track_state);

            // Ownership is transferred to track_state. The old AnimationState
            // may emit Dispose while it is deleted and must not destroy it.
            track.m_CallbackInfo = 0;
            track.m_AnimationInstance = 0;
        }
        component->m_AnimationTracks.SetSize(0);
    }

    static void DestroyReloadCallbacks(dmArray<ReloadTrackState>& track_states)
    {
        for (uint32_t i = 0; i < track_states.Size(); ++i)
        {
            if (track_states[i].m_CallbackInfo)
            {
                dmScript::DestroyCallback(track_states[i].m_CallbackInfo);
                track_states[i].m_CallbackInfo = 0;
            }
        }
    }

    static void CaptureReloadIKTargets(SpineModelComponent* component, dmArray<ReloadIKTargetState>& states)
    {
        for (uint32_t i = 0; i < component->m_IKTargets.Size(); ++i)
        {
            const IKTarget& target = component->m_IKTargets[i];
            ReloadIKTargetState state;
            state.m_ConstraintHash = target.m_ConstraintHash;
            state.m_Target = target.m_Target;
            state.m_Position = target.m_Position;
            state.m_Mix = target.m_Constraint ? target.m_Constraint->getPose().getMix() : 0.0f;
            state.m_IsPosition = 0;
            state.m_TargetWasGeneratedBone = 0;
            for (uint32_t bone_index = 0; bone_index < component->m_BoneInstances.Size(); ++bone_index)
            {
                if (component->m_BoneInstances[bone_index] == target.m_Target)
                {
                    state.m_TargetWasGeneratedBone = 1;
                    break;
                }
            }
            PushReloadValue(states, state);
        }
        for (uint32_t i = 0; i < component->m_IKTargetPositions.Size(); ++i)
        {
            const IKTarget& target = component->m_IKTargetPositions[i];
            ReloadIKTargetState state;
            state.m_ConstraintHash = target.m_ConstraintHash;
            state.m_Target = target.m_Target;
            state.m_Position = target.m_Position;
            state.m_Mix = target.m_Constraint ? target.m_Constraint->getPose().getMix() : 0.0f;
            state.m_IsPosition = 1;
            state.m_TargetWasGeneratedBone = 0;
            PushReloadValue(states, state);
        }
        ClearIKTargetBindings(component);
    }

    static dmhash_t CaptureReloadSkin(SpineModelComponent* component)
    {
        spine::Skin* skin = component->m_SkeletonInstance ? component->m_SkeletonInstance->getSkin() : 0;
        spine::Skin* default_skin = component->m_SceneData ? component->m_SceneData->m_Skeleton->getDefaultSkin() : 0;
        return skin && skin != default_skin ? dmHashString64(skin->getName().buffer()) : 0;
    }

    static spine::Animation* FindReloadAnimation(SpineModelComponent* component, dmhash_t animation_id)
    {
        uint32_t index = FindAnimationIndex(component, animation_id);
        if (index == INVALID_ANIMATION_INDEX)
            return 0;

        spine::Array<spine::Animation*>& animations = component->m_SceneData->m_Skeleton->getAnimations();
        return index < animations.size() ? animations[index] : 0;
    }

    static void RestoreReloadTrackEntry(spine::TrackEntry& entry, const ReloadTrackEntryState& state)
    {
        const float new_animation_duration = entry.getAnimation().getDuration();
        const bool custom_range = fabsf(state.m_AnimationStart) > 0.00001f ||
                                  fabsf(state.m_AnimationEnd - state.m_AnimationDuration) > 0.00001f;
        if (custom_range && state.m_AnimationDuration > 0.0f)
        {
            float start = dmMath::Clamp(state.m_AnimationStart / state.m_AnimationDuration * new_animation_duration,
                                        0.0f, new_animation_duration);
            float end = dmMath::Clamp(state.m_AnimationEnd / state.m_AnimationDuration * new_animation_duration,
                                      start, new_animation_duration);
            entry.setAnimationStart(start);
            entry.setAnimationEnd(end);
        }

        const float old_range = state.m_AnimationEnd - state.m_AnimationStart;
        const float new_range = entry.getAnimationEnd() - entry.getAnimationStart();
        const float range_scale = old_range > 0.0f ? new_range / old_range : 1.0f;

        entry.setLoop(state.m_Loop != 0);
        entry.setAdditive(state.m_Additive != 0);
        entry.setReverse(state.m_Reverse != 0);
        entry.setShortestRotation(state.m_ShortestRotation != 0);
        entry.setDelay(state.m_Delay);
        entry.setTrackTime(state.m_TrackTime * range_scale);
        entry.setTrackEnd(state.m_TrackEnd);
        entry.setTimeScale(state.m_TimeScale);
        entry.setAlpha(state.m_Alpha);
        entry.setMixDuration(state.m_MixDuration);
        entry.setMixTime(state.m_MixTime);
        entry.setEventThreshold(state.m_EventThreshold);
        entry.setMixAttachmentThreshold(state.m_MixAttachmentThreshold);
        entry.setAlphaAttachmentThreshold(state.m_AlphaAttachmentThreshold);
        entry.setMixDrawOrderThreshold(state.m_MixDrawOrderThreshold);

        if (state.m_AnimationLast < 0.0f || old_range <= 0.0f)
        {
            entry.setAnimationLast(state.m_AnimationLast);
        }
        else
        {
            float last_unit = (state.m_AnimationLast - state.m_AnimationStart) / old_range;
            entry.setAnimationLast(entry.getAnimationStart() + last_unit * new_range);
        }
    }

    static SpineAnimationTrack* CreateReloadTrackBinding(SpineModelComponent* component, uint32_t track_index)
    {
        if (track_index >= component->m_AnimationTracks.Capacity())
            component->m_AnimationTracks.SetCapacity(track_index + 4);

        while (track_index >= component->m_AnimationTracks.Size())
        {
            SpineAnimationTrack empty_track;
            memset(&empty_track, 0, sizeof(empty_track));
            dmMessage::ResetURL(&empty_track.m_Listener);
            component->m_AnimationTracks.Push(empty_track);
        }
        return &component->m_AnimationTracks[track_index];
    }

    static void IgnoreReloadSpineEvent(spine::AnimationState* state, spine::EventType type,
                                       spine::TrackEntry* entry, spine::Event* event, void* user_data)
    {
        (void)state;
        (void)type;
        (void)entry;
        (void)event;
        (void)user_data;
    }

    static void RestoreReloadTracks(SpineModelComponent* component, dmArray<ReloadTrackState>& track_states,
                                    const dmArray<ReloadTrackEntryState>& entry_states, float animation_state_time_scale)
    {
        component->m_AnimationStateInstance->setTimeScale(animation_state_time_scale);
        component->m_AnimationStateInstance->setListener(IgnoreReloadSpineEvent, 0);

        for (uint32_t track_state_index = 0; track_state_index < track_states.Size(); ++track_state_index)
        {
            ReloadTrackState& track_state = track_states[track_state_index];
            if (track_state.m_EntryCount == 0)
                continue;

            const uint32_t last_entry_index = track_state.m_FirstEntry + track_state.m_EntryCount - 1;
            if (!FindReloadAnimation(component, entry_states[last_entry_index].m_AnimationId))
            {
                dmLogWarning("Dropping Spine track %u after reload because animation '%s' no longer exists",
                             track_state.m_TrackIndex + 1,
                             dmHashReverseSafe64(entry_states[last_entry_index].m_AnimationId));
                continue;
            }

            spine::TrackEntry* current_entry = 0;
            for (uint32_t entry_offset = 0; entry_offset < track_state.m_EntryCount; ++entry_offset)
            {
                const ReloadTrackEntryState& entry_state = entry_states[track_state.m_FirstEntry + entry_offset];
                spine::Animation* animation = FindReloadAnimation(component, entry_state.m_AnimationId);
                if (!animation)
                {
                    dmLogWarning("Dropping missing Spine mixing animation '%s' on track %u after reload",
                                 dmHashReverseSafe64(entry_state.m_AnimationId), track_state.m_TrackIndex + 1);
                    continue;
                }

                current_entry = &component->m_AnimationStateInstance->setAnimation(track_state.m_TrackIndex, *animation,
                                                                                    entry_state.m_Loop != 0);
                RestoreReloadTrackEntry(*current_entry, entry_state);

                // Mark mixing-from entries as applied before installing the
                // next entry. This also preserves same-animation crossfades.
                if (entry_offset + 1 < track_state.m_EntryCount)
                    component->m_AnimationStateInstance->apply(*component->m_SkeletonInstance);
            }

            if (!current_entry)
                continue;

            SpineAnimationTrack* track = CreateReloadTrackBinding(component, track_state.m_TrackIndex);
            track->m_AnimationInstance = current_entry;
            track->m_AnimationId = track_state.m_AnimationId ? track_state.m_AnimationId
                                                             : entry_states[last_entry_index].m_AnimationId;
            track->m_Playback = track_state.m_Playback;
            track->m_Listener = track_state.m_Listener;
            track->m_Context = track_state.m_Context;
            track->m_CallbackInfo = track_state.m_CallbackInfo;
            track->m_CallbackId = track_state.m_CallbackId;
            track_state.m_CallbackInfo = 0;
        }

        // Prime nextAnimationLast/nextTrackLast without delivering duplicate
        // events from the restored cursor, then resume the normal listener.
        component->m_AnimationStateInstance->apply(*component->m_SkeletonInstance);
        component->m_AnimationStateInstance->setListener(SpineEventListener, component);
        component->m_SkeletonInstance->updateWorldTransform(spine::Physics_None);

        DestroyReloadCallbacks(track_states);
    }

    static void RestoreReloadIKTargets(SpineModelComponent* component, const dmArray<ReloadIKTargetState>& states)
    {
        for (uint32_t i = 0; i < states.Size(); ++i)
        {
            const ReloadIKTargetState& state = states[i];
            if (!state.m_IsPosition && state.m_TargetWasGeneratedBone)
            {
                dmLogWarning("Dropping Spine IK target '%s' after reload because its generated bone target is being rebuilt",
                             dmHashReverseSafe64(state.m_ConstraintHash));
                continue;
            }

            spine::IkConstraint* constraint = FindIKConstraint(component, state.m_ConstraintHash);
            if (!constraint)
            {
                dmLogWarning("Dropping Spine IK target '%s' after reload because the constraint no longer exists",
                             dmHashReverseSafe64(state.m_ConstraintHash));
                continue;
            }

            IKTarget target;
            target.m_ConstraintHash = state.m_ConstraintHash;
            target.m_Constraint = constraint;
            target.m_Target = state.m_Target;
            target.m_Position = state.m_Position;

            dmArray<IKTarget>& targets = state.m_IsPosition ? component->m_IKTargetPositions : component->m_IKTargets;
            if (targets.Full())
                targets.OffsetCapacity(2);
            targets.Push(target);
            constraint->getPose().setMix(state.m_Mix);
        }
    }

    static bool OnResourceReloaded(SpineModelWorld* world, SpineModelComponent* component, int index, bool force_rebuild)
    {
        (void)index;
        SpineSceneResource* spine_scene = GetSpineScene(component);
        if (!spine_scene)
            return false;
        if (!force_rebuild && component->m_SceneData == spine_scene->m_Data)
            return true;

        const dmhash_t skin_id = CaptureReloadSkin(component);
        const float animation_state_time_scale = component->m_AnimationStateInstance
            ? component->m_AnimationStateInstance->getTimeScale()
            : 1.0f;
        dmArray<ReloadTrackState> track_states;
        dmArray<ReloadTrackEntryState> entry_states;
        dmArray<ReloadIKTargetState> ik_states;
        CaptureReloadTracks(component, track_states, entry_states);
        CaptureReloadIKTargets(component, ik_states);

        // Tear down every object that stores pointers into the old generation
        // before releasing the component's generation reference.
        if (component->m_AnimationStateInstance)
            delete component->m_AnimationStateInstance;
        component->m_AnimationStateInstance = 0;
        if (component->m_SkeletonInstance)
            delete component->m_SkeletonInstance;
        component->m_SkeletonInstance = 0;
        ReleaseSceneData(component->m_SceneData);
        component->m_SceneData = 0;

        ScheduleBoneRebuild(component);
        if (!SetupComponentFromScene(world, component, spine_scene, false, false))
        {
            DestroyReloadCallbacks(track_states);
            return false;
        }

        if (!skin_id)
        {
            // Zero explicitly represents the default skin. Reapply it so a
            // runtime choice of default is not replaced by the model's authored skin.
            CompSpineModelSetSkin(component, 0);
        }
        else
        {
            uint32_t* skin_index = component->m_SceneData->m_SkinNameToIndex.Get(skin_id);
            if (!skin_index || *skin_index >= component->m_SceneData->m_Skeleton->getSkins().size())
            {
                dmLogWarning("Keeping the configured Spine skin after reload because skin '%s' no longer exists",
                             dmHashReverseSafe64(skin_id));
            }
            else
            {
                CompSpineModelSetSkin(component, skin_id);
            }
        }

        RestoreReloadTracks(component, track_states, entry_states, animation_state_time_scale);
        RestoreReloadIKTargets(component, ik_states);
        return true;
    }

    void CompSpineModelOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        int index = *params.m_UserData;
        SpineModelComponent* component = GetComponentFromIndex(world, index);
        component->m_Resource = (SpineModelResource*)params.m_Resource;
        (void)OnResourceReloaded(world, component, index, true);
    }

    dmGameObject::PropertyResult CompSpineModelGetProperty(const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        SpineModelComponent* component = GetComponentFromIndex(world, *params.m_UserData);
        if (params.m_PropertyId == PROP_SKIN)
        {
            spine::Skin* skin = component->m_SkeletonInstance->getSkin();
            out_value.m_Variant = dmGameObject::PropertyVar(dmHashString64(skin ? skin->getName().buffer() : ""));
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_ANIMATION)
        {
            dmhash_t tmp;
            if (dmGameObject::GetPropertyOptionsKey(params.m_Options, 0, &tmp) == dmGameObject::PROPERTY_RESULT_OK)
            {
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;
            }

            int32_t value_index = 0;
            dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

            dmhash_t value = 0;
            SpineAnimationTrack* track = GetTrackFromIndex(component, value_index);
            if (track && track->m_AnimationInstance)
            {
                value = track->m_AnimationId;
            }

            out_value.m_Variant = dmGameObject::PropertyVar(value);
            out_value.m_ValueType = dmGameObject::PROP_VALUE_ARRAY;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_CURSOR)
        {
            dmhash_t tmp;
            if (dmGameObject::GetPropertyOptionsKey(params.m_Options, 0, &tmp) == dmGameObject::PROPERTY_RESULT_OK)
            {
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;
            }

            int32_t value_index = 0;
            dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

            float unit = 0.0f;
            SpineAnimationTrack* track = GetTrackFromIndex(component, value_index);

            if (track)
            {
                spine::TrackEntry* entry = track->m_AnimationInstance;
                if (entry)
                {
                    float duration = entry->getAnimationEnd() - entry->getAnimationStart();
                    if (duration != 0)
                    {
                        unit = fmodf(entry->getTrackTime(), duration) / duration;
                    }
                }
            }

            out_value.m_Variant = dmGameObject::PropertyVar(unit);
            out_value.m_ValueType = dmGameObject::PROP_VALUE_ARRAY;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            dmhash_t tmp;
            if (dmGameObject::GetPropertyOptionsKey(params.m_Options, 0, &tmp) == dmGameObject::PROPERTY_RESULT_OK)
            {
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;
            }

            int32_t value_index = 0;
            dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

            float value = 0.0f;
            SpineAnimationTrack* track = GetTrackFromIndex(component, value_index);

            if (track && track->m_AnimationInstance)
            {
                value = track->m_AnimationInstance->getTimeScale();
            }

            out_value.m_Variant = dmGameObject::PropertyVar(value);
            out_value.m_ValueType = dmGameObject::PROP_VALUE_ARRAY;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            return dmGameSystem::GetResourceProperty(context->m_Factory, GetMaterialResource(component), out_value);
        }
        else if (params.m_PropertyId == SPINE_SCENE)
        {
            return dmGameSystem::GetResourceProperty(context->m_Factory, (void*)GetSpineScene(component), out_value);
        }

        int32_t value_index = 0;
        dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

        return dmGameSystem::GetMaterialConstant(GetMaterial(component), params.m_PropertyId, value_index, out_value, false, CompSpineModelGetConstantCallback, component);
    }

    dmGameObject::PropertyResult CompSpineModelSetProperty(const dmGameObject::ComponentSetPropertyParams& params)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        SpineModelComponent* component = GetComponentFromIndex(world, *params.m_UserData);
        if (params.m_PropertyId == PROP_SKIN)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_HASH)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            dmhash_t skin_id = params.m_Value.m_Hash;
            if (skin_id == dmHashString64(""))
                skin_id = 0;
            if (!CompSpineModelSetSkin(component, skin_id))
            {
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_CURSOR)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            dmhash_t tmp;
            if (dmGameObject::GetPropertyOptionsKey(params.m_Options, 0, &tmp) == dmGameObject::PROPERTY_RESULT_OK)
            {
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;
            }

            int32_t value_index = 0;
            dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

            SpineAnimationTrack* track = GetTrackFromIndex(component, value_index);
            if (!track)
                return dmGameObject::PROPERTY_RESULT_INVALID_INDEX;

            if (!track->m_AnimationInstance)
            {
                dmLogError("Could not set cursor since no animation is playing");
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }

            float unit_0_1 = fmodf(params.m_Value.m_Number + 1.0f, 1.0f);

            float duration = track->m_AnimationInstance->getAnimationEnd() - track->m_AnimationInstance->getAnimationStart();
            float t = unit_0_1 * duration;

            track->m_AnimationInstance->setTrackTime(t);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            dmhash_t tmp;
            if (dmGameObject::GetPropertyOptionsKey(params.m_Options, 0, &tmp) == dmGameObject::PROPERTY_RESULT_OK)
            {
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;
            }

            int32_t value_index = 0;
            dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);

            SpineAnimationTrack* track = GetTrackFromIndex(component, value_index);
            if (!track)
                return dmGameObject::PROPERTY_RESULT_INVALID_INDEX;

            if (!track->m_AnimationInstance)
            {
                dmLogError("Could not set playback rate since no animation is playing");
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }

            track->m_AnimationInstance->setTimeScale(params.m_Value.m_Number);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            dmGameObject::PropertyResult res = dmGameSystem::SetResourceProperty(context->m_Factory, params.m_Value, MATERIAL_EXT_HASH, (void**)&component->m_Material);
            component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;
            return res;
        }
        else if (params.m_PropertyId == SPINE_SCENE)
        {
            // Acquire the replacement separately. SetResourceProperty releases
            // its output's old value, but the live Skeleton still references
            // the old scene until it has been destroyed below.
            SpineSceneResource* spine_scene_new = 0;
            dmGameObject::PropertyResult res = dmGameSystem::SetResourceProperty(context->m_Factory, params.m_Value, SPINE_SCENE_EXT_HASH, (void**)&spine_scene_new);
            if (res == dmGameObject::PROPERTY_RESULT_OK)
            {
                SpineSceneResource* spine_scene_old_override = component->m_SpineScene;
                ClearIKTargetBindings(component);
                ClearAnimationTrackBindings(component);
                if (component->m_AnimationStateInstance)
                {
                    delete component->m_AnimationStateInstance;
                }
                component->m_AnimationStateInstance = 0;
                if (component->m_SkeletonInstance)
                {
                    delete component->m_SkeletonInstance;
                }
                component->m_SkeletonInstance = 0;
                ReleaseSceneData(component->m_SceneData);
                component->m_SceneData = 0;
                ScheduleBoneRebuild(component);

                component->m_SpineScene = spine_scene_new;
                if (spine_scene_old_override)
                {
                    dmResource::Release(context->m_Factory, spine_scene_old_override);
                }

                bool create_bones_now = component->m_Resource->m_CreateGoBones && !component->m_RebuildBonesPending;
                if (!SetupComponentFromScene(world, component, spine_scene_new, create_bones_now, false))
                {
                    return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
                }
            }
            return res;
        }
        int32_t value_index = 0;
        dmGameObject::GetPropertyOptionsIndex(params.m_Options, 0, &value_index);
        return dmGameSystem::SetMaterialConstant(GetMaterial(component), params.m_PropertyId, params.m_Value, value_index, CompSpineModelSetConstantCallback, component);
    }

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params)
    {
        SpineSceneResource* reloaded = (SpineSceneResource*)dmResource::GetResource(params->m_Resource);
        SpineModelWorld* world = (SpineModelWorld*)params->m_UserData;
        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();
        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent* component = components[i];
            if (component && GetSpineScene(component) == reloaded && component->m_SceneData != reloaded->m_Data)
            {
                if (!OnResourceReloaded(world, component, i, false))
                    dmLogError("Failed to rebuild Spine model after scene resource reload");
            }
        }
    }

    static dmGameObject::Result CompTypeSpineModelCreate(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
    {
        SpineModelContext* spinemodelctx = new SpineModelContext;
        spinemodelctx->m_Factory = ctx->m_Factory;
        spinemodelctx->m_GraphicsContext = *(dmGraphics::HContext*)ctx->m_Contexts.Get(dmHashString64("graphics"));
        spinemodelctx->m_RenderContext = *(dmRender::HRenderContext*)ctx->m_Contexts.Get(dmHashString64("render"));

        int32_t max_rig_instance = dmConfigFile::GetInt(ctx->m_Config, "rig.max_instance_count", 128);
        spinemodelctx->m_MaxSpineModelCount = dmMath::Max(dmConfigFile::GetInt(ctx->m_Config, "spine.max_count", 128), max_rig_instance);

        // Spine system setup
        spine::Bone::setYDown(false); // so we'll only call it once

        // Component type setup

        // Ideally, we'd like to move this priority a lot earlier
        // We sould be able to avoid doing UpdateTransforms again in the Render() function
        //ComponentTypeSetPrio(type, 1300);
        ComponentTypeSetPrio(type, 350); // 400 is the collision objects, and others come after that

        ComponentTypeSetContext(type, spinemodelctx);
        ComponentTypeSetHasUserData(type, true);
        ComponentTypeSetReadsTransforms(type, false);

        ComponentTypeSetNewWorldFn(type, CompSpineModelNewWorld);
        ComponentTypeSetDeleteWorldFn(type, CompSpineModelDeleteWorld);
        ComponentTypeSetCreateFn(type, CompSpineModelCreate);
        ComponentTypeSetDestroyFn(type, CompSpineModelDestroy);
            // ComponentTypeSetInitFn(type, CompSpineModelInit);
            // ComponentTypeSetFinalFn(type, CompSpineModelFinal);
        ComponentTypeSetAddToUpdateFn(type, CompSpineModelAddToUpdate);
        ComponentTypeSetUpdateFn(type, CompSpineModelUpdate);
        ComponentTypeSetPostUpdateFn(type, CompSpineModelPostUpdate);
        ComponentTypeSetRenderFn(type, CompSpineModelRender);
        ComponentTypeSetOnMessageFn(type, CompSpineModelOnMessage);
            // ComponentTypeSetOnInputFn(type, CompSpineModelOnInput);
        ComponentTypeSetOnReloadFn(type, CompSpineModelOnReload);
            // ComponentTypeSetSetPropertiesFn(type, CompSpineModelSetProperties);
        ComponentTypeSetGetPropertyFn(type, CompSpineModelGetProperty);
        ComponentTypeSetSetPropertyFn(type, CompSpineModelSetProperty);
            // ComponentTypeSetPropertyIteratorFn(type, CompSpineModelIterProperties);
        ComponentTypeSetGetFn(type, CompSpineModelGetComponent);

        return dmGameObject::RESULT_OK;
    }

    static dmGameObject::Result CompTypeSpineModelDestroy(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
    {
        SpineModelContext* spinemodelctx = (SpineModelContext*)ComponentTypeGetContext(type);
        delete spinemodelctx;
        return dmGameObject::RESULT_OK;
    }

    static dmGameObject::Result ComponentType_Destroy(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
     {
         SpineModelContext* spinemodelctx = (SpineModelContext*)ComponentTypeGetContext(type);
         delete spinemodelctx;
         return dmGameObject::RESULT_OK;
     }

    // ******************************************************************************
    // SCRIPTING HELPER FUNCTIONS
    // ******************************************************************************

    static spine::IkConstraint* FindIKConstraint(SpineModelComponent* component, dmhash_t constraint_id)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);
        uint32_t* index = scene_data->m_IKNameToIndex.Get(constraint_id);
        if (!index)
            return 0;

        spine::Array<spine::Constraint*>& constraints = component->m_SkeletonInstance->getConstraints();
        if (*index >= constraints.size())
            return 0;

        spine::Constraint* constraint = constraints[*index];
        if (!constraint || !constraint->getRTTI().instanceOf(spine::IkConstraint::rtti))
            return 0;

        return static_cast<spine::IkConstraint*>(constraint);
    }

    static bool FindSkin(SpineSceneData* scene_data, dmhash_t skin_id, spine::Skin** out_skin)
    {
        if (!skin_id)
        {
            *out_skin = scene_data->m_Skeleton->getDefaultSkin();
            return true;
        }

        uint32_t* index = scene_data->m_SkinNameToIndex.Get(skin_id);
        spine::Array<spine::Skin*>& skins = scene_data->m_Skeleton->getSkins();
        if (!index || *index >= skins.size())
        {
            dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id));
            return false;
        }

        *out_skin = skins[*index];
        return true;
    }

    bool CompSpineModelSetIKTargetInstance(SpineModelComponent* component, dmhash_t constraint_id, float mix, dmhash_t instance_id)
    {
        if (instance_id == 0)
        {
            return CompSpineModelResetIKTarget(component, constraint_id);
        }

        spine::IkConstraint* constraint = FindIKConstraint(component, constraint_id);
        if (!constraint)
            return false;

        dmGameObject::HInstance target_instance = dmGameObject::GetInstanceFromIdentifier(
            dmGameObject::GetCollection(component->m_Instance), instance_id);
        if (!target_instance)
            return false;

        if (component->m_IKTargets.Full())
            component->m_IKTargets.OffsetCapacity(2);

        IKTarget target;
        target.m_ConstraintHash = constraint_id; // for removing a constraint from this list
        target.m_Constraint = constraint;
        target.m_Position = dmVMath::Point3(0,0,0); // unused
        target.m_Target = target_instance;
        component->m_IKTargets.Push(target);

        constraint->getPose().setMix(mix);

        return true;
    }

    bool CompSpineModelSetIKTargetPosition(SpineModelComponent* component, dmhash_t constraint_id, float mix, Point3 position)
    {
        spine::IkConstraint* constraint = FindIKConstraint(component, constraint_id);
        if (!constraint)
            return false;

        if (component->m_IKTargetPositions.Full())
            component->m_IKTargetPositions.OffsetCapacity(2);

        IKTarget target;
        target.m_ConstraintHash = constraint_id; // for debugging
        target.m_Constraint = constraint;
        target.m_Position = position;
        target.m_Target = 0;
        component->m_IKTargetPositions.Push(target);

        constraint->getPose().setMix(mix);

        return true;
    }

    bool CompSpineModelResetIKTarget(SpineModelComponent* component, dmhash_t constraint_id)
    {
        // Remove the constraint
        for (uint32_t i = 0; i < component->m_IKTargets.Size(); ++i)
        {
            if (constraint_id == component->m_IKTargets[i].m_ConstraintHash)
            {
                component->m_IKTargets.EraseSwap(i);
                return true;
            }
        }
        return false;
    }

    bool CompSpineModelSetSkin(SpineModelComponent* component, dmhash_t skin_id)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);

        spine::Skin* skin = 0;
        if (!FindSkin(scene_data, skin_id, &skin))
            return false;

        component->m_SkeletonInstance->setSkin(skin);
        component->m_SkeletonInstance->setupPoseSlots();

        return true;
    }

    bool CompSpineModelClearSkin(SpineModelComponent* component, dmhash_t skin_id)
    {
        SpineSceneResource* spine_scene = GetSpineScene(component);
        SpineSceneData* scene_data = GetSpineSceneData(component);
        spine::Skin* skin = 0;
        if (!FindSkin(scene_data, skin_id, &skin) || !skin)
            return false;

        ClearSkinAttachments(component->m_SceneData, component->m_SkeletonInstance, skin);

        return true;
    }

    bool CompSpineModelAddSkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);
        spine::Skin* skin_a = 0;
        spine::Skin* skin_b = 0;
        if (!FindSkin(scene_data, skin_id_a, &skin_a) || !skin_a ||
            !FindSkin(scene_data, skin_id_b, &skin_b) || !skin_b)
            return false;

        skin_a->addSkin(*skin_b);
        if (component->m_SkeletonInstance->getSkin() == skin_a)
            component->m_SkeletonInstance->updateCache();

        return true;
    }

    bool CompSpineModelCopySkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);
        spine::Skin* skin_a = 0;
        spine::Skin* skin_b = 0;
        if (!FindSkin(scene_data, skin_id_a, &skin_a) || !skin_a ||
            !FindSkin(scene_data, skin_id_b, &skin_b) || !skin_b)
            return false;

        skin_a->copySkin(*skin_b);
        if (component->m_SkeletonInstance->getSkin() == skin_a)
            component->m_SkeletonInstance->updateCache();

        return true;
    }

    bool CompSpineModelSetSlotColor(SpineModelComponent* component, dmhash_t slot_id,  Vectormath::Aos::Vector4* color)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);

        uint32_t* index = scene_data->m_SlotNameToIndex.Get(slot_id);
        if (!index)
        {
            dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
            return false;
        }

        spine::Array<spine::Slot*>& slots = component->m_SkeletonInstance->getSlots();
        if (*index >= slots.size())
            return false;

        slots[*index]->getPose().getColor().set(color->getX(), color->getY(), color->getZ(), color->getW());

        return true;
    }

    bool CompSpineModelSetAttachment(SpineModelComponent* component, dmhash_t slot_id, dmhash_t attachment_id)
    {
        SpineSceneData* scene_data = GetSpineSceneData(component);

        uint32_t* index = scene_data->m_SlotNameToIndex.Get(slot_id);
        if (!index)
        {
            dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
            return false;
        }

        const char* attachment_name = 0;
        if (attachment_id)
        {
            const char** p_attachment_name = scene_data->m_AttachmentHashToName.Get(attachment_id);
            if (!p_attachment_name)
            {
                dmLogError("No attachment named '%s'", dmHashReverseSafe64(attachment_id));
                return false;
            }
            attachment_name = *p_attachment_name;
        }

        spine::Array<spine::Slot*>& slots = component->m_SkeletonInstance->getSlots();
        if (*index >= slots.size())
            return false;

        spine::Attachment* attachment = 0;
        if (attachment_name)
        {
            attachment = component->m_SkeletonInstance->getAttachment((int)*index, attachment_name);
            if (!attachment)
                return false;
        }

        slots[*index]->getPose().setAttachment(attachment);
        return true;
    }

    bool CompSpineModelGetBone(SpineModelComponent* component, dmhash_t bone_name, dmhash_t* instance_id)
    {
        uint32_t* index = component->m_BoneNameToNodeInstanceIndex.Get(bone_name);
        if (!index)
            return false;
        dmGameObject::HInstance bone_instance = component->m_BoneInstances[*index];
        *instance_id = dmGameObject::GetIdentifier(bone_instance);
        return true;
    }

    void CompSpineModelPhysicsTranslate(SpineModelComponent* component, Point3 translation)
    {
        component->m_SkeletonInstance->physicsTranslate(translation.getX(), translation.getY());
    }

    void CompSpineModelPhysicsRotate(SpineModelComponent* component, Point3 center, float degrees)
    {
        component->m_SkeletonInstance->physicsRotate(center.getX(), center.getY(), degrees);
    }
}

DM_DECLARE_COMPONENT_TYPE(ComponentTypeSpineModelExt, "spinemodelc", dmSpine::CompTypeSpineModelCreate, dmSpine::CompTypeSpineModelDestroy);
