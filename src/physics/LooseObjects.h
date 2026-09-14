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

#ifndef ARX_PHYSICS_LOOSEOBJECTS_H
#define ARX_PHYSICS_LOOSEOBJECTS_H

#include <cstddef>
#include <functional>

#include "math/Angle.h"
#include "math/Types.h"

class Entity;
struct EERIE_3DOBJ;

/*!
 * ArxModern: dropped and thrown objects as rigid bodies.
 *
 * The engine simulates a dropped object as a box of springs that never rotates
 * (EERIE_PHYSICS_BOX_*). When the physics world exists, the launch is taken over: the object
 * becomes a convex hull that tumbles, slides and comes to rest against the level, the fixed
 * entities (tables, doors, platforms) and the NPCs. The engine's box stays "active" for the
 * rest of the game (it is what makes an object un-pickable while flying) and is put to rest
 * exactly as the engine would when the body falls asleep.
 *
 * Scripted objects are never touched: only what the engine itself launches goes through here.
 */
namespace physics {

/*!
 * Called by EERIE_PHYSICS_BOX_Launch: the object of an entity was just dropped or thrown.
 * pos and angle are where the launch starts - not always the entity's own (a weapon dropped by
 * a dying NPC starts at the hand it was attached to). io may be null (looked up by obj then).
 */
void launchObject(EERIE_3DOBJ * obj, const Vec3f & pos, const Anglef & angle, const Vec3f & vect, Entity * io);

/*!
 * Called by ARX_PHYSICS_Apply for entities with an active box: move the entity to its body.
 * \return true if the entity is simulated here (the engine's box must be skipped)
 */
bool updateLooseObject(Entity & io);

//! Create the static bodies of the fixed entities of the level (after the level mesh)
void createObstacles();

//! Before a step: fixed entities that moved, the living NPCs around the moving bodies
void syncObstacles();

//! After a step: collision sounds, bodies that came to rest
void updateLooseObjects();

//! Drop the bodies of an entity being destroyed
void removeLooseObject(Entity & io);

//! Remove every body (level unload)
void clearLooseObjects();

//! Number of objects being simulated
size_t looseObjectCount();

//! Log the loose objects (tests)
void dumpLooseObjects();

/*
 * Mirroring (see Ragdoll.h): in mirror mode no object is simulated here, its position and
 * orientation come from mirrorLooseObject() and the engine's box is kept out of the way.
 */

//! State of a loose object received from the simulating machine; active = still moving
void mirrorLooseObject(Entity & io, const Vec3f & pos, const Anglef & angle, bool active);

//! Visit the objects simulated here (active = still moving)
void forEachLooseObject(const std::function<void(Entity & io, bool active)> & visit);

} // namespace physics

#endif // ARX_PHYSICS_LOOSEOBJECTS_H
