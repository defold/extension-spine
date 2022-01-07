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

    /*# creates a new spine node
     * Dynamically create a new spine node.
     *
     * @name gui.new_spine_node
     * @param pos [type:vector3|vector4] node position
     * @param spine_scene [type:string|hash] spine scene id
     * @return node [type:node] new spine node
     */
    // static int NewSpineNode(lua_State* L)
    // {
    //     Point3 pos = GetPositionFromArgumentIndex(L, 1);

    //     dmGui::HScene scene = GuiScriptInstance_Check(L);
    //     HNode node = dmGui::NewNode(scene, pos, Vector3(1,1,0), dmGui::NODE_TYPE_CUSTOM, SPINE_NODE_TYPE);
    //     if (!node)
    //     {
    //         return luaL_error(L, "Out of nodes (max %d)", scene->m_Nodes.Capacity());
    //     }

    //     dmhash_t spine_scene_id = dmScript::CheckHashOrString(L, 2);
    //     if (RESULT_OK != SetNodeSpineScene(scene, node, spine_scene_id, 0, 0, true))
    //     {
    //         GetNode(scene, node)->m_Deleted = 1;
    //         return luaL_error(L, "failed to set spine scene for new node");
    //     }

    //     dmGui::LuaPushNode(L, scene, node);
    //     return 1;
    // }

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

        uint32_t custom_type = GetNodeCustomType(scene, node);
        if (SPINE_NODE_TYPE != custom_type) {
            return luaL_error(L, "Cannot play spine animation on a non-spine node: %u", custom_type);
        }

        dmhash_t anim_id = dmScript::CheckHashOrString(L, 2);
        dmGui::Playback playback = (dmGui::Playback)luaL_checkinteger(L, 3);
        float blend_duration = 0.0, offset = 0.0, playback_rate = 1.0;

// TODO: Check if nil
        if (top > 3) // table with args, parse
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

            lua_pop(L, 1);
        }

        int node_ref = LUA_NOREF;
        dmScript::LuaCallbackInfo* cbk = 0x0;
        if (top > 4) // completed cb
        {
            if (lua_isfunction(L, 5))
            {
                cbk = dmScript::CreateCallback(L, 5);

                // lua_rawgeti(L, LUA_REGISTRYINDEX, scene->m_ContextTableReference);
                // lua_pushvalue(L, 1);
                // node_ref = luaL_ref(L, -2);
                // lua_pop(L, 1);
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

        bool result = dmSpine::PlayAnimation(scene, node, anim_id, playback, blend_duration, offset, playback_rate, cbk);

        if (!result)
        {
            dmLogError("Could not play spine animation '%s'.", dmHashReverseSafe64(anim_id));
        }

        assert(top == lua_gettop(L));
        return 0;
    }

    static const luaL_reg SPINE_FUNCTIONS[] =
    {
        //{"new_spine_node", NewSpineNode},
        {"play_spine_anim", PlaySpineAnim},
        {0, 0}
    };

    void ScriptSpineGuiRegister(lua_State* L)
    {
        luaL_register(L, "gui", SPINE_FUNCTIONS);
        lua_pop(L, 1);
    }
}
