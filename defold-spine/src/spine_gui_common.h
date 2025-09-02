// Common helpers/constants for GUI Spine integration

#pragma once

#include <dmsdk/dlib/hash.h>

namespace dmSpine {

// Canonical property name used by GUI spine nodes
static const dmhash_t SPINE_SCENE         = dmHashString64("spine_scene");
// Suffix and extension hash used when resolving spine scene resources
static const dmhash_t SPINE_SCENE_SUFFIX  = dmHashString64(".spinescenec");
static const dmhash_t SPINE_SCENE_EXT_HASH= dmHashString64("spinescenec");

} // namespace dmSpine

