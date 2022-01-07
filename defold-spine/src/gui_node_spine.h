//

#ifndef DM_GAMESYS_COMP_SPINE_GUI_H
#define DM_GAMESYS_COMP_SPINE_GUI_H

#include <dmsdk/dlib/hash.h>
#include <dmsdk/gui/gui.h>
#include <dmsdk/script/script.h>

namespace dmSpine
{

bool PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback);
void CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode);

} // namespace


#endif // DM_GAMESYS_COMP_SPINE_GUI_H
