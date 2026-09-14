/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ARX_PHYSICS_RAGDOLL_H
#define ARX_PHYSICS_RAGDOLL_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "math/Types.h"

class Entity;
struct Skeleton;

/*!
 * ArxModern: corpses as ragdolls.
 *
 * When an NPC dies its skeleton becomes a chain of rigid bodies (one convex hull per bone,
 * swing/twist joints at the bone origins) dropped into the physics world from the pose it
 * died in. From then on the bones follow the bodies instead of the death animation, so the
 * body falls down stairs, slumps against walls and lies on slopes. The entity itself stays
 * what it was for the game (lootable, scripted), only its position follows the pelvis.
 *
 * Ragdolls are saved with the level (their pose, body by body, in a file of their own in the
 * save block, so the engine's save format is untouched) and restored asleep where they lay.
 */
namespace physics {

//! Called when an NPC dies (from ARX_DAMAGES_ForceDeath): start a ragdoll if possible
void onEntityDied(Entity & io, Entity * killer);

//! Drop the ragdoll of an entity (being destroyed)
void removeRagdoll(Entity & io);

/*!
 * Replace the animated bone transforms of an entity by its ragdoll pose.
 * Called after the skeleton was animated and before the vertices are transformed.
 * \return true if the entity has a ragdoll and the pose was applied
 */
bool applyRagdollPose(Entity & io, Skeleton & skeleton);

//! After a simulation step: move the entities to follow their ragdoll
void updateRagdolls();

//! Remove every ragdoll (level unload)
void clearRagdolls();

//! Number of live ragdolls
size_t ragdollCount();

//! Log the ragdolls (tests)
void dumpRagdolls();

//! The ragdolls of the level as a save block file (empty if there are none)
std::string serializeRagdolls();

/*
 * Mirroring: on a machine that does not simulate (a co-op client), the ragdoll poses come from
 * outside and the bones follow them. No Jolt needed for this part.
 */

struct BonePose {
	Vec3f pos = Vec3f(0.f);
	glm::quat rot = glm::quat(1.f, 0.f, 0.f, 0.f);
};

//! In mirror mode nothing is simulated locally: deaths make no ragdoll, poses come from mirrorRagdoll()
void setMirrorMode(bool mirrored);
bool isMirrorMode();

//! Pose of an entity's ragdoll received from the simulating machine (bones in world space)
void mirrorRagdoll(Entity & io, const Vec3f & pos, bool active, const std::vector<BonePose> & bones);

//! Snapshot of a local ragdoll, for sending; false if the entity has none
bool getRagdollPose(const Entity & io, Vec3f & pos, bool & active, std::vector<BonePose> & bones);

//! Visit the local ragdolls
void forEachRagdoll(const std::function<void(Entity & io, bool active)> & visit);

//! Recreate the ragdolls saved by serializeRagdolls(), once the level's entities are restored
void restoreRagdolls(std::string_view buffer);

} // namespace physics

#endif // ARX_PHYSICS_RAGDOLL_H
