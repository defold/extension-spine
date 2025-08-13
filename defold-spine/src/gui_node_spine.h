//

#ifndef DM_GAMESYS_COMP_SPINE_GUI_H
#define DM_GAMESYS_COMP_SPINE_GUI_H

#include <dmsdk/dlib/hash.h>
#include <dmsdk/gui/gui.h>
#include <dmsdk/script/script.h>
#include <dmsdk/dlib/vmath.h>
#include <dmsdk/dlib/array.h>

// Forward declarations for Spine types (global namespace, not dmSpine)
struct spIkConstraint;
struct spTrackEntry;

namespace dmSpine
{

// IK Target structure for GUI spine nodes
struct GuiIKTarget
{
    dmhash_t                                m_ConstraintHash;
    spIkConstraint*                         m_Constraint;
    dmGui::HNode                            m_TargetNode;     // for following a GUI node
    dmVMath::Point3                         m_Position;       // for fixed position
};

// Animation track structure for GUI spine nodes
struct GuiSpineAnimationTrack 
{
    spTrackEntry*                           m_AnimationInstance;
    dmhash_t                                m_AnimationId;
    dmGui::Playback                         m_Playback;
    dmScript::LuaCallbackInfo*              m_CallbackInfo;
    uint32_t                                m_CallbackId;
};

bool        SetScene(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_scene);
dmhash_t    GetScene(dmGui::HScene scene, dmGui::HNode hnode);

bool        PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, int32_t track, dmScript::LuaCallbackInfo* callback);
void        CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode);
void        CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode, int32_t track);

bool        AddSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_skin_id_a, dmhash_t spine_skine_id_b);
bool        ClearSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_skin_id);
bool        CopySkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_skin_id_a, dmhash_t spine_skine_id_b);
bool        SetSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_skin_id);
dmhash_t    GetSkin(dmGui::HScene scene, dmGui::HNode hnode);
dmhash_t    GetAnimation(dmGui::HScene scene, dmGui::HNode hnode, int32_t track);

bool        SetCursor(dmGui::HScene scene, dmGui::HNode hnode, float cursor, int32_t track);
float       GetCursor(dmGui::HScene scene, dmGui::HNode hnode, int32_t track);

bool        SetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, float playback_rate, int32_t track);
float       GetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, int32_t track);

dmGui::HNode GetBone(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t bone_id);

bool        SetAttachment(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, dmhash_t attachment_id);
bool        SetSlotColor(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t slot_id, Vectormath::Aos::Vector4* color);

void        PhysicsTranslate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* translation);
void        PhysicsRotate(dmGui::HScene scene, dmGui::HNode hnode, Vectormath::Aos::Vector3* center, float degrees);

// IK functions for GUI spine nodes
bool        SetIKTargetPosition(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id, Vectormath::Aos::Point3 position);
bool        SetIKTarget(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id, dmGui::HNode target_node);
bool        ResetIKTarget(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t constraint_id);

} // namespace


#endif // DM_GAMESYS_COMP_SPINE_GUI_H
