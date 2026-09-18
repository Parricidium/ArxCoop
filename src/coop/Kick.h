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

#ifndef ARX_COOP_KICK_H
#define ARX_COOP_KICK_H

/*!
 * The kick, after Dark Messiah of Might & Magic: a key sends the right foot forward; what
 * stands in front (a monster, a loose object) is shoved back - a monster is hurt a little and
 * thrown off balance, an object flies. Procedural animation of the leg (no kick animation in
 * the game's sets), seen on the puppets through their player state; the shove itself is done
 * by the host (PlayerKick) so that everybody sees the same thing.
 */
#include "coop/Protocol.h"
#include "math/Types.h"

class Entity;
struct EERIE_3DOBJ;
struct Skeleton;

namespace coop {

class Reader;

//! Registers the network callback. Call once at startup.
void kickInit();

//! Once per frame in game: the key, the phases, the shove at the moment of impact.
void kickUpdate();

//! Our own kick's progress, 0 = none, 1 = done.
float kickPhase();

//! Our stamina, 0..1: a kick costs a share of it, it comes back with time (see the local HUD).
float kickStamina();

//! The kick to show on an entity: the local player's, or a puppet's (from its player state).
float kickPhaseOf(const Entity & io);

//! Bends the right leg of the skeleton for the kick, in local space (called by the skeleton animation).
void kickPose(const Entity & io, EERIE_3DOBJ * obj, Skeleton & skeleton);

//! First person: the view lunges with the kick.
void kickCameraEffect(Anglef & angle);

//! Received a PlayerKick message from player \a id.
void handlePlayerKick(PlayerId id, Reader & reader);

/*!
 * A living NPC knocked down by a kick (Close combat 70 and up): its ragdoll flies, it lies
 * there until the ragdoll rests, then gets up. The NPC's own logic (movement, fighting,
 * physics) is on hold meanwhile - NPC.cpp asks.
 */
bool isKnockedDown(const Entity & io);

//! Test: the number of NPCs knocked down right now
size_t knockedDownCount();
//! Test: an NPC standing inside a wall is moved to the nearest clear spot; the distance moved (0 = it was fine)
float kickTestStandClear(Entity & npc);

//! Test: kick now (no key, no cooldown).
extern bool g_kickTestRequest;

} // namespace coop

#endif // ARX_COOP_KICK_H
