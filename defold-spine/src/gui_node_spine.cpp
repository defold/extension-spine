#include "gui_node_spine.h"
#include "res_spine_scene.h"
#include <common/vertices.h>

#include <dmsdk/dlib/buffer.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/gui.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/Slot.h>
#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>

namespace dmSpine
{

static const dmhash_t SPINE_SCENE               = dmHashString64("spine_scene");
static const dmhash_t SPINE_DEFAULT_ANIMATION   = dmHashString64("spine_default_animation");
static const dmhash_t SPINE_SKIN                = dmHashString64("spine_skin");


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
            // // Should we look at the looping state?
            // if (!IsLooping(node->m_Playback))
            // {
            //     // We only send the event if it's not looping (same behavior as before)
            //     SendAnimationDone(node, state, entry, event);

            //     dmMessage::ResetURL(&node->m_Listener); // The animation has ended, so we won't send any more on this
            // }

            // if (IsPingPong(node->m_Playback))
            // {
            //     node->m_AnimationInstance->reverse = !component->m_AnimationInstance->reverse;
            // }
        }
        break;
    // case SP_ANIMATION_DISPOSE:
    //     printf("Track entry for animation %s disposed on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    case SP_ANIMATION_EVENT:
        //SendSpineEvent(node, state, entry, event);
        break;
    default:
        break;
    }
}

static void DestroyNode(InternalGuiNode* node)
{
    //node->m_BoneInstances.SetCapacity(0);
    // if (node->m_Material)
    // {
    //     dmResource::Release(world->m_Factory, (void*)node->m_Material);
    // }
    // if (node->m_RenderConstants)
    // {
    //     dmGameSystem::DestroyRenderConstants(node->m_RenderConstants);
    // }

    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);

    //delete node; // don't delete it. It's already been registered with the comp_gui and we need to wait for the GuiDestroyNode
}

static void* GuiCreateNode(const dmGameSystem::CompGuiCreateContext* ctx, void* context, dmGui::HScene scene, dmGui::HNode node, uint32_t custom_type)
{
    InternalGuiNode* node_data = new InternalGuiNode;
    memset(node_data, 0, sizeof(InternalGuiNode));
    dmLogWarning("MAWE %s %d",  __FUNCTION__, __LINE__);
    node_data->m_GuiScene = scene;
    node_data->m_GuiNode = node;
    return node_data;
}

static void GuiDestroyNode(const dmGameSystem::CompGuiCreateContext* ctx, void* context, void* node_data)
{
    dmLogWarning("MAWE %s %d",  __FUNCTION__, __LINE__);
    delete (InternalGuiNode*)node_data;
}

static void GuiSetProperty(const dmGameSystem::CompGuiCreateContext* ctx, void* context, void* node_data, dmhash_t name_hash, const dmGuiDDF::PropertyVariant* variant)
{
    dmLogWarning("MAWE %s %d %016llX %s  type: %d",  __FUNCTION__, __LINE__, name_hash, dmHashReverseSafe64(name_hash), variant->m_Type);

    if (variant->m_Type != dmGuiDDF::PROPERTY_TYPE_STRING)
    {
        dmLogError("Property is not of string type");
        return;
    }

    InternalGuiNode* node = (InternalGuiNode*)node_data;
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
        spSkeleton_setSkin(node->m_SkeletonInstance, node->m_SpineScene->m_Skeleton->defaultSkin);

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
}

static void GuiGetVertices(void* context, void* node_data, uint32_t decl_size, dmBuffer::StreamDeclaration* decl, uint32_t struct_size, dmArray<uint8_t>& vertices)
{
    // In theory, we can check the vertex format to see which components to output
    //dmLogWarning("MAWE %s %d",  __FUNCTION__, __LINE__);
    InternalGuiNode* node = (InternalGuiNode*)node_data;

    //TODO: Verify the vertex declaration

    // We currently know it's xyz-uv-rgba
    dmArray<dmSpine::SpineVertex>* vbdata = (dmArray<dmSpine::SpineVertex>*)&vertices;

    uint32_t num_vertices = dmSpine::GenerateVertexData(*vbdata, node->m_SkeletonInstance, node->m_Transform);
    (void)num_vertices;

    //uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, const dmVMath::Matrix4& world)

}


// typedef dmGameObject::Result (*GuiNodeTypeCreateFunction)(const struct CompGuiNodeTypeCtx* ctx, CompGuiNodeType* type);
// typedef dmGameObject::Result (*GuiNodeTypeDestroyFunction)(const struct CompGuiNodeTypeCtx* ctx, CompGuiNodeType* type);

static dmGameObject::Result GuiNodeTypeSpineCreate(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    dmLogWarning("MAWE: %s", __FUNCTION__);
    // // Spine gui node type setup
    // dmGameSystem::CompGuiNodeType gui_node_type;
    // gui_node_type.m_Name = "Spine";
    // gui_node_type.m_NameHash = dmHashString32("Spine"); //TODO: we should do this in comp_gui.cpp, in order to make sure it's a hash32



    // const char*             m_Name;
    // dmhash_t                m_NameHash;
    // void*                   m_Context;

    // void* (*CreateContext)(const struct CompGuiNodeTypeCtx* ctx);

    // dmGameObject::Result RegisterCompGuiNodeType(dmGameObject::HRegister regist, const char* name, const CompGuiNodeType& type, void* context);

    dmGameSystem::CompGuiNodeTypeSetCreateNodeFn(type, GuiCreateNode);
    dmGameSystem::CompGuiNodeTypeSetDestroyNodeFn(type, GuiDestroyNode);
    dmGameSystem::CompGuiNodeTypeSetGetVerticesFn(type, GuiGetVertices);
    dmGameSystem::CompGuiNodeTypeSetSetPropertyFn(type, GuiSetProperty);


    return dmGameObject::RESULT_OK;
}

static dmGameObject::Result GuiNodeTypeSpineDestroy(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    dmLogWarning("MAWE: %s", __FUNCTION__);

    return dmGameObject::RESULT_OK;
}

} // namespace



DM_DECLARE_COMPGUI_NODE_TYPE(ComponentTypeGuiNodeSpineModelExt, "Spine", dmSpine::GuiNodeTypeSpineCreate, dmSpine::GuiNodeTypeSpineDestroy)
