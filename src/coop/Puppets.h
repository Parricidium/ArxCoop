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
#include <string>
#include <glm/gtc/quaternion.hpp>
#include "game/GameTypes.h"
#include "graphics/Color.h"
#include "math/Rectangle.h"
#include "math/Vector.h"

class Entity;
struct IO_HALO;

namespace coop {

//! Registers the network callbacks. Call once at startup.
void puppetsInit();

//! Sends our own state to the other players (rate limited). Call once per in-game frame.
void puppetsSendLocalState();

//! Creates / moves / removes puppet entities from the last received states. Call once per in-game frame.
void puppetsUpdate();

//! Draws the nicknames above the puppets. Call during 3D rendering with the game camera active.
void puppetsDrawNames();
void partyHudDraw(); //!< 2D overlay: game clock, teammates' life and hunger

/*!
 * In a co-op session the local player's life orb is replaced by the same name / life / hunger
 * bars the teammates get, so everyone reads the same HUD.
 */
bool localHudActive();
constexpr Vec2f LocalHudSize = Vec2f(160.f, 44.f); //!< unscaled, takes the orb's place at the bottom left
void localHudDraw(const Rectf & rect, float scale, Color lifeColor, float life);

//! Forgets every remote state and puppet (level change, session end).
void puppetsReset();

//! Third person: shows the local player's lit torch at the hip (a display copy). Call once per in-game frame.
void localTorchDisplayUpdate();

//! Host: eye position of the nearest player (ourselves or a puppet) to a position; player.pos otherwise.
Vec3f nearestPlayerEyePos(const Vec3f & from);

/*!
 * Host: the entity an NPC attacking "the player" should actually hit: the nearest standing
 * player (our own entity or a teammate's puppet). Anything else is returned unchanged.
 * NPC AI and scripts only ever know one player; this is where the blow gets its real victim.
 */
EntityHandle attackTarget(const Entity & npc, EntityHandle target);

//! Owner of a puppet entity, or InvalidPlayerId.
PlayerId puppetOwner(const Entity & io);

//! Progress of a puppet's dodge roll (0 = none), from its player state.
float puppetRollPhase(const Entity & puppet);
//! The kick phase of a puppet from its last player state (0 = none), see coop/Kick.cpp
float puppetKickPhase(const Entity & puppet);

//! The yaw of a puppet's dodge roll direction (player.angle convention), from its player state.
float puppetRollYaw(const Entity & puppet);

//! Halo of a puppet's worn helmet (0), armor (1) or leggings (2) when it glows, else null (AnimationRender.cpp).
IO_HALO * puppetSlotHalo(const Entity & puppet, unsigned slot);

//! The puppet of a player in this level, or null
Entity * puppetOf(PlayerId id);
//! The entity id of a player's puppet (coop_player_N), whether it exists here or not
std::string puppetIdString(PlayerId id);

//! Host: is a teammate (in this level) within \a limit of \a pos?
bool teammateWithin(const Vec3f & pos, float limit);

/*!
 * A hit drew blood here: the others replay the same effect on their copy of the victim
 * (a mirrored NPC, or a player through its puppet). \a effects: 1 splat + decal, 2 bleeding.
 */
void bloodSpawned(const Entity & target, const Vec3f & pos, const Vec3f & sourcePos, float dmgs, Color color, u8 effects);

/*!
 * NPC mirroring: the host streams the state of its NPCs (they only live there), clients
 * apply it and run no NPC AI or physics.
 */
//! The local player launched a spell: the others see it cast by our puppet (visual only there).
void spellCast(unsigned spell, float level, unsigned flags, const Entity * target, long long durationUs);

/*!
 * Aimed spells and missiles fly the same path everywhere: right before a spell's Launch()
 * (ARX_SPELLS_Launch), our own cast draws a random seed and the spell reports where its
 * missiles started; both travel with the SpellCast message and are handed to the same
 * spell code on the other machines.
 */
void castStarting(const Entity & source);
//! The seed of the cast being launched, 0 when none (an NPC's cast: the spell keeps its own randomness).
unsigned castSeed();
//! Another player's cast: \a origin becomes where their missiles started; false for our own casts and NPCs'.
bool castOrigin(Vec3f & origin);
//! Our own cast: where the missiles started (sent with the cast).
void castOriginUsed(const Vec3f & origin);

//! The local player shot an arrow: the others see it fly from our puppet (no damage there).
void projectileFired(const Vec3f & pos, const Vec3f & vect, float gravity, const glm::quat & rotation, bool fiery);

//! Co-op death: the local player stays down until a teammate revives it, unless everyone is down.
bool localPlayerDowned();
bool localPlayerDownedRaw(); //!< same, ignoring the bleed-out
bool allPlayersDowned();

//! The downed teammate we are looking at from close by, or InvalidPlayerId.
PlayerId lookedAtDownedTeammate();

//! A teammate in this level is locked in a cinematic dialogue (InvalidPlayerId if none).
PlayerId teammateInDialogue();
//! Option "dialogue_hold": our controls stay frozen while a teammate talks to an NPC.
bool dialogueHold();
//! Asks the host (or tells the client) to get a teammate back up.
void reviveTeammate(PlayerId target);
//! Admin: gets our own character back up (host healing itself).
void adminReviveLocal();

void npcSyncSend();   //!< Host: streams changed NPC states
void npcSyncUpdate(); //!< Per in-game frame, both sides
bool npcsAreMirrored(); //!< True on a client whose world comes from the host

/*!
 * Developer aid (--coop-test): once in a level, steps the local player back after a few seconds,
 * takes an in-game screenshot and quits, so puppets can be checked without any input.
 */
extern bool g_puppetsTestMode;
extern bool g_puppetsTestLean; //!< --coop-test: acts as a held "lean left" key
extern int g_puppetsTestLevel; //!< --coop-fieldtest: level the host jumps to (-1: none)
extern std::string g_puppetsTestTarget; //!< --coop-fieldtest: arrival marker in that level
void puppetsTestUpdate();

} // namespace coop

#endif // ARX_COOP_PUPPETS_H
