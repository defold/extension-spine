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
    static const char* SPINESKEL_EXT = ".skelc";
    static const char* ATLAS_EXT = ".texturesetc";

    static bool HasSuffix(const char* s, const char* suffix)
    {
        size_t ls = strlen(s);
        size_t lt = strlen(suffix);
        if (lt > ls) return false;
        return strcmp(s + (ls - lt), suffix) == 0;
    }

    static uint32_t SkipJsonWhitespace(const int8_t* data, uint32_t data_size, uint32_t offset)
    {
        while (offset < data_size)
        {
            char c = data[offset];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                break;
            ++offset;
        }
        return offset;
    }

    static bool IsJsonData(const int8_t* data, uint32_t data_size)
    {
        uint32_t offset = 0;
        if (data_size >= 3 &&
            (uint8_t)data[0] == 0xef &&
            (uint8_t)data[1] == 0xbb &&
            (uint8_t)data[2] == 0xbf)
        {
            offset = 3;
        }

        offset = SkipJsonWhitespace(data, data_size, offset);
        if (offset >= data_size || data[offset++] != '{')
            return false;

        // Spine binary has no magic bytes, so identify JSON by its opening
        // object/key syntax instead of relying on a single leading byte.
        offset = SkipJsonWhitespace(data, data_size, offset);
        if (offset >= data_size || data[offset++] != '"')
            return false;

        bool escaped = false;
        while (offset < data_size)
        {
            char c = data[offset];
            ++offset;
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                offset = SkipJsonWhitespace(data, data_size, offset);
                return offset < data_size && data[offset] == ':';
            }
            else if ((uint8_t)c < 0x20)
            {
                return false;
            }
        }
        return false;
    }

    /*# Creates a spinescene resource (.spinescenec) from runtime data
     *
     * Creates a Spine scene resource dynamically at runtime. This allows loading
     * Spine animations from JSON or binary data rather than pre-built assets.
     * The data format is detected automatically.
     *
     * Resources created with this function are automatically cleaned up when the
     * collection is destroyed, similar to engine functions like resource.create_atlas().
     *
     * @name resource.create_spinescene
     * @param path [type:string] The target resource path. Must end with .spinescenec
     * @param options [type:table] Table with fields:
     *  - spine_data [type:string] JSON or binary bytes of the Spine skeleton
     *  - atlas_path   [type:string] Path to the compiled atlas resource (.texturesetc)
     * @return path_hash [type:hash] canonical path hash of the created resource
     *
     * @examples
     * ```lua
     * function init(self)
     *     -- Load Spine JSON or binary data
     *     local spine_data = sys.load_resource("/data/character.skel")
     *     
     *     -- Create spinescene dynamically
     *     local scene = resource.create_spinescene("/dyn/character.spinescenec", {
     *         spine_data = spine_data,
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
        uint32_t spine_data_size = 0;
        int8_t* spine_data;
        if (lua_isstring(L, -1))
        {
            size_t string_len;
            spine_data = (int8_t*)luaL_checklstring(L, -1, &string_len);
            spine_data_size = (uint32_t)string_len;
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

        // Create a child resource using the type matching the supplied data.
        const char* spine_data_ext = IsJsonData(spine_data, spine_data_size) ? SPINEJSON_EXT : SPINESKEL_EXT;
        char spine_data_path[2048];
        size_t scene_path_len = strlen(scene_path);
        size_t spine_data_path_len = scene_path_len + strlen(spine_data_ext) + 1;
        if (spine_data_path_len > sizeof(spine_data_path))
            spine_data_path_len = sizeof(spine_data_path);
        snprintf(spine_data_path, spine_data_path_len, "%s%s", scene_path, spine_data_ext);
        // Ensure any stale intermediate file is cleared first
        dmResource::RemoveFile(g_Factory, spine_data_path);

        // Ensure no collision
        HResourceDescriptor tmp;
        if (ResourceGetDescriptor(g_Factory, spine_data_path, &tmp) == RESOURCE_RESULT_OK)
        {
            return luaL_error(L, "Unable to create resource, a resource is already registered at path '%s'", spine_data_path);
        }

        // Register the Spine data payload as a file and load it.
        ResourceResult add_spine_data = ResourceAddFile(g_Factory, spine_data_path, spine_data_size, (void*)spine_data);
        if (add_spine_data != RESOURCE_RESULT_OK)
        {
            return luaL_error(L, "Failed to add intermediate Spine data resource '%s' (error %d)", spine_data_path, add_spine_data);
        }
        void* out_spine_data_res = 0;
        ResourceResult get_spine_data = ResourceGet(g_Factory, spine_data_path, &out_spine_data_res);
        if (get_spine_data != RESOURCE_RESULT_OK)
        {
            // Clean up the added file if loading failed
            dmResource::RemoveFile(g_Factory, spine_data_path);
            return luaL_error(L, "Failed to load intermediate Spine data resource '%s' (error %d)", spine_data_path, get_spine_data);
        }

        // Build DDF for spinescenec
        dmGameSystemDDF::SpineSceneDesc ddf = {};
        ddf.m_SpineJson = (char*)spine_data_path; // stored/serialized as string
        // Validate atlas resource exists
        void* atlas_res = 0;
        ResourceResult atlas_result = ResourceGet(g_Factory, atlas_path, &atlas_res);
        if (atlas_result != RESOURCE_RESULT_OK)
        {
            dmResource::Release(g_Factory, out_spine_data_res);
            return luaL_error(L, "'atlas_path' must reference a valid atlas resource (%s)", ATLAS_EXT);
        }
        // Release atlas resource - we only needed to validate it exists
        dmResource::Release(g_Factory, atlas_res);
        ddf.m_Atlas = (char*)atlas_path;

        dmArray<uint8_t> ddf_buffer;
        dmDDF::Result ddf_res = dmDDF::SaveMessageToArray(&ddf, dmGameSystemDDF::SpineSceneDesc::m_DDFDescriptor, ddf_buffer);
        if (ddf_res != dmDDF::RESULT_OK)
        {
            dmResource::Release(g_Factory, out_spine_data_res);
            return luaL_error(L, "Failed to serialize SpineSceneDesc");
        }

        // Add spinescene file data and load resource
        ResourceResult add_scene = ResourceAddFile(g_Factory, scene_path, ddf_buffer.Size(), ddf_buffer.Begin());
        if (add_scene != RESOURCE_RESULT_OK)
        {
            dmResource::Release(g_Factory, out_spine_data_res);
            dmResource::RemoveFile(g_Factory, spine_data_path);
            return luaL_error(L, "Failed to add spinescene resource '%s' (error %d)", scene_path, add_scene);
        }
        void* out_scene_res = 0;
        ResourceResult get_scene = ResourceGet(g_Factory, scene_path, &out_scene_res);
        if (get_scene != RESOURCE_RESULT_OK)
        {
            // Clean up both added files if final loading failed
            dmResource::RemoveFile(g_Factory, scene_path);
            dmResource::Release(g_Factory, out_spine_data_res);
            dmResource::RemoveFile(g_Factory, spine_data_path);
            return luaL_error(L, "Failed to load spinescene resource '%s' (error %d)", scene_path, get_scene);
        }

        dmhash_t canonical_hash = 0;
        ResourceGetPath(g_Factory, out_scene_res, &canonical_hash);

        // Get collection for automatic resource cleanup (works from both .script and .gui_script)
        dmGameObject::HCollection collection = dmScript::CheckCollection(L);

        // Register only the scene resource for automatic cleanup
        // (The Spine data resource is intermediate and will be released immediately.)
        dmGameObject::AddDynamicResourceHash(collection, canonical_hash);

        // Release and remove the intermediate Spine data resource (no longer needed after scene loaded).
        dmResource::Release(g_Factory, out_spine_data_res);
        dmResource::RemoveFile(g_Factory, spine_data_path);
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
