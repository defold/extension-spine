#include "gui_node_spine.h"
#include "res_spine_scene.h"
#include <common/vertices.h>

#include <dmsdk/dlib/buffer.h>
#include <dmsdk/dlib/dstrings.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/dlib/profile.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/gui.h>
#include <dmsdk/script/script.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonClipping.h>
#include <spine/Slot.h>
#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/Bone.h>
#include <spine/IkConstraint.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto
#include "script_spine_gui.h"

DM_PROPERTY_EXTERN(rmtp_Spine);
DM_PROPERTY_EXTERN(rmtp_SpineBones);
DM_PROPERTY_U32(rmtp_SpineGuiNodes, 0, PROFILE_PROPERTY_FRAME_RESET, "", &rmtp_Spine);

namespace dmSpine
{

static const dmhash_t SPINE_SCENE               = dmHashString64("spine_scene");
static const dmhash_t SPINE_DEFAULT_ANIMATION   = dmHashString64("spine_default_animation");
static const dmhash_t SPINE_SKIN                = dmHashString64("spine_skin");
static const dmhash_t SPINE_SCENE_SUFFIX        = dmHashString64(".spinescenec");

struct GuiNodeTypeContext
{
    spSkeletonClipping* m_SkeletonClipper;
};

struct InternalGuiNode
{
    dmhash_t            m_SpinePath;
    SpineSceneResource* m_SpineScene;

    spSkeleton*         m_SkeletonInstance;
    spAnimationState*   m_AnimationStateInstance;
    dmArray<dmSpine::GuiSpineAnimationTrack> m_AnimationTracks;
    
    dmhash_t            m_SkinId;

    dmVMath::Matrix4    m_Transform; // the world transform

    dmGui::HScene       m_GuiScene;
    dmGui::HNode        m_GuiNode;
    dmGui::AdjustMode   m_AdjustMode;
    const char*         m_Id;

    dmArray<dmGui::HNode>   m_BonesNodes;
    dmArray<dmhash_t>       m_BonesIds;     // Matches 1:1 with m_BoneNodes     (each element is hash(scene_name/bone_name))
    dmArray<dmhash_t>       m_BonesNames;   // Matches 1:1 with m_BoneNodes (each element is hash(bone_name)))
    dmArray<spBone*>        m_Bones;        // Matches 1:1 with m_BoneNodes

    // IK targets for GUI spine nodes
    dmArray<GuiIKTarget>    m_IKTargets;           // targets that follow GUI nodes
    dmArray<GuiIKTarget>    m_IKTargetPositions;   // targets with fixed positions

    uint8_t             m_FindBones : 1;
    uint8_t             m_FirstUpdate : 1;
    uint8_t             : 6;

    InternalGuiNode()
    : m_SpinePath(0)
    , m_SpineScene(0)
    , m_SkeletonInstance(0)
    , m_AnimationStateInstance(0)
    , m_SkinId(0)
    , m_Id(0)
    , m_FindBones(0)
    , m_FirstUpdate(1)
    {}
};

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node, bool create_bones);

static inline bool IsLooping(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_BACKWARD ||
            playback == dmGui::PLAYBACK_LOOP_FORWARD ||
            playback == dmGui::PLAYBACK_LOOP_PINGPONG;
}

static inline bool IsReverse(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_BACKWARD ||
            playback == dmGui::PLAYBACK_ONCE_BACKWARD;
}

static inline bool IsPingPong(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_PINGPONG ||
            playback == dmGui::PLAYBACK_ONCE_PINGPONG;
}

// static void printStack(lua_State* L)
// {
//     int top = lua_gettop(L);
//     int bottom = 1;
//     lua_getglobal(L, "tostring");
//     for(int i = top; i >= bottom; i--)
//     {
//         lua_pushvalue(L, -1);
//         lua_pushvalue(L, i);
//         lua_pcall(L, 1, 1, 0);
//         const char *str = lua_tostring(L, -1);
//         if (str) {
//             printf("%2d: %s\n", i, str);
//         }else{
//             printf("%2d: %s\n", i, luaL_typename(L, i));
//         }
//         lua_pop(L, 1);
//     }
//     lua_pop(L, 1);
// }


static GuiSpineAnimationTrack* GetTrackFromIndex(InternalGuiNode* node, int track_index)
{
    if (track_index < 0 || track_index >= node->m_AnimationTracks.Size())
        return nullptr;
    return &node->m_AnimationTracks[track_index];
}

static void ClearTrackCallback(GuiSpineAnimationTrack* track)
{
    if (track->m_CallbackInfo)
    {
        dmScript::DestroyCallback(track->m_CallbackInfo);
        track->m_CallbackInfo = 0x0;
    }
}


