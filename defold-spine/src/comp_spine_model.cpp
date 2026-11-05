// Copyright 2020-2026 The Defold Foundation
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

#include <spine/Animation.h>
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
    // 1 << 5 gives 32 render objects per overflow block. Keeping the block size
    // a power of two lets us map an overflow index with a shift and a mask.
    static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_SHIFT = 5;
    static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_SIZE = 1U << RENDER_OBJECT_OVERFLOW_BLOCK_SHIFT;
    static const uint32_t RENDER_OBJECT_OVERFLOW_BLOCK_MASK = RENDER_OBJECT_OVERFLOW_BLOCK_SIZE - 1;

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams* params);
    static void DestroyComponent(struct SpineModelWorld* world, uint32_t index);
    static void SpineEventListener(spAnimationState* state, spEventType type, spTrackEntry* entry, spEvent* event);

    struct SpinePrefixEntry
    {
        SpineModelComponent* m_Component;
        uint64_t m_Revision;
        uint32_t m_VertexStart, m_IndexStart, m_VertexEnd, m_IndexEnd;
    };
    struct SpineROCheck { dmRender::RenderObject m_Expected; };
    static uint32_t g_GeometryWorldId = 0;
    static void InvalidateGeometryCache(SpineModelComponent* component)
    {
        ++component->m_GeometryRevision;
        if (!component->m_GeometryRevision) ++component->m_GeometryRevision;
    }

    struct SpineModelWorld
    {
        dmObjectPool<SpineModelComponent*>      m_Components;
        dmArray<dmRender::RenderObject>         m_RenderObjects;
        dmArray<dmRender::RenderObject*>        m_RenderObjectOverflowBlocks;
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
        spSkeletonClipping*                     m_SkeletonClipper;
        uint32_t                                m_RenderObjectsInUse;
        dmArray<SpinePrefixEntry>                m_ResidentComponents;
        dmArray<SpinePrefixEntry>                m_PassComponents;
        uint64_t                                m_GeometryEpoch, m_ResidentEpoch;
        uint32_t                                m_ResidentVertices, m_ResidentIndices, m_ResidentPackedBytes;
        uint32_t                                m_GeometryWorldId;
        int32_t                                 m_GeometryCacheMode;
        bool                                    m_PassPrefixMatch, m_PassEligible, m_PassWrote;
        bool                                    m_GeometryCacheFailed;
        uint64_t                                m_CacheHits, m_CacheMisses, m_CacheVerified;
        uint64_t                                m_CacheVertices, m_CacheIndices, m_CacheDrawDescs;
        uint64_t                                m_CacheMismatches, m_CacheUnsupported;
        uint64_t                                m_CompletePassReuse, m_PackedChecks, m_ROChecks, m_MetadataPeak;
        float                                   m_CacheMaxDelta;
        dmArray<SpineVertex>                     m_CheckVertices;
        dmArray<uint32_t>                        m_CheckIndices;
        dmArray<SpineIndexedDrawDesc>            m_CheckDrawDescs;
        dmArray<SpineROCheck>                    m_CheckRO;
        SpineModelWorld*                         m_PackReference;
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
        int32_t                     m_GeometryCacheMode;
    };

    dmGameObject::CreateResult CompSpineModelNewWorld(const dmGameObject::ComponentNewWorldParams& params)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        //dmRender::HRenderContext render_context = context->m_RenderContext;
        SpineModelWorld* world = new SpineModelWorld();
        world->m_Factory = context->m_Factory;
        world->m_GeometryCacheMode = context->m_GeometryCacheMode;
        world->m_GeometryWorldId = ++g_GeometryWorldId;
        // new SpineModelWorld() value-initializes scalar cache state to zero.

        uint32_t comp_count = dmMath::Min(params.m_MaxComponentInstances, context->m_MaxSpineModelCount);

        world->m_Components.SetCapacity(comp_count);
        // The common case produces at most one render object per component.
        // Reserving that storage before rendering keeps it contiguous and means
        // it cannot move after AddToRender has retained pointers into it.
        world->m_RenderObjects.SetCapacity(comp_count);
        world->m_BoundingBoxes.SetCapacity(comp_count);
        world->m_BoundingBoxes.SetSize(comp_count);
        world->m_RenderObjectsInUse = 0;

        dmGraphics::HVertexStreamDeclaration stream_declaration = dmGraphics::NewVertexStreamDeclaration(context->m_GraphicsContext);
        dmGraphics::AddVertexStream(stream_declaration, "position", 3, dmGraphics::TYPE_FLOAT, false);
        dmGraphics::AddVertexStream(stream_declaration, "texcoord0", 2, dmGraphics::TYPE_FLOAT, true);
        dmGraphics::AddVertexStream(stream_declaration, "color", 4, dmGraphics::TYPE_FLOAT, true);
        dmGraphics::AddVertexStream(stream_declaration, "darkcolor", 3, dmGraphics::TYPE_FLOAT, true);
        dmGraphics::AddVertexStream(stream_declaration, "page_index", 1, dmGraphics::TYPE_FLOAT, false);

        world->m_VertexDeclaration = dmGraphics::NewVertexDeclaration(context->m_GraphicsContext, stream_declaration);
        world->m_VertexBuffer = dmGraphics::NewVertexBuffer(context->m_GraphicsContext, 0, 0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        world->m_IndexBuffer = dmGraphics::NewIndexBuffer(context->m_GraphicsContext, 0, 0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        world->m_Is16BitIndex = 1;

        dmGraphics::DeleteVertexStreamDeclaration(stream_declaration);

        *params.m_World = world;

        dmResource::RegisterResourceReloadedCallback(context->m_Factory, ResourceReloadedCallback, world);

        world->m_SkeletonClipper = spSkeletonClipping_create();

        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompSpineModelDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        for (uint32_t i = 0; i < world->m_RenderObjectOverflowBlocks.Size(); ++i)
        {
            delete[] world->m_RenderObjectOverflowBlocks[i];
        }
        dmGraphics::DeleteVertexDeclaration(world->m_VertexDeclaration);
        dmGraphics::DeleteVertexBuffer(world->m_VertexBuffer);
        dmGraphics::DeleteIndexBuffer(world->m_IndexBuffer);

        dmResource::UnregisterResourceReloadedCallback(((SpineModelContext*)params.m_Context)->m_Factory, ResourceReloadedCallback, world);

        spSkeletonClipping_dispose(world->m_SkeletonClipper);

        delete world->m_PackReference;
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

    static void ReHash(SpineModelComponent* component)
    {
        // material, texture set, blend mode and render constants
        HashState32 state;
        bool reverse = false;
        SpineModelResource* resource = component->m_Resource;
        dmGameSystemDDF::SpineModelDesc* ddf = resource->m_Ddf;
        dmRender::HMaterial material = GetMaterial(component);

        dmGameSystem::TextureSetResource* texture_set = GetSpineScene(component)->m_TextureSet;
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

    static void DeleteGOBones(SpineModelComponent* component)
    {
        dmGameObject::HCollection collection = dmGameObject::GetCollection(component->m_Instance);
        // Other components can have bone game objects under the same parent.
        // Resolve identifiers because recursive deletion may have freed their handles.
        for (uint32_t i = 0; i < component->m_BoneInstanceIds.Size(); ++i)
        {
            dmGameObject::HInstance bone_instance = dmGameObject::GetInstanceFromIdentifier(collection, component->m_BoneInstanceIds[i]);
            if (bone_instance)
                dmGameObject::Delete(collection, bone_instance, false);
        }
        component->m_BoneInstanceIds.SetSize(0);
        component->m_BoneInstances.SetSize(0);
        component->m_Bones.SetSize(0);
        component->m_BoneNameToNodeInstanceIndex.Clear();
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
            dmGameObject::Delete(collection, bone_instance, false);
            return false;
        }

        dmhash_t id = dmGameObject::CreateInstanceId();
        dmGameObject::AssignInstanceIndex(index, bone_instance);

        dmGameObject::Result result = dmGameObject::SetIdentifier(collection, bone_instance, id);
        if (dmGameObject::RESULT_OK != result)
        {
            dmLogError("Failed to set identifier for bone game object");
            dmGameObject::Delete(collection, bone_instance, false);
            return false;
        }

        SetTransformFromBone(bone_instance, component->m_Transform, bone);

        dmhash_t name_hash = dmHashString64(bone->data->name);
        component->m_BoneNameToNodeInstanceIndex.Put(name_hash, component->m_BoneInstances.Size());

        component->m_BoneInstances.Push(bone_instance);
        component->m_BoneInstanceIds.Push(id);
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
        component->m_BoneInstanceIds.SetCapacity(skeleton->bonesCount);
        component->m_BoneNameToNodeInstanceIndex.OffsetCapacity(skeleton->bonesCount);
        if (!CreateGOBone(component, dmGameObject::GetCollection(component->m_Instance), component->m_Instance, 0, skeleton->root, 0))
        {
            dmLogError("Failed to create bones");
            DeleteGOBones(component);
            return false;
        }
        return true;
    }

    static void ScheduleBoneRebuild(SpineModelComponent* component)
    {
        DeleteGOBones(component);
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
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);
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

    static void DestroyDeferredCallbacks(SpineModelComponent* component)
    {
        for (uint32_t i = 0; i < component->m_DeferredCallbacks.Size(); ++i)
        {
            dmScript::DestroyCallback(component->m_DeferredCallbacks[i]);
        }
        component->m_DeferredCallbacks.SetSize(0);
    }

    static void DestroyOrDeferCallback(SpineModelComponent* component, dmScript::LuaCallbackInfo* callback)
    {
        if (!callback)
            return;

        // A callback can replace or cancel its own track while it is running. Keep
        // its Lua userdata alive until RunTrackCallback() has finished teardown.
        if (component->m_CallbackInvocationDepth == 0)
        {
            dmScript::DestroyCallback(callback);
            return;
        }

        if (component->m_DeferredCallbacks.Full())
        {
            component->m_DeferredCallbacks.SetCapacity(component->m_DeferredCallbacks.Capacity() + 4);
        }
        component->m_DeferredCallbacks.Push(callback);
    }

    static void ClearCompletionCallback(SpineModelComponent* component, SpineAnimationTrack* track)
    {
        if (track->m_CallbackInfo)
        {
            dmScript::LuaCallbackInfo* callback = track->m_CallbackInfo;
            track->m_CallbackInfo = 0x0;
            DestroyOrDeferCallback(component, callback);
        }
    }

    static void RunComponentTrackCallback(SpineModelComponent* component, dmScript::LuaCallbackInfo* callback,
        const dmDDF::Descriptor* desc, const char* data, const dmMessage::URL* sender)
    {
        ++component->m_CallbackInvocationDepth;
        RunTrackCallback(callback, desc, data, sender);
        assert(component->m_CallbackInvocationDepth > 0);
        --component->m_CallbackInvocationDepth;

        if (component->m_CallbackInvocationDepth == 0)
        {
            DestroyDeferredCallbacks(component);
        }
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

        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);

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

        spMixBlend mixBlend;
        switch (mix_blend) {
            case dmGameSystemDDF::MixBlend::MIX_BLEND_SETUP:
                mixBlend = SP_MIX_BLEND_SETUP;
                break;
            case dmGameSystemDDF::MixBlend::MIX_BLEND_FIRST:
                mixBlend = SP_MIX_BLEND_FIRST;
                break;
            case dmGameSystemDDF::MixBlend::MIX_BLEND_REPLACE:
                mixBlend = SP_MIX_BLEND_REPLACE;
                break;
            case dmGameSystemDDF::MixBlend::MIX_BLEND_ADD:
                mixBlend = SP_MIX_BLEND_ADD;
                break;
            default:
                mixBlend = SP_MIX_BLEND_REPLACE;
                break;
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

        ClearCompletionCallback(component, &track);

        track.m_AnimationId = animation_id;
        track.m_AnimationInstance = spAnimationState_setAnimation(component->m_AnimationStateInstance, track_index, animation, loop);

        track.m_Playback = playback;
        track.m_AnimationInstance->timeScale = playback_rate;
        track.m_AnimationInstance->reverse = IsReverse(playback);
        track.m_AnimationInstance->mixDuration = blend_duration;
        track.m_AnimationInstance->trackTime = dmMath::Clamp(offset, track.m_AnimationInstance->animationStart, track.m_AnimationInstance->animationEnd);
        track.m_AnimationInstance->mixBlend = mixBlend;
        track.m_AnimationInstance->alpha = alpha;

        track.m_CallbackInfo = 0x0;
        dmMessage::ResetURL(&track.m_Listener);

        return true;
    }

    static bool SetupComponentFromScene(SpineModelWorld* world, SpineModelComponent* component, SpineSceneResource* spine_scene, bool create_bones, bool play_default_animation)
    {
        InvalidateGeometryCache(component);
        component->m_SkeletonInstance = spSkeleton_create(spine_scene->m_Skeleton);
        if (!component->m_SkeletonInstance)
        {
            dmLogError("Failed to create skeleton instance");
            return false;
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
            return false;
        }

        component->m_AnimationStateInstance->userData = component;
        component->m_AnimationStateInstance->listener = SpineEventListener;

        if (component->m_AnimationTracks.Capacity() < 8)
        {
            component->m_AnimationTracks.SetCapacity(8);
        }

        spSkeleton_setToSetupPose(component->m_SkeletonInstance);
        spSkeleton_updateWorldTransform(component->m_SkeletonInstance, SP_PHYSICS_UPDATE);

        if (create_bones)
        {
            if (!CreateGOBones(world, component))
            {
                dmLogError("Failed to create game objects for bones in spine model. Consider increasing collection max instances (collection.max_instances).");
                return false;
            }
        }

        if (play_default_animation)
        {
            dmhash_t animation_id = dmHashString64(component->m_Resource->m_Ddf->m_DefaultAnimation);
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

        spAnimationState_clearTrack(component->m_AnimationStateInstance, track->m_AnimationInstance->trackIndex);

        ClearCompletionCallback(component, track);
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
            RunComponentTrackCallback(component, track.m_CallbackInfo, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message, &sender);
            // If, in a Lua callback, the user calls spine.play_anim(),
            // it will destroy the current callback and create a new one (if specified).
            // Therefore, we need to check whether we are going to remove the same callback
            // that we are running. If not, it has already been removed.
            if (id == track.m_CallbackId)
            {
                ClearCompletionCallback(component, &track);
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
            RunComponentTrackCallback(component, track.m_CallbackInfo, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message, &sender);
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
                    ClearCompletionCallback(component, track);
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

        world->m_ResidentEpoch = 0; // A new instance must never alias a retained pointer.
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
        world->m_ResidentEpoch = 0;
        DeleteGOBones(component);
        assert(component->m_CallbackInvocationDepth == 0);
        for (uint32_t i = 0; i < component->m_AnimationTracks.Size(); ++i)
        {
            ClearCompletionCallback(component, &component->m_AnimationTracks[i]);
        }
        DestroyDeferredCallbacks(component);
        // If we're going to use memset, then we should explicitly clear pose and instance arrays.
        component->m_BoneInstances.SetCapacity(0);
        component->m_BoneInstanceIds.SetCapacity(0);
        component->m_AnimationTracks.SetCapacity(0);
        component->m_DeferredCallbacks.SetCapacity(0);
        if (component->m_Material)
        {
            dmResource::Release(world->m_Factory, (void*)component->m_Material);
        }
        if (component->m_SpineScene)
        {
            dmResource::Release(world->m_Factory, (void*)component->m_SpineScene);
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

    static bool UpdateBones(SpineModelComponent* component)
    {
        if (component->m_BoneInstances.Empty())
            return false;

        uint32_t size = component->m_Bones.Size();
        DM_PROPERTY_ADD_U32(rmtp_SpineBones, size);
        for (uint32_t n = 0; n < size; ++n)
        {
            spBone* bone = component->m_Bones[n];

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

            target.m_Constraint->target->x = model_pos.getX();
            target.m_Constraint->target->y = model_pos.getY();
        }
        component->m_IKTargetPositions.SetSize(0);

        for (uint32_t i = 0; i < instance_count; ++i)
        {
            const IKTarget& target = component->m_IKTargets[i];

            dmVMath::Vector3 model_pos = (dmVMath::Vector3)dmTransform::Apply(world_to_model, dmGameObject::GetWorldPosition(target.m_Target));

            target.m_Constraint->target->x = model_pos.getX();
            target.m_Constraint->target->y = model_pos.getY();
        }
    }


    dmGameObject::UpdateResult CompSpineModelLateUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
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
            spAnimationState_update(component.m_AnimationStateInstance, dt);
            spAnimationState_apply(component.m_AnimationStateInstance, component.m_SkeletonInstance);

            ApplyIKTargets(&component);

            spSkeleton_update(component.m_SkeletonInstance, dt);
            spSkeleton_updateWorldTransform(component.m_SkeletonInstance, SP_PHYSICS_UPDATE);

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

    // Render object submission and storage:
    //
    // Visible geometry is generated during RENDER_LIST_OPERATION_BATCH, after
    // frustum culling. Render objects must also be submitted during BATCH to
    // preserve their sorted order relative to sprites and other components.
    //
    // Precomputing exact buffer and render-object requirements before BATCH
    // would require another traversal of the visible Spine geometry. That work
    // can be expensive, so geometry and draw descriptors are generated only
    // once, while unpredictable render-object growth is handled by overflow.
    //
    // AddToRender retains RenderObject pointers until drawing, while the final
    // 16/32-bit index type is only known after all visible batches are complete.
    // Storage must therefore remain stable during BATCH, and retained objects
    // are patched with their final index type and byte offset during END.
    //
    // m_RenderObjects is the contiguous steady-state storage. It reserves one
    // entry per possible Spine component before rendering starts and never grows
    // during BATCH. Inherited slot blend modes can produce more than one render
    // object per component, so excess objects use stable blocks of 32. On the
    // next frame's BEGIN, after previous pointers have been consumed, the primary
    // array grows to the previous high-water mark (rounded to 32), the overflow
    // blocks are released, and subsequent frames are contiguous again. The
    // primary array keeps that capacity and repeats this flow only after a new
    // high-water mark.
    static void InitRenderObject(SpineModelWorld*  world,
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

    static void FillRenderObject(SpineModelWorld* world, dmRender::HRenderContext render_context,
        dmRender::RenderObject& ro, dmGameSystem::HComponentRenderConstants constants,
        dmGraphics::HTexture texture, dmRender::HMaterial material,
        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode, uint32_t index_start, uint32_t index_count)
    {
        InitRenderObject(world, ro, constants, texture, material, blend_mode, index_start, index_count);
        // Submit in BATCH for correct sorting; END completes the retained object.
        dmRender::AddToRender(render_context, &ro);
    }

    static dmRender::RenderObject& AcquireRenderObject(SpineModelWorld* world)
    {
        uint32_t render_object_index = world->m_RenderObjectsInUse;

        // SetSize cannot relocate here because it remains within reserved capacity.
        if (render_object_index < world->m_RenderObjects.Capacity())
        {
            if (render_object_index == world->m_RenderObjects.Size())
            {
                world->m_RenderObjects.SetSize(render_object_index + 1);
            }

            ++world->m_RenderObjectsInUse;
            return world->m_RenderObjects[render_object_index];
        }

        // Overflow blocks do not move when the block-pointer array grows.
        uint32_t overflow_index = render_object_index - world->m_RenderObjects.Capacity();
        uint32_t block_index = overflow_index >> RENDER_OBJECT_OVERFLOW_BLOCK_SHIFT;
        uint32_t block_offset = overflow_index & RENDER_OBJECT_OVERFLOW_BLOCK_MASK;
        if (block_index == world->m_RenderObjectOverflowBlocks.Size())
        {
            if (world->m_RenderObjectOverflowBlocks.Full())
            {
                uint32_t new_capacity = dmMath::Max(4U, world->m_RenderObjectOverflowBlocks.Capacity() * 2);
                world->m_RenderObjectOverflowBlocks.SetCapacity(new_capacity);
            }
            world->m_RenderObjectOverflowBlocks.Push(new dmRender::RenderObject[RENDER_OBJECT_OVERFLOW_BLOCK_SIZE]);
        }

        ++world->m_RenderObjectsInUse;
        return world->m_RenderObjectOverflowBlocks[block_index][block_offset];
    }

    static void PrepareRenderObjectsForFrame(SpineModelWorld* world)
    {
        // BEGIN is the safe relocation point described above. Absorb the previous
        // high-water mark so overflow storage remains a one-frame spike path.
        if (world->m_RenderObjectsInUse > world->m_RenderObjects.Capacity())
        {
            uint32_t new_capacity = (world->m_RenderObjectsInUse + RENDER_OBJECT_OVERFLOW_BLOCK_MASK) & ~RENDER_OBJECT_OVERFLOW_BLOCK_MASK;
            world->m_RenderObjects.SetCapacity(new_capacity);

            for (uint32_t i = 0; i < world->m_RenderObjectOverflowBlocks.Size(); ++i)
            {
                delete[] world->m_RenderObjectOverflowBlocks[i];
            }
            world->m_RenderObjectOverflowBlocks.SetSize(0);
        }

        world->m_RenderObjectsInUse = 0;
    }

    template <typename T>
    static uint32_t ExtendCacheArray(dmArray<T>& array, uint32_t count)
    {
        uint32_t start = array.Size(), size = start + count;
        if (size > array.Capacity()) array.SetCapacity(dmMath::Max(size, dmMath::Max(16U, array.Capacity() * 2)));
        array.SetSize(size);
        return start;
    }

    static uint64_t PrefixMetadataBytes(const SpineModelWorld* world)
    {
        return (uint64_t)(world->m_ResidentComponents.Capacity() + world->m_PassComponents.Capacity()) * sizeof(SpinePrefixEntry);
    }

    static void ReportGeometryCache(SpineModelWorld* world)
    {
        uint64_t scratch = (uint64_t)world->m_CheckVertices.Capacity() * sizeof(SpineVertex)
            + (uint64_t)world->m_CheckIndices.Capacity() * sizeof(uint32_t)
            + (uint64_t)world->m_CheckDrawDescs.Capacity() * sizeof(SpineIndexedDrawDesc)
            + (uint64_t)world->m_CheckRO.Capacity() * sizeof(SpineROCheck);
        if (world->m_PackReference) scratch += (uint64_t)world->m_PackReference->m_IndexBufferData.Capacity() * sizeof(uint32_t)
            + world->m_PackReference->m_PackedIndexBufferData.Capacity();
        uint64_t bytes = PrefixMetadataBytes(world);
        world->m_MetadataPeak = dmMath::Max(world->m_MetadataPeak, bytes);
        dmLogInfo("[SPINE-CACHE-CHECK] {\"mode\":%d,\"world_id\":%u,\"epoch\":%llu,\"hits\":%llu,\"misses\":%llu,"
            "\"verified_components\":%llu,\"vertices\":%llu,\"indices\":%llu,\"draw_descs\":%llu,\"mismatches\":%llu,"
            "\"max_delta\":%.9g,\"bytes_cached\":%llu,\"metadata_bytes\":%llu,\"peak_metadata_bytes\":%llu,"
            "\"diagnostic_scratch_bytes\":%llu,\"cache_entries\":%u,\"complete_pass_reuse\":%llu,\"packed_checks\":%llu,"
            "\"ro_checks\":%llu,\"unsupported\":%llu,\"failed\":%s}",
            world->m_GeometryCacheMode, world->m_GeometryWorldId, (unsigned long long)world->m_GeometryEpoch,
            (unsigned long long)world->m_CacheHits, (unsigned long long)world->m_CacheMisses,
            (unsigned long long)world->m_CacheVerified, (unsigned long long)world->m_CacheVertices,
            (unsigned long long)world->m_CacheIndices, (unsigned long long)world->m_CacheDrawDescs,
            (unsigned long long)world->m_CacheMismatches, world->m_CacheMaxDelta,
            (unsigned long long)bytes, (unsigned long long)bytes, (unsigned long long)world->m_MetadataPeak,
            (unsigned long long)scratch, world->m_ResidentComponents.Size(), (unsigned long long)world->m_CompletePassReuse,
            (unsigned long long)world->m_PackedChecks, (unsigned long long)world->m_ROChecks,
            (unsigned long long)world->m_CacheUnsupported, world->m_GeometryCacheFailed ? "true" : "false");
    }

    static void FailPrefixCache(SpineModelWorld* world, const char* detail)
    {
        ++world->m_CacheMismatches;
        world->m_GeometryCacheFailed = true;
        world->m_PassEligible = world->m_PassPrefixMatch = false;
        world->m_ResidentEpoch = 0;
        dmLogError("Spine resident prefix cache mismatch: %s", detail);
        ReportGeometryCache(world);
    }

    static bool CompareCacheVertex(const SpineVertex& a, const SpineVertex& b, float& max_delta)
    {
        // Compare the 13 public fields, not struct bytes/padding. NaNs fail equality.
        const float av[] = {a.x,a.y,a.z,a.u,a.v,a.r,a.g,a.b,a.a,a.dark_r,a.dark_g,a.dark_b,a.page_index};
        const float bv[] = {b.x,b.y,b.z,b.u,b.v,b.r,b.g,b.b,b.a,b.dark_r,b.dark_g,b.dark_b,b.page_index};
        bool equal = true;
        for (uint32_t i = 0; i < sizeof(av) / sizeof(av[0]); ++i)
        {
            if (av[i] != bv[i])
            {
                equal = false;
                float delta = fabsf(av[i] - bv[i]);
                // JSON remains valid even if the corrupt value is non-finite.
                if (!(delta <= 3.402823466e+38F)) delta = 3.402823466e+38F;
                max_delta = dmMath::Max(max_delta, delta);
            }
        }
        return equal;
    }

    static bool VerifyAppendedGeometry(SpineModelWorld* world, const SpineModelComponent* component,
        bool inherit, uint32_t vertex_base, uint32_t index_base, uint32_t desc_base)
    {
        // Independent original generator, with the same pre-append sizes/bases as
        // the actual pass. Prefix values are never read by that generator or this
        // comparison; only its newly generated segment is the expected output.
        if (world->m_CheckVertices.Capacity() < vertex_base)
            world->m_CheckVertices.SetCapacity(vertex_base);
        if (world->m_CheckIndices.Capacity() < index_base)
            world->m_CheckIndices.SetCapacity(index_base);
        uint32_t desc_capacity = desc_base + (inherit ? component->m_SkeletonInstance->slotsCount : 0);
        if (world->m_CheckDrawDescs.Capacity() < desc_capacity)
            world->m_CheckDrawDescs.SetCapacity(desc_capacity);
        world->m_CheckVertices.SetSize(vertex_base);
        world->m_CheckIndices.SetSize(index_base);
        world->m_CheckDrawDescs.SetSize(desc_base);
        GenerateIndexedVertexData(world->m_CheckVertices, world->m_CheckIndices,
            component->m_SkeletonInstance, world->m_SkeletonClipper, component->m_World,
            Vector4(1.0f), inherit ? &world->m_CheckDrawDescs : 0, world->m_GeometryScratch);
        bool equal = world->m_VertexBufferData.Size() == world->m_CheckVertices.Size()
            && world->m_IndexBufferData.Size() == world->m_CheckIndices.Size()
            && world->m_DrawDescBuffer.Size() == world->m_CheckDrawDescs.Size();
        for (uint32_t i = vertex_base; i < dmMath::Min(world->m_VertexBufferData.Size(), world->m_CheckVertices.Size()); ++i)
            equal = CompareCacheVertex(world->m_VertexBufferData[i], world->m_CheckVertices[i], world->m_CacheMaxDelta) && equal;
        for (uint32_t i = index_base; i < dmMath::Min(world->m_IndexBufferData.Size(), world->m_CheckIndices.Size()); ++i)
            equal = (world->m_IndexBufferData[i] == world->m_CheckIndices[i]) && equal;
        for (uint32_t i = desc_base; i < dmMath::Min(world->m_DrawDescBuffer.Size(), world->m_CheckDrawDescs.Size()); ++i)
        {
            const SpineIndexedDrawDesc& a = world->m_DrawDescBuffer[i];
            const SpineIndexedDrawDesc& b = world->m_CheckDrawDescs[i];
            equal = (a.m_IndexStart == b.m_IndexStart && a.m_IndexCount == b.m_IndexCount && a.m_BlendMode == b.m_BlendMode) && equal;
        }
        ++world->m_CacheVerified;
        if (!equal)
        {
            ++world->m_CacheMismatches;
            world->m_GeometryCacheFailed = true;
            dmLogError("Spine geometry cache mismatch at epoch %llu, component %u; upstream path enabled for this world",
                (unsigned long long)world->m_GeometryEpoch, component->m_ComponentIndex);
            world->m_PassEligible = world->m_PassPrefixMatch = false;
            world->m_PassWrote = true;
            world->m_ResidentEpoch = 0;
            ReportGeometryCache(world);
            // Preserve the untouched prefix. Restore only the independently
            // generated suffix, including its absolute indices and actual sizes.
            if (world->m_VertexBufferData.Size() < world->m_CheckVertices.Size())
                ExtendCacheArray(world->m_VertexBufferData, world->m_CheckVertices.Size() - world->m_VertexBufferData.Size());
            if (world->m_IndexBufferData.Size() < world->m_CheckIndices.Size())
                ExtendCacheArray(world->m_IndexBufferData, world->m_CheckIndices.Size() - world->m_IndexBufferData.Size());
            if (world->m_DrawDescBuffer.Size() < world->m_CheckDrawDescs.Size())
                ExtendCacheArray(world->m_DrawDescBuffer, world->m_CheckDrawDescs.Size() - world->m_DrawDescBuffer.Size());
            world->m_VertexBufferData.SetSize(world->m_CheckVertices.Size());
            world->m_IndexBufferData.SetSize(world->m_CheckIndices.Size());
            world->m_DrawDescBuffer.SetSize(world->m_CheckDrawDescs.Size());
            uint32_t vertices = world->m_CheckVertices.Size() - vertex_base;
            uint32_t indices = world->m_CheckIndices.Size() - index_base;
            uint32_t descs = world->m_CheckDrawDescs.Size() - desc_base;
            if (vertices) memcpy(world->m_VertexBufferData.Begin() + vertex_base, world->m_CheckVertices.Begin() + vertex_base, vertices * sizeof(SpineVertex));
            if (indices) memcpy(world->m_IndexBufferData.Begin() + index_base, world->m_CheckIndices.Begin() + index_base, indices * sizeof(uint32_t));
            if (descs) memcpy(world->m_DrawDescBuffer.Begin() + desc_base, world->m_CheckDrawDescs.Begin() + desc_base, descs * sizeof(SpineIndexedDrawDesc));
        }
        return equal;
    }



    static void BeginPrefixPass(SpineModelWorld* world)
    {
        if (!world->m_GeometryCacheMode) return;
        world->m_PassComponents.SetSize(0);
        world->m_PassWrote = false;
        world->m_PassEligible = !world->m_GeometryCacheFailed;
        world->m_PassPrefixMatch = !world->m_GeometryCacheFailed && world->m_ResidentEpoch == world->m_GeometryEpoch;
        if (world->m_GeometryCacheMode == 2)
        {
            world->m_CheckVertices.SetSize(0);
            world->m_CheckIndices.SetSize(0);
            world->m_CheckDrawDescs.SetSize(0);
            world->m_CheckRO.SetSize(0);
        }
    }

    static void GenerateTrackedGeometry(SpineModelWorld* world, const SpineModelComponent* component, bool inherit)
    {
        uint32_t vertices = world->m_VertexBufferData.Size(), indices = world->m_IndexBufferData.Size();
        GenerateIndexedVertexData(world->m_VertexBufferData, world->m_IndexBufferData,
            component->m_SkeletonInstance, world->m_SkeletonClipper, component->m_World,
            Vector4(1.0f), inherit ? &world->m_DrawDescBuffer : 0, world->m_GeometryScratch);
        if (world->m_VertexBufferData.Size() != vertices || world->m_IndexBufferData.Size() != indices)
        {
            world->m_PassWrote = true;
            // Generation may overwrite the old suffix, or SetCapacity may discard
            // bytes beyond current Size. No old signature may survive that write.
            world->m_ResidentEpoch = 0;
        }
    }

    static void AppendPrefixGeometry(SpineModelWorld* world, SpineModelComponent* component)
    {
        uint32_t vertex_base = world->m_VertexBufferData.Size(), index_base = world->m_IndexBufferData.Size();
        uint32_t desc_base = world->m_DrawDescBuffer.Size(), ordinal = world->m_PassComponents.Size();
        bool hit = false;
        if (world->m_PassPrefixMatch && ordinal < world->m_ResidentComponents.Size())
        {
            const SpinePrefixEntry& old = world->m_ResidentComponents.Begin()[ordinal];
            hit = old.m_Component == component && old.m_Revision == component->m_GeometryRevision
                && old.m_VertexStart == vertex_base && old.m_IndexStart == index_base
                && old.m_VertexEnd >= vertex_base && old.m_IndexEnd >= index_base
                && old.m_VertexEnd <= world->m_VertexBufferData.Capacity()
                && old.m_IndexEnd <= world->m_IndexBufferData.Capacity();
            if (hit)
            {
                world->m_VertexBufferData.SetSize(old.m_VertexEnd);
                world->m_IndexBufferData.SetSize(old.m_IndexEnd);
            }
        }
        if (!hit)
        {
            world->m_PassPrefixMatch = false; // Never reuse a suffix after any divergence.
            GenerateTrackedGeometry(world, component, false);
        }
        SpinePrefixEntry entry = {component, component->m_GeometryRevision, vertex_base, index_base,
            world->m_VertexBufferData.Size(), world->m_IndexBufferData.Size()};
        uint32_t at = ExtendCacheArray(world->m_PassComponents, 1);
        world->m_PassComponents.Begin()[at] = entry;
        if (world->m_GeometryCacheMode == 2)
        {
            if (hit) ++world->m_CacheHits;
            else
            {
                ++world->m_CacheMisses;
                world->m_CacheVertices += entry.m_VertexEnd - vertex_base;
                world->m_CacheIndices += entry.m_IndexEnd - index_base;
            }
            world->m_MetadataPeak = dmMath::Max(world->m_MetadataPeak, PrefixMetadataBytes(world));
            VerifyAppendedGeometry(world, component, false, vertex_base, index_base, desc_base);
        }
    }

    // Mode 0, unsupported passes and failed diagnostics retain the original
    // packed-array upload. Supported 32-bit passes already own final-format data.
    static bool UseDirectIndexUpload(const SpineModelWorld* world)
    {
        return world->m_GeometryCacheMode && !world->m_GeometryCacheFailed
            && world->m_PassEligible && !world->m_Is16BitIndex;
    }

    static const uint8_t* IndexUploadData(const SpineModelWorld* world)
    {
        return UseDirectIndexUpload(world)
            ? reinterpret_cast<const uint8_t*>(world->m_IndexBufferData.Begin())
            : world->m_PackedIndexBufferData.Begin();
    }

    static uint32_t IndexUploadSize(const SpineModelWorld* world)
    {
        return UseDirectIndexUpload(world)
            ? world->m_IndexBufferData.Size() * sizeof(uint32_t)
            : world->m_PackedIndexBufferData.Size();
    }

    static uint64_t IndexUploadCapacity(const SpineModelWorld* world)
    {
        return UseDirectIndexUpload(world)
            ? (uint64_t)world->m_IndexBufferData.Capacity() * sizeof(uint32_t)
            : world->m_PackedIndexBufferData.Capacity();
    }

    static bool CanReusePrefixUpload(SpineModelWorld* world, uint32_t packed_bytes)
    {
        return world->m_GeometryCacheMode && !world->m_GeometryCacheFailed && world->m_PassEligible
            && world->m_PassPrefixMatch && !world->m_PassWrote && world->m_ResidentEpoch == world->m_GeometryEpoch
            && world->m_PassComponents.Size() == world->m_ResidentComponents.Size()
            && world->m_VertexBufferData.Size() == world->m_ResidentVertices
            && world->m_IndexBufferData.Size() == world->m_ResidentIndices
            && packed_bytes == world->m_ResidentPackedBytes && packed_bytes <= IndexUploadCapacity(world);
    }

    static bool VerifyPrefixPacked(SpineModelWorld* world)
    {
        ++world->m_PackedChecks;
        if (!world->m_PackReference) world->m_PackReference = new SpineModelWorld();
        SpineModelWorld* reference = world->m_PackReference;
        reference->m_IndexBufferData.SetSize(0);
        uint32_t count = world->m_CheckIndices.Size();
        ExtendCacheArray(reference->m_IndexBufferData, count);
        if (count) memcpy(reference->m_IndexBufferData.Begin(), world->m_CheckIndices.Begin(), count * sizeof(uint32_t));
        reference->m_Is16BitIndex = world->m_CheckVertices.Size() <= 65536;
        PackIndexBufferData(reference); // The original packer, on independent original-generator indices.
        bool equal = world->m_VertexBufferData.Size() == world->m_CheckVertices.Size()
            && world->m_IndexBufferData.Size() == count && GetIndexTypeSize(world) == GetIndexTypeSize(reference)
            && IndexUploadSize(world) == reference->m_PackedIndexBufferData.Size();
        const uint8_t* actual = IndexUploadData(world);
        const uint8_t* expected = reference->m_PackedIndexBufferData.Begin();
        for (uint32_t i = 0; i < dmMath::Min(IndexUploadSize(world), reference->m_PackedIndexBufferData.Size()); ++i)
            equal = (actual[i] == expected[i]) && equal;
        if (!equal) FailPrefixCache(world, "retained packed bytes/index width differ from original generator + packer");
        return equal;
    }

    template <typename T>
    static void RestoreCacheArray(dmArray<T>& actual, const dmArray<T>& expected)
    {
        actual.SetSize(0);
        ExtendCacheArray(actual, expected.Size());
        if (expected.Size()) memcpy(actual.Begin(), expected.Begin(), expected.Size() * sizeof(T));
    }

    static void RestoreVerifiedGeometry(SpineModelWorld* world)
    {
        // Only called while a complete supported pass has an independent reference.
        RestoreCacheArray(world->m_VertexBufferData, world->m_CheckVertices);
        RestoreCacheArray(world->m_IndexBufferData, world->m_CheckIndices);
        world->m_Is16BitIndex = world->m_VertexBufferData.Size() <= 65536;
    }

    static dmRender::RenderObject& PrefixRenderObject(SpineModelWorld* world, uint32_t index)
    {
        if (index < world->m_RenderObjects.Capacity()) return world->m_RenderObjects[index];
        index -= world->m_RenderObjects.Capacity();
        return world->m_RenderObjectOverflowBlocks[index >> RENDER_OBJECT_OVERFLOW_BLOCK_SHIFT][index & RENDER_OBJECT_OVERFLOW_BLOCK_MASK];
    }

    static void AppendROCheck(SpineModelWorld* world, const SpineModelComponent* component,
        dmGraphics::HTexture texture, dmRender::HMaterial material,
        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode, uint32_t index_start, uint32_t index_count)
    {
        uint32_t at = ExtendCacheArray(world->m_CheckRO, 1);
        // Build from batch inputs and original-generator ranges, never snapshot
        // the actual RenderObject: a wrong but in-bounds range must be detected.
        InitRenderObject(world, world->m_CheckRO[at].m_Expected, component->m_RenderConstants,
            texture, material, blend_mode, index_start, index_count);
    }

    static bool VerifyROChecks(SpineModelWorld* world)
    {
        bool equal = world->m_CheckRO.Size() == world->m_RenderObjectsInUse;
        if (!equal)
        {
            // The render list already retains these pointers. Suppress a broken
            // submission rather than drawing ranges with no independent reference.
            for (uint32_t i = 0; i < world->m_RenderObjectsInUse; ++i)
                PrefixRenderObject(world, i).m_VertexCount = 0;
            FailPrefixCache(world, "RenderObject count differs from original-generator batches");
            return false;
        }
        for (uint32_t i = 0; i < dmMath::Min(world->m_CheckRO.Size(), world->m_RenderObjectsInUse); ++i)
        {
            dmRender::RenderObject& ro = PrefixRenderObject(world, i);
            dmRender::RenderObject expected = world->m_CheckRO[i].m_Expected;
            bool in_bounds = expected.m_VertexStart <= world->m_CheckIndices.Size()
                && expected.m_VertexCount <= world->m_CheckIndices.Size() - expected.m_VertexStart;
            expected.m_IndexType = GetIndexType(world);
            expected.m_VertexStart *= GetIndexTypeSize(world);
            bool matches = in_bounds && ro.m_IndexType == expected.m_IndexType
                && ro.m_VertexStart == expected.m_VertexStart && ro.m_VertexCount == expected.m_VertexCount
                && ro.m_VertexDeclaration == expected.m_VertexDeclaration
                && ro.m_VertexBuffer == expected.m_VertexBuffer && ro.m_IndexBuffer == expected.m_IndexBuffer
                && ro.m_PrimitiveType == expected.m_PrimitiveType && ro.m_Material == expected.m_Material
                && ro.m_Textures[0] == expected.m_Textures[0] && ro.m_ConstantBuffer == expected.m_ConstantBuffer
                && ro.m_SetBlendFactors == expected.m_SetBlendFactors
                && ro.m_SourceBlendFactor == expected.m_SourceBlendFactor
                && ro.m_DestinationBlendFactor == expected.m_DestinationBlendFactor;
            if (!matches)
            {
                ro = expected; // Keep the submitted address and repair this pass before drawing.
                if (!in_bounds) ro.m_VertexCount = 0;
                equal = false;
            }
        }
        world->m_ROChecks += world->m_RenderObjectsInUse;
        if (!equal) FailPrefixCache(world, "RenderObject range or render state differs from original-generator batch");
        return equal;
    }

    static void CommitPrefixResident(SpineModelWorld* world, uint32_t packed_bytes)
    {
        if (!world->m_GeometryCacheMode) return;
        if (!world->m_PassEligible || world->m_GeometryCacheFailed)
        {
            world->m_ResidentEpoch = 0;
            return;
        }
        // Every nonempty END replaces the signature, including a shorter matching
        // prefix that just uploaded a smaller GPU buffer. CPU/GPU identity stays unified.
        world->m_ResidentComponents.Swap(world->m_PassComponents);
        world->m_ResidentVertices = world->m_VertexBufferData.Size();
        world->m_ResidentIndices = world->m_IndexBufferData.Size();
        world->m_ResidentPackedBytes = packed_bytes;
        world->m_ResidentEpoch = world->m_GeometryEpoch;
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

        uint32_t index_start            = world->m_IndexBufferData.Size();
        uint32_t check_index_start      = world->m_CheckIndices.Size();
        uint32_t draw_desc_buffer_count = 0;

        // This is a temporary scratch buffer just used for this batch call, so we make sure to reset it.
        world->m_DrawDescBuffer.SetSize(0);

        for (uint32_t *i = begin; i != end; ++i)
        {
            component_index = (uint32_t)buf[*i].m_UserData;
            if (use_inherit_blend)
            {
                const SpineModelComponent* component = (const SpineModelComponent*) components[component_index];
                draw_desc_buffer_count += (uint32_t)component->m_SkeletonInstance->slotsCount;
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
            SpineModelComponent* component = components[component_index];
            if (world->m_GeometryCacheMode && !world->m_GeometryCacheFailed && blend_mode == dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA)
                AppendPrefixGeometry(world, component);
            else if (world->m_GeometryCacheMode)
            {
                world->m_PassEligible = world->m_PassPrefixMatch = false;
                if (world->m_GeometryCacheMode == 2) ++world->m_CacheUnsupported;
                GenerateTrackedGeometry(world, component, use_inherit_blend);
            }
            else
                dmSpine::GenerateIndexedVertexData(world->m_VertexBufferData, world->m_IndexBufferData, component->m_SkeletonInstance, world->m_SkeletonClipper, component->m_World, Vector4(1.0f), use_inherit_blend ? &world->m_DrawDescBuffer : 0, world->m_GeometryScratch);
        }

        uint32_t index_count = world->m_IndexBufferData.Size() - index_start;
        if (index_count == 0)
        {
            return;
        }

        dmGraphics::HTexture texture = GetSpineScene(first)->m_TextureSet->m_Texture->m_Texture; // spine - texture set resource - texture resource - texture
        dmRender::HMaterial material = GetMaterial(first);

        if (use_inherit_blend)
        {
            uint32_t draw_desc_count = world->m_DrawDescBuffer.Size();
            if (draw_desc_count > 0)
            {
                MergeIndexedDrawDescs(world->m_DrawDescBuffer, world->m_MergedDrawDescBuffer);

                uint32_t merged_size = world->m_MergedDrawDescBuffer.Size();
                for (int i = 0; i < merged_size; ++i)
                {
                    dmRender::RenderObject& ro = AcquireRenderObject(world);
                    FillRenderObject(world, render_context, ro, first->m_RenderConstants, texture, material,
                        SpineBlendModeToRenderBlendMode((spBlendMode) world->m_MergedDrawDescBuffer[i].m_BlendMode),
                        world->m_MergedDrawDescBuffer[i].m_IndexStart,
                        world->m_MergedDrawDescBuffer[i].m_IndexCount);
                }
            }
        }
        else
        {
            dmRender::RenderObject& ro = AcquireRenderObject(world);
            FillRenderObject(world, render_context, ro, first->m_RenderConstants, texture, material, blend_mode, index_start, index_count);
            if (world->m_GeometryCacheMode == 2 && world->m_PassEligible)
                AppendROCheck(world, first, texture, material, blend_mode, check_index_start,
                    world->m_CheckIndices.Size() - check_index_start);
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
                BeginPrefixPass(world);
                PrepareRenderObjectsForFrame(world);
                world->m_VertexBufferData.SetSize(0);
                world->m_IndexBufferData.SetSize(0);
                world->m_PackedIndexBufferData.SetSize(0);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_BATCH:
            {
                RenderBatch(world, params.m_Context, params.m_Buf, params.m_Begin, params.m_End);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_END:
            {
                world->m_Is16BitIndex = world->m_VertexBufferData.Size() <= 65536;
                uint32_t vertex_data_size = sizeof(dmSpine::SpineVertex) * world->m_VertexBufferData.Size();
                uint32_t index_data_size = GetIndexTypeSize(world) * world->m_IndexBufferData.Size();
                if (vertex_data_size && index_data_size)
                {
                    bool reuse_upload = CanReusePrefixUpload(world, index_data_size);
                    bool verify_pass = world->m_GeometryCacheMode == 2 && world->m_PassEligible;
                    bool packed_ready = !UseDirectIndexUpload(world);
                    if (packed_ready)
                    {
                        if (reuse_upload) world->m_PackedIndexBufferData.SetSize(index_data_size);
                        else PackIndexBufferData(world);
                    }
                    // Validate before either retaining or uploading GPU contents.
                    // Failure changes the upload path, so rebuild from the independent
                    // reference and use the original packer for this same pass.
                    if (verify_pass && !VerifyPrefixPacked(world))
                    {
                        RestoreVerifiedGeometry(world);
                        PackIndexBufferData(world);
                        packed_ready = true;
                        reuse_upload = false;
                        vertex_data_size = sizeof(dmSpine::SpineVertex) * world->m_VertexBufferData.Size();
                        index_data_size = GetIndexTypeSize(world) * world->m_IndexBufferData.Size();
                    }

                    uint32_t index_type_size = GetIndexTypeSize(world);
                    dmGraphics::Type index_type = GetIndexType(world);
                    uint32_t primary_render_object_count = dmMath::Min(world->m_RenderObjectsInUse, world->m_RenderObjects.Capacity());
                    for (uint32_t i = 0; i < primary_render_object_count; ++i)
                    {
                        world->m_RenderObjects[i].m_IndexType = index_type;
                        world->m_RenderObjects[i].m_VertexStart *= index_type_size;
                    }

                    uint32_t render_objects_left = world->m_RenderObjectsInUse - primary_render_object_count;
                    for (uint32_t block_index = 0; render_objects_left > 0; ++block_index)
                    {
                        dmRender::RenderObject* block = world->m_RenderObjectOverflowBlocks[block_index];
                        uint32_t render_objects_in_block = dmMath::Min(RENDER_OBJECT_OVERFLOW_BLOCK_SIZE, render_objects_left);
                        for (uint32_t i = 0; i < render_objects_in_block; ++i)
                        {
                            block[i].m_IndexType = index_type;
                            block[i].m_VertexStart *= index_type_size;
                        }
                        render_objects_left -= render_objects_in_block;
                    }

                    if (verify_pass && !VerifyROChecks(world))
                        reuse_upload = false;
                    if (!reuse_upload)
                    {
                        // An RO failure can switch a direct 32-bit pass back to packing.
                        if (!UseDirectIndexUpload(world) && !packed_ready) PackIndexBufferData(world);
                        dmGraphics::SetVertexBufferData(world->m_VertexBuffer, vertex_data_size,
                            world->m_VertexBufferData.Begin(), dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
                        dmGraphics::SetIndexBufferData(world->m_IndexBuffer, index_data_size,
                            IndexUploadData(world), dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
                    }
                    else if (world->m_GeometryCacheMode == 2) ++world->m_CompletePassReuse;
                    CommitPrefixResident(world, index_data_size);
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

        if (world->m_GeometryCacheMode == 2 && world->m_GeometryEpoch && world->m_GeometryEpoch % 60 == 0)
            ReportGeometryCache(world);
        ++world->m_GeometryEpoch;
        if (!world->m_GeometryEpoch) { world->m_ResidentEpoch = 0; ++world->m_GeometryEpoch; }
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
        InvalidateGeometryCache(component);
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
        InvalidateGeometryCache(component);
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
        InvalidateGeometryCache(component);
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
        ScheduleBoneRebuild(component);
        component->m_ReHash = 1;
        return true;
    }

    void CompSpineModelOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        int index = *params.m_UserData;
        SpineModelComponent* component = GetComponentFromIndex(world, index);
        InvalidateGeometryCache(component);
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
        InvalidateGeometryCache(component);
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

            float duration = track->m_AnimationInstance->animationEnd - track->m_AnimationInstance->animationStart;
            float t = unit_0_1 * duration;

            track->m_AnimationInstance->trackTime = t;
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

            track->m_AnimationInstance->timeScale = params.m_Value.m_Number;
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
            dmGameObject::PropertyResult res = dmGameSystem::SetResourceProperty(context->m_Factory, params.m_Value, SPINE_SCENE_EXT_HASH, (void**)&component->m_SpineScene);
            if (res == dmGameObject::PROPERTY_RESULT_OK)
            {
                if (component->m_AnimationStateInstance)
                {
                    spAnimationState_dispose(component->m_AnimationStateInstance);
                }
                component->m_AnimationStateInstance = 0;
                if (component->m_SkeletonInstance)
                {
                    spSkeleton_dispose(component->m_SkeletonInstance);
                }
                component->m_SkeletonInstance = 0;
                component->m_AnimationTracks.SetSize(0);
                ScheduleBoneRebuild(component);

                SpineSceneResource* spine_scene_new = GetSpineScene(component);
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
        SpineModelWorld* world = (SpineModelWorld*)params->m_UserData;
        world->m_ResidentEpoch = 0;
        dmArray<SpineModelComponent*>& components = world->m_Components.GetRawObjects();
        for (uint32_t i = 0; i < components.Size(); ++i) InvalidateGeometryCache(components[i]);
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
        spinemodelctx->m_GeometryCacheMode = dmConfigFile::GetInt(ctx->m_Config, "spine.geometry_cache", 0);
        if (spinemodelctx->m_GeometryCacheMode < 0 || spinemodelctx->m_GeometryCacheMode > 2)
            spinemodelctx->m_GeometryCacheMode = 0;
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
        ComponentTypeSetLateUpdateFn(type, CompSpineModelLateUpdate);
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

    bool CompSpineModelSetIKTargetInstance(SpineModelComponent* component, dmhash_t constraint_id, float mix, dmhash_t instance_id)
    {
        InvalidateGeometryCache(component);
        if (instance_id == 0)
        {
            return CompSpineModelResetIKTarget(component, constraint_id);
        }

        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);

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
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);
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
        InvalidateGeometryCache(component);
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
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);

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

    bool CompSpineModelClearSkin(SpineModelComponent* component, dmhash_t skin_id)
    {
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = GetSpineScene(component);
        spSkin* skin = spine_scene->m_Skeleton->defaultSkin;;

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

        spSkin_clear(skin);

        return true;
    }

    bool CompSpineModelAddSkin(SpineModelComponent* component, dmhash_t skin_id_a, dmhash_t skin_id_b)
    {
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        spSkin* skin_a = spine_scene->m_Skeleton->defaultSkin;
        spSkin* skin_b = spine_scene->m_Skeleton->defaultSkin;

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
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;
        spSkin* skin_a = spine_scene->m_Skeleton->defaultSkin;;
        spSkin* skin_b = spine_scene->m_Skeleton->defaultSkin;;

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

    bool CompSpineModelSetSlotColor(SpineModelComponent* component, dmhash_t slot_id,  Vectormath::Aos::Vector4* color)
    {
        InvalidateGeometryCache(component);
        SpineModelResource* spine_model = component->m_Resource;
        SpineSceneResource* spine_scene = spine_model->m_SpineScene;

        uint32_t* index = spine_scene->m_SlotNameToIndex.Get(slot_id);
        if (!index)
        {
            dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
            return false;
        }

        spSlot* slot = component->m_SkeletonInstance->slots[*index];
        spColor_setFromFloats(&slot->color, color->getX(), color->getY(), color->getZ(), color->getW());

        return true;
    }

    bool CompSpineModelSetAttachment(SpineModelComponent* component, dmhash_t slot_id, dmhash_t attachment_id)
    {
        InvalidateGeometryCache(component);
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
        *instance_id = component->m_BoneInstanceIds[*index];
        return true;
    }

    void CompSpineModelPhysicsTranslate(SpineModelComponent* component, Point3 translation)
    {
        InvalidateGeometryCache(component);
        spSkeleton_physicsTranslate(component->m_SkeletonInstance, translation.getX(), translation.getY());
    }

    void CompSpineModelPhysicsRotate(SpineModelComponent* component, Point3 center, float degrees)
    {
        InvalidateGeometryCache(component);
        spSkeleton_physicsRotate(component->m_SkeletonInstance, center.getX(), center.getY(), degrees);
    }
}

DM_DECLARE_COMPONENT_TYPE(ComponentTypeSpineModelExt, "spinemodelc", dmSpine::CompTypeSpineModelCreate, dmSpine::CompTypeSpineModelDestroy);
