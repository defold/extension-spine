#include "gui_node_spine.h"
#include "res_spine_scene.h"
#include <common/vertices.h>

#include <dmsdk/dlib/buffer.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/gui.h>
#include <dmsdk/script/script.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/Slot.h>
#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto
#include "script_spine_gui.h"

namespace dmSpine
{

static const dmhash_t SPINE_SCENE               = dmHashString64("spine_scene");
static const dmhash_t SPINE_DEFAULT_ANIMATION   = dmHashString64("spine_default_animation");
static const dmhash_t SPINE_SKIN                = dmHashString64("spine_skin");

struct GuiNodeTypeContext
{
    // In case we need something later. Here for visibility
};

struct InternalGuiNode
{
    dmhash_t            m_SpinePath;
    SpineSceneResource* m_SpineScene;

    spSkeleton*         m_SkeletonInstance;
    spAnimationState*   m_AnimationStateInstance;
    spTrackEntry*       m_AnimationInstance;
    dmhash_t            m_AnimationId;

    dmVMath::Matrix4    m_Transform; // the world transform

    dmGui::Playback     m_Playback;
    dmGui::HScene       m_GuiScene;
    dmGui::HNode        m_GuiNode;

    dmScript::LuaCallbackInfo* m_Callback;

    uint8_t             m_Playing : 1;
};

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

static void SendDDF(InternalGuiNode* node, const dmDDF::Descriptor* descriptor, const char* data)
{
    if (!dmScript::IsCallbackValid(node->m_Callback))
        return;

    lua_State* L = dmScript::GetCallbackLuaContext(node->m_Callback);
    DM_LUA_STACK_CHECK(L, 0);

    if (!dmScript::SetupCallback(node->m_Callback))
    {
        dmLogError("Failed to setup callback");
        return;
    }

    dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
    dmScript::PushHash(L, descriptor->m_NameHash);
    dmScript::PushDDF(L, descriptor, data, true); // from comp_script.cpp

    dmScript::PCall(L, 4, 0); // instance + 3

    dmScript::TeardownCallback(node->m_Callback);
}

static void SendAnimationDone(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
    dmGameSystemDDF::SpineAnimationDone message;
    message.m_AnimationId = dmHashString64(entry->animation->name);
    message.m_Playback    = node->m_Playback;
    message.m_Track       = entry->trackIndex;

    SendDDF(node, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message);
}

static void SendSpineEvent(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
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

    SendDDF(node, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message);
}

static void SpineEventListener(spAnimationState* state, spEventType type, spTrackEntry* entry, spEvent* event)
{
    InternalGuiNode* node = (InternalGuiNode*)state->userData;

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
        printf("Animation %s complete on track %i\n", entry->animation->name, entry->trackIndex);
// TODO: Should we send event for looping animations as well?

            if (!IsLooping(node->m_Playback))
            {
                if (node->m_Callback)
                {
                    // We only send the event if it's not looping (same behavior as before)
                    SendAnimationDone(node, state, entry, event);

                    dmScript::DestroyCallback(node->m_Callback); // The animation has ended, so we won't send any more on this
                    node->m_Callback = 0;
                }
            }

            if (IsPingPong(node->m_Playback))
            {
                node->m_AnimationInstance->reverse = !node->m_AnimationInstance->reverse;
            }
        }
        break;
    // case SP_ANIMATION_DISPOSE:
    //     printf("Track entry for animation %s disposed on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
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
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback)
{
    uint32_t index = FindAnimationIndex(node, animation_id);
    if (index == INVALID_ANIMATION_INDEX)
    {
        dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
        return false;
    }

    SpineSceneResource* spine_scene = node->m_SpineScene;

    int trackIndex = 0;
    int loop = IsLooping(playback);
    if (index >= spine_scene->m_Skeleton->animationsCount)
    {
        dmLogError("No animation index %u is too large. Number of animations are %u", index, spine_scene->m_Skeleton->animationsCount);
        return false;
    }

    spAnimation* animation = spine_scene->m_Skeleton->animations[index];

    node->m_AnimationId = animation_id;
    node->m_AnimationInstance = spAnimationState_setAnimation(node->m_AnimationStateInstance, trackIndex, animation, loop);

    node->m_Playing = 1;
    node->m_Playback = playback;
    node->m_AnimationInstance->timeScale = playback_rate;
    node->m_AnimationInstance->reverse = IsReverse(playback);
    node->m_AnimationInstance->mixDuration = blend_duration;

    if (node->m_Callback)
    {
        dmScript::DestroyCallback(node->m_Callback);
    }
    node->m_Callback = callback;

    return true;
}

// SCRIPTING

bool PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return PlayAnimation(node, animation_id, playback, blend_duration, offset, playback_rate, callback);
}

void CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    node->m_Playing = 0;
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
    return true;
}

dmhash_t GetSkin(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin = node->m_SkeletonInstance->skin;
    return dmHashString64(skin->name);
}

dmhash_t GetAnimation(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_AnimationId;
}

// END SCRIPTING

static void DestroyNode(InternalGuiNode* node)
{
    //node->m_BoneInstances.SetCapacity(0);
    if (node->m_Callback)
    {
        dmScript::DestroyCallback(node->m_Callback);
    }

    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);

    //delete node; // don't delete it. It's already been registered with the comp_gui and we need to wait for the GuiDestroy
}

static void* GuiCreate(const dmGameSystem::CompGuiNodeContext* ctx, void* context, dmGui::HScene scene, dmGui::HNode node, uint32_t custom_type)
{
    InternalGuiNode* node_data = new InternalGuiNode;
    memset(node_data, 0, sizeof(InternalGuiNode));
    node_data->m_GuiScene = scene;
    node_data->m_GuiNode = node;
    return node_data;
}

