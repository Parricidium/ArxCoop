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

#ifndef ARX_COOP_PUPPETS_H
#define ARX_COOP_PUPPETS_H

/*!
 * Puppets are the local representation of the other players: human_base entities
 * without any script or AI, driven purely by the PlayerState messages we receive.
 *
 * Design rule: a puppet has no identity for the engine's AI. NPCs never target,
 * detect or remember a specific puppet; hostility only ever comes from the host's
 * synchronized world state.
 */
#include "coop/Protocol.h"
#include "math/Vector.h"

class Entity;

namespace coop {

//! Registers the network callbacks. Call once at startup.
void puppetsInit();

//! Sends our own state to the other players (rate limited). Call once per in-game frame.
void puppetsSendLocalState();

//! Creates / moves / removes puppet entities from the last received states. Call once per in-game frame.
void puppetsUpdate();

//! Draws the nicknames above the puppets. Call during 3D rendering with the game camera active.
void puppetsDrawNames();

//! Forgets every remote state and puppet (level change, session end).
void puppetsReset();

//! Host: eye position of the nearest player (ourselves or a puppet) to  from; player.pos otherwise.
Vec3f nearestPlayerEyePos(const Vec3f & from);

//! Owner of a puppet entity, or InvalidPlayerId.
PlayerId puppetOwner(const Entity & io);

/*!
 * NPC mirroring: the host streams the state of its NPCs (they only live there), clients
 * apply it and run no NPC AI or physics.
 */
//! Co-op death: the local player stays down until a teammate revives it, unless everyone is down.
bool localPlayerDowned();
bool allPlayersDowned();

void npcSyncSend();   //!< Host: streams changed NPC states
void npcSyncUpdate(); //!< Per in-game frame, both sides
bool npcsAreMirrored(); //!< True on a client whose world comes from the host

/*!
 * Developer aid (--coop-test): once in a level, steps the local player back after a few seconds,
 * takes an in-game screenshot and quits, so puppets can be checked without any input.
 */
extern bool g_puppetsTestMode;
void puppetsTestUpdate();

} // namespace coop

#endif // ARX_COOP_PUPPETS_H
