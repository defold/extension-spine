#include "gui_node_spine.h"
#include "res_spine_scene.h"
#include "spine_skin_utils.h"
#include <common/vertices.h>

#include <dmsdk/dlib/buffer.h>
#include <dmsdk/dlib/dstrings.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/dlib/math.h>
#include <dmsdk/dlib/profile.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/gui.h>
#include <dmsdk/script/script.h>

#include <spine/Extension.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonRenderer.h>
#include <spine/Slot.h>
#include <spine/SlotData.h>
#include <spine/AnimationState.h>
#include <spine/Animation.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>
#include <spine/Bone.h>
#include <spine/BoneData.h>
#include <spine/IkConstraint.h>
#include <spine/Skin.h>
#include <spine/Event.h>
#include <spine/EventData.h>
#include <spine/Constraint.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto
#include "script_spine_gui.h"
#include "gui_spine.h"
#include "spine_gui_common.h"

DM_PROPERTY_EXTERN(rmtp_Spine);
DM_PROPERTY_EXTERN(rmtp_SpineBones);
DM_PROPERTY_U32(rmtp_SpineGuiNodes, 0, PROFILE_PROPERTY_FRAME_RESET, "", &rmtp_Spine);

namespace dmSpine
{
static const dmhash_t SPINE_DEFAULT_ANIMATION   = dmHashString64("spine_default_animation");
static const dmhash_t SPINE_SKIN                = dmHashString64("spine_skin");
static const dmhash_t SPINE_CREATE_BONES        = dmHashString64("spine_create_bones");

struct GuiNodeTypeContext
{
    spine::SkeletonRenderer* m_SkeletonRenderer;
};

struct InternalGuiNode
{
    dmhash_t            m_SpinePath;
    SpineSceneResource* m_SpineScene;
    SpineSceneData*     m_SceneData;

    spine::Skeleton*         m_SkeletonInstance;
    spine::AnimationState*   m_AnimationStateInstance;
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
    dmArray<spine::Bone*>   m_Bones;        // Matches 1:1 with m_BoneNodes

    // IK targets for GUI spine nodes
    dmArray<GuiIKTarget>    m_IKTargets;           // targets that follow GUI nodes
    dmArray<GuiIKTarget>    m_IKTargetPositions;   // targets with fixed positions

    uint8_t             m_FindBones : 1;
    uint8_t             m_FirstUpdate : 1;
    uint8_t             : 6;

    InternalGuiNode()
    : m_SpinePath(0)
    , m_SpineScene(0)
    , m_SceneData(0)
    , m_SkeletonInstance(0)
    , m_AnimationStateInstance(0)
    , m_SkinId(0)
    , m_Id(0)
    , m_FindBones(0)
    , m_FirstUpdate(1)
    {}
};

// State owned by the integration layer that can be rebound to a new Spine
// resource generation. Runtime pointers are deliberately not retained here.
struct ReloadAnimationTrack
{
    dmhash_t                       m_AnimationId;
    dmGui::Playback                m_Playback;
    dmScript::LuaCallbackInfo*     m_CallbackInfo;
    uint32_t                       m_CallbackId;
    float                          m_Delay;
    float                          m_TrackTime;
    float                          m_TrackEnd;
    float                          m_AnimationLast;
    float                          m_TimeScale;
    float                          m_Alpha;
    float                          m_EventThreshold;
    float                          m_MixAttachmentThreshold;
    float                          m_AlphaAttachmentThreshold;
    float                          m_MixDrawOrderThreshold;
    float                          m_MixTime;
    float                          m_MixDuration;
    bool                           m_Active;
    bool                           m_Loop;
    bool                           m_Additive;
    bool                           m_Reverse;
    bool                           m_ShortestRotation;
};

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node, bool create_bones);
static void DestroyNode(InternalGuiNode* node);
static spine::IkConstraint* GetIKConstraint(InternalGuiNode* node, dmhash_t constraint_id);

static bool GetCustomHashProperty(dmGui::HScene scene, dmGui::HNode node, dmhash_t property_id, dmhash_t* value)
{
    dmGui::CustomProperty property;
    dmGui::Result result = dmGui::GetNodeCustomProperty(scene, node, property_id, &property);
    if (result == dmGui::RESULT_RESOURCE_NOT_FOUND)
    {
        *value = 0;
        return true;
    }
    if (result != dmGui::RESULT_OK)
    {
        dmLogError("Failed to get GUI custom property '%s'", dmHashReverseSafe64(property_id));
        return false;
    }

    switch (property.m_Type)
    {
    case dmGui::CUSTOM_PROPERTY_TYPE_HASH:
        *value = property.m_Hash;
        return true;
    case dmGui::CUSTOM_PROPERTY_TYPE_STRING:
        *value = dmHashString64(property.m_String ? property.m_String : "");
        return true;
    default:
        dmLogError("GUI custom property '%s' has unsupported type %d", dmHashReverseSafe64(property_id), property.m_Type);
        return false;
    }
}

