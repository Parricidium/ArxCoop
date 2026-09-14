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

#ifndef ARX_COOP_PHYSICSSYNC_H
#define ARX_COOP_PHYSICSSYNC_H

/*!
 * Host-authoritative physics (ArxModern's ragdolls and loose objects, src/physics/).
 *
 * The host simulates. Ten times a second it sends the pose of every ragdoll and the position
 * of every object still moving, plus one last state when something comes to rest, and a
 * keyframe of everything every few seconds for late joiners. Clients simulate nothing
 * (physics::setMirrorMode): their bones and objects follow the host.
 */
namespace coop {

//! Registers the network callback. Call once at startup.
void physicsSyncInit();

//! Per in-game frame, both sides
void physicsSyncUpdate();

} // namespace coop

#endif // ARX_COOP_PHYSICSSYNC_H
