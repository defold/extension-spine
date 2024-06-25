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

#ifndef SPINE_SKELETONDATA_H_
#define SPINE_SKELETONDATA_H_

#include <spine/dll.h>
#include <spine/BoneData.h>
#include <spine/SlotData.h>
#include <spine/Skin.h>
#include <spine/EventData.h>
#include <spine/Animation.h>
#include <spine/IkConstraintData.h>
#include <spine/TransformConstraintData.h>
#include <spine/PathConstraintData.h>
#include <spine/PhysicsConstraintData.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spSkeletonData {
	char *version;
	char *hash;
	float x, y, width, height;
    float referenceScale;
	float fps;
	const char *imagesPath;
	const char *audioPath;

	int stringsCount;
	char **strings;

	int bonesCount;
	spBoneData **bones;

	int slotsCount;
	spSlotData **slots;

	int skinsCount;
	spSkin **skins;
	spSkin *defaultSkin;

	int eventsCount;
	spEventData **events;

	int animationsCount;
	spAnimation **animations;

	int ikConstraintsCount;
	spIkConstraintData **ikConstraints;

	int transformConstraintsCount;
	spTransformConstraintData **transformConstraints;

	int pathConstraintsCount;
	spPathConstraintData **pathConstraints;

    int physicsConstraintsCount;
    spPhysicsConstraintData **physicsConstraints;
} spSkeletonData;

SP_API spSkeletonData *spSkeletonData_create(void);

SP_API void spSkeletonData_dispose(spSkeletonData *self);

SP_API spBoneData *spSkeletonData_findBone(const spSkeletonData *self, const char *boneName);

SP_API spSlotData *spSkeletonData_findSlot(const spSkeletonData *self, const char *slotName);

SP_API spSkin *spSkeletonData_findSkin(const spSkeletonData *self, const char *skinName);

SP_API spEventData *spSkeletonData_findEvent(const spSkeletonData *self, const char *eventName);

SP_API spAnimation *spSkeletonData_findAnimation(const spSkeletonData *self, const char *animationName);

SP_API spIkConstraintData *spSkeletonData_findIkConstraint(const spSkeletonData *self, const char *constraintName);

SP_API spTransformConstraintData *
spSkeletonData_findTransformConstraint(const spSkeletonData *self, const char *constraintName);

SP_API spPathConstraintData *spSkeletonData_findPathConstraint(const spSkeletonData *self, const char *constraintName);

SP_API spPhysicsConstraintData *spSkeletonData_findPhysicsConstraint(const spSkeletonData *self, const char *constraintName);

#ifdef __cplusplus
}
#endif

#endif /* SPINE_SKELETONDATA_H_ */