static void SendAnimationDone(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->trackIndex);
    if (!track)
        return;

    dmGameSystemDDF::SpineAnimationDone message;
    message.m_AnimationId = dmHashString64(entry->animation->name);
    message.m_Playback    = track->m_Playback;
    message.m_Track       = entry->trackIndex + 1; // Convert to 1-based indexing for API

    // Send to track-specific callback if available
    if (track->m_CallbackInfo && dmScript::IsCallbackValid(track->m_CallbackInfo))
    {
        // Store callback ID to check if callback is still valid after execution
        uint32_t callbackId = track->m_CallbackId;
        
        lua_State* L = dmScript::GetCallbackLuaContext(track->m_CallbackInfo);
        DM_LUA_STACK_CHECK(L, 0);

        if (dmScript::SetupCallback(track->m_CallbackInfo))
        {
            dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
            dmScript::PushHash(L, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor->m_NameHash);
            dmScript::PushDDF(L, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message, true);

            dmScript::PCall(L, 4, 0); // instance + 3
            dmScript::TeardownCallback(track->m_CallbackInfo);
        }
        
        // Only clear callback if it hasn't been replaced during callback execution
        // (user might have called gui.play_spine_anim from within the callback)
        if (callbackId == track->m_CallbackId)
        {
            ClearTrackCallback(track);
        }
    }
}

static void SendSpineEvent(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->trackIndex);
    if (!track)
        return;

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
    message.m_Track       = entry->trackIndex + 1; // Convert to 1-based indexing for API

    // Send to track-specific callback if available
    if (track->m_CallbackInfo && dmScript::IsCallbackValid(track->m_CallbackInfo))
    {
        lua_State* L = dmScript::GetCallbackLuaContext(track->m_CallbackInfo);
        DM_LUA_STACK_CHECK(L, 0);

        if (dmScript::SetupCallback(track->m_CallbackInfo))
        {
            dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
            dmScript::PushHash(L, dmGameSystemDDF::SpineEvent::m_DDFDescriptor->m_NameHash);
            dmScript::PushDDF(L, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message, true);

            dmScript::PCall(L, 4, 0); // instance + 3
            dmScript::TeardownCallback(track->m_CallbackInfo);
        }
        
        // Note: For spine events, we don't clear the callback since events can occur multiple times
        // during an animation. The callback will be cleared when the animation completes or is cancelled.
    }
}

static void SpineEventListener(spAnimationState* state, spEventType type, spTrackEntry* entry, spEvent* event)
{
    InternalGuiNode* node = (InternalGuiNode*)state->userData;
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->trackIndex);

    switch (type)
    {
    // case SP_ANIMATION_START:
    //     printf("Animation %s started on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    // case SP_ANIMATION_INTERRUPT:
    //     printf("Animation %s interrupted on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    // case SP_ANIMATION_END:
    //     printf("Animation %s ended on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    case SP_ANIMATION_COMPLETE:
        {
            if (!track)
                break;

            //printf("Animation %s complete on track %i\n", entry->animation->name, entry->trackIndex);
            // TODO: Should we send event for looping animations as well?

            if (!IsLooping(track->m_Playback))
            {
                // Only send the animation done event if it's not looping
                // Note: SendAnimationDone will safely clear the callback with ID check
                SendAnimationDone(node, state, entry, event);
            }

            if (IsPingPong(track->m_Playback))
            {
                entry->reverse = !entry->reverse;
            }
            
        }
        break;
    case SP_ANIMATION_DISPOSE:
        {
            if (track && track->m_AnimationInstance == entry)
            {
                ClearTrackCallback(track);
                track->m_AnimationInstance = nullptr;
                track->m_AnimationId = 0;
            }
        }
        break;
    case SP_ANIMATION_EVENT:
        SendSpineEvent(node, state, entry, event);
        break;
    default:
        break;
    }
}

static const uint32_t INVALID_ANIMATION_INDEX = 0xFFFFFFFF;

static inline uint32_t FindAnimationIndex(InternalGuiNode* node, dmhash_t animation)
{
    SpineSceneResource* spine_scene = node->m_SpineScene;
    uint32_t* index = spine_scene->m_AnimationNameToIndex.Get(animation);
    return index ? *index : INVALID_ANIMATION_INDEX;
}

