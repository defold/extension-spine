#include "gui_spine.h"

#include <dmsdk/gamesys/gui.h>
#include <dmsdk/gamesys/components/comp_gui.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/property.h>
#include <dmsdk/resource/resource.hpp>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/hashtable.h>

// Local includes
#include "spine_gui_common.h"
#include "res_spine_scene.h"
#include "gui_node_spine.h"

namespace dmSpine {

// Per-scene override map and scene refcounts
struct SceneOverrides {
    dmHashTable64<void*>   m_AliasToResource; // alias -> SpineSceneResource*
    dmArray<dmhash_t>      m_Keys;            // track keys for cleanup
};

static dmHashTable64<SceneOverrides*> g_SceneOverrides; // (scene_ptr) -> SceneOverrides*
static dmHashTable64<uint32_t>        g_SceneRefcounts; // (scene_ptr) -> count
static dmHashTable64< dmArray<dmGui::HNode>* > g_SceneNodes; // (scene_ptr) -> list of spine gui nodes
static dmArray<uint64_t>              g_SceneKeys;      // list of scene keys for iteration
static dmResource::HFactory           g_ResourceFactory = 0x0;

static inline uint64_t SceneKey(dmGui::HScene scene) {
    return (uint64_t)(uintptr_t)scene;
}

// Property handler implementations (moved from gui_node_spine.cpp and optimized)
static dmGameObject::PropertyResult CompSpineGuiSetProperty(dmGui::HScene scene, const dmGameObject::ComponentSetPropertyParams& params)
{
    if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_HASH)
        return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

    dmhash_t name_hash = params.m_Options.m_HasKey ? params.m_Options.m_Key : params.m_Value.m_Hash; // prefer keyed alias if provided

    // Fetch or create scene overrides bucket
    uint64_t skey = SceneKey(scene);
    SceneOverrides** scene_bucket_ptr = g_SceneOverrides.Get(skey);
    if (!scene_bucket_ptr) {
        // allocate
        SceneOverrides* ov = new SceneOverrides();
        ov->m_AliasToResource.SetCapacity(4, 8);
        ov->m_Keys.SetCapacity(8);
        g_SceneOverrides.Put(skey, ov);
        if (g_SceneKeys.Full()) g_SceneKeys.OffsetCapacity(4);
        g_SceneKeys.Push(skey);
        scene_bucket_ptr = g_SceneOverrides.Get(skey);
    }
    SceneOverrides* scene_bucket = *scene_bucket_ptr;

    // Fetch or create alias storage
    void** slot = scene_bucket->m_AliasToResource.Get(name_hash);
    if (!slot) {
        scene_bucket->m_AliasToResource.Put(name_hash, 0);
        scene_bucket->m_Keys.Push(name_hash);
        slot = scene_bucket->m_AliasToResource.Get(name_hash);
    }

    // Update resource pointer and reference counts via helper
    dmGameObject::PropertyResult pres = dmGameSystem::SetResourceProperty(g_ResourceFactory, params.m_Value, SPINE_SCENE_EXT_HASH, slot);
    if (pres != dmGameObject::PROPERTY_RESULT_OK)
        return pres;

    // Propagate the override to any existing nodes in this scene using the same alias
    dmArray<dmGui::HNode>** nodes_ptr = g_SceneNodes.Get(skey);
    if (nodes_ptr) {
        dmArray<dmGui::HNode>& nodes = **nodes_ptr;
        const uint32_t n = nodes.Size();
        for (uint32_t i = 0; i < n; ++i) {
            dmGui::HNode hnode = nodes[i];
            if (dmSpine::GetScene(scene, hnode) == name_hash) {
                dmSpine::SetScene(scene, hnode, name_hash);
            }
        }
    }

    return pres;
}

