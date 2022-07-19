//

#ifndef DM_GAMESYS_COMP_SPINE_GUI_H
#define DM_GAMESYS_COMP_SPINE_GUI_H

#include <dmsdk/dlib/hash.h>
#include <dmsdk/gui/gui.h>
#include <dmsdk/script/script.h>

namespace dmSpine
{

bool        SetScene(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_scene);
dmhash_t    GetScene(dmGui::HScene scene, dmGui::HNode hnode);

bool        PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback);
void        CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode);

bool        SetSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_skin_id);
dmhash_t    GetSkin(dmGui::HScene scene, dmGui::HNode hnode);
dmhash_t    GetAnimation(dmGui::HScene scene, dmGui::HNode hnode);

bool        SetCursor(dmGui::HScene scene, dmGui::HNode hnode, float cursor);
float       GetCursor(dmGui::HScene scene, dmGui::HNode hnode);

bool        SetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, float playback_rate);
float       GetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode);

dmGui::HNode GetBone(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t bone_id);

bool        SetAttachment(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, dmhash_t attachment_id);


} // namespace


#endif // DM_GAMESYS_COMP_SPINE_GUI_H