static bool GetCustomBoolProperty(dmGui::HScene scene, dmGui::HNode node, dmhash_t property_id, bool* value)
{
    dmGui::CustomProperty property;
    dmGui::Result result = dmGui::GetNodeCustomProperty(scene, node, property_id, &property);
    if (result == dmGui::RESULT_RESOURCE_NOT_FOUND)
    {
        *value = false;
        return true;
    }
    if (result != dmGui::RESULT_OK)
    {
        dmLogError("Failed to get GUI custom property '%s'", dmHashReverseSafe64(property_id));
        return false;
    }
    if (property.m_Type != dmGui::CUSTOM_PROPERTY_TYPE_BOOLEAN)
    {
        dmLogError("GUI custom property '%s' has unsupported type %d", dmHashReverseSafe64(property_id), property.m_Type);
        return false;
    }

    *value = property.m_Boolean;
    return true;
}

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

static void SendAnimationDone(InternalGuiNode* node, spine::AnimationState* state, spine::TrackEntry* entry, spine::Event* event)
{
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->getTrackIndex());
    if (!track)
        return;

    dmGameSystemDDF::SpineAnimationDone message;
    message.m_AnimationId = dmHashString64(entry->getAnimation().getName().buffer());
    message.m_Playback    = track->m_Playback;
    message.m_Track       = entry->getTrackIndex() + 1; // Convert to 1-based indexing for API

    // Send to track-specific callback if available
    if (track->m_CallbackInfo && dmScript::IsCallbackValid(track->m_CallbackInfo))
    {
        // Take a local copy of the callback pointer to avoid using a potentially
        // cleared/replaced pointer if the Lua callback calls gui.play_spine_anim()
        // (which may clear the track callback immediately).
        // Fix https://github.com/defold/extension-spine/issues/240
        dmScript::LuaCallbackInfo* cbk = track->m_CallbackInfo;
        // Store callback ID to check if callback is still valid after execution
        uint32_t callbackId = track->m_CallbackId;

        lua_State* L = dmScript::GetCallbackLuaContext(cbk);
        DM_LUA_STACK_CHECK(L, 0);

        if (dmScript::SetupCallback(cbk))
        {
            dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
            dmScript::PushHash(L, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor->m_NameHash);
            dmScript::PushDDF(L, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message, true);

            dmScript::PCall(L, 4, 0); // instance + 3
            dmScript::TeardownCallback(cbk);
        }

        // Only clear callback if it hasn't been replaced during callback execution
        // (user might have called gui.play_spine_anim from within the callback)
        if (callbackId == track->m_CallbackId)
        {
            ClearTrackCallback(track);
        }
    }
}

static void SendSpineEvent(InternalGuiNode* node, spine::AnimationState* state, spine::TrackEntry* entry, spine::Event* event)
{
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->getTrackIndex());
    if (!track)
        return;

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
    message.m_Track       = entry->getTrackIndex() + 1; // Convert to 1-based indexing for API

    // Send to track-specific callback if available
    if (track->m_CallbackInfo && dmScript::IsCallbackValid(track->m_CallbackInfo))
    {
        // Take a local copy of the callback pointer in case it's cleared/replaced
        // during the callback execution by gui.play_spine_anim().
        // Fix https://github.com/defold/extension-spine/issues/240
        dmScript::LuaCallbackInfo* cbk = track->m_CallbackInfo;
        lua_State* L = dmScript::GetCallbackLuaContext(cbk);
        DM_LUA_STACK_CHECK(L, 0);

        if (dmScript::SetupCallback(cbk))
        {
            dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
            dmScript::PushHash(L, dmGameSystemDDF::SpineEvent::m_DDFDescriptor->m_NameHash);
            dmScript::PushDDF(L, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message, true);

            dmScript::PCall(L, 4, 0); // instance + 3
            dmScript::TeardownCallback(cbk);
        }
        
        // Note: For spine events, we don't clear the callback since events can occur multiple times
        // during an animation. The callback will be cleared when the animation completes or is cancelled.
    }
}

static void SpineEventListener(spine::AnimationState* state, spine::EventType type, spine::TrackEntry* entry, spine::Event* event, void* user_data)
{
    InternalGuiNode* node = (InternalGuiNode*)user_data;
    GuiSpineAnimationTrack* track = GetTrackFromIndex(node, entry->getTrackIndex());

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
    case spine::EventType_Complete:
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
                entry->setReverse(!entry->getReverse());
            }
            
        }
        break;
    case spine::EventType_Dispose:
        {
            if (track && track->m_AnimationInstance == entry)
            {
                ClearTrackCallback(track);
                track->m_AnimationInstance = nullptr;
                track->m_AnimationId = 0;
            }
        }
        break;
    case spine::EventType_Event:
        SendSpineEvent(node, state, entry, event);
        break;
    default:
        break;
    }
}

static const uint32_t INVALID_ANIMATION_INDEX = 0xFFFFFFFF;

static inline uint32_t FindAnimationIndex(InternalGuiNode* node, dmhash_t animation)
{
    SpineSceneData* scene_data = node->m_SceneData;
    uint32_t* index = scene_data->m_AnimationNameToIndex.Get(animation);
    return index ? *index : INVALID_ANIMATION_INDEX;
}