static bool PlayAnimation(InternalGuiNode* node, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, int32_t track, dmScript::LuaCallbackInfo* callback)
{
    SpineSceneResource* spine_scene = node->m_SpineScene;
    uint32_t index = FindAnimationIndex(node, animation_id);
    if (index == INVALID_ANIMATION_INDEX)
    {
        dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
        return false;
    }
    else if (index >= spine_scene->m_Skeleton->animationsCount)
    {
        dmLogError("Animation index %u is too large. Number of animations are %u", index, spine_scene->m_Skeleton->animationsCount);
        return false;
    }

    int trackIndex = track - 1; // Convert from 1-based to 0-based indexing
    int loop = IsLooping(playback);

    spAnimation* animation = spine_scene->m_Skeleton->animations[index];

    // Ensure we have enough tracks
    if (trackIndex >= node->m_AnimationTracks.Capacity())
    {
        node->m_AnimationTracks.SetCapacity(trackIndex + 4);
    }

    while (trackIndex >= node->m_AnimationTracks.Size())
    {
        GuiSpineAnimationTrack track;
        track.m_AnimationInstance = nullptr;
        track.m_AnimationId = 0;
        track.m_Playback = dmGui::PLAYBACK_LOOP_FORWARD;
        track.m_CallbackInfo = nullptr;
        track.m_CallbackId = 0;
        node->m_AnimationTracks.Push(track);
    }

    GuiSpineAnimationTrack& targetTrack = node->m_AnimationTracks[trackIndex];

    // Clear any existing callback for this track
    ClearTrackCallback(&targetTrack);

    // Set up the track
    targetTrack.m_AnimationId = animation_id;
    targetTrack.m_AnimationInstance = spAnimationState_setAnimation(node->m_AnimationStateInstance, trackIndex, animation, loop);
    targetTrack.m_Playback = playback;
    targetTrack.m_CallbackInfo = callback;
    targetTrack.m_CallbackId++;

    // Configure animation properties
    targetTrack.m_AnimationInstance->timeScale = playback_rate;
    targetTrack.m_AnimationInstance->reverse = IsReverse(playback);
    targetTrack.m_AnimationInstance->mixDuration = blend_duration;
    targetTrack.m_AnimationInstance->trackTime = dmMath::Clamp(offset, 
        targetTrack.m_AnimationInstance->animationStart, 
        targetTrack.m_AnimationInstance->animationEnd);


    return true;
}

// SCRIPTING

static void FindBones(InternalGuiNode* node);

bool SetScene(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_scene)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);

    if (spine_scene == node->m_SpinePath)
        return true;

    SpineSceneResource* resource = (SpineSceneResource*)dmGui::GetResource(scene, spine_scene, SPINE_SCENE_SUFFIX);
    if (!resource)
        return false;

    if (node->m_FindBones)
    {
        node->m_FindBones = 0;
        FindBones(node);
    }

    // A possible improvement is to find an animation with the same name in the new scene
    // and try to use the same unit time cursor
    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    node->m_AnimationStateInstance = 0;
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);
    node->m_SkeletonInstance = 0;

    // Clean up all track callbacks since we're changing scenes
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++)
    {
        ClearTrackCallback(&node->m_AnimationTracks[i]);
    }
    node->m_AnimationTracks.SetSize(0);

    return SetupNode(spine_scene, resource, node, true);
}

dmhash_t GetScene(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_SpinePath;
}

dmGui::HNode GetBone(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t bone_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (node->m_FindBones)
    {
        node->m_FindBones = 0;
        FindBones(node);
    }
    uint32_t count = node->m_BonesIds.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (node->m_BonesIds[i] == bone_id)
            return node->m_BonesNodes[i];
    }
    count = node->m_BonesNames.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (node->m_BonesNames[i] == bone_id)
            return node->m_BonesNodes[i];
    }
    return 0;
}

bool PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, int32_t track, dmScript::LuaCallbackInfo* callback)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return PlayAnimation(node, animation_id, playback, blend_duration, offset, playback_rate, track, callback);
}

static void CancelTrackAnimation(InternalGuiNode* node, int32_t track_index)
{
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, track_index);
    if (!track || !track->m_AnimationInstance)
        return;

    spAnimationState_clearTrack(node->m_AnimationStateInstance, track->m_AnimationInstance->trackIndex);

    ClearTrackCallback(track);
    track->m_AnimationInstance = nullptr;
    track->m_AnimationId = 0;
}

static void CancelAllAnimations(InternalGuiNode* node)
{
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++) {
        CancelTrackAnimation(node, i);
    }
}

void CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    CancelAllAnimations(node);
}

void CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1; // Convert from 1-based to 0-based indexing
    CancelTrackAnimation(node, trackIndex);
}

bool AddSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id_a, dmhash_t skin_id_b){
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin_a = node->m_SpineScene->m_Skeleton->defaultSkin;
    spSkin* skin_b = node->m_SpineScene->m_Skeleton->defaultSkin;

    if (skin_id_a)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id_a);
        if (!index)
        {
            dmLogError("No skin '%s' found", dmHashReverseSafe64(skin_id_a));
            return false;
        }

        skin_a = node->m_SpineScene->m_Skeleton->skins[*index];
    }

    if (skin_id_b)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id_b);
        if (!index)
        {
            dmLogError("No skin '%s' found", dmHashReverseSafe64(skin_id_b));
            return false;
        }

        skin_b = node->m_SpineScene->m_Skeleton->skins[*index];
    }

    spSkin_addSkin(skin_a,skin_b);
    return true;
}

bool CopySkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id_a, dmhash_t skin_id_b){
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin_a = node->m_SpineScene->m_Skeleton->defaultSkin;
    spSkin* skin_b = node->m_SpineScene->m_Skeleton->defaultSkin;

    if (skin_id_a)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id_a);
        if (!index)
        {
            return false;
        }

        skin_a = node->m_SpineScene->m_Skeleton->skins[*index];
    }
    if (skin_id_b)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id_b);
        if (!index)
        {
            return false;
        }

        skin_b = node->m_SpineScene->m_Skeleton->skins[*index];
    }

    spSkin_copySkin(skin_a,skin_b);
    return true;
}

