// GUI-specific Spine utilities: init/finalize, property hooks, and resource lookup

#pragma once

#include <dmsdk/sdk.h>
#include <dmsdk/gamesys/gui.h>

namespace dmSpine {

// Initialize GUI spine module (stores factory, registers property handlers, allocates storage)
void GuiSpineInitialize(dmResource::HFactory resource_factory);

// Finalize GUI spine module (unregisters property handlers, frees storage)
void GuiSpineFinalize();

// Track lifetime of GUI scenes that use Spine nodes
void GuiSpineSceneRetain(dmGui::HScene scene);
void GuiSpineSceneRelease(dmGui::HScene scene);

// Register/unregister Spine GUI nodes within a scene (for override propagation)
void GuiSpineRegisterNode(dmGui::HScene scene, dmGui::HNode node);
void GuiSpineUnregisterNode(dmGui::HScene scene, dmGui::HNode node);

// Resource lookup wrapper for GUI: checks local overrides for spinescenec, then falls back
void* GetResource(dmGui::HScene scene, dmhash_t name_hash, dmhash_t suffix_hash);

} // namespace dmSpine
