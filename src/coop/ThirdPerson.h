/*
 * Copyright 2026 ArxCoop contributors
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

#ifndef ARX_COOP_THIRDPERSON_H
#define ARX_COOP_THIRDPERSON_H

/*!
 * Optional third person view: an over-the-shoulder camera (the character sits on the left
 * third of the screen, or on the right after a shoulder swap), with an "orbit" mode where
 * the mouse turns the camera around the character instead of the character itself.
 *
 * Keys: CONTROLS_CUST_THIRDPERSON, CONTROLS_CUST_CAMERA_ORBIT, CONTROLS_CUST_SWAP_SHOULDER.
 */
#include "math/Vector.h"

namespace coop {

//! The camera is currently behind the character.
bool thirdPersonActive();

//! Third person with the mouse turning the camera only.
bool cameraOrbitActive();

//! Reads the toggle keys. Call once per in-game frame.
void thirdPersonHandleInput();

//! Mouse look while orbiting: the same rotation the first person code would apply to the player.
void cameraOrbitTurn(const Vec2f & rotation);

//! Places g_playerCamera behind the character (collision aware). Replaces the first person camera update.
void thirdPersonUpdateCamera();

//! Developer aid (--coop-test): drives the view without key presses.
void thirdPersonTestSet(bool thirdPerson, bool orbit, bool rightShoulder, float orbitYawOffset);

} // namespace coop

#endif // ARX_COOP_THIRDPERSON_H