bool ClearSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id){
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin = node->m_SpineScene->m_Skeleton->defaultSkin;

    if (skin_id)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id);
        if (!index)
        {
            return false;
        }

        skin = node->m_SpineScene->m_Skeleton->skins[*index];
    }

    spSkin_clear(skin);
    return true;
}

bool SetSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin = node->m_SpineScene->m_Skeleton->defaultSkin;
    if (skin_id)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id);
        if (!index)
        {
            return false;
        } else {
            skin = node->m_SpineScene->m_Skeleton->skins[*index];
        }
    }

    spSkeleton_setSkin(node->m_SkeletonInstance, skin);
    spSkeleton_setSlotsToSetupPose(node->m_SkeletonInstance);
    return true;
}

dmhash_t GetSkin(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_SkinId;
}

dmhash_t GetAnimation(dmGui::HScene scene, dmGui::HNode hnode, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    return targetTrack ? targetTrack->m_AnimationId : 0;
}

bool SetCursor(dmGui::HScene scene, dmGui::HNode hnode, float cursor, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    if (!targetTrack || !targetTrack->m_AnimationInstance)
    {
        return false;
    }

    float unit_0_1 = fmodf(cursor + 1.0f, 1.0f);

    float duration = targetTrack->m_AnimationInstance->animationEnd - targetTrack->m_AnimationInstance->animationStart;
    float t = unit_0_1 * duration;

    targetTrack->m_AnimationInstance->trackTime = t;
    return true;
}

float GetCursor(dmGui::HScene scene, dmGui::HNode hnode, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    if (!targetTrack || !targetTrack->m_AnimationInstance)
    {
        return 0.0f;
    }
    
    spTrackEntry* entry = targetTrack->m_AnimationInstance;
    float unit = 0.0f;
    if (entry)
    {
        float duration = entry->animationEnd - entry->animationStart;
        if (duration != 0)
        {
            unit = fmodf(entry->trackTime, duration) / duration;
        }
    }
    return unit;
}

bool SetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, float playback_rate, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    if (!targetTrack || !targetTrack->m_AnimationInstance)
        return false;
    
    targetTrack->m_AnimationInstance->timeScale = playback_rate;
    return true;
}

float GetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    if (!targetTrack || !targetTrack->m_AnimationInstance)
        return 1.0f;
    
    return targetTrack->m_AnimationInstance->timeScale;
}

bool SetAttachment(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, dmhash_t attachment_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    SpineSceneResource* spine_scene = node->m_SpineScene;

    uint32_t* index = spine_scene->m_SlotNameToIndex.Get(slot_id);
    if (!index)
    {
        dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
        return false;
    }

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

    spSlot* slot = node->m_SkeletonInstance->slots[*index];

    // it's a bit weird to use strings here, but we'd rather not use too much knowledge about the internals
    return 1 == spSkeleton_setAttachment(node->m_SkeletonInstance, slot->data->name, attachment_name);
}

bool SetSlotColor(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, Vectormath::Aos::Vector4* color)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    SpineSceneResource* spine_scene = node->m_SpineScene;

    uint32_t* index = spine_scene->m_SlotNameToIndex.Get(slot_id);
    if (!index)
    {
        dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
        return false;
    }

    spSlot* slot = node->m_SkeletonInstance->slots[*index];
    spColor_setFromFloats(&slot->color, color->getX(), color->getY(), color->getZ(), color->getW());

    return true;
}

void PhysicsTranslate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* translation)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkeleton_physicsTranslate(node->m_SkeletonInstance, translation->getX(), translation->getY());
}

void PhysicsRotate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* center, float degrees)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkeleton_physicsRotate(node->m_SkeletonInstance, center->getX(), center->getY(), degrees);
}


// END SCRIPTING

static void DeleteBones(InternalGuiNode* node)
{
    uint32_t count = node->m_BonesNodes.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        dmGui::DeleteNode(node->m_GuiScene, node->m_BonesNodes[i]);
    }
    node->m_BonesNodes.SetSize(0);
    node->m_BonesIds.SetSize(0);
    node->m_BonesNames.SetSize(0);
    node->m_Bones.SetSize(0);
}

