#include <assert.h>

#include <dmsdk/dlib/hash.h>
#include <dmsdk/dlib/message.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/gamesys/script.h>
#include <dmsdk/gamesys/gui.h>
#include <dmsdk/gui/gui.h>

// The engine ddf formats aren't stored in the "dmsdk" folder (yet)
#include <gamesys/gamesys_ddf.h>

#include "script_spine_gui.h"
#include "gui_node_spine.h"

// #include "spine_ddf.h"
// #include "res_spine_model.h"

//#include <dmsdk/sdk.h>

namespace dmSpine
{
    using namespace dmVMath;

    static const uint32_t SPINE_NODE_TYPE = dmHashString32("Spine");

    int VerifySpineNode(lua_State* L, dmGui::HScene scene, dmGui::HNode node)
    {
        uint32_t custom_type = GetNodeCustomType(scene, node);
        if (SPINE_NODE_TYPE != custom_type) {
            return luaL_error(L, "Cannot play spine animation on a non-spine node: %u (expected: %u)", custom_type, SPINE_NODE_TYPE);
        }
        return 0;
    }

    #define VERIFY_SPINE_NODE(scene, node) \
    { \
        uint32_t custom_type = GetNodeCustomType(scene, node); \
        if (SPINE_NODE_TYPE != custom_type) { \
            return luaL_error(L, "Cannot play spine animation on a non-spine node: %u (expected: %u)", custom_type, SPINE_NODE_TYPE); \
        } \
    }

    /*# creates a new spine node
     * Dynamically create a new spine node.
     *
     * @name gui.new_spine_node
     * @param pos [type:vector3|vector4] node position
     * @param spine_scene [type:string|hash] spine scene id
     * @return node [type:node] new spine node
     */
    static int NewSpineNode(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        Vector3* pos = dmScript::CheckVector3(L, 1);
        dmhash_t spine_scene_id = dmScript::CheckHashOrString(L, 2);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);

        dmGui::HNode node = dmGui::NewNode(scene, Point3(*pos), Vector3(1,1,0), dmGui::NODE_TYPE_CUSTOM, SPINE_NODE_TYPE);
        if (!node)
        {
            return DM_LUA_ERROR("Failed to create spine scene node with scene %s", dmHashReverseSafe64(spine_scene_id));
        }

        if (!dmSpine::SetScene(scene, node, spine_scene_id))
        {
            dmGui::DeleteNode(scene, node);
            return DM_LUA_ERROR("failed to set spine scene for new node");
        }

