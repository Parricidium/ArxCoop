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

#ifndef ARX_COOP_QOL_H
#define ARX_COOP_QOL_H

/*!
 * Quality of life for co-op sessions: ping markers, teammate arrows and map icons,
 * latency display, downed-player alerts and bleed-out, giving items, reconnection spots.
 */
#include <string>
#include <vector>

#include "coop/Protocol.h"
#include "math/Vector.h"

class Entity;

namespace coop {

class Reader;

//! Registers the network callbacks. Call once at startup (after puppetsInit()).
void qolInit();

//! Session end / level change housekeeping.
void qolReset();

//! Per in-game frame: keys (ping), timers (bleed-out, autosave), alerts.
void qolUpdate();

//! 3D overlay (with the game camera): ping markers.
void qolDraw3D();

//! 2D overlay: off-screen teammate arrows, ping labels, downed countdown.
void qolDraw2D();

//! Where a teammate is, for the map and the arrows.
struct TeammateInfo {
	PlayerId id;
	std::string name;
	u32 area;
	Vec3f pos;   //!< entity (feet) position
	float yaw;   //!< entity yaw (NPC convention)
	bool downed;
	bool here;   //!< same area as us
};
std::vector<TeammateInfo> teammates();

//! Latency to the host (client) or 0; per-player latencies as last broadcast by the host.
u16 latencyOf(PlayerId id);

//! Give the item we are holding (combine mode) to a teammate's puppet.
bool giveItemToPuppet(Entity & item, const Entity & puppet);

//! Network handlers (wired by qolInit through the session / replication).
void handlePlayerMarker(PlayerId id, Reader & reader);
void handleLatencies(Reader & reader);
void handleGiveItem(PlayerId from, Reader & reader);

//! Give some of our gold to another player (false: not enough, or no such player)
bool giveGoldToPlayer(PlayerId to, long amount);
void handleGiveGold(PlayerId from, Reader & reader);

//! Downed local player: seconds left before the real death (0 when not downed / no limit).
float bleedOutSecondsLeft();
//! The bleed-out timer ran out: the normal death may proceed.
bool bledOut();

//! Host: remembers where a player was when it left, so a rejoin can put it back there.
void rememberLeavingPlayer(PlayerId id);
//! Host: a player is synced in a level: teleports it to its remembered spot, if any.
bool restoreRejoiningPlayer(PlayerId id); //!< true when a spot was sent

//! Developer aid (--coop-test): places a marker as if the key was pressed.
void qolTestPing();

/*!
 * One of our arrows stuck in the world: it can be picked up again. A one-arrow quiver item
 * (the "arrows" class, whose durability is its arrow count) drops there, shared like any
 * dropped item; picking it up refills a quiver we carry (Inventory::mergeArrows) or becomes
 * a new one.
 */
void arrowLanded(const Vec3f & pos, const Vec3f & direction);

} // namespace coop

#endif // ARX_COOP_QOL_H