static void UpdateTransform(dmGui::HScene scene, dmGui::HNode node, const spBone* bone)
{
    float radians = spBone_getWorldRotationX((spBone*)bone);
    float sx = spBone_getWorldScaleX((spBone*)bone);
    float sy = spBone_getWorldScaleY((spBone*)bone);

    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_POSITION, dmVMath::Vector4(bone->worldX, bone->worldY, 0, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_EULER, dmVMath::Vector4(0, 0, radians, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_SCALE, dmVMath::Vector4(sx, sy, 1, 0));
}

static dmGui::HNode CreateBone(dmGui::HScene scene, dmGui::HNode gui_parent, dmGui::AdjustMode adjust_mode, const char* spine_gui_node_id, spBone* bone)
{
    dmVMath::Point3 position = dmVMath::Point3(bone->x, bone->y, 0);
    dmGui::HNode gui_bone = dmGui::NewNode(scene, position, dmVMath::Vector3(0,0,0), dmGui::NODE_TYPE_BOX, 0);
    if (!gui_bone)
        return 0;

    char id_str[256];
    dmSnPrintf(id_str, sizeof(id_str), "%s/%s", spine_gui_node_id, bone->data->name);
    dmhash_t id = dmHashString64(id_str);

    dmGui::SetNodeId(scene, gui_bone, id);
    dmGui::SetNodeAdjustMode(scene, gui_bone, adjust_mode);
    dmGui::SetNodeParent(scene, gui_bone, gui_parent, false);
    dmGui::SetNodeIsBone(scene, gui_bone, true);

    UpdateTransform(scene, gui_bone, bone);

    return gui_bone;
}

static bool CreateBones(InternalGuiNode* node, dmGui::HScene scene, dmGui::HNode gui_parent, spBone* bone)
{
    dmGui::HNode gui_bone = CreateBone(scene, gui_parent, node->m_AdjustMode, node->m_Id, bone);
    if (!gui_bone)
        return false;

    node->m_BonesNodes.Push(gui_bone);
    node->m_BonesIds.Push(dmGui::GetNodeId(scene, gui_bone));
    node->m_BonesNames.Push(dmHashString64(bone->data->name));
    node->m_Bones.Push(bone);

    int count = bone->childrenCount;
    for (int i = 0; i < count; ++i)
    {
        spBone* child_bone = bone->children[i];

        if (!CreateBones(node, scene, gui_parent, child_bone))
            return false;
    }
    return true;
}

static bool CreateBones(InternalGuiNode* node)
{
    DeleteBones(node);

    if (node->m_Id == 0)
    {
        // If we don't have an id, we have no way of getting the bones by name anyways
        // since the names are a combination of the "node_id/bone_name"
        // If we wanted to in the future, we can add an id parameter to gui.new_spine_node()
        return true;
    }

    uint32_t num_bones = (uint32_t)node->m_SkeletonInstance->bonesCount;
    if (node->m_BonesNodes.Capacity() < num_bones)
    {
        node->m_BonesNodes.SetCapacity(num_bones);
        node->m_BonesIds.SetCapacity(num_bones);
        node->m_BonesNames.SetCapacity(num_bones);
        node->m_Bones.SetCapacity(num_bones);
    }

    return CreateBones(node, node->m_GuiScene, node->m_GuiNode, node->m_SkeletonInstance->root);
}

static void UpdateBones(InternalGuiNode* node)
{
    dmGui::HScene scene = node->m_GuiScene;
    uint32_t num_bones = node->m_BonesNodes.Size();

    dmVMath::Vector4 scale = dmGui::GetNodeProperty(scene, node->m_GuiNode, dmGui::PROPERTY_SCALE);
    DM_PROPERTY_ADD_U32(rmtp_SpineBones, num_bones);
    for (uint32_t i = 0; i < num_bones; ++i)
    {
        dmGui::HNode gui_bone = node->m_BonesNodes[i];
        spBone* bone = node->m_Bones[i];
        UpdateTransform(scene, gui_bone, bone);
    }
}

static void FindGuiBones(InternalGuiNode* node, dmGui::HScene scene, dmGui::HNode hnode)
{
    // We assume the order is the same as when we created them in the original spine node
    node->m_BonesNodes.Push(hnode);

    dmGui::HNode child = dmGui::GetFirstChildNode(scene, hnode);

    while (child != dmGui::INVALID_HANDLE)
    {
        if (dmGui::GetNodeIsBone(scene, child)) // We cannot have bones as a child of another node type
        {
            FindGuiBones(node, scene, child);
        }

        child = dmGui::GetNextNode(scene, child);
    }
}

static void FindSpineBones(InternalGuiNode* node, spBone* bone)
{
    // We add them in the same order as they were created
    node->m_Bones.Push(bone);

    int count = bone->childrenCount;
    for (int i = 0; i < count; ++i)
    {
        spBone* child_bone = bone->children[i];
        FindSpineBones(node, child_bone);
    }
}

static void FindBones(InternalGuiNode* node)
{
    dmGui::HNode child = GetFirstChildNode(node->m_GuiScene, node->m_GuiNode);
    while (child != dmGui::INVALID_HANDLE)
    {
        if (dmGui::GetNodeIsBone(node->m_GuiScene, child))
        {
            FindGuiBones(node, node->m_GuiScene, child);
            // we don't break here, as we currently keep all bones as children directly under the spine node
        }
        child = dmGui::GetNextNode(node->m_GuiScene, child);
    }

    FindSpineBones(node, node->m_SkeletonInstance->root);
}

static void DestroyNode(InternalGuiNode* node)
{
    DeleteBones(node);

    // Clean up all track callbacks
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++)
    {
        ClearTrackCallback(&node->m_AnimationTracks[i]);
    }
    node->m_AnimationTracks.SetCapacity(0);

    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);

    //delete node; // don't delete it. It's already been registered with the comp_gui and we need to wait for the GuiDestroy
}