static bool PlayAnimation(InternalGuiNode* node, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, int32_t track, dmScript::LuaCallbackInfo* callback)
{
    SpineSceneData* scene_data = node->m_SceneData;
    uint32_t index = FindAnimationIndex(node, animation_id);
    if (index == INVALID_ANIMATION_INDEX)
    {
        dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
        return false;
    }
    spine::Array<spine::Animation*>& animations = scene_data->m_Skeleton->getAnimations();
    if (index >= animations.size())
    {
        dmLogError("Animation index %u is too large. Number of animations are %u", index, (uint32_t)animations.size());
        return false;
    }

    int trackIndex = track - 1; // Convert from 1-based to 0-based indexing
    int loop = IsLooping(playback);

    spine::Animation* animation = animations[index];

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
    targetTrack.m_AnimationInstance = &node->m_AnimationStateInstance->setAnimation(trackIndex, *animation, loop != 0);
    targetTrack.m_Playback = playback;
    targetTrack.m_CallbackInfo = callback;
    targetTrack.m_CallbackId++;

    // Configure animation properties
    targetTrack.m_AnimationInstance->setTimeScale(playback_rate);
    targetTrack.m_AnimationInstance->setReverse(IsReverse(playback));
    targetTrack.m_AnimationInstance->setMixDuration(blend_duration);
    targetTrack.m_AnimationInstance->setTrackTime(dmMath::Clamp(offset,
        targetTrack.m_AnimationInstance->getAnimationStart(),
        targetTrack.m_AnimationInstance->getAnimationEnd()));


    return true;
}

// SCRIPTING

static void FindBones(InternalGuiNode* node);

bool SetScene(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_scene)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);

    SpineSceneResource* resource = (SpineSceneResource*)dmSpine::GetResource(scene, spine_scene, dmSpine::SPINE_SCENE_SUFFIX);
    if (!resource)
        return false;

    // The wrapper remains stable across a resource recreate, but its generation changes.
    if (spine_scene == node->m_SpinePath && resource == node->m_SpineScene)
        return ReloadSceneResource(scene, hnode, resource);

    if (node->m_FindBones)
    {
        node->m_FindBones = 0;
        FindBones(node);
    }

    DestroyNode(node);
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

    node->m_AnimationStateInstance->clearTrack(track->m_AnimationInstance->getTrackIndex());

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
    spine::Skin* skin_a = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Skin* skin_b = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Array<spine::Skin*>& skins = node->m_SceneData->m_Skeleton->getSkins();

    if (skin_id_a)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id_a);
        if (!index)
        {
            dmLogError("No skin '%s' found", dmHashReverseSafe64(skin_id_a));
            return false;
        }

        skin_a = skins[*index];
    }

    if (skin_id_b)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id_b);
        if (!index)
        {
            dmLogError("No skin '%s' found", dmHashReverseSafe64(skin_id_b));
            return false;
        }

        skin_b = skins[*index];
    }

    if (!skin_a || !skin_b)
        return false;

    skin_a->addSkin(*skin_b);
    if (node->m_SkeletonInstance->getSkin() == skin_a)
        node->m_SkeletonInstance->updateCache();
    return true;
}

bool CopySkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id_a, dmhash_t skin_id_b){
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spine::Skin* skin_a = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Skin* skin_b = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Array<spine::Skin*>& skins = node->m_SceneData->m_Skeleton->getSkins();

    if (skin_id_a)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id_a);
        if (!index)
        {
            return false;
        }

        skin_a = skins[*index];
    }
    if (skin_id_b)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id_b);
        if (!index)
        {
            return false;
        }

        skin_b = skins[*index];
    }

    if (!skin_a || !skin_b)
        return false;

    skin_a->copySkin(*skin_b);
    if (node->m_SkeletonInstance->getSkin() == skin_a)
        node->m_SkeletonInstance->updateCache();
    return true;
}

bool ClearSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id){
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spine::Skin* skin = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Array<spine::Skin*>& skins = node->m_SceneData->m_Skeleton->getSkins();

    if (skin_id)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id);
        if (!index)
        {
            return false;
        }

        skin = skins[*index];
    }

    if (!skin)
        return false;

    ClearSkinAttachments(node->m_SceneData, node->m_SkeletonInstance, skin);
    return true;
}

static bool SetSkin(InternalGuiNode* node, dmhash_t skin_id)
{
    spine::Skin* skin = node->m_SceneData->m_Skeleton->getDefaultSkin();
    spine::Array<spine::Skin*>& skins = node->m_SceneData->m_Skeleton->getSkins();
    if (skin_id)
    {
        uint32_t* index = node->m_SceneData->m_SkinNameToIndex.Get(skin_id);
        if (!index)
        {
            return false;
        } else {
            skin = skins[*index];
        }
    }

    node->m_SkeletonInstance->setSkin(skin);
    node->m_SkeletonInstance->setupPoseSlots();
    node->m_SkinId = skin_id;
    return true;
}

bool SetSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return SetSkin(node, skin_id);
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

    float duration = targetTrack->m_AnimationInstance->getAnimationEnd() - targetTrack->m_AnimationInstance->getAnimationStart();
    float t = unit_0_1 * duration;

    targetTrack->m_AnimationInstance->setTrackTime(t);
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
    
    spine::TrackEntry* entry = targetTrack->m_AnimationInstance;
    float unit = 0.0f;
    if (entry)
    {
        float duration = entry->getAnimationEnd() - entry->getAnimationStart();
        if (duration != 0)
        {
            unit = fmodf(entry->getTrackTime(), duration) / duration;
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
    
    targetTrack->m_AnimationInstance->setTimeScale(playback_rate);
    return true;
}

float GetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, int32_t track)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    
    int trackIndex = track - 1;
    GuiSpineAnimationTrack* targetTrack = GetTrackFromIndex(node, trackIndex);
    if (!targetTrack || !targetTrack->m_AnimationInstance)
        return 1.0f;
    
    return targetTrack->m_AnimationInstance->getTimeScale();
}

