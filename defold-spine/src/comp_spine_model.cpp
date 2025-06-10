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

extern "C" {

#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/Bone.h>
#include <spine/MeshAttachment.h>
#include <spine/RegionAttachment.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonBounds.h>
#include <spine/SkeletonClipping.h>
#include <spine/Slot.h>
#include <spine/extension.h>

} // extern C

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


#define _USE_MATH_DEFINES
#include <math.h> // M_PI

DM_PROPERTY_GROUP(rmtp_Spine, "Spine", 0);
DM_PROPERTY_U32(rmtp_SpineBones, 0, PROFILE_PROPERTY_FRAME_RESET, "# spine bones", &rmtp_Spine);
DM_PROPERTY_U32(rmtp_SpineComponents, 0, PROFILE_PROPERTY_FRAME_RESET, "# spine components", &rmtp_Spine);

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

    struct SpineModelWorld
    {
        dmObjectPool<SpineModelComponent*>  m_Components;
        dmArray<dmRender::RenderObject>     m_RenderObjects;
        dmArray<dmSpine::SpineModelBounds>  m_BoundingBoxes;
        dmGraphics::HVertexDeclaration      m_VertexDeclaration;
        dmGraphics::HVertexBuffer           m_VertexBuffer;
        dmArray<dmSpine::SpineVertex>       m_VertexBufferData;
        dmArray<SpineDrawDesc>              m_DrawDescBuffer;
        dmResource::HFactory                m_Factory;
        spSkeletonClipping*                 m_SkeletonClipper;
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
        world->m_VertexBuffer = dmGraphics::NewVertexBuffer(context->m_GraphicsContext, 0, 0x0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);

        dmGraphics::DeleteVertexStreamDeclaration(stream_declaration);

        *params.m_World = world;

        dmResource::RegisterResourceReloadedCallback(context->m_Factory, ResourceReloadedCallback, world);

        world->m_SkeletonClipper = spSkeletonClipping_create();

        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompSpineModelDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        dmGraphics::DeleteVertexDeclaration(world->m_VertexDeclaration);
        dmGraphics::DeleteVertexBuffer(world->m_VertexBuffer);

        dmResource::UnregisterResourceReloadedCallback(((SpineModelContext*)params.m_Context)->m_Factory, ResourceReloadedCallback, world);

        spSkeletonClipping_dispose(world->m_SkeletonClipper);

        delete world;

        return dmGameObject::CREATE_RESULT_OK;
    }

    static inline dmGameSystem::MaterialResource* GetMaterialResource(const SpineModelComponent* component) {
        return component->m_Material ? component->m_Material : component->m_Resource->m_Material;
    }
    static inline dmRender::HMaterial GetMaterial(const SpineModelComponent* component) {
        return GetMaterialResource(component)->m_Material;
    }

    static void ReHash(SpineModelComponent* component)
    {
        // material, texture set, blend mode and render constants
        HashState32 state;
        bool reverse = false;
        SpineModelResource* resource = component->m_Resource;
        dmGameSystemDDF::SpineModelDesc* ddf = resource->m_Ddf;
        dmRender::HMaterial material = GetMaterial(component);

        dmGameSystem::TextureSetResource* texture_set = resource->m_SpineScene->m_TextureSet;
        dmHashInit32(&state, reverse);
        dmHashUpdateBuffer32(&state, &material, sizeof(material));
        dmHashUpdateBuffer32(&state, &texture_set, sizeof(texture_set));
        dmHashUpdateBuffer32(&state, &ddf->m_BlendMode, sizeof(ddf->m_BlendMode));
        if (component->m_RenderConstants)
            dmGameSystem::HashRenderConstants(component->m_RenderConstants, &state);
        component->m_MixedHash = dmHashFinal32(&state);
        component->m_ReHash = 0;
    }

    static void SetTransformFromBone(dmGameObject::HInstance instance, const dmTransform::Transform& parent, const spBone* bone)
    {
        float radians = spBone_getWorldRotationX((spBone*)bone) * M_PI / 180.0f;
        float sx = spBone_getWorldScaleX((spBone*)bone);
        float sy = spBone_getWorldScaleY((spBone*)bone);

        dmTransform::Transform local = dmTransform::Transform(  dmVMath::Vector3(bone->worldX, bone->worldY, 0),
                                                                dmVMath::Quat::rotationZ(radians),
                                                                dmVMath::Vector3(sx, sy, 1));

        dmTransform::Transform transform = dmTransform::Mul(parent, local);

        dmGameObject::SetPosition(instance, Point3(transform.GetTranslation()));
        dmGameObject::SetRotation(instance, transform.GetRotation());
        dmGameObject::SetScale(instance, transform.GetScale());
    }

    static bool CreateGOBone(SpineModelComponent* component, dmGameObject::HCollection collection, dmGameObject::HInstance goparent, spBone* parent, spBone* bone, int indent)
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

        dmhash_t name_hash = dmHashString64(bone->data->name);
        component->m_BoneNameToNodeInstanceIndex.Put(name_hash, component->m_BoneInstances.Size());

        component->m_BoneInstances.Push(bone_instance);
        component->m_Bones.Push(bone);

        // Create the children
        for (int n = 0; n < bone->childrenCount; ++n)
        {
            // Since I haven't figured out how to update the local transforms, we do it in world space
            // However, that means all bones are parented to the top game object

            if (!CreateGOBone(component, collection, component->m_Instance, bone, bone->children[n], indent + 2))
            //if (!CreateGOBone(component, collection, bone_instance, bone, bone->children[n], indent + 2))
                return false;
        }
        return true;
    }

    static bool CreateGOBones(SpineModelWorld* world, SpineModelComponent* component)
    {
        //dmGameObject::HCollection collection = dmGameObject::GetCollection(component->m_Instance);

        SpineModelResource* spine_model = component->m_Resource;
        //SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        spSkeleton* skeleton = component->m_SkeletonInstance;

        component->m_Bones.SetCapacity(skeleton->bonesCount);
        component->m_BoneInstances.SetCapacity(skeleton->bonesCount);
        component->m_BoneNameToNodeInstanceIndex.SetCapacity((skeleton->bonesCount+1)/2, skeleton->bonesCount);
        if (!CreateGOBone(component, dmGameObject::GetCollection(component->m_Instance), component->m_Instance, 0, skeleton->root, 0))
        {
            dmLogError("Failed to create bones");
            dmGameObject::DeleteBones(component->m_Instance); // iterates recursively and deletes the ones marked as a bone
            component->m_BoneInstances.SetSize(0);
            return false;
        }
        return true;
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
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        uint32_t* index = spine_scene->m_AnimationNameToIndex.Get(animation);
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

    static bool PlayAnimation(SpineModelComponent* component, dmhash_t animation_id, dmGameObject::Playback playback,
        float blend_duration, float offset, float playback_rate, int track_index)
    {
        uint32_t index = FindAnimationIndex(component, animation_id);
        if (index == INVALID_ANIMATION_INDEX)
        {
            dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
            return false;
        }

        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        int loop = IsLooping(playback);
        if (index >= spine_scene->m_Skeleton->animationsCount)
        {
            dmLogError("No animation index %u is too large. Number of animations are %u", index, spine_scene->m_Skeleton->animationsCount);
            return false;
        }

        spAnimation* animation = spine_scene->m_Skeleton->animations[index];

        if (track_index < 0)
        {
            dmLogError("Invalid track index %d", track_index);
            return false;
        }

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
        track.m_AnimationInstance = spAnimationState_setAnimation(component->m_AnimationStateInstance, track_index, animation, loop);

        track.m_Playback = playback;
        track.m_AnimationInstance->timeScale = playback_rate;
        track.m_AnimationInstance->reverse = IsReverse(playback);
        track.m_AnimationInstance->mixDuration = blend_duration;
        track.m_AnimationInstance->trackTime = dmMath::Clamp(offset, track.m_AnimationInstance->animationStart, track.m_AnimationInstance->animationEnd);

        track.m_CallbackInfo = 0x0;
        dmMessage::ResetURL(&track.m_Listener);

        return true;
    }

    static void CancelTrackAnimation(SpineModelComponent* component, int32_t track_index)
    {
        SpineAnimationTrack* track = GetTrackFromIndex(component, track_index);
        if (!track || !track->m_AnimationInstance)
            return;

        spAnimationState_clearTrack(component->m_AnimationStateInstance, track->m_AnimationInstance->trackIndex);

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

    static void SendAnimationDone(SpineModelComponent* component, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
    {
        SpineAnimationTrack& track = component->m_AnimationTracks[entry->trackIndex];

        dmMessage::URL sender;
        dmMessage::URL receiver = track.m_Listener;

        if (!GetSender(component, &sender))
        {
            dmLogError("Could not send animation_done to listener because of incomplete component.");
            return;
        }

        dmGameSystemDDF::SpineAnimationDone message;
        message.m_AnimationId = dmHashString64(entry->animation->name);
        message.m_Playback    = track.m_Playback;
        message.m_Track       = entry->trackIndex + 1;

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

    static void SendSpineEvent(SpineModelComponent* component, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
    {
        SpineAnimationTrack& track = component->m_AnimationTracks[entry->trackIndex];

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
        message.m_AnimationId = dmHashString64(entry->animation->name);
        message.m_EventId     = dmHashString64(event->data->name);
        message.m_BlendWeight = 0.0f;//keyframe_event->m_BlendWeight;
        message.m_T           = event->time;
        message.m_Integer     = event->intValue;
        message.m_Float       = event->floatValue;
        message.m_String      = dmHashString64(event->stringValue?event->stringValue:"");
        message.m_Node.m_Ref  = 0;
        message.m_Node.m_ContextTableRef = 0;
        message.m_Track       = entry->trackIndex + 1;

        if (track.m_CallbackInfo)
        {
            RunTrackCallback(track.m_CallbackInfo, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message, &sender);
        }
        else
        {
            dmGameObject::Result result = dmGameObject::PostDDF(&message, &sender, &receiver, 0, false);
            if (result != dmGameObject::RESULT_OK)
            {
                dmLogError("Could not send animation event '%s' from animation '%s' to listener: %d", entry->animation->name, event->data->name, result);
            }
        }
    }

    static void SpineEventListener(spAnimationState* state, spEventType type, spTrackEntry* entry, spEvent* event)
    {
        SpineModelComponent* component = (SpineModelComponent*)state->userData;

        // Events are explained here: http://esotericsoftware.com/spine-api-reference#AnimationStateListener
        switch (type)
        {
            // case SP_ANIMATION_START:
            // {
            //     printf("Animation %s started on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            // case SP_ANIMATION_INTERRUPT:
            // {
            //     printf("Animation %s interrupted on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            // case SP_ANIMATION_END:
            // {
            //     printf("Animation %s ended on track %i\n", entry->animation->name, entry->trackIndex);
            //     break;
            // }
            case SP_ANIMATION_COMPLETE:
            {
                if (entry->mixingTo != 0)
                {
                    // While mixing, the previous animation completed.
                    // The problem is that the track is already owned by the new animation, and we
                    // cannot call the previous callback (as we haven't stored it)
                    return;
                }

                SpineAnimationTrack& track = component->m_AnimationTracks[entry->trackIndex];

                // Should we look at the looping state?
                if (!IsLooping(track.m_Playback))
                {
                    // We only send the event if it's not looping (same behavior as before)
                    SendAnimationDone(component, state, entry, event);
                }

                if (IsPingPong(track.m_Playback))
                {
                    track.m_AnimationInstance->reverse = !track.m_AnimationInstance->reverse;
                }
                break;
            }
            case SP_ANIMATION_DISPOSE:
            {
                SpineAnimationTrack* track = GetTrackFromIndex(component, entry->trackIndex);
                if (track && track->m_AnimationInstance == entry)
                {
                    ClearCompletionCallback(track);
                    track->m_AnimationInstance = nullptr;
                }
                break;
            }
            case SP_ANIMATION_EVENT:
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
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        uint32_t index = world->m_Components.Alloc();
        SpineModelComponent* component = new SpineModelComponent;
        memset(component, 0, sizeof(SpineModelComponent));
        world->m_Components.Set(index, component);
        component->m_Instance = params.m_Instance;
        component->m_Transform = dmTransform::Transform(Vector3(params.m_Position), params.m_Rotation, params.m_Scale);
        component->m_Resource = (SpineModelResource*)params.m_Resource;

        component->m_ComponentIndex = params.m_ComponentIndex;
        component->m_Enabled = 1;
        component->m_World = Matrix4::identity();
        component->m_DoRender = 0;
        component->m_RenderConstants = 0;

        component->m_SkeletonInstance = spSkeleton_create(spine_scene->m_Skeleton);
        if (!component->m_SkeletonInstance)
        {
            dmLogError("Failed to create skeleton instance");
            DestroyComponent(world, index);
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }

        if (0 == spSkeleton_setSkinByName(component->m_SkeletonInstance, component->m_Resource->m_Ddf->m_Skin))
        {
            spSkeleton_setSkin(component->m_SkeletonInstance, spine_scene->m_Skeleton->defaultSkin);
        }
        spSkeleton_setSlotsToSetupPose(component->m_SkeletonInstance);

        component->m_AnimationStateInstance = spAnimationState_create(spine_scene->m_AnimationStateData);
        if (!component->m_AnimationStateInstance)
        {
            dmLogError("Failed to create animation state instance");
            DestroyComponent(world, index);
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }

        component->m_AnimationStateInstance->userData = component;
        component->m_AnimationStateInstance->listener = SpineEventListener;

        component->m_AnimationTracks.SetCapacity(8);

        spSkeleton_setToSetupPose(component->m_SkeletonInstance);
        spSkeleton_updateWorldTransform(component->m_SkeletonInstance, SP_PHYSICS_UPDATE);

        // Create GO<->bone representation
        // We need to make sure that bone GOs are created before we start the default animation.
        if (spine_model->m_CreateGoBones)
        {
            if (!CreateGOBones(world, component))
            {
                dmLogError("Failed to create game objects for bones in spine model. Consider increasing collection max instances (collection.max_instances).");
                DestroyComponent(world, index);
                return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
            }
        }
        dmhash_t animation_id = dmHashString64(component->m_Resource->m_Ddf->m_DefaultAnimation);
        PlayAnimation(component, animation_id, dmGameObject::PLAYBACK_LOOP_FORWARD, 0.0f,
            component->m_Resource->m_Ddf->m_Offset, component->m_Resource->m_Ddf->m_PlaybackRate, 0);
            // TODO: Is the default playmode specified anywhere?

        component->m_ReHash = 1;

        *params.m_UserData = (uintptr_t)index;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static void DestroyComponent(SpineModelWorld* world, uint32_t index)
    {
        SpineModelComponent* component = world->m_Components.Get(index);
        dmGameObject::DeleteBones(component->m_Instance);
        // If we're going to use memset, then we should explicitly clear pose and instance arrays.
        component->m_BoneInstances.SetCapacity(0);
        component->m_AnimationTracks.SetCapacity(0);
        if (component->m_Material)
        {
            dmResource::Release(world->m_Factory, (void*)component->m_Material);
        }
        if (component->m_RenderConstants)
        {
            dmGameSystem::DestroyRenderConstants(component->m_RenderConstants);
        }

        if (component->m_AnimationStateInstance)
            spAnimationState_dispose(component->m_AnimationStateInstance);
        if (component->m_SkeletonInstance)
            spSkeleton_dispose(component->m_SkeletonInstance);

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

    static void UpdateBones(SpineModelComponent* component)
    {
        if (component->m_BoneInstances.Empty())
            return;

        uint32_t size = component->m_Bones.Size();
        dmArray<dmTransform::Transform> transforms;
        transforms.SetCapacity(size);
        transforms.SetSize(size);

        DM_PROPERTY_ADD_U32(rmtp_SpineBones, size);
        for (uint32_t n = 0; n < size; ++n)
        {
            spBone* bone = component->m_Bones[n];

            dmGameObject::HInstance bone_instance = component->m_BoneInstances[n];
            SetTransformFromBone(bone_instance, component->m_Transform, bone);
        }
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
        for (uint32_t i = 0; i < count; ++i)
        {
            const IKTarget& target = component->m_IKTargetPositions[i];
            dmVMath::Vector3 model_pos = (dmVMath::Vector3)dmTransform::Apply(dmTransform::Inv(dmTransform::Mul(dmGameObject::GetWorldTransform(component->m_Instance), component->m_Transform)), target.m_Position);

            target.m_Constraint->target->x = model_pos.getX();
            target.m_Constraint->target->y = model_pos.getY();
        }
        component->m_IKTargetPositions.SetSize(0);

        count = component->m_IKTargets.Size();
        for (uint32_t i = 0; i < count; ++i)
        {
            const IKTarget& target = component->m_IKTargets[i];

            dmVMath::Vector3 model_pos = (dmVMath::Vector3)dmTransform::Apply(dmTransform::Inv(dmTransform::Mul(dmGameObject::GetWorldTransform(component->m_Instance), component->m_Transform)),
                                                                              dmGameObject::GetWorldPosition(target.m_Target));

            target.m_Constraint->target->x = model_pos.getX();
            target.m_Constraint->target->y = model_pos.getY();
        }
    }


    dmGameObject::UpdateResult CompSpineModelUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        float dt = params.m_UpdateContext->m_DT;

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        const uint32_t count = components.Size();
        DM_PROPERTY_ADD_U32(rmtp_SpineComponents, count);
        uint32_t num_active = 0;
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
            // if (dmGameObject::ScaleAlongZ(component.m_Instance))
            // {
            //     component.m_World = go_world * local;
            // }
            // else
            {
                component.m_World = dmTransform::MulNoScaleZ(go_world, local);
            }

            ++num_active;

            // docs: http://esotericsoftware.com/spine-runtime-skeletons
            spAnimationState_update(component.m_AnimationStateInstance, dt);
            spAnimationState_apply(component.m_AnimationStateInstance, component.m_SkeletonInstance);

            ApplyIKTargets(&component);

            spSkeleton_update(component.m_SkeletonInstance, dt);
            spSkeleton_updateWorldTransform(component.m_SkeletonInstance, SP_PHYSICS_UPDATE);

            // Update the game world objects
            UpdateBones(&component);

            if (component.m_ReHash || (component.m_RenderConstants && dmGameSystem::AreRenderConstantsUpdated(component.m_RenderConstants)))
            {
                ReHash(&component);
            }

            component.m_DoRender = 1;
        }

        // Since we've moved the child game objects (bones), we need to sync back the transforms
        update_result.m_TransformsUpdated = num_active > 0;
        return dmGameObject::UPDATE_RESULT_OK;
    }

    static inline dmGameSystemDDF::SpineModelDesc::BlendMode SpineBlendModeToRenderBlendMode(spBlendMode sp_blend_mode)
    {
        switch(sp_blend_mode)
        {
            case SP_BLEND_MODE_NORMAL:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA;
            case SP_BLEND_MODE_ADDITIVE:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ADD;
            case SP_BLEND_MODE_MULTIPLY:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_MULT;
            case SP_BLEND_MODE_SCREEN:
                return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_SCREEN;
            default:break;
        }
        return dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA;
    }

    static void FillRenderObject(SpineModelWorld*  world,
        dmRender::HRenderContext                   render_context,
        dmRender::RenderObject&                    ro,
        dmGameSystem::HComponentRenderConstants    constants,
        dmGraphics::HTexture                       texture,
        dmRender::HMaterial                        material,
        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode,
        uint32_t                                   vertex_start,
        uint32_t                                   vertex_count)
    {
        ro.Init();
        ro.m_VertexDeclaration = world->m_VertexDeclaration;
        ro.m_VertexBuffer      = world->m_VertexBuffer;
        ro.m_PrimitiveType     = dmGraphics::PRIMITIVE_TRIANGLES;
        ro.m_VertexStart       = vertex_start;
        ro.m_VertexCount       = vertex_count;
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

        dmRender::AddToRender(render_context, &ro);
    }

    static void RenderBatch(SpineModelWorld* world, dmRender::HRenderContext render_context, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        //DM_PROFILE(SpineModel, "RenderBatch");

        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();

        uint32_t component_index           = (uint32_t)buf[*begin].m_UserData;
        const SpineModelComponent* first   = (const SpineModelComponent*) components[component_index];
        const SpineModelResource* resource = first->m_Resource;

        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode = resource->m_Ddf->m_BlendMode;
        bool use_inherit_blend = blend_mode == dmGameSystemDDF::SpineModelDesc::BLEND_MODE_INHERIT;

        uint32_t vertex_start           = world->m_VertexBufferData.Size();
        uint32_t vertex_count           = 0;
        uint32_t draw_desc_buffer_count = 0;

        // This is a temporary scratch buffer just used for this batch call, so we make sure to reset it.
        world->m_DrawDescBuffer.SetSize(0);

        for (uint32_t *i = begin; i != end; ++i)
        {
            component_index = (uint32_t)buf[*i].m_UserData;
            const SpineModelComponent* component = (const SpineModelComponent*) components[component_index];
            vertex_count += dmSpine::CalcVertexBufferSize(component->m_SkeletonInstance, world->m_SkeletonClipper, 0);

            if (use_inherit_blend)
            {
                draw_desc_buffer_count += dmSpine::CalcDrawDescCount(component->m_SkeletonInstance);
            }
        }

        if (draw_desc_buffer_count && draw_desc_buffer_count > world->m_DrawDescBuffer.Capacity())
        {
            world->m_DrawDescBuffer.SetCapacity(draw_desc_buffer_count);
        }

        if (vertex_count > world->m_VertexBufferData.Capacity())
        {
            world->m_VertexBufferData.SetCapacity(vertex_count);
        }

        vertex_count = 0;
        for (uint32_t *i = begin; i != end; ++i)
        {
            component_index = (uint32_t)buf[*i].m_UserData;
            const SpineModelComponent* component = (const SpineModelComponent*) components[component_index];
            vertex_count += dmSpine::GenerateVertexData(world->m_VertexBufferData, component->m_SkeletonInstance, world->m_SkeletonClipper, component->m_World, use_inherit_blend ? &world->m_DrawDescBuffer : 0);
        }

        dmGraphics::HTexture texture = resource->m_SpineScene->m_TextureSet->m_Texture->m_Texture; // spine - texture set resource - texture resource - texture
        dmRender::HMaterial material = GetMaterial(first);

        if (use_inherit_blend)
        {
            uint32_t draw_desc_count = world->m_DrawDescBuffer.Size();
            if (draw_desc_count > 0)
            {
                dmArray<SpineDrawDesc> scratch_draw_descs;
                MergeDrawDescs(world->m_DrawDescBuffer, scratch_draw_descs);

                uint32_t merged_size = scratch_draw_descs.Size();
                uint32_t ro_count_begin = world->m_RenderObjects.Size();
                world->m_RenderObjects.SetSize(world->m_RenderObjects.Size() + merged_size);

                for (int i = 0; i < merged_size; ++i)
                {
                    dmRender::RenderObject& ro = world->m_RenderObjects[ro_count_begin + i];
                    FillRenderObject(world, render_context, ro, first->m_RenderConstants, texture, material,
                        SpineBlendModeToRenderBlendMode((spBlendMode) scratch_draw_descs[i].m_BlendMode),
                        scratch_draw_descs[i].m_VertexStart,
                        scratch_draw_descs[i].m_VertexCount);
                }
            }
        }
        else
        {
            uint32_t ro_index = world->m_RenderObjects.Size();
            world->m_RenderObjects.SetSize(ro_index + 1);
            dmRender::RenderObject& ro = world->m_RenderObjects[ro_index];
            FillRenderObject(world, render_context, ro, first->m_RenderConstants, texture, material, blend_mode, vertex_start, vertex_count);
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
                dmGraphics::SetVertexBufferData(world->m_VertexBuffer, 0, 0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
                world->m_RenderObjects.SetSize(0);
                world->m_VertexBufferData.SetSize(0);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_BATCH:
            {
                RenderBatch(world, params.m_Context, params.m_Buf, params.m_Begin, params.m_End);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_END:
            {
                dmGraphics::SetVertexBufferData(world->m_VertexBuffer, sizeof(dmSpine::SpineVertex) * world->m_VertexBufferData.Size(),
                                                world->m_VertexBufferData.Begin(), dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
                //DM_COUNTER("SpineVertexBuffer", world->m_VertexBufferData.Size() * sizeof(dmSpine::SpineVertex));
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

        // Prepare list submit
        dmRender::RenderListEntry* render_list = dmRender::RenderListAlloc(render_context, count);
        dmRender::HRenderListDispatch dispatch = dmRender::RenderListMakeDispatch(render_context, &RenderListDispatch, &RenderListFrustumCulling, world);
        dmRender::RenderListEntry* write_ptr = render_list;

        for (uint32_t i = 0; i < count; ++i)
        {
            SpineModelComponent& component = *components[i];
            if (!component.m_DoRender || !component.m_Enabled)
                continue;

            // Update bounding boxes
            SpineModelBounds& bounds = world->m_BoundingBoxes[i];
            GetSkeletonBounds(component.m_SkeletonInstance,bounds);

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
                                                message->m_Offset, message->m_PlaybackRate, message->m_Track - 1);
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

    bool CompSpineModelSetConstant(SpineModelComponent* component, dmGameSystemDDF::SetConstant* message)
    {
        dmGameObject::PropertyResult result = dmGameSystem::SetMaterialConstant(GetMaterial(component), message->m_NameHash,
                                                                dmGameObject::PropertyVar(message->m_Value), message->m_Index, CompSpineModelSetConstantCallback, component);
        return result == dmGameObject::PROPERTY_RESULT_OK;
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

    static bool OnResourceReloaded(SpineModelWorld* world, SpineModelComponent* component, int index)
    {
        // Delete old bones, then recreate with new data.
        // We need to make sure that bone GOs are created before we start the default animation.
        dmGameObject::DeleteBones(component->m_Instance);
        if (component->m_Resource->m_CreateGoBones)
        {
            if (!CreateGOBones(world, component))
            {
                dmLogError("Failed to create game objects for bones in spine model. Consider increasing collection max instances (collection.max_instances).");
                DestroyComponent(world, index);
                return false;
            }
        }

        component->m_ReHash = 1;

        return true;
    }

    void CompSpineModelOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        int index = *params.m_UserData;
        SpineModelComponent* component = GetComponentFromIndex(world, index);
        component->m_Resource = (SpineModelResource*)params.m_Resource;
        (void)OnResourceReloaded(world, component, index);
    }

    dmGameObject::PropertyResult CompSpineModelGetProperty(const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        SpineModelComponent* component = GetComponentFromIndex(world, *params.m_UserData);
        if (params.m_PropertyId == PROP_SKIN)
        {
            spSkin* skin = component->m_SkeletonInstance->skin;// ? component->m_SkeletonInstance->skin : component->m_SkeletonInstance->defaultSkin;
            out_value.m_Variant = dmGameObject::PropertyVar(dmHashString64(skin->name));
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_ANIMATION)
        {
            dmhash_t value = 0;

            if (params.m_Options.m_HasKey)
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;

            SpineAnimationTrack* track = GetTrackFromIndex(component, params.m_Options.m_Index);
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
            float unit = 0.0f;

            if (params.m_Options.m_HasKey)
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;

            SpineAnimationTrack* track = GetTrackFromIndex(component, params.m_Options.m_Index);

            if (track)
            {
                spTrackEntry* entry = track->m_AnimationInstance;
                if (entry)
                {
                    float duration = entry->animationEnd - entry->animationStart;
                    if (duration != 0)
                    {
                        unit = fmodf(entry->trackTime, duration) / duration;
                    }
                }
            }

            out_value.m_Variant = dmGameObject::PropertyVar(unit);
            out_value.m_ValueType = dmGameObject::PROP_VALUE_ARRAY;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            float value = 0.0f;

            if (params.m_Options.m_HasKey)
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;

            SpineAnimationTrack* track = GetTrackFromIndex(component, params.m_Options.m_Index);

            if (track && track->m_AnimationInstance)
            {
                value = track->m_AnimationInstance->timeScale;
            }

            out_value.m_Variant = dmGameObject::PropertyVar(value);
            out_value.m_ValueType = dmGameObject::PROP_VALUE_ARRAY;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            return dmGameSystem::GetResourceProperty(context->m_Factory, GetMaterialResource(component), out_value);
        }
        return dmGameSystem::GetMaterialConstant(GetMaterial(component), params.m_PropertyId, params.m_Options.m_Index, out_value, false, CompSpineModelGetConstantCallback, component);
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

            if (params.m_Options.m_HasKey)
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;

            SpineAnimationTrack* track = GetTrackFromIndex(component, params.m_Options.m_Index);
            if (!track)
                return dmGameObject::PROPERTY_RESULT_INVALID_INDEX;

            if (!track->m_AnimationInstance)
            {
                dmLogError("Could not set cursor since no animation is playing");
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }

            float unit_0_1 = fmodf(params.m_Value.m_Number + 1.0f, 1.0f);

            float duration = track->m_AnimationInstance->animationEnd - track->m_AnimationInstance->animationStart;
            float t = unit_0_1 * duration;

            track->m_AnimationInstance->trackTime = t;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            if (params.m_Options.m_HasKey)
                return dmGameObject::PROPERTY_RESULT_INVALID_KEY;

            SpineAnimationTrack* track = GetTrackFromIndex(component, params.m_Options.m_Index);
            if (!track)
                return dmGameObject::PROPERTY_RESULT_INVALID_INDEX;

            if (!track->m_AnimationInstance)
            {
                dmLogError("Could not set playback rate since no animation is playing");
                return dmGameObject::PROPERTY_RESULT_UNSUPPORTED_VALUE;
            }

            track->m_AnimationInstance->timeScale = params.m_Value.m_Number;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            dmGameObject::PropertyResult res = dmGameSystem::SetResourceProperty(context->m_Factory, params.m_Value, MATERIAL_EXT_HASH, (void**)&component->m_Material);
            component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;
            return res;
        }
        return dmGameSystem::SetMaterialConstant(GetMaterial(component), params.m_PropertyId, params.m_Value, params.m_Options.m_Index, CompSpineModelSetConstantCallback, component);
    }

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params)
    {
        // E.g. if the Spine json or atlas has changed, we may need to update things here

        // SpineModelWorld* world = (SpineModelWorld*) params.m_UserData;
        // dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        // uint32_t n = components.Size();
        // for (uint32_t i = 0; i < n; ++i)
        // {
        //     SpineModelComponent* component = components[i];
        //     if (component->m_Resource != 0x0 && component->m_Resource->m_RigScene == params.m_Resource->m_Resource)
        //         OnResourceReloaded(world, component, i);
        // }
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
        spBone_setYDown(0); // so we'll only call it once

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

    bool CompSpineModelSetIKTargetInstance(SpineModelComponent* component, dmhash_t constraint_id, float mix, dmhash_t instance_id)
    {
        if (instance_id == 0)
        {
            return CompSpineModelResetIKTarget(component, constraint_id);
        }

        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        uint32_t* index = spine_scene->m_IKNameToIndex.Get(constraint_id);
        if (!index)
            return false;
        if (*index > component->m_SkeletonInstance->ikConstraintsCount)
            return false;

        if (component->m_IKTargets.Full())
            component->m_IKTargets.OffsetCapacity(2);

        // Convert game world space to model space
        spIkConstraint* constraint = component->m_SkeletonInstance->ikConstraints[*index];

        IKTarget target;
        target.m_ConstraintHash = constraint_id; // for removing a constraint from this list
        target.m_Constraint = component->m_SkeletonInstance->ikConstraints[*index];
        target.m_Position = dmVMath::Point3(0,0,0); // unused
        target.m_Target = dmGameObject::GetInstanceFromIdentifier(dmGameObject::GetCollection(component->m_Instance), instance_id);
        component->m_IKTargets.Push(target);

        return true;
    }

    bool CompSpineModelSetIKTargetPosition(SpineModelComponent* component, dmhash_t constraint_id, float mix, Point3 position)
    {
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        uint32_t* index = spine_scene->m_IKNameToIndex.Get(constraint_id);
        if (!index)
            return false;
        if (*index > component->m_SkeletonInstance->ikConstraintsCount)
            return false;

        if (component->m_IKTargetPositions.Full())
            component->m_IKTargetPositions.OffsetCapacity(2);

        // Convert game world space to model space
        spIkConstraint* constraint = component->m_SkeletonInstance->ikConstraints[*index];

        IKTarget target;
        target.m_ConstraintHash = constraint_id; // for debugging
        target.m_Constraint = component->m_SkeletonInstance->ikConstraints[*index];
        target.m_Position = position;
        target.m_Target = 0;
        component->m_IKTargetPositions.Push(target);

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
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        spSkin* skin = spine_scene->m_Skeleton->defaultSkin;
        if (skin_id)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id));
                return false;
            }

            skin = spine_scene->m_Skeleton->skins[*index];
        }

        spSkeleton_setSkin(component->m_SkeletonInstance, skin);
        spSkeleton_setSlotsToSetupPose(component->m_SkeletonInstance);

        return true;
    }

    bool CompSpineModelClear(SpineModelComponent* component, dmhash_t skin_id)
    {

        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        spSkin* skin_a;

        if (skin_id)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id));
                return false;
            }

            skin_a = spine_scene->m_Skeleton->skins[*index];
        }

        spSkin_clear(skin_a);
        return true;
    }


    bool CompSpineModelAddSkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b)
    {
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        spSkin* skin_a;
        spSkin* skin_b;

        if (skin_id_a)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id_a);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id_a));
                return false;
            }

            skin_a = spine_scene->m_Skeleton->skins[*index];
        }
         if (skin_id_b)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id_b);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id_b));
                return false;
            }

            skin_b = spine_scene->m_Skeleton->skins[*index];
        }
        spSkin_addSkin(skin_a,skin_b);
        return true;
    }


    bool CompSpineModelCopySkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b)
    {
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        spSkin* skin_a;
        spSkin* skin_b;

        if (skin_id_a)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id_a);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id_a));
                return false;
            }

            skin_a = spine_scene->m_Skeleton->skins[*index];
        }
         if (skin_id_b)
        {
            uint32_t* index = spine_scene->m_SkinNameToIndex.Get(skin_id_b);
            if (!index)
            {
                dmLogError("No skin named '%s'", dmHashReverseSafe64(skin_id_b));
                return false;
            }

            skin_b = spine_scene->m_Skeleton->skins[*index];
        }
        spSkin_copySkin(skin_a,skin_b);
        return true;
    }

    bool CompSpineModelSetAttachment(SpineModelComponent* component, dmhash_t slot_id, dmhash_t attachment_id)
    {
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        uint32_t* index = spine_scene->m_SlotNameToIndex.Get(slot_id);
        if (!index)
        {
            dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
            return false;
        }

        //SP_API int spSkeleton_setAttachment(spSkeleton *self, const char *slotName, const char *attachmentName);

        const char* attachment_name = 0;
        if (attachment_id)
        {
            const char** p_attachment_name = spine_scene->m_AttachmentHashToName.Get(attachment_id);
            if (!p_attachment_name)
            {
                dmLogError("No attachment named '%s'", dmHashReverseSafe64(attachment_id));
                return false;
            }
            attachment_name = *p_attachment_name;
        }

        spSlot* slot = component->m_SkeletonInstance->slots[*index];

        // it's a bit weird to use strings here, but we'd rather not use too much knowledge about the internals
        return 1 == spSkeleton_setAttachment(component->m_SkeletonInstance, slot->data->name, attachment_name);
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
}

DM_DECLARE_COMPONENT_TYPE(ComponentTypeSpineModelExt, "spinemodelc", dmSpine::CompTypeSpineModelCreate, dmSpine::CompTypeSpineModelDestroy);