static void* GuiCreate(const dmGameSystem::CompGuiNodeContext* ctx, void* context, dmGui::HScene scene, dmGui::HNode node, uint32_t custom_type)
{
    InternalGuiNode* node_data = new InternalGuiNode();
    node_data->m_GuiScene = scene;
    node_data->m_GuiNode = node;
    node_data->m_FirstUpdate = 1;
    return node_data;
}

static void GuiDestroy(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    InternalGuiNode* node = (InternalGuiNode*)nodectx->m_NodeData;

    // Clean up all track callbacks
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++)
    {
        ClearTrackCallback(&node->m_AnimationTracks[i]);
    }

    // Track callbacks are already cleaned up above

    delete node;
}

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node, bool create_bones)
{
    node->m_SpinePath    = path;
    node->m_SpineScene   = resource;

    node->m_SkeletonInstance = spSkeleton_create(node->m_SpineScene->m_Skeleton);
    if (!node->m_SkeletonInstance)
    {
        dmLogError("%s: Failed to create skeleton instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    SetSkin(node->m_GuiScene, node->m_GuiNode, 0);

    node->m_AnimationStateInstance = spAnimationState_create(node->m_SpineScene->m_AnimationStateData);
    if (!node->m_AnimationStateInstance)
    {
        dmLogError("%s: Failed to create animation state instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    node->m_AnimationStateInstance->userData = node;
    node->m_AnimationStateInstance->listener = SpineEventListener;

    // Initialize animation tracks array
    node->m_AnimationTracks.SetCapacity(8); // Start with capacity for 8 tracks

    spSkeleton_setToSetupPose(node->m_SkeletonInstance);
    spSkeleton_updateWorldTransform(node->m_SkeletonInstance, SP_PHYSICS_NONE);

    node->m_Transform = dmVMath::Matrix4::identity();

    dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, (dmGui::HTextureSource)node->m_SpineScene->m_TextureSet);

    if (create_bones)
    {
        CreateBones(node);
    }

    return true;

}

static void* GuiClone(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    InternalGuiNode* src = (InternalGuiNode*)nodectx->m_NodeData;
    InternalGuiNode* dst = new InternalGuiNode();

    dst->m_GuiScene = nodectx->m_Scene;
    dst->m_GuiNode = nodectx->m_Node;

    // We don't get a GuiSetNodeDesc call when cloning, as we should already have the data we need in the node itself
    dst->m_Id = src->m_Id;
    dst->m_AdjustMode = src->m_AdjustMode;
    dst->m_SkinId = src->m_SkinId;

    // Setup the spine structures
    // We don't create bones, as we may be part of a gui.clone_tree, which does the entire subtree, and returns a list of nodes to the user
    // As such, we need to retrieve the child nodes at a later step
    // But, since the cloned nodes doesn't have any id's, we can't fetch them via id
    // So, we instead create specific gui node type for the bones, and let them register themselves to this cloned node
    SetupNode(src->m_SpinePath, src->m_SpineScene, dst, false);
    dst->m_FindBones = 1;

    uint32_t num_bones = src->m_BonesNodes.Size();
    dst->m_BonesNodes.SetCapacity(num_bones);
    dst->m_BonesIds.SetCapacity(num_bones);
    dst->m_BonesNames.SetCapacity(num_bones);
    dst->m_Bones.SetCapacity(num_bones);

    // Since we cannot get the id's from the gui nodes, we need to copy the data now
    dst->m_BonesIds.SetSize(num_bones);
    memcpy(dst->m_BonesIds.Begin(), src->m_BonesIds.Begin(), sizeof(dmhash_t) * num_bones);
    dst->m_BonesNames.SetSize(num_bones);
    memcpy(dst->m_BonesNames.Begin(), src->m_BonesNames.Begin(), sizeof(dmhash_t) * num_bones);


    // Copy transform
    dst->m_Transform    = src->m_Transform;

    // Copy all tracks from source
    uint32_t num_tracks = src->m_AnimationTracks.Size();
    if (num_tracks > 0)
    {
        dst->m_AnimationTracks.SetCapacity(num_tracks);
        dst->m_AnimationTracks.SetSize(num_tracks);

        for (uint32_t i = 0; i < num_tracks; i++)
        {
            const GuiSpineAnimationTrack& srcTrack = src->m_AnimationTracks[i];
            GuiSpineAnimationTrack& dstTrack = dst->m_AnimationTracks[i];

            dstTrack.m_AnimationId = srcTrack.m_AnimationId;
            dstTrack.m_Playback = srcTrack.m_Playback;
            dstTrack.m_CallbackInfo = nullptr; // Don't copy callbacks
            dstTrack.m_CallbackId = 0;
            dstTrack.m_AnimationInstance = nullptr;

            if (srcTrack.m_AnimationId && srcTrack.m_AnimationInstance)
            {
                uint32_t index = FindAnimationIndex(dst, srcTrack.m_AnimationId);
                if (index != INVALID_ANIMATION_INDEX && index < dst->m_SpineScene->m_Skeleton->animationsCount)
                {
                    spAnimation* animation = dst->m_SpineScene->m_Skeleton->animations[index];
                    if (animation)
                    {
                        int loop = IsLooping(srcTrack.m_Playback);
                        dstTrack.m_AnimationInstance = spAnimationState_setAnimation(dst->m_AnimationStateInstance, i, animation, loop);

                        // Copy the state of the animation
                        if (dstTrack.m_AnimationInstance)
                        {
                            dstTrack.m_AnimationInstance->trackTime = srcTrack.m_AnimationInstance->trackTime;
                            dstTrack.m_AnimationInstance->reverse = srcTrack.m_AnimationInstance->reverse;
                            dstTrack.m_AnimationInstance->timeScale = srcTrack.m_AnimationInstance->timeScale;
                        }
                    }
                }
            }
        }
    }


    return dst;
}

static void GuiSetNodeDesc(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx, const dmGuiDDF::NodeDesc* node_desc)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

    dmhash_t name_hash = dmHashString64(node_desc->m_SpineScene);
    SpineSceneResource* resource = (SpineSceneResource*)dmGui::GetResource(nodectx->m_Scene, name_hash, SPINE_SCENE_SUFFIX);
    if (!resource) {
        dmLogError("Failed to get resource: %s", node_desc->m_SpineScene);
        return;
    }

    node->m_Id = node_desc->m_Id;
    node->m_AdjustMode = (dmGui::AdjustMode)node_desc->m_AdjustMode;
    dmhash_t default_animation_id = dmHashString64(node_desc->m_SpineDefaultAnimation); // TODO: Q: Is the default playmode specified anywhere?
    node->m_SkinId = dmHashString64(node_desc->m_SpineSkin);

    SetupNode(name_hash, resource, node, true);

    if (node->m_SkinId) {
        SetSkin(node->m_GuiScene, node->m_GuiNode, node->m_SkinId);
    }

    if (default_animation_id) {
        PlayAnimation(node, default_animation_id, dmGui::PLAYBACK_LOOP_FORWARD, 0.0f, 0.0f, 1.0f, 1, 0);
    }
}

static void GuiGetVertices(const dmGameSystem::CustomNodeCtx* nodectx, uint32_t decl_size, dmBuffer::StreamDeclaration* decl, uint32_t struct_size, dmArray<uint8_t>& vertices)
{
    InternalGuiNode* node = (InternalGuiNode*) nodectx->m_NodeData;

    if (sizeof(dmSpine::SpineVertex) != struct_size)
    {
        dmLogOnceError("Size of SpineVertex is %u, but sizeof of gui BoxVertex is %u. Skipping GUI rendering\n", (uint32_t)sizeof(dmSpine::SpineVertex), struct_size);
        return;
    }

    GuiNodeTypeContext* type_context = (GuiNodeTypeContext*) nodectx->m_TypeContext;

    //TODO: Verify the vertex declaration
    // In theory, we can check the vertex format to see which components to output
    // We currently know it's xyz-uv-rgba
    dmArray<dmSpine::SpineVertex>* vbdata = (dmArray<dmSpine::SpineVertex>*)&vertices;

    uint32_t num_vertices = dmSpine::GenerateVertexData(*vbdata, node->m_SkeletonInstance, type_context->m_SkeletonClipper, node->m_Transform, 0);
    (void)num_vertices;
}

// IK functions for GUI spine nodes
static void ApplyIKTargets(InternalGuiNode* node)
{
    // Apply node-based targets (following GUI nodes)
    uint32_t count = node->m_IKTargets.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        const GuiIKTarget& target = node->m_IKTargets[i];
        if (target.m_Constraint && target.m_TargetNode != dmGui::INVALID_HANDLE)
        {
            // TODO: Convert target node space into IK space
            dmVMath::Vector4 target_pos = dmGui::GetNodeProperty(node->m_GuiScene, target.m_TargetNode, dmGui::PROPERTY_POSITION);
            target.m_Constraint->target->x = target_pos.getX();
            target.m_Constraint->target->y = target_pos.getY();
        }
    }

    // Apply position-based targets (fixed positions)
    count = node->m_IKTargetPositions.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        const GuiIKTarget& target = node->m_IKTargetPositions[i];
        if (target.m_Constraint)
        {
            // TODO: Convert target node space into IK space
            target.m_Constraint->target->x = target.m_Position.getX();
            target.m_Constraint->target->y = target.m_Position.getY();
        }
    }
    // Clear the position-based targets after applying them (they're one-shot)
    node->m_IKTargetPositions.SetSize(0);
}