bool SetAttachment(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, dmhash_t attachment_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    SpineSceneData* scene_data = node->m_SceneData;

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

    spine::Array<spine::Slot*>& slots = node->m_SkeletonInstance->getSlots();
    if (*index >= slots.size())
        return false;

    spine::Slot* slot = slots[*index];
    spine::Attachment* attachment = 0;
    if (attachment_name)
    {
        attachment = node->m_SkeletonInstance->getAttachment((int)*index, attachment_name);
        if (!attachment)
            return false;
    }
    slot->getPose().setAttachment(attachment);
    return true;
}

bool SetSlotColor(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, Vectormath::Aos::Vector4* color)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    SpineSceneData* scene_data = node->m_SceneData;

    uint32_t* index = scene_data->m_SlotNameToIndex.Get(slot_id);
    if (!index)
    {
        dmLogError("No slot named '%s'", dmHashReverseSafe64(slot_id));
        return false;
    }

    spine::Array<spine::Slot*>& slots = node->m_SkeletonInstance->getSlots();
    if (*index >= slots.size())
        return false;

    spine::Slot* slot = slots[*index];
    slot->getPose().getColor().set(color->getX(), color->getY(), color->getZ(), color->getW());

    return true;
}

void PhysicsTranslate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* translation)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    node->m_SkeletonInstance->physicsTranslate(translation->getX(), translation->getY());
}

void PhysicsRotate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* center, float degrees)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    node->m_SkeletonInstance->physicsRotate(center->getX(), center->getY(), degrees);
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

