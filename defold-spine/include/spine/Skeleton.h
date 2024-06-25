/******************************************************************************
 * Spine Runtimes License Agreement
 * Last updated July 28, 2023. Replaces all prior versions.
 *
 * Copyright (c) 2013-2023, Esoteric Software LLC
 *
 * Integration of the Spine Runtimes into software or otherwise creating
 * derivative works of the Spine Runtimes is permitted under the terms and
 * conditions of Section 2 of the Spine Editor License Agreement:
 * http://esotericsoftware.com/spine-editor-license
 *
 * Otherwise, it is permitted to integrate the Spine Runtimes into software or
 * otherwise create derivative works of the Spine Runtimes (collectively,
 * "Products"), provided that each user of the Products must obtain their own
 * Spine Editor license and redistribution of the Products in any form must
 * include this license and copyright notice.
 *
 * THE SPINE RUNTIMES ARE PROVIDED BY ESOTERIC SOFTWARE LLC "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ESOTERIC SOFTWARE LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES,
 * BUSINESS INTERRUPTION, OR LOSS OF USE, DATA, OR PROFITS) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THE
 * SPINE RUNTIMES, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#ifndef SPINE_SKELETON_H_
#define SPINE_SKELETON_H_

#include <spine/dll.h>
#include <spine/SkeletonData.h>
#include <spine/Slot.h>
#include <spine/Skin.h>
#include <spine/IkConstraint.h>
#include <spine/TransformConstraint.h>
#include <spine/PathConstraint.h>
#include <spine/PhysicsConstraint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spSkeleton {
	spSkeletonData *data;

	int bonesCount;
	spBone **bones;
	spBone *root;

	int slotsCount;
	spSlot **slots;
	spSlot **drawOrder;

	int ikConstraintsCount;
	spIkConstraint **ikConstraints;

	int transformConstraintsCount;
	spTransformConstraint **transformConstraints;

	int pathConstraintsCount;
	spPathConstraint **pathConstraints;

    int physicsConstraintsCount;
    spPhysicsConstraint **physicsConstraints;

	spSkin *skin;
	spColor color;
	float scaleX, scaleY;
	float x, y;

    float time;
} spSkeleton;

SP_API spSkeleton *spSkeleton_create(spSkeletonData *data);

SP_API void spSkeleton_dispose(spSkeleton *self);

/* Caches information about bones and constraints. Must be called if bones or constraints, or weighted path attachments
 * are added or removed. */
SP_API void spSkeleton_updateCache(spSkeleton *self);

SP_API void spSkeleton_updateWorldTransform(const spSkeleton *self, spPhysics physics);

SP_API void spSkeleton_update(spSkeleton *self, float delta);

/* Sets the bones, constraints, and slots to their setup pose values. */
SP_API void spSkeleton_setToSetupPose(const spSkeleton *self);
/* Sets the bones and constraints to their setup pose values. */
SP_API void spSkeleton_setBonesToSetupPose(const spSkeleton *self);

SP_API void spSkeleton_setSlotsToSetupPose(const spSkeleton *self);

/* Returns 0 if the bone was not found. */
SP_API spBone *spSkeleton_findBone(const spSkeleton *self, const char *boneName);

/* Returns 0 if the slot was not found. */
SP_API spSlot *spSkeleton_findSlot(const spSkeleton *self, const char *slotName);

/* Sets the skin used to look up attachments before looking in the SkeletonData defaultSkin. Attachments from the new skin are
 * attached if the corresponding attachment from the old skin was attached. If there was no old skin, each slot's setup mode
 * attachment is attached from the new skin.
 * @param skin May be 0.*/
SP_API void spSkeleton_setSkin(spSkeleton *self, spSkin *skin);
/* Returns 0 if the skin was not found. See spSkeleton_setSkin.
 * @param skinName May be 0. */
SP_API int spSkeleton_setSkinByName(spSkeleton *self, const char *skinName);

/* Returns 0 if the slot or attachment was not found. */
SP_API spAttachment *
spSkeleton_getAttachmentForSlotName(const spSkeleton *self, const char *slotName, const char *attachmentName);
/* Returns 0 if the slot or attachment was not found. */
SP_API spAttachment *
spSkeleton_getAttachmentForSlotIndex(const spSkeleton *self, int slotIndex, const char *attachmentName);
/* Returns 0 if the slot or attachment was not found.
 * @param attachmentName May be 0. */
SP_API int spSkeleton_setAttachment(spSkeleton *self, const char *slotName, const char *attachmentName);

/* Returns 0 if the IK constraint was not found. */
SP_API spIkConstraint *spSkeleton_findIkConstraint(const spSkeleton *self, const char *constraintName);

/* Returns 0 if the transform constraint was not found. */
SP_API spTransformConstraint *spSkeleton_findTransformConstraint(const spSkeleton *self, const char *constraintName);

/* Returns 0 if the path constraint was not found. */
SP_API spPathConstraint *spSkeleton_findPathConstraint(const spSkeleton *self, const char *constraintName);

/* Returns 0 if the physics constraint was not found. */
SP_API spPhysicsConstraint *spSkeleton_findPhysicsConstraint(const spSkeleton *self, const char *constraintName);

SP_API void spSkeleton_physicsTranslate(spSkeleton *self, float x, float y);

SP_API void spSkeleton_physicsRotate(spSkeleton *self, float x, float y, float degrees);

#ifdef __cplusplus
}
#endif

#endif /* SPINE_SKELETON_H_*/
