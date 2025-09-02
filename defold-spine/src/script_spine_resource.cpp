#include <assert.h>

#include <dmsdk/dlib/hash.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/resource/resource.h>
#include <dmsdk/ddf/ddf.h>
#include <dmsdk/script/script.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/script.h>
#include <dmsdk/lua/lauxlib.h>
#include <dmsdk/lua/lua.h>
#include <string.h>
#include <stdio.h>

#include "spine_ddf.h"

#include "script_spine_resource.h"

namespace dmSpine
{
    static dmResource::HFactory g_Factory = 0;

    // File extension constants
    static const char* SPINESCENE_EXT = ".spinescenec";
    static const char* SPINEJSON_EXT = ".spinejsonc";  
    static const char* ATLAS_EXT = ".texturesetc";

    static bool HasSuffix(const char* s, const char* suffix)
    {
        size_t ls = strlen(s);
        size_t lt = strlen(suffix);
        if (lt > ls) return false;
        return strcmp(s + (ls - lt), suffix) == 0;
    }

    /*# Creates a spinescene resource (.spinescenec) from runtime data
     *
     * Creates a Spine scene resource dynamically at runtime. This allows loading
     * Spine animations from data rather than pre-built assets.
     *
     * Resources created with this function are automatically cleaned up when the
     * collection is destroyed, similar to engine functions like resource.create_atlas().
     *
     * @name resource.create_spinescene
     * @param path [type:string] The target resource path. Must end with .spinescenec
     * @param options [type:table] Table with fields:
     *  - spine_data [type:string] JSON bytes of the Spine skeleton
     *  - atlas_path   [type:string] Path to the compiled atlas resource (.texturesetc)
     * @return path_hash [type:hash] canonical path hash of the created resource
     *
     * @examples
     * ```lua
     * function init(self)
     *     -- Load Spine JSON data
     *     local json = sys.load_resource("/data/character.spinejson")
     *     
     *     -- Create spinescene dynamically
     *     local scene = resource.create_spinescene("/dyn/character.spinescenec", {
     *         spine_data = json,
     *         atlas_path = "/textures/character.a.texturesetc"
     *     })
     *     
     *     -- Use the created resource
     *     go.set("#spine", "spine_scene", scene)
     * end
     * ```
     */
    static int CreateSpineScene(lua_State* L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        if (g_Factory == 0)
        {
            return luaL_error(L, "Spine resource module not initialized");
        }

        const char* scene_path = luaL_checkstring(L, 1);
        // Validate extension and absolute path
        if (!HasSuffix(scene_path, SPINESCENE_EXT))
        {
            return luaL_error(L, "Unable to create resource, path '%s' must have extension %s", scene_path, SPINESCENE_EXT);
        }
        if (scene_path[0] != '/')
        {
            return luaL_error(L, "'path' must be an absolute resource path starting with '/'");
        }

        // Remove any stale registered file for this path (safe if none exists)
        dmResource::RemoveFile(g_Factory, scene_path);

        // If a live resource is already loaded at this path, abort to avoid conflicts
        void* existing_scene_res = 0;
        if (ResourceGet(g_Factory, scene_path, &existing_scene_res) == RESOURCE_RESULT_OK)
        {
            dmResource::Release(g_Factory, existing_scene_res);
            return luaL_error(L, "Unable to create resource, a resource is already loaded at path '%s'", scene_path);
        }

        luaL_checktype(L, 2, LUA_TTABLE);
        lua_pushvalue(L, 2);

        // spine_data
        lua_getfield(L, -1, "spine_data");
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 2); // pop nil and options
            return luaL_error(L, "Missing required field 'spine_data'");
        }
        uint32_t json_size = 0;
        int8_t* json_data;
        if (lua_isstring(L, -1))
        {
            size_t string_len;
            json_data = (int8_t*)luaL_checklstring(L, -1, &string_len);
            json_size = (uint32_t)string_len;
        }
        else
        {
            return luaL_error(L, "Expected 'string' for 'spine_data'");
        }
        lua_pop(L, 1); // spine_data

        // atlas_path (string)
        lua_getfield(L, -1, "atlas_path");
        if (!lua_isstring(L, -1))
        {
            lua_pop(L, 2); // pop nil and options
            return luaL_error(L, "Expected 'string' for 'atlas_path'");
        }
        const char* atlas_path = luaL_checkstring(L, -1);
        
        // Validate atlas path format first
        if (!atlas_path || atlas_path[0] != '/')
        {
            lua_pop(L, 2); // pop atlas_path and options
            return luaL_error(L, "'atlas_path' must be an absolute resource path starting with '/'");
        }
        
        lua_pop(L, 1); // atlas_path
        lua_pop(L, 1); // options

        // Create child spinejsonc resource path
        size_t scene_path_len = strlen(scene_path);
        size_t json_path_len = scene_path_len + strlen(SPINEJSON_EXT) + 1;
        char json_path[json_path_len];
        snprintf(json_path, json_path_len, "%s%s", scene_path, SPINEJSON_EXT);
        // Ensure any stale intermediate file is cleared first
        dmResource::RemoveFile(g_Factory, json_path);

        // Ensure no collision
        HResourceDescriptor tmp;
        if (ResourceGetDescriptor(g_Factory, json_path, &tmp) == RESOURCE_RESULT_OK)
        {
            return luaL_error(L, "Unable to create resource, a resource is already registered at path '%s'", json_path);
        }

        // Register spinejsonc payload as a file and load it
        ResourceResult add_json = ResourceAddFile(g_Factory, json_path, json_size, (void*)json_data);
        if (add_json != RESOURCE_RESULT_OK)
        {
            return luaL_error(L, "Failed to add intermediate spinejson resource '%s' (error %d)", json_path, add_json);
        }
        void* out_json_res = 0;
        ResourceResult get_json = ResourceGet(g_Factory, json_path, &out_json_res);
        if (get_json != RESOURCE_RESULT_OK)
        {
            // Clean up the added file if loading failed
            dmResource::RemoveFile(g_Factory, json_path);
            return luaL_error(L, "Failed to load intermediate spinejson resource '%s' (error %d)", json_path, get_json);
        }

        // Build DDF for spinescenec
        dmGameSystemDDF::SpineSceneDesc ddf = {};
        ddf.m_SpineJson = (char*)json_path; // stored/serialized as string
        // Validate atlas resource exists
        void* atlas_res = 0;
        ResourceResult atlas_result = ResourceGet(g_Factory, atlas_path, &atlas_res);
        if (atlas_result != RESOURCE_RESULT_OK)
        {
            dmResource::Release(g_Factory, out_json_res);
            return luaL_error(L, "'atlas_path' must reference a valid atlas resource (%s)", ATLAS_EXT);
        }
        // Release atlas resource - we only needed to validate it exists
        dmResource::Release(g_Factory, atlas_res);
        ddf.m_Atlas = (char*)atlas_path;

        dmArray<uint8_t> ddf_buffer;
        dmDDF::Result ddf_res = dmDDF::SaveMessageToArray(&ddf, dmGameSystemDDF::SpineSceneDesc::m_DDFDescriptor, ddf_buffer);
        if (ddf_res != dmDDF::RESULT_OK)
        {
            dmResource::Release(g_Factory, out_json_res);
            return luaL_error(L, "Failed to serialize SpineSceneDesc");
        }

        // Add spinescene file data and load resource
        ResourceResult add_scene = ResourceAddFile(g_Factory, scene_path, ddf_buffer.Size(), ddf_buffer.Begin());
        if (add_scene != RESOURCE_RESULT_OK)
        {
            dmResource::Release(g_Factory, out_json_res);
            dmResource::RemoveFile(g_Factory, json_path);
            return luaL_error(L, "Failed to add spinescene resource '%s' (error %d)", scene_path, add_scene);
        }
        void* out_scene_res = 0;
        ResourceResult get_scene = ResourceGet(g_Factory, scene_path, &out_scene_res);
        if (get_scene != RESOURCE_RESULT_OK)
        {
            // Clean up both added files if final loading failed
            dmResource::RemoveFile(g_Factory, scene_path);
            dmResource::Release(g_Factory, out_json_res);
            dmResource::RemoveFile(g_Factory, json_path);
            return luaL_error(L, "Failed to load spinescene resource '%s' (error %d)", scene_path, get_scene);
        }

        dmhash_t canonical_hash = 0;
        ResourceGetPath(g_Factory, out_scene_res, &canonical_hash);

        // Get collection for automatic resource cleanup (works from both .script and .gui_script)
        dmGameObject::HCollection collection = dmScript::CheckCollection(L);

        // Register only the scene resource for automatic cleanup
        // (JSON resource is intermediate and will be released immediately)
        dmGameObject::AddDynamicResourceHash(collection, canonical_hash);

        // Release and remove the intermediate JSON resource (no longer needed after scene loaded)
        dmResource::Release(g_Factory, out_json_res);
        dmResource::RemoveFile(g_Factory, json_path);
        // Remove the spinescene backing file (resource instance remains alive in memory)
        dmResource::RemoveFile(g_Factory, scene_path);
        
        // Note: Don't release out_scene_res! That's the resource the caller will use
        // The reference count from ResourceGet() stays to keep the resource alive

        dmScript::PushHash(L, canonical_hash);
        return 1;
    }

    static const luaL_reg MODULE_FUNCTIONS[] =
    {
        {"create_spinescene", CreateSpineScene},
        {0, 0}
    };

    void ScriptSpineResourceInitialize(dmResource::HFactory factory)
    {
        g_Factory = factory;
    }

    void ScriptSpineResourceRegister(lua_State* L)
    {
        luaL_register(L, "resource", MODULE_FUNCTIONS);
        lua_pop(L, 1);
    }
}