static void UpdateTransform(dmGui::HScene scene, dmGui::HNode node, spine::Bone* bone)
{
    spine::BonePose& pose = bone->getAppliedPose();
    float rotation = pose.getWorldRotationX();
    float sx = pose.getWorldScaleX();
    float sy = pose.getWorldScaleY();

    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_POSITION, dmVMath::Vector4(pose.getWorldX(), pose.getWorldY(), 0, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_EULER, dmVMath::Vector4(0, 0, rotation, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_SCALE, dmVMath::Vector4(sx, sy, 1, 0));
}

static dmGui::HNode CreateBone(dmGui::HScene scene, dmGui::HNode gui_parent, dmGui::AdjustMode adjust_mode, const char* spine_gui_node_id, spine::Bone* bone)
{
    spine::BonePose& pose = bone->getPose();
    dmVMath::Point3 position = dmVMath::Point3(pose.getX(), pose.getY(), 0);
    dmGui::HNode gui_bone = dmGui::NewNode(scene, position, dmVMath::Vector3(0,0,0), dmGui::NODE_TYPE_BOX, 0);
    if (!gui_bone)
        return 0;

    char id_str[256];
    dmSnPrintf(id_str, sizeof(id_str), "%s/%s", spine_gui_node_id, bone->getData().getName().buffer());
    dmhash_t id = dmHashString64(id_str);

    dmGui::SetNodeId(scene, gui_bone, id);
    dmGui::SetNodeAdjustMode(scene, gui_bone, adjust_mode);
    dmGui::SetNodeParent(scene, gui_bone, gui_parent, false);
    dmGui::SetNodeIsBone(scene, gui_bone, true);

    UpdateTransform(scene, gui_bone, bone);

    return gui_bone;
}

static bool CreateBones(InternalGuiNode* node, dmGui::HScene scene, dmGui::HNode gui_parent, spine::Bone* bone)
{
    dmGui::HNode gui_bone = CreateBone(scene, gui_parent, node->m_AdjustMode, node->m_Id, bone);
    if (!gui_bone)
        return false;

    node->m_BonesNodes.Push(gui_bone);
    node->m_BonesIds.Push(dmGui::GetNodeId(scene, gui_bone));
    node->m_BonesNames.Push(dmHashString64(bone->getData().getName().buffer()));
    node->m_Bones.Push(bone);

    spine::Array<spine::Bone*>& children = bone->getChildren();
    for (size_t i = 0; i < children.size(); ++i)
    {
        spine::Bone* child_bone = children[i];

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

    uint32_t num_bones = (uint32_t)node->m_SkeletonInstance->getBones().size();
    if (node->m_BonesNodes.Capacity() < num_bones)
    {
        node->m_BonesNodes.SetCapacity(num_bones);
        node->m_BonesIds.SetCapacity(num_bones);
        node->m_BonesNames.SetCapacity(num_bones);
        node->m_Bones.SetCapacity(num_bones);
    }

    return CreateBones(node, node->m_GuiScene, node->m_GuiNode, node->m_SkeletonInstance->getRootBone());
}

static void UpdateBones(InternalGuiNode* node)
{
    dmGui::HScene scene = node->m_GuiScene;
    uint32_t num_bones = node->m_BonesNodes.Size();

    DM_PROPERTY_ADD_U32(rmtp_SpineBones, num_bones);
    for (uint32_t i = 0; i < num_bones; ++i)
    {
        dmGui::HNode gui_bone = node->m_BonesNodes[i];
        spine::Bone* bone = node->m_Bones[i];
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

static void FindSpineBones(InternalGuiNode* node, spine::Bone* bone)
{
    // We add them in the same order as they were created
    node->m_Bones.Push(bone);

    spine::Array<spine::Bone*>& children = bone->getChildren();
    for (size_t i = 0; i < children.size(); ++i)
    {
        spine::Bone* child_bone = children[i];
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

    FindSpineBones(node, node->m_SkeletonInstance->getRootBone());
}

static void DestroyNode(InternalGuiNode* node)
{
    SpineSceneData* scene_data = node->m_SceneData;

    DeleteBones(node);

    // Clean up all track callbacks
    for (int32_t i = 0; i < node->m_AnimationTracks.Size(); i++)
    {
        ClearTrackCallback(&node->m_AnimationTracks[i]);
    }
    node->m_AnimationTracks.SetCapacity(0);

    node->m_IKTargets.SetSize(0);
    node->m_IKTargetPositions.SetSize(0);

    if (node->m_AnimationStateInstance)
    {
        delete node->m_AnimationStateInstance;
        node->m_AnimationStateInstance = 0;
    }
    if (node->m_SkeletonInstance)
    {
        delete node->m_SkeletonInstance;
        node->m_SkeletonInstance = 0;
    }

    node->m_SceneData = 0;
    node->m_SpineScene = 0;
    ReleaseSceneData(scene_data);

    //delete node; // don't delete it. It's already been registered with the comp_gui and we need to wait for the GuiDestroy
}

static void* GuiCreate(const dmGameSystem::CompGuiNodeContext* ctx, void* context, dmGui::HScene scene, dmGui::HNode node, uint32_t custom_type)
{
    InternalGuiNode* node_data = new InternalGuiNode();
    node_data->m_GuiScene = scene;
    node_data->m_GuiNode = node;
    node_data->m_FirstUpdate = 1;
    dmSpine::GuiSpineSceneRetain(scene);
    dmSpine::GuiSpineRegisterNode(scene, node);
    return node_data;
}

static void GuiDestroy(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    InternalGuiNode* node = (InternalGuiNode*)nodectx->m_NodeData;
    DestroyNode(node);
    delete node;
    dmSpine::GuiSpineSceneRelease(nodectx->m_Scene);
    dmSpine::GuiSpineUnregisterNode(nodectx->m_Scene, nodectx->m_Node);
}

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node, bool create_bones)
{
    SpineSceneData* scene_data = RetainSceneData(resource);
    if (!scene_data)
    {
        dmLogError("%s: Spine scene has no data", __FUNCTION__);
        return false;
    }

    node->m_SpinePath    = path;
    node->m_SpineScene   = resource;
    node->m_SceneData    = scene_data;

    node->m_SkeletonInstance = new spine::Skeleton(*node->m_SceneData->m_Skeleton);
    if (!node->m_SkeletonInstance)
    {
        dmLogError("%s: Failed to create skeleton instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    SetSkin(node, 0);

    node->m_AnimationStateInstance = new spine::AnimationState(*node->m_SceneData->m_AnimationStateData);
    if (!node->m_AnimationStateInstance)
    {
        dmLogError("%s: Failed to create animation state instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    node->m_AnimationStateInstance->setListener(SpineEventListener, node);

    // Initialize animation tracks array
    node->m_AnimationTracks.SetCapacity(8); // Start with capacity for 8 tracks

    node->m_SkeletonInstance->setupPose();
    node->m_SkeletonInstance->updateWorldTransform(spine::Physics_None);

    node->m_Transform = dmVMath::Matrix4::identity();

    dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, (dmGui::HTextureSource)node->m_SceneData->m_TextureSet);

    if (create_bones)
    {
        CreateBones(node);
    }

    return true;

}

static void CaptureReloadAnimationTracks(InternalGuiNode* node, dmArray<ReloadAnimationTrack>& tracks)
{
    uint32_t track_count = node->m_AnimationTracks.Size();
    if (track_count > tracks.Capacity())
        tracks.SetCapacity(track_count);

    for (uint32_t i = 0; i < track_count; ++i)
    {
        GuiSpineAnimationTrack& source = node->m_AnimationTracks[i];
        ReloadAnimationTrack saved = {};
        saved.m_AnimationId = source.m_AnimationId;
        saved.m_Playback = source.m_Playback;
        saved.m_CallbackInfo = source.m_CallbackInfo;
        saved.m_CallbackId = source.m_CallbackId;

        // Move callback ownership out of the live track before DestroyNode. The
        // callback is either moved back to a rebuilt track or destroyed once.
        source.m_CallbackInfo = 0;

        spine::TrackEntry* entry = source.m_AnimationInstance;
        saved.m_Active = entry != 0 && source.m_AnimationId != 0;
        if (saved.m_Active)
        {
            saved.m_Loop = entry->getLoop();
            saved.m_Additive = entry->getAdditive();
            saved.m_Reverse = entry->getReverse();
            saved.m_ShortestRotation = entry->getShortestRotation();
            saved.m_Delay = entry->getDelay();
            saved.m_TrackTime = entry->getTrackTime();
            saved.m_TrackEnd = entry->getTrackEnd();
            saved.m_AnimationLast = entry->getAnimationLast();
            saved.m_TimeScale = entry->getTimeScale();
            saved.m_Alpha = entry->getAlpha();
            saved.m_EventThreshold = entry->getEventThreshold();
            saved.m_MixAttachmentThreshold = entry->getMixAttachmentThreshold();
            saved.m_AlphaAttachmentThreshold = entry->getAlphaAttachmentThreshold();
            saved.m_MixDrawOrderThreshold = entry->getMixDrawOrderThreshold();
            saved.m_MixTime = entry->getMixTime();
            saved.m_MixDuration = entry->getMixDuration();
        }
        tracks.Push(saved);
    }
}

static void DestroyReloadCallbacks(dmArray<ReloadAnimationTrack>& tracks)
{
    for (uint32_t i = 0; i < tracks.Size(); ++i)
    {
        if (tracks[i].m_CallbackInfo)
        {
            dmScript::DestroyCallback(tracks[i].m_CallbackInfo);
            tracks[i].m_CallbackInfo = 0;
        }
    }
}

static void RestoreReloadAnimationTracks(InternalGuiNode* node, dmArray<ReloadAnimationTrack>& tracks)
{
    uint32_t track_count = tracks.Size();
    if (track_count > node->m_AnimationTracks.Capacity())
        node->m_AnimationTracks.SetCapacity(track_count);

    while (node->m_AnimationTracks.Size() < track_count)
    {
        GuiSpineAnimationTrack track = {};
        track.m_Playback = dmGui::PLAYBACK_LOOP_FORWARD;
        node->m_AnimationTracks.Push(track);
    }

    spine::Array<spine::Animation*>& animations = node->m_SceneData->m_Skeleton->getAnimations();
    for (uint32_t i = 0; i < track_count; ++i)
    {
        ReloadAnimationTrack& saved = tracks[i];
        if (!saved.m_Active)
            continue;

        uint32_t animation_index = FindAnimationIndex(node, saved.m_AnimationId);
        if (animation_index == INVALID_ANIMATION_INDEX || animation_index >= animations.size() || !animations[animation_index])
        {
            dmLogWarning("Spine animation '%s' on track %u no longer exists after scene reload",
                         dmHashReverseSafe64(saved.m_AnimationId), i + 1);
            continue;
        }

        spine::TrackEntry& entry = node->m_AnimationStateInstance->setAnimation((int)i, *animations[animation_index], saved.m_Loop);
        entry.setAdditive(saved.m_Additive);
        entry.setReverse(saved.m_Reverse);
        entry.setShortestRotation(saved.m_ShortestRotation);
        entry.setDelay(saved.m_Delay);
        entry.setTrackEnd(saved.m_TrackEnd);
        entry.setTimeScale(saved.m_TimeScale);
        entry.setAlpha(saved.m_Alpha);
        entry.setEventThreshold(saved.m_EventThreshold);
        entry.setMixAttachmentThreshold(saved.m_MixAttachmentThreshold);
        entry.setAlphaAttachmentThreshold(saved.m_AlphaAttachmentThreshold);
        entry.setMixDrawOrderThreshold(saved.m_MixDrawOrderThreshold);
        entry.setMixDuration(saved.m_MixDuration);
        entry.setMixTime(saved.m_MixTime);
        entry.setTrackTime(saved.m_TrackTime);
        entry.setAnimationLast(saved.m_AnimationLast);

        GuiSpineAnimationTrack& target = node->m_AnimationTracks[i];
        target.m_AnimationInstance = &entry;
        target.m_AnimationId = saved.m_AnimationId;
        target.m_Playback = saved.m_Playback;
        target.m_CallbackInfo = saved.m_CallbackInfo;
        target.m_CallbackId = saved.m_CallbackId;
        saved.m_CallbackInfo = 0;
    }

    // Destroys callbacks for tracks that could not be restored (for example,
    // because their animation was removed from the reloaded scene).
    DestroyReloadCallbacks(tracks);
}

static void CaptureReloadIKTargets(const dmArray<GuiIKTarget>& source, dmArray<GuiIKTarget>& targets)
{
    uint32_t target_count = source.Size();
    if (target_count > targets.Capacity())
        targets.SetCapacity(target_count);
    for (uint32_t i = 0; i < target_count; ++i)
        targets.Push(source[i]);
}

static void RestoreReloadIKTargets(InternalGuiNode* node, const dmArray<GuiIKTarget>& targets, dmArray<GuiIKTarget>& destination)
{
    uint32_t target_count = targets.Size();
    if (target_count > destination.Capacity())
        destination.SetCapacity(target_count);

    for (uint32_t i = 0; i < target_count; ++i)
    {
        GuiIKTarget target = targets[i];
        target.m_Constraint = GetIKConstraint(node, target.m_ConstraintHash);
        if (!target.m_Constraint)
        {
            dmLogWarning("Spine IK constraint '%s' no longer exists after scene reload",
                         dmHashReverseSafe64(target.m_ConstraintHash));
            continue;
        }
        destination.Push(target);
    }
}

bool ReloadSceneResource(dmGui::HScene scene, dmGui::HNode hnode, SpineSceneResource* resource)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node || !resource || node->m_SpineScene != resource)
        return false;

    if (node->m_SceneData == resource->m_Data)
        return true;

    const dmhash_t path = node->m_SpinePath;
    const dmhash_t skin_id = node->m_SkinId;
    bool create_bones = node->m_FindBones || node->m_BonesNodes.Size() > 0;
    float animation_state_time_scale = node->m_AnimationStateInstance ? node->m_AnimationStateInstance->getTimeScale() : 1.0f;

    dmArray<ReloadAnimationTrack> animation_tracks;
    dmArray<GuiIKTarget> ik_targets;
    dmArray<GuiIKTarget> ik_target_positions;
    CaptureReloadAnimationTracks(node, animation_tracks);
    CaptureReloadIKTargets(node->m_IKTargets, ik_targets);
    CaptureReloadIKTargets(node->m_IKTargetPositions, ik_target_positions);

    // Cloned bone nodes are discovered lazily. Find them before teardown so
    // DestroyNode can remove the old hierarchy before creating replacements.
    if (node->m_FindBones)
    {
        node->m_FindBones = 0;
        FindBones(node);
    }

    DestroyNode(node);
    if (!SetupNode(path, resource, node, create_bones))
    {
        DestroyReloadCallbacks(animation_tracks);
        return false;
    }

    if (skin_id && !SetSkin(node, skin_id))
    {
        dmLogWarning("Spine skin '%s' no longer exists after scene reload", dmHashReverseSafe64(skin_id));
    }

    node->m_AnimationStateInstance->setTimeScale(animation_state_time_scale);
    RestoreReloadAnimationTracks(node, animation_tracks);
    RestoreReloadIKTargets(node, ik_targets, node->m_IKTargets);
    RestoreReloadIKTargets(node, ik_target_positions, node->m_IKTargetPositions);
    return true;
}

static void* GuiClone(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    InternalGuiNode* src = (InternalGuiNode*)nodectx->m_NodeData;
    InternalGuiNode* dst = new InternalGuiNode();

    dst->m_GuiScene = nodectx->m_Scene;
    dst->m_GuiNode = nodectx->m_Node;
    dmSpine::GuiSpineSceneRetain(nodectx->m_Scene);
    dmSpine::GuiSpineRegisterNode(nodectx->m_Scene, nodectx->m_Node);

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
    if (src->m_SkinId)
        SetSkin(dst, src->m_SkinId);
    // Only attempt to find bones on the cloned node if the source node had bones.
    // Avoids unnecessary scanning and array growth when the original had no bones created.
    dst->m_FindBones = src->m_BonesNodes.Size() > 0 ? 1 : 0;

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
                spine::Array<spine::Animation*>& animations = dst->m_SceneData->m_Skeleton->getAnimations();
                if (index != INVALID_ANIMATION_INDEX && index < animations.size())
                {
                    spine::Animation* animation = animations[index];
                    if (animation)
                    {
                        int loop = IsLooping(srcTrack.m_Playback);
                        dstTrack.m_AnimationInstance = &dst->m_AnimationStateInstance->setAnimation(i, *animation, loop != 0);

                        // Copy the state of the animation
                        if (dstTrack.m_AnimationInstance)
                        {
                            dstTrack.m_AnimationInstance->setTrackTime(srcTrack.m_AnimationInstance->getTrackTime());
                            dstTrack.m_AnimationInstance->setReverse(srcTrack.m_AnimationInstance->getReverse());
                            dstTrack.m_AnimationInstance->setTimeScale(srcTrack.m_AnimationInstance->getTimeScale());
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

    dmhash_t name_hash = 0;
    dmhash_t default_animation_id = 0;
    dmhash_t skin_id = 0;
    bool create_bones = false;
    if (!GetCustomHashProperty(nodectx->m_Scene, nodectx->m_Node, SPINE_SCENE, &name_hash) ||
        !GetCustomHashProperty(nodectx->m_Scene, nodectx->m_Node, SPINE_DEFAULT_ANIMATION, &default_animation_id) ||
        !GetCustomHashProperty(nodectx->m_Scene, nodectx->m_Node, SPINE_SKIN, &skin_id) ||
        !GetCustomBoolProperty(nodectx->m_Scene, nodectx->m_Node, SPINE_CREATE_BONES, &create_bones))
    {
        return;
    }

    SpineSceneResource* resource = (SpineSceneResource*)dmSpine::GetResource(nodectx->m_Scene, name_hash, dmSpine::SPINE_SCENE_SUFFIX);
    if (!resource) {
        dmLogError("Failed to get resource: %s", dmHashReverseSafe64(name_hash));
        return;
    }

    node->m_Id = node_desc->m_Id;
    node->m_AdjustMode = (dmGui::AdjustMode)node_desc->m_AdjustMode;

    SetupNode(name_hash, resource, node, create_bones);

    if (skin_id) {
        SetSkin(node, skin_id);
    }

    if (default_animation_id) {
        PlayAnimation(node, default_animation_id, dmGui::PLAYBACK_LOOP_FORWARD, 0.0f, 0.0f, 1.0f, 1, 0);
    }
}

static void GuiGetVertices(const dmGameSystem::CustomNodeCtx* nodectx, uint32_t decl_size, dmBuffer::StreamDeclaration* decl, uint32_t struct_size, dmArray<uint8_t>& vertices)
{
    InternalGuiNode* node = (InternalGuiNode*) nodectx->m_NodeData;

    if (node->m_SpineScene && node->m_SceneData != node->m_SpineScene->m_Data &&
        !ReloadSceneResource(nodectx->m_Scene, nodectx->m_Node, node->m_SpineScene))
    {
        return;
    }

    if (!node->m_SkeletonInstance)
        return;

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

    uint32_t num_vertices = dmSpine::GenerateVertexData(*vbdata, node->m_SkeletonInstance, type_context->m_SkeletonRenderer, node->m_Transform, dmVMath::Vector4(1.0f), 0);
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
            target.m_Constraint->getTarget().getPose().setPosition(target_pos.getX(), target_pos.getY());
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
            target.m_Constraint->getTarget().getPose().setPosition(target.m_Position.getX(), target.m_Position.getY());
        }
    }
}

static void GuiUpdate(const dmGameSystem::CustomNodeCtx* nodectx, float dt)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

    if (node->m_SpineScene && node->m_SceneData != node->m_SpineScene->m_Data &&
        !ReloadSceneResource(nodectx->m_Scene, nodectx->m_Node, node->m_SpineScene))
    {
        return;
    }

    if (!node->m_SkeletonInstance)
        return;

// Temp fix begin!
    // since the comp_gui.cpp call dmGui::SetNodeTexture() with a null texture, we set it here again
    // Remove once the bug fix is in Defold 1.3.4
    if (node->m_FirstUpdate)
    {
        node->m_FirstUpdate = 0;
        dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, (dmGui::HTextureSource)node->m_SceneData->m_TextureSet);
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
        node->m_AnimationStateInstance->update(anim_dt);
        node->m_AnimationStateInstance->apply(*node->m_SkeletonInstance);
    }

    // Apply custom IK target poses after animation has written its pose, but
    // before constraints consume those targets during world-transform update.
    ApplyIKTargets(node);

    if (anyTrackPlaying)
    {
        node->m_SkeletonInstance->update(anim_dt);
        node->m_SkeletonInstance->updateWorldTransform(spine::Physics_Update);
    }
    else
    {
        node->m_SkeletonInstance->updateWorldTransform(spine::Physics_None);
    }
    
    DM_PROPERTY_ADD_U32(rmtp_SpineGuiNodes, 1);
    UpdateBones(node);
}

static dmGameObject::Result GuiNodeTypeSpineCreate(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = new GuiNodeTypeContext;

    type_context->m_SkeletonRenderer = new spine::SkeletonRenderer();

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
    delete type_context->m_SkeletonRenderer;

    delete type_context;
    return dmGameObject::RESULT_OK;
}

static spine::IkConstraint* GetIKConstraint(InternalGuiNode* node, dmhash_t constraint_id)
{
    if (!node || !node->m_SceneData || !node->m_SkeletonInstance)
        return 0;

    uint32_t* index = node->m_SceneData->m_IKNameToIndex.Get(constraint_id);
    if (!index)
        return 0;

    spine::Array<spine::Constraint*>& constraints = node->m_SkeletonInstance->getConstraints();
    if (*index >= constraints.size())
        return 0;

    spine::Constraint* constraint = constraints[*index];
    if (!constraint || !constraint->getRTTI().instanceOf(spine::IkConstraint::rtti))
        return 0;
    return static_cast<spine::IkConstraint*>(constraint);
}

static bool RemoveIKTargets(dmArray<GuiIKTarget>& targets, dmhash_t constraint_id)
{
    bool removed = false;
    for (uint32_t i = 0; i < targets.Size();)
    {
        if (constraint_id == targets[i].m_ConstraintHash)
        {
            targets.EraseSwap(i);
            removed = true;
        }
        else
        {
            ++i;
        }
    }
    return removed;
}

bool SetIKTargetPosition(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id, Vectormath::Aos::Point3 position)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spine::IkConstraint* constraint = GetIKConstraint(node, constraint_id);
    if (!constraint)
        return false;

    // A constraint has one persistent override mode. Replacing rather than
    // appending also keeps repeated script calls from accumulating bindings.
    RemoveIKTargets(node->m_IKTargets, constraint_id);
    RemoveIKTargets(node->m_IKTargetPositions, constraint_id);

    if (node->m_IKTargetPositions.Full())
        node->m_IKTargetPositions.OffsetCapacity(2);

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
    spine::IkConstraint* constraint = GetIKConstraint(node, constraint_id);
    if (!constraint)
        return false;

    RemoveIKTargets(node->m_IKTargetPositions, constraint_id);
    RemoveIKTargets(node->m_IKTargets, constraint_id);

    if (node->m_IKTargets.Full())
        node->m_IKTargets.OffsetCapacity(2);

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

    bool removed = RemoveIKTargets(node->m_IKTargetPositions, constraint_id);
    removed = RemoveIKTargets(node->m_IKTargets, constraint_id) || removed;
    return removed;
}

} // namespace

DM_DECLARE_COMPGUI_NODE_TYPE(ComponentTypeGuiNodeSpineModelExt, "Spine", dmSpine::GuiNodeTypeSpineCreate, dmSpine::GuiNodeTypeSpineDestroy)
