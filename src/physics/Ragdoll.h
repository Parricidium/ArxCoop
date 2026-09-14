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
#include <string>
#include <string_view>

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

//! Recreate the ragdolls saved by serializeRagdolls(), once the level's entities are restored
void restoreRagdolls(std::string_view buffer);

} // namespace physics

#endif // ARX_PHYSICS_RAGDOLL_H
