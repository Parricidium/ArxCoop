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

#ifndef ARX_PHYSICS_DEBRIS_H
#define ARX_PHYSICS_DEBRIS_H

#include <cstddef>

#include "io/resource/ResourcePath.h"

class Entity;
struct Skeleton;

/*!
 * ArxModern: the breakable objects of the game break into physical pieces.
 *
 * The game breaks a few fixed objects (glass cases, a wooden grid, a brittle wall, grave
 * stones, doors) by playing a "break" animation that moves their vertex groups apart, then
 * destroys the entity. When such an animation starts, each vertex group becomes a rigid body
 * instead (thrown outwards from the object's centre) and the mesh follows the bodies; when
 * the entity is destroyed the pieces stay behind, drawn from a copy of the mesh, until they
 * have lain still for a while.
 */
namespace physics {

//! A fixed entity starts playing an animation: if it is a break animation, shatter the object
void onBreakAnimation(Entity & io, const res::path & animation);

//! true while the entity's pieces are simulated (the intact object is no obstacle any more)
bool isShattered(const Entity & io);

/*!
 * Replace the animated bone transforms of a shattered entity by its pieces' transforms.
 * \return true if the entity is shattered and the pose was applied
 */
bool applyDebrisPose(Entity & io, Skeleton & skeleton);

//! The entity of shattered pieces is being destroyed: keep the pieces on their own
void detachDebris(Entity & io);

//! After a simulation step: age the pieces, drop the old ones
void updateDebris();

//! Draw the pieces that no longer have an entity (before the entities are batched)
void renderDebris();

//! Remove every piece (level unload)
void clearDebris();

//! Number of shattered objects (with or without their entity)
size_t debrisCount();

} // namespace physics

#endif // ARX_PHYSICS_DEBRIS_H