static dmGameObject::PropertyResult CompSpineGuiGetProperty(dmGui::HScene scene, const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
{
    bool has_key = params.m_Options.m_HasKey;
    dmhash_t key_hash = params.m_Options.m_Key;

    SceneOverrides** scene_bucket_ptr = g_SceneOverrides.Get(SceneKey(scene));
    if (!scene_bucket_ptr)
        return dmGameObject::PROPERTY_RESULT_NOT_FOUND;

    SceneOverrides* scene_bucket = *scene_bucket_ptr;
    if (has_key) {
        if (void** slot = scene_bucket->m_AliasToResource.Get(key_hash)) {
            if (*slot)
                return dmGameSystem::GetResourceProperty(g_ResourceFactory, *slot, out_value);
        }
        return dmGameObject::PROPERTY_RESULT_NOT_FOUND;
    }

    // No key provided: return first available override for this scene
    for (uint32_t i = 0; i < scene_bucket->m_Keys.Size(); ++i) {
        if (void** slot = scene_bucket->m_AliasToResource.Get(scene_bucket->m_Keys[i])) {
            if (*slot)
                return dmGameSystem::GetResourceProperty(g_ResourceFactory, *slot, out_value);
        }
    }
    return dmGameObject::PROPERTY_RESULT_NOT_FOUND;
}

void GuiSpineInitialize(dmResource::HFactory resource_factory)
{
    g_ResourceFactory = resource_factory;
    g_SceneOverrides.SetCapacity(4, 8);
    g_SceneRefcounts.SetCapacity(4, 8);
    g_SceneNodes.SetCapacity(4, 8);

    // Register per-property handlers for GUI spine_scene property
    dmGameSystem::CompGuiRegisterSetPropertyFn(SPINE_SCENE, CompSpineGuiSetProperty);
    dmGameSystem::CompGuiRegisterGetPropertyFn(SPINE_SCENE, CompSpineGuiGetProperty);
}

static void CleanupSceneOverrides(dmGui::HScene scene)
{
    SceneOverrides** scene_bucket_ptr = g_SceneOverrides.Get(SceneKey(scene));
    if (!scene_bucket_ptr)
        return;
    SceneOverrides* scene_bucket = *scene_bucket_ptr;

    // Release all held resources
    for (uint32_t i = 0; i < scene_bucket->m_Keys.Size(); ++i) {
        dmhash_t key = scene_bucket->m_Keys[i];
        void** slot = scene_bucket->m_AliasToResource.Get(key);
        if (slot && *slot) {
            dmResource::Release(g_ResourceFactory, *slot);
            *slot = 0;
        }
    }

    // Remove from map and delete bucket
    uint64_t skey = SceneKey(scene);
    g_SceneOverrides.Erase(skey);
    // remove from keys list
    for (uint32_t i = 0; i < g_SceneKeys.Size(); ++i) {
        if (g_SceneKeys[i] == skey) { g_SceneKeys.EraseSwap(i); break; }
    }
    delete scene_bucket;
}

static void CleanupSceneNodes(dmGui::HScene scene)
{
    uint64_t skey = SceneKey(scene);
    dmArray<dmGui::HNode>** nodes_ptr = g_SceneNodes.Get(skey);
    if (!nodes_ptr)
        return;
    dmArray<dmGui::HNode>* nodes = *nodes_ptr;
    delete nodes;
    g_SceneNodes.Erase(skey);
}

void GuiSpineSceneRetain(dmGui::HScene scene)
{
    uint64_t skey = SceneKey(scene);
    uint32_t* cnt = g_SceneRefcounts.Get(skey);
    if (!cnt) {
        g_SceneRefcounts.Put(skey, 1);
    } else {
        (*cnt)++;
    }
}

void GuiSpineSceneRelease(dmGui::HScene scene)
{
    uint64_t skey = SceneKey(scene);
    uint32_t* cnt = g_SceneRefcounts.Get(skey);
    if (!cnt)
        return;
    if (*cnt > 1) {
        (*cnt)--;
        return;
    }
    // last reference
    g_SceneRefcounts.Erase(skey);
    CleanupSceneOverrides(scene);
    CleanupSceneNodes(scene);
}

void GuiSpineFinalize()
{
    // Unregister per-property handlers for GUI spine_scene property
    dmGameSystem::CompGuiUnregisterSetPropertyFn(SPINE_SCENE);
    dmGameSystem::CompGuiUnregisterGetPropertyFn(SPINE_SCENE);

    // Cleanup all remaining scenes (safety)
    for (uint32_t i = 0; i < g_SceneKeys.Size(); ++i) {
        dmGui::HScene scene = (dmGui::HScene)(uintptr_t)g_SceneKeys[i];
        CleanupSceneOverrides(scene);
        CleanupSceneNodes(scene);
    }
    g_SceneKeys.SetSize(0);

    g_ResourceFactory = 0x0;
}

void GuiSpineRegisterNode(dmGui::HScene scene, dmGui::HNode node)
{
    uint64_t skey = SceneKey(scene);
    dmArray<dmGui::HNode>** nodes_ptr = g_SceneNodes.Get(skey);
    if (!nodes_ptr) {
        dmArray<dmGui::HNode>* list = new dmArray<dmGui::HNode>();
        list->SetCapacity(8);
        g_SceneNodes.Put(skey, list);
        nodes_ptr = g_SceneNodes.Get(skey);
        if (g_SceneKeys.Full()) g_SceneKeys.OffsetCapacity(4);
        g_SceneKeys.Push(skey);
    }
    dmArray<dmGui::HNode>* nodes = *nodes_ptr;
    if (nodes->Full()) nodes->OffsetCapacity(8);
    nodes->Push(node);
}

void GuiSpineUnregisterNode(dmGui::HScene scene, dmGui::HNode node)
{
    dmArray<dmGui::HNode>** nodes_ptr = g_SceneNodes.Get(SceneKey(scene));
    if (!nodes_ptr) return;
    dmArray<dmGui::HNode>* nodes = *nodes_ptr;
    for (uint32_t i = 0; i < nodes->Size(); ++i) {
        if ((*nodes)[i] == node) { nodes->EraseSwap(i); break; }
    }
}

void* GetResource(dmGui::HScene scene, dmhash_t name_hash, dmhash_t suffix_hash)
{
    // Only intercept Spine scene lookups
    if (suffix_hash == SPINE_SCENE_SUFFIX) {
        SceneOverrides** scene_bucket_ptr = g_SceneOverrides.Get(SceneKey(scene));
        if (scene_bucket_ptr) {
            if (void** slot = (*scene_bucket_ptr)->m_AliasToResource.Get(name_hash)) {
                if (*slot)
                    return *slot;
            }
        }
    }

    // Fallback to default GUI resource retrieval
    return dmGui::GetResource(scene, name_hash, suffix_hash);
}

} // namespace dmSpine