static void GuiUpdate(const dmGameSystem::CustomNodeCtx* nodectx, float dt)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

// Temp fix begin!
    // since the comp_gui.cpp call dmGui::SetNodeTexture() with a null texture, we set it here again
    // Remove once the bug fix is in Defold 1.3.4
    if (node->m_FirstUpdate)
    {
        node->m_FirstUpdate = 0;
        dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, (dmGui::HTextureSource)node->m_SpineScene->m_TextureSet);
    }
// end temp fix

    if (node->m_FindBones)
    {
        node->m_FindBones = 0;
        FindBones(node);
    }

    if (!node->m_AnimationStateInstance)
        return;
    float anim_dt = dt;

    // Check if any track is playing
    bool anyTrackPlaying = false;
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++)
    {
        if (node->m_AnimationTracks[i].m_AnimationInstance)
        {
            anyTrackPlaying = true;
            break;
        }
    }

    if (anyTrackPlaying)
    {
        spAnimationState_update(node->m_AnimationStateInstance, anim_dt);
        spAnimationState_apply(node->m_AnimationStateInstance, node->m_SkeletonInstance);
        spSkeleton_update(node->m_SkeletonInstance, anim_dt);
        spSkeleton_updateWorldTransform(node->m_SkeletonInstance, SP_PHYSICS_UPDATE);
    }
    else
    {
        spSkeleton_updateWorldTransform(node->m_SkeletonInstance, SP_PHYSICS_NONE);
    }   
    
    // Apply IK targets
    ApplyIKTargets(node);
    
    DM_PROPERTY_ADD_U32(rmtp_SpineGuiNodes, 1);
    UpdateBones(node);
}