static void GuiDestroy(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    dmLogWarning("MAWE %s %d",  __FUNCTION__, __LINE__);
    delete (InternalGuiNode*)(nodectx->m_NodeData);
}

static void GuiSetProperty(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx, dmhash_t name_hash, const dmGuiDDF::PropertyVariant* variant)
{
    dmLogWarning("MAWE %s %d %016llX %s  type: %d",  __FUNCTION__, __LINE__, name_hash, dmHashReverseSafe64(name_hash), variant->m_Type);

    if (variant->m_Type != dmGuiDDF::PROPERTY_TYPE_STRING)
    {
        dmLogError("Property is not of string type");
        return;
    }

    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);
    if (SPINE_SCENE == name_hash)
    {
        dmhash_t name_hash = dmHashString64(variant->m_VString);
        node->m_SpineScene = (SpineSceneResource*)ctx->m_GetResourceFn(ctx->m_GetResourceContext, name_hash);

        node->m_SkeletonInstance = spSkeleton_create(node->m_SpineScene->m_Skeleton);
        if (!node->m_SkeletonInstance)
        {
            dmLogError("%s: Failed to create skeleton instance", __FUNCTION__);
            DestroyNode(node);
            return;
        }

        SetSkin(node->m_GuiScene, node->m_GuiNode, 0);

        node->m_AnimationStateInstance = spAnimationState_create(node->m_SpineScene->m_AnimationStateData);
        if (!node->m_AnimationStateInstance)
        {
            dmLogError("%s: Failed to create animation state instance", __FUNCTION__);
            DestroyNode(node);
            return;
        }

        node->m_AnimationStateInstance->userData = node;
        node->m_AnimationStateInstance->listener = SpineEventListener;

        spSkeleton_setToSetupPose(node->m_SkeletonInstance);
        spSkeleton_updateWorldTransform(node->m_SkeletonInstance);

        node->m_Transform = dmVMath::Matrix4::identity();

        dmLogWarning("    spine_scene: %s  %p", variant->m_VString, node->m_SpineScene);

        dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, node->m_SpineScene->m_TextureSet);
    }
    else if (SPINE_DEFAULT_ANIMATION == name_hash)
    {
        node->m_AnimationId = dmHashString64(variant->m_VString);
    }
    else if (SPINE_SKIN == name_hash)
    {
        if (node->m_SkeletonInstance)
        {
            dmhash_t skin_id = dmHashString64(variant->m_VString);
            SetSkin(node->m_GuiScene, node->m_GuiNode, skin_id);
        }
    }

// this state management is a bit awkward. Do we need a a "post create" function?
    if (node->m_AnimationId != 0 && node->m_AnimationStateInstance != 0)
    {
// TODO: Q: Is the default playmode specified anywhere?
        PlayAnimation(node, node->m_AnimationId, dmGui::PLAYBACK_LOOP_FORWARD, 0.0f, 0.0f, 1.0f, 0);
    }
}

static void GuiGetVertices(const dmGameSystem::CustomNodeCtx* nodectx, uint32_t decl_size, dmBuffer::StreamDeclaration* decl, uint32_t struct_size, dmArray<uint8_t>& vertices)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

    //TODO: Verify the vertex declaration
    // In theory, we can check the vertex format to see which components to output
    // We currently know it's xyz-uv-rgba
    dmArray<dmSpine::SpineVertex>* vbdata = (dmArray<dmSpine::SpineVertex>*)&vertices;

    uint32_t num_vertices = dmSpine::GenerateVertexData(*vbdata, node->m_SkeletonInstance, node->m_Transform);
    (void)num_vertices;
}

static void GuiUpdate(const dmGameSystem::CustomNodeCtx* nodectx, float dt)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);
    if (!node->m_AnimationStateInstance)
        return;

    //float anim_dt = component.m_UseCursor ? 0.0f : dt;
    float anim_dt = dt;

    if (node->m_Playing)
    {
        spAnimationState_update(node->m_AnimationStateInstance, anim_dt);
    }

    spAnimationState_apply(node->m_AnimationStateInstance, node->m_SkeletonInstance);
    spSkeleton_updateWorldTransform(node->m_SkeletonInstance);
}

static dmGameObject::Result GuiNodeTypeSpineCreate(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = new GuiNodeTypeContext;
    dmGameSystem::CompGuiNodeTypeSetContext(type, type_context);


    dmGameSystem::CompGuiNodeTypeSetCreateFn(type, GuiCreate);
    dmGameSystem::CompGuiNodeTypeSetDestroyFn(type, GuiDestroy);
    dmGameSystem::CompGuiNodeTypeSetUpdateFn(type, GuiUpdate);
    dmGameSystem::CompGuiNodeTypeSetGetVerticesFn(type, GuiGetVertices);
    dmGameSystem::CompGuiNodeTypeSetSetPropertyFn(type, GuiSetProperty);

    lua_State* L = dmGameSystem::GetLuaState(ctx);
    ScriptSpineGuiRegister(L);

    return dmGameObject::RESULT_OK;
}

static dmGameObject::Result GuiNodeTypeSpineDestroy(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    dmLogWarning("MAWE: %s", __FUNCTION__);
    GuiNodeTypeContext* type_context = (GuiNodeTypeContext*)dmGameSystem::CompGuiNodeTypeGetContext(type);
    delete type_context;
    return dmGameObject::RESULT_OK;
}

} // namespace



DM_DECLARE_COMPGUI_NODE_TYPE(ComponentTypeGuiNodeSpineModelExt, "Spine", dmSpine::GuiNodeTypeSpineCreate, dmSpine::GuiNodeTypeSpineDestroy)
