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

#ifndef ARX_COOP_ROLL_H
#define ARX_COOP_ROLL_H

/*!
 * Dodge roll (ArxCoop, JD's request, 16/09): a Dark Souls-like tumble in the direction of the
 * movement keys (forward by default), with a burst of speed, a short invulnerability window
 * and a cooldown. The character crouches into the roll and stands up out of it (the engine's
 * crouch animations, so the puppets play it too), the whole body somersaulting on the puppets
 * and in third person; in first person the view dips forward.
 */

#include "math/Angle.h"
#include "math/Vector.h"

class Entity;

namespace coop {

//! Per-frame: timing, invulnerability, sounds (before the player controls).
void rollUpdate();

//! Developer aid (--coop-fieldtest): acts as the roll key pressed this frame.
extern bool g_rollTestRequest;

//! True while the local player is rolling (attacks, jumps, casting are held back).
bool rollActive();

//! 0 = not rolling, else the progress of the roll (0..1), for the network and the visuals.
float rollPhase();

/*!
 * From the player controls: starts a roll on the key (direction from the movement keys held,
 * forward otherwise) and, while rolling, replaces the keys' movement by the roll's.
 * \a tm is the frame's movement to apply (in / out), \a unit the per-frame distance of one
 * "run forward" unit (10 * FD * MoveDiv in the controls code).
 * \return true when the roll took over the movement.
 */
bool rollMovement(bool forward, bool backward, bool left, bool right, float unit, Vec3f & tm);

/*!
 * From the player physics: while rolling, replaces the frame's movement impulse (whose size the
 * engine takes from the current animation - the crouch carries none) by the roll's burst.
 */
void rollImpulse(Vec3f & impulse);

//! First person: the view dips forward through the roll.
void rollCameraEffect(Anglef & angle);

/*!
 * The body somersault, for the puppets and the third-person view: given the entity's render
 * angle and position (feet), turns them into the tumbled ones for \a phase (0..1). No-op at 0.
 */
void rollTumble(float phase, Anglef & angle, Vec3f & pos);

//! The roll phase to show on an entity: the local player's, or a puppet's (from its player state).
float rollPhaseOf(const Entity & io);

} // namespace coop

#endif // ARX_COOP_ROLL_H