static dmGameObject::Result GuiNodeTypeSpineCreate(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = new GuiNodeTypeContext;

    type_context->m_SkeletonClipper = spSkeletonClipping_create();

    dmGameSystem::CompGuiNodeTypeSetContext(type, type_context);

    dmGameSystem::CompGuiNodeTypeSetCreateFn(type, GuiCreate);
    dmGameSystem::CompGuiNodeTypeSetDestroyFn(type, GuiDestroy);
    dmGameSystem::CompGuiNodeTypeSetCloneFn(type, GuiClone);
    dmGameSystem::CompGuiNodeTypeSetUpdateFn(type, GuiUpdate);
    dmGameSystem::CompGuiNodeTypeSetGetVerticesFn(type, GuiGetVertices);
    dmGameSystem::CompGuiNodeTypeSetNodeDescFn(type, GuiSetNodeDesc);

    lua_State* L = dmGameSystem::GetLuaState(ctx);
    ScriptSpineGuiRegister(L);

    return dmGameObject::RESULT_OK;
}

static dmGameObject::Result GuiNodeTypeSpineDestroy(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = (GuiNodeTypeContext*)dmGameSystem::CompGuiNodeTypeGetContext(type);
    spSkeletonClipping_dispose(type_context->m_SkeletonClipper);

    delete type_context;
    return dmGameObject::RESULT_OK;
}

bool SetIKTargetPosition(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id, Vectormath::Aos::Point3 position)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node)
        return false;

    SpineSceneResource* spine_scene = node->m_SpineScene;
    if (!spine_scene)
        return false;

    uint32_t* index = spine_scene->m_IKNameToIndex.Get(constraint_id);
    if (!index)
        return false;
    if (*index > node->m_SkeletonInstance->ikConstraintsCount)
        return false;

    if (node->m_IKTargetPositions.Full())
        node->m_IKTargetPositions.OffsetCapacity(2);

    spIkConstraint* constraint = node->m_SkeletonInstance->ikConstraints[*index];

    GuiIKTarget target;
    target.m_ConstraintHash = constraint_id;
    target.m_Constraint = constraint;
    target.m_TargetNode = dmGui::INVALID_HANDLE;
    target.m_Position = position;
    node->m_IKTargetPositions.Push(target);

    return true;
}

bool SetIKTarget(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id, dmGui::HNode target_node)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node)
        return false;

    SpineSceneResource* spine_scene = node->m_SpineScene;
    if (!spine_scene)
        return false;

    uint32_t* index = spine_scene->m_IKNameToIndex.Get(constraint_id);
    if (!index)
        return false;
    if (*index > node->m_SkeletonInstance->ikConstraintsCount)
        return false;

    if (node->m_IKTargets.Full())
        node->m_IKTargets.OffsetCapacity(2);

    spIkConstraint* constraint = node->m_SkeletonInstance->ikConstraints[*index];

    GuiIKTarget target;
    target.m_ConstraintHash = constraint_id;
    target.m_Constraint = constraint;
    target.m_TargetNode = target_node;
    target.m_Position = dmVMath::Point3(0, 0, 0);
    node->m_IKTargets.Push(target);

    return true;
}

bool ResetIKTarget(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node)
        return false;

    // Remove the constraint from position-based targets
    for (uint32_t i = 0; i < node->m_IKTargetPositions.Size(); ++i)
    {
        if (constraint_id == node->m_IKTargetPositions[i].m_ConstraintHash)
        {
            node->m_IKTargetPositions.EraseSwap(i);
            return true;
        }
    }

    // Remove the constraint from node-based targets
    for (uint32_t i = 0; i < node->m_IKTargets.Size(); ++i)
    {
        if (constraint_id == node->m_IKTargets[i].m_ConstraintHash)
        {
            node->m_IKTargets.EraseSwap(i);
            return true;
        }
    }

    return false;
}

} // namespace



DM_DECLARE_COMPGUI_NODE_TYPE(ComponentTypeGuiNodeSpineModelExt, "Spine", dmSpine::GuiNodeTypeSpineCreate, dmSpine::GuiNodeTypeSpineDestroy)
