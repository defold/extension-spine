#pragma once

#include "res_spine_scene.h"

#include <spine/Attachment.h>
#include <spine/Skeleton.h>
#include <spine/Skin.h>
#include <spine/Slot.h>

namespace dmSpine
{
    // Spine 4.3 skin maps own attachments, while SlotPose and attachment
    // timelines only keep raw pointers. Keep an explicit scene-lifetime
    // reference before removing an attachment's final skin-map reference.
    static inline void DetachAttachment(spine::Skeleton* skeleton, spine::Attachment* attachment)
    {
        spine::Array<spine::Slot*>& slots = skeleton->getSlots();
        for (size_t i = 0; i < slots.size(); ++i)
        {
            spine::SlotPose& pose = slots[i]->getPose();
            spine::SlotPose& applied_pose = slots[i]->getAppliedPose();
            if (pose.getAttachment() == attachment)
            {
                pose.setAttachment(0);
            }
            if (&applied_pose != &pose && applied_pose.getAttachment() == attachment)
            {
                applied_pose.setAttachment(0);
            }
        }
    }

    static inline void ClearSkinAttachments(SpineSceneData* scene_data, spine::Skeleton* skeleton, spine::Skin* skin)
    {
        // Restart iteration after every removal because AttachmentMap iterators
        // are invalidated by removeAttachment. This also lets refCount identify
        // the final skin-map reference when one attachment has multiple entries.
        while (true)
        {
            spine::Skin::AttachmentMap::Entries entries = skin->getAttachments();
            if (!entries.hasNext())
            {
                break;
            }

            spine::Skin::AttachmentMap::Entry entry = entries.next();
            spine::Attachment* attachment = entry._attachment;
            bool final_skin_reference = attachment && attachment->getRefCount() == 1;
            if (final_skin_reference)
            {
                // Timelines and sibling skeletons can keep using their
                // non-owning pointers until the scene data is destroyed.
                RetainSceneAttachment(scene_data, attachment);
                DetachAttachment(skeleton, attachment);
            }

            skin->removeAttachment(entry._slotIndex, entry._placeholder);
        }

        skin->getBones().clear();
        skin->getConstraints().clear();

        if (skeleton->getSkin() == skin)
        {
            skeleton->updateCache();
            skeleton->setupPoseSlots();
        }
    }
}
