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

#ifndef ARX_PHYSICS_PHYSICSWORLD_H
#define ARX_PHYSICS_PHYSICSWORLD_H

/*!
 * ArxModern: rigid body physics (Jolt) next to the engine's own collision code.
 *
 * The engine keeps handling the player, the NPCs and everything the scripts move; Jolt only
 * simulates what the engine used to leave frozen or faked: corpses (ragdolls, see Ragdoll.h)
 * and, later, loose objects. The level geometry is a static mesh in the Jolt world.
 *
 * Units: Jolt works in metres, the engine in centimetres (1 unit = 1 cm); the world keeps the
 * engine's axes (y down) with gravity along +y, so rotations pass through unchanged.
 *
 * Everything here is a no-op when built without Jolt (ARX_HAVE_JOLT) or when
 * [video] physics is off.
 */
class Entity;

namespace physics {

//! true if the game was built with Jolt
bool isAvailable();

//! true if a physics world exists for the current level (built with Jolt, enabled, level loaded)
bool isActive();

//! Create the Jolt runtime (once, at startup)
void init();

//! Destroy the Jolt runtime
void shutdown();

//! Build the physics world of the level that was just loaded (level mesh)
void levelLoaded();

//! Destroy the physics world of the level being unloaded
void levelCleared();

//! Advance the simulation by the last frame's game time and write the results back to the entities
void update();

//! Drop everything simulated for an entity that is being destroyed
void onEntityDestroyed(Entity & io);

//! Write what is being simulated to the log (tests)
void dumpState();

} // namespace physics

#endif // ARX_PHYSICS_PHYSICSWORLD_H