        dmGui::LuaPushNode(L, scene, node);
        return 1;
    }

    /*# play a spine animation
     * Starts a spine animation.
     *
     * @name gui.play_spine_anim
     * @replaces gui.play_spine
     * @param node [type:node] spine node that should play the animation
     * @param animation_id [type:string|hash] id of the animation to play
     * @param playback [type:constant] playback mode
     *
     * - `gui.PLAYBACK_ONCE_FORWARD`
     * - `gui.PLAYBACK_ONCE_BACKWARD`
     * - `gui.PLAYBACK_ONCE_PINGPONG`
     * - `gui.PLAYBACK_LOOP_FORWARD`
     * - `gui.PLAYBACK_LOOP_BACKWARD`
     * - `gui.PLAYBACK_LOOP_PINGPONG`
     *
     * @param [play_properties] [type:table] optional table with properties
     *
     * `blend_duration`
     * : [type:number] The duration of a linear blend between the current and new animation
     *
     * `offset`
     * : [type:number] The normalized initial value of the animation cursor when the animation starts playing
     *
     * `playback_rate`
     * : [type:number] The rate with which the animation will be played. Must be positive
     *
     * @param [complete_function] [type:function(self, node)] function to call when the animation has completed
     */
    static int PlaySpineAnim(lua_State* L)
    {
        int top = lua_gettop(L);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        (void)VerifySpineNode(L, scene, node);

        dmhash_t anim_id = dmScript::CheckHashOrString(L, 2);
        dmGui::Playback playback = (dmGui::Playback)luaL_checkinteger(L, 3);
        lua_Integer track = 1;
        float blend_duration = 0.0, offset = 0.0, playback_rate = 1.0;

        if (top > 3 && !lua_isnil(L, 4)) // table with args, parse
        {
            luaL_checktype(L, 4, LUA_TTABLE);
            lua_pushvalue(L, 4);

            lua_getfield(L, -1, "blend_duration");
            blend_duration = lua_isnil(L, -1) ? 0.0 : luaL_checknumber(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "offset");
            offset = lua_isnil(L, -1) ? 0.0 : luaL_checknumber(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "playback_rate");
            playback_rate = lua_isnil(L, -1) ? 1.0 : luaL_checknumber(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "track");
            track = lua_isnil(L, -1) ? 1 : luaL_checkinteger(L, -1);
            lua_pop(L, 1);

            lua_pop(L, 1);
        }

        int node_ref = LUA_NOREF;
        dmScript::LuaCallbackInfo* cbk = 0x0;
        if (top > 4) // completed cb
        {
            if (lua_isfunction(L, 5))
            {
                cbk = dmScript::CreateCallback(L, 5);
            }
        }
// TODO: Q: Should we support this? If so, then it's in the MVP2
        // else
        // {
        //     lua_rawgeti(L, LUA_REGISTRYINDEX, scene->m_ContextTableReference);
        //     lua_pushvalue(L, 1);
        //     node_ref = dmScript::Ref(L, -2);
        //     lua_pop(L, 1);
        // }

        bool result = dmSpine::PlayAnimation(scene, node, anim_id, playback, blend_duration, offset, playback_rate, track, cbk);

        if (!result)
        {
            dmLogError("Could not play spine animation '%s'.", dmHashReverseSafe64(anim_id));
        }

        assert(top == lua_gettop(L));
        return 0;
    }

    /*# cancel a spine animation
     *
     * @name gui.cancel_spine
     * @param node [type:node] spine node that should cancel its animation
     */
    static int CancelSpine(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmSpine::CancelAnimation(scene, node);
        return 0;
    }


    /*# retrieve the GUI node corresponding to a spine skeleton bone
     * The returned node can be used for parenting and transform queries.
     * This function has complexity O(n), where n is the number of bones in the spine model skeleton.
     *
     * @name gui.get_spine_bone
     * @param node [type:node] spine node to query for bone node
     * @param bone_id [type:string|hash] id of the corresponding bone
     * @return bone [type:node] node corresponding to the spine bone
     */
    static int GetSpineBone(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t bone_id = dmScript::CheckHashOrString(L, 2);

        dmGui::HNode bone_node = dmSpine::GetBone(scene, node, bone_id);
        if (bone_node == 0)
        {
            char buffer[128];
            return DM_LUA_ERROR("No gui node found for the bone '%s'", dmHashReverseSafe64(bone_id));
        }

        dmGui::LuaPushNode(L, scene, bone_node);

        return 1;
    }

    /*# sets the spine scene of a node
     * Set the spine scene on a spine node. The spine scene must be mapped to the gui scene in the gui editor.
     *
     * @name gui.set_spine_scene
     * @param node [type:node] node to set spine scene for
     * @param spine_scene [type:string|hash] spine scene id
     */
    static int SetSpineScene(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        dmhash_t spine_scene_id = dmScript::CheckHashOrString(L, 2);

        VERIFY_SPINE_NODE(scene, node);

        if (!dmSpine::SetScene(scene, node, spine_scene_id))
        {
            return DM_LUA_ERROR("failed to set spine scene for new node");
        }

        return 0;
    }

    /*# gets the spine scene of a node
     * Returns the spine scene id of the supplied node.
     * This is currently only useful for spine nodes.
     * The returned spine scene must be mapped to the gui scene in the gui editor.
     *
     * @name gui.get_spine_scene
     * @param node [type:node] node to get texture from
     * @return spine_scene [type:hash] spine scene id
     */
    static int GetSpineScene(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmScript::PushHash(L, dmSpine::GetScene(scene, node));
        return 1;
    }

    /*# adds a skin to a skin
     * Adds one spine skin on a spine node to another spine skin on the same node.
     * This adds attachments and constraints from one skin to another.
     *
     * This function call does alter the recieving skin in runtime. A skin
     * that has had another skin added will have both sets of attachments.
     * Setting the model to another skin and then back to the altered skin will
     * not reset the skin to its original composition.
     *
     * @name gui.add_spine_skin
     * @param node [type:node] node to get texture from
     * @param spine_skin [type:string|hash] spine skin id to recieve the added skin
     * @param spine_skin [type:string|hash] spine skin id to add
     */
    static int AddSpineSkin(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t skin_id_a = dmScript::CheckHashOrString(L, 2);
        dmhash_t skin_id_b = dmScript::CheckHashOrString(L, 3);

        bool result = dmSpine::AddSkin(scene, node, skin_id_a, skin_id_b);
        if (!result)
        {
            return DM_LUA_ERROR("Failed to add skin '%s' to skin '%s' for spine node", dmHashReverseSafe64(skin_id_b),dmHashReverseSafe64(skin_id_a));
        }

        return 0;
    }

    /*# copies a skin to another skin
     * Copies one spine skin on a spine node to another spine skin on the same node.
     * This copies attachments and constraints from one skin to another.
     * This function call does alter the recieving skin in runtime.
     * Copying a skin to another, setting the model to another skin
     * and then back to the skin that recieved the copied skin
     * will not reset the skin to its original composition.
     *
     * @name gui.copy_spine_skin
     * @param node [type:node] node to get texture from
     * @param spine_skin  [type:hash] spine scene id to receive copied skin
     * @param spine_skin  [type:hash] spine scene id to copy
     */
    static int CopySpineSkin(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t skin_id_a = dmScript::CheckHashOrString(L, 2);
        dmhash_t skin_id_b = dmScript::CheckHashOrString(L, 3);

        bool result = dmSpine::CopySkin(scene, node, skin_id_a, skin_id_b);
        if (!result)
        {
            return DM_LUA_ERROR("Failed to copy skin '%s' to skin '%s' for spine node", dmHashReverseSafe64(skin_id_b),dmHashReverseSafe64(skin_id_a));
        }

        return 0;
    }

    /*# clears a skin
     * Clears all attachments and constraints from a skin on a gui spine node
     *
     * This function call does alter the recieving skin in runtime.
     * Clearing a skin, setting the model to another skin
     * and then back to the cleared skin will
     * not reset the cleared skin to its original composition.
     *
     * @name gui.clear_spine_skin
     * @param node [type:node] node to get texture from
     * @param spine_skin [type:string|hash] spine skin id
     */
    static int ClearSpineSkin(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t skin_id = dmScript::CheckHashOrString(L, 2);

        bool result = dmSpine::ClearSkin(scene, node, skin_id);
        if (!result)
        {
            return DM_LUA_ERROR("Failed to clear skin '%s' for spine node", dmHashReverseSafe64(skin_id));
        }

        return 0;
    }

    /*# sets the spine skin
     * Sets the spine skin on a spine node.
     *
     * @name gui.set_spine_skin
     * @param node [type:node] node to set the spine skin on
     * @param spine_skin [type:string|hash] spine skin id
     * @examples
     *
     * Change skin of a Spine node
     *
     * ```lua
     * function init(self)
     *   gui.set_spine_skin(gui.get_node("spine_node"), "monster")
     * end
     * ```
     */
    static int SetSpineSkin(lua_State* L)
    {
        //int top = lua_gettop(L);
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t skin_id = dmScript::CheckHashOrString(L, 2);

        // if (top > 2) {
        //     dmhash_t slot_id = dmScript::CheckHashOrString(L, 3);
        //     if (RESULT_OK != dmGui::SetNodeSpineSkinSlot(scene, node, skin_id, slot_id)) {
        //         return luaL_error(L, "failed to set spine skin ('%s') slot '%s' for gui node", dmHashReverseSafe64(skin_id), dmHashReverseSafe64(slot_id));
        //     }
        // } else
        {
            bool result = dmSpine::SetSkin(scene, node, skin_id);
            if (!result) {
                return DM_LUA_ERROR("Failed to set skin '%s' for spine node", dmHashReverseSafe64(skin_id));
            }
        }
        return 0;
    }

    /*# gets the skin of a spine node
     * Gets the spine skin of a spine node
     *
     * @name gui.get_spine_skin
     * @param node [type:node] node to get spine skin from
     * @return id [type:hash] spine skin id, 0 if no explicit skin is set
     */
    static int GetSpineSkin(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1); // hash pushed onto state will increase stack by 1

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t skin_id = dmSpine::GetSkin(scene, node);
        dmScript::PushHash(L, skin_id);
        return 1;
    }

    /*# gets the playing animation on a spine node
     * Gets the playing animation on a spine node
     *
     * @name gui.get_spine_animation
     * @param node [type:node] node to get spine skin from
     * @return id [type:hash] spine animation id, 0 if no animation is playing
     */
    static int GetSpineAnimation(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1); // hash pushed onto state will increase stack by 1

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t anim_id = dmSpine::GetAnimation(scene, node);
        dmScript::PushHash(L, anim_id);
        return 1;
    }

    /*# sets the normalized cursor of the animation on a spine node
     * This is only useful for spine nodes. The cursor is normalized.
     *
     * @name gui.set_spine_cursor
     * @param node [type:node] spine node to set the cursor for
     * @param cursor [type:number] cursor value
     */
    static int SetSpineCursor(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        float cursor = luaL_checknumber(L, 2);

        bool result = dmSpine::SetCursor(scene, node, cursor);
        if (!result)
        {
            return DM_LUA_ERROR("Failed to set spine cursor for gui spine node");
        }

        return 0;
    }

    /*# gets the normalized cursor of the animation on a spine node
     * This is only useful for spine nodes. Gets the normalized cursor of the animation on a spine node.
     *
     * @name gui.get_spine_cursor
     * @param node [type:node] spine node to get the cursor for (node)
     * @return cursor value [type:number] cursor value
     */
    static int GetSpineCursor(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        float cursor = dmSpine::GetCursor(scene, node);
        lua_pushnumber(L, cursor);
        return 1;
    }

    /*# sets the playback rate of the animation on a spine node
     * This is only useful for spine nodes. Sets the playback rate of the animation on a spine node. Must be positive.
     *
     * @name gui.set_spine_playback_rate
     * @param node [type:node] spine node to set the cursor for
     * @param playback_rate [type:number] playback rate
     */
    static int SetSpinePlaybackRate(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        float playback_rate = luaL_checknumber(L, 2);
        bool result = dmSpine::SetPlaybackRate(scene, node, playback_rate);

        if (!result)
        {
            return DM_LUA_ERROR("Failed to set spine playback rate for gui spine node");
        }
        return 0;
    }

    /*# gets the playback rate of the animation on a spine node
     * This is only useful for spine nodes. Gets the playback rate of the animation on a spine node.
     *
     * @name gui.get_spine_playback_rate
     * @param node [type:node] spine node to set the cursor for
     * @return rate [type:number] playback rate
     */
    static int GetSpinePlaybackRate(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        float playback_rate = dmSpine::GetPlaybackRate(scene, node);
        lua_pushnumber(L, playback_rate);
        return 1;
    }

    /*# sets a color used to tint all attachments in a slot
     * This is only useful for spine nodes. Sets a tint to a slot on a spine node.
     *
     * @name gui.set_spine_slot_color
     * @param node [type:node] spine node to set the slot for
     * @param slot [type:string|hash] slot name
     * @param color [type:vector4]  desired color
     */
    static int SetSpineSlotColor(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t slot_id = dmScript::CheckHashOrString(L, 2);
        Vectormath::Aos::Vector4* color = dmScript::CheckVector4(L, 3);

        bool result = dmSpine::SetSlotColor(scene, node, slot_id, color);

        if (!result)
        {
            return DM_LUA_ERROR("Failed to set color in slot for gui spine node");
        }
        return 0;
    }

    /*# sets an attachment to a slot
     * This is only useful for spine nodes. Sets an attachment to a slot on a spine node.
     *
     * @name gui.set_spine_attachment
     * @param node [type:node] spine node to set the slot for
     * @param slot [type:string|hash] slot name
     * @param attachment [type:string|hash] attachment name. May be nil.
     */
    static int SetSpineAttachment(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        dmhash_t slot_id = dmScript::CheckHashOrString(L, 2);
        dmhash_t attachment_id = 0;
        if (!lua_isnil(L, 3))
        {
            attachment_id = dmScript::CheckHashOrString(L, 3);
        }

        bool result = dmSpine::SetAttachment(scene, node, slot_id, attachment_id);

        if (!result)
        {
            return DM_LUA_ERROR("Failed to set spine attachment for gui spine node");
        }
        return 0;
    }

    /*# apply a physics-based translation to a Spine GUI node
     *
     * Applies a translation vector to the Spine GUI node through the physics system.
     * This function influences the physical simulation.
     *
     * @name gui.spine_physics_translate
     * @param node [type:node] the Spine GUI node to translate
     * @param translation [type:vector3] the translation vector to apply
     * @examples
     *
     * The following example applies a translation vector to a Spine GUI node with the id "spine_node":
     *
     * ```lua
     * function init(self)
     *     local translation = vmath.vector3(10, 0, 0)
     *     gui.spine_physics_translate(gui.get_node("spine_node"), translation)
     * end
     * ```
     */
    static int SpineComp_PhysicsTranslate(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        Vectormath::Aos::Vector3* translation = dmScript::CheckVector3(L, 2);
        dmSpine::PhysicsTranslate(scene, node, translation);

        return 0;
    }

    /*# apply a physics-based rotation to a Spine GUI node
     *
     * Applies a rotation to the Spine GUI node through the physics system.
     * This function influences the physical simulation by rotating around a specified point.
     *
     * @name gui.spine_physics_rotate
     * @param node [type:node] the Spine GUI node to rotate
     * @param center [type:vector3] the center point around which to rotate
     * @param degrees [type:number] the rotation angle in degrees
     * @examples
     *
     * The following example applies a rotation to a Spine GUI node with the id "spine_node":
     *
     * ```lua
     * function init(self)
     *     -- Rotate 45 degrees around the point (10, 5, 0)
     *     gui.spine_physics_rotate(gui.get_node("spine_node"), vmath.vector3(10, 5, 0), 45)
     * end
     * ```
     */
    static int SpineComp_PhysicsRotate(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        dmGui::HScene scene = dmGui::LuaCheckScene(L);
        dmGui::HNode node = dmGui::LuaCheckNode(L, 1);

        VERIFY_SPINE_NODE(scene, node);

        Vectormath::Aos::Vector3* center = dmScript::CheckVector3(L, 2);
        float degrees = luaL_checknumber(L, 3);

        dmSpine::PhysicsRotate(scene, node, center, degrees);

        return 0;
    }

    static const luaL_reg SPINE_FUNCTIONS[] =
    {
        {"new_spine_node", NewSpineNode},
        {"play_spine_anim",     PlaySpineAnim},
        {"cancel_spine",        CancelSpine},
        {"get_spine_bone",      GetSpineBone},
        {"set_spine_scene",     SetSpineScene},
        {"get_spine_scene",     GetSpineScene},
        {"clear_spine_skin",    ClearSpineSkin},
        {"add_spine_skin",      AddSpineSkin},
        {"copy_spine_skin",     CopySpineSkin},
        {"set_spine_skin",      SetSpineSkin},
        {"get_spine_skin",      GetSpineSkin},
        {"get_spine_animation", GetSpineAnimation},
        {"set_spine_cursor",    SetSpineCursor},
        {"get_spine_cursor",    GetSpineCursor},
        {"set_spine_playback_rate", SetSpinePlaybackRate},
        {"get_spine_playback_rate", GetSpinePlaybackRate},
        {"set_spine_slot_color",    SetSpineSlotColor},
        {"set_spine_attachment",    SetSpineAttachment},
        {"spine_physics_translate", SpineComp_PhysicsTranslate},
        {"spine_physics_rotate",    SpineComp_PhysicsRotate},

        // Also gui.set_spine_attachment to mimic the the go.set_attachment
        {0, 0}
    };

    void ScriptSpineGuiRegister(lua_State* L)
    {
        luaL_register(L, "gui", SPINE_FUNCTIONS);
        lua_pop(L, 1);
    }
}
