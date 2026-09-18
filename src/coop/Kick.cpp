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

#include "coop/Kick.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "animation/Skeleton.h"
#include "coop/Puppets.h"
#include "coop/Replication.h"
#include "coop/Roll.h"
#include "coop/Session.h"
#include "coop/ThirdPerson.h"
#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Damage.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "graphics/Math.h"
#include "graphics/data/Mesh.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "physics/Physics.h"
#include "platform/Time.h"
#include "scene/GameSound.h"
#include "scene/Object.h"

extern bool BLOCK_PLAYER_CONTROLS;
extern bool EXTERNALVIEW;

namespace coop {

namespace {

// Tunables (JD judges the feel)
constexpr PlatformDuration KickDuration = std::chrono::milliseconds(560);
constexpr PlatformDuration KickCooldown = std::chrono::milliseconds(350); //!< after the kick ends
constexpr float ImpactPhase = 0.34f;      //!< when the foot lands
constexpr float KickReach = 135.f;        //!< world units from the player's feet, plus the target's radius
constexpr float KickHalfAngle = 40.f;     //!< degrees either side of the facing
constexpr float KickForce = 330.f;        //!< the shove of a monster (world units of forced move, see NPC.cpp)
constexpr float KickDamageBase = 3.f;     //!< hit points, plus a share of the strength
constexpr float KickDamageStrength = 0.2f;
constexpr float ObjectSpeed = 1.2f;       //!< a loose object flies off at this (throw units)
constexpr float StaminaCost = 0.34f;      //!< share of the stamina a kick takes (three in a row, then wait)
constexpr float StaminaRegen = 0.28f;     //!< per second, once the delay below has passed
constexpr PlatformDuration StaminaDelay = std::chrono::milliseconds(900); //!< after a kick, before it comes back
constexpr float ViewLunge = 22.f;         //!< degrees, first-person forward dip at the impact (brings the boot into view)

// The leg, degrees. Thigh: forward is positive; knee: bent is positive.
constexpr float ThighBack = -18.f;        //!< wind-up
constexpr float ThighKick = 82.f;         //!< the kick
constexpr float KneeBent = 75.f;          //!< at the wind-up
constexpr float KneeReturn = 35.f;        //!< coming back down

bool g_kicking = false;
bool g_hitDone = false;
float g_stamina = 1.f;
PlatformInstant g_start;
PlatformInstant g_end;
Vec3f g_direction(0.f);

//! The cone in front of the kicker: the monster (or object) it lands on
struct KickResult {
	Entity * npc = nullptr;
	std::vector<Entity *> objects;
};

KickResult findTargets(const Vec3f & from, const Vec3f & forward) {
	KickResult result;
	float bestNpc = 1e9f;
	float cosLimit = std::cos(glm::radians(KickHalfAngle));
	for(Entity & io : entities) {
		if(&io == entities.player() || io.coopPuppet || io.show != SHOW_FLAG_IN_SCENE || !io.obj) {
			continue;
		}
		bool npc = (io.ioflags & IO_NPC) != 0;
		bool object = (io.ioflags & IO_ITEM) && io.obj->pbox;
		if(!npc && !object) {
			continue;
		}
		if(npc && (IsDeadNPC(io) || (io.ioflags & IO_NO_COLLISIONS))) {
			continue;
		}
		Vec3f to = io.pos - from;
		float dy = to.y;
		to.y = 0.f;
		float dist = glm::length(to);
		float radius = npc ? io.physics.cyl.radius : 20.f;
		if(dist > KickReach + radius || std::abs(dy) > (npc ? 140.f : 90.f)) {
			continue;
		}
		if(dist > 1.f && glm::dot(to / dist, forward) < cosLimit) {
			continue;
		}
		if(npc) {
			if(dist < bestNpc) {
				bestNpc = dist;
				result.npc = &io;
			}
		} else {
			result.objects.push_back(&io);
		}
	}
	return result;
}

/*!
 * The shove: the host's side (or single player). \a kicker is the player entity or a puppet,
 * \a from its feet, \a forward its facing on the ground.
 */
void resolveKick(Entity * kicker, const Vec3f & from, const Vec3f & forward) {

	KickResult targets = findTargets(from, forward);

	if(Entity * npc = targets.npc) {
		// Thrown off balance: the engine's forced move (what a weapon blow does, much stronger)
		npc->forcedmove += forward * KickForce;
		// The kicker's strength: a client's travels with its stats (PlayerStats), ours is at hand
		const PlayerStats * stats = actingPlayerStats();
		float strength = stats ? stats->strength : player.m_attributeFull.strength;
		float damage = KickDamageBase + strength * KickDamageStrength;
		Vec3f hit = npc->pos + Vec3f(0.f, -npc->physics.cyl.height * 0.5f, 0.f);
		ARX_SOUND_PlayCollision("flesh", "flesh", 1.f, 1.f, hit, kicker);
		damageNpc(*npc, damage, entities.player(), nullptr, DAMAGE_TYPE_GENERIC, &hit);
		LogInfo << "[coop] kick: " << npc->idString() << " shoved for " << damage;
	}

	for(Entity * item : targets.objects) {
		// A loose object is sent flying, the way a thrown item is (replicated as one)
		Vec3f direction = glm::normalize(forward + Vec3f(0.f, -0.45f, 0.f)) * ObjectSpeed;
		EERIE_PHYSICS_BOX_Launch(item->obj, item->pos, item->angle, direction, item);
		itemDropped(*item, true, direction);
		ARX_SOUND_PlayCollision("flesh", "wood", 0.7f, 1.f, item->pos, kicker);
	}

}

void sendKick(const Vec3f & from, const Vec3f & forward) {
	if(!g_coop.isActive()) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.f32_(from.x);
	writer.f32_(from.y);
	writer.f32_(from.z);
	writer.f32_(forward.x);
	writer.f32_(forward.y);
	writer.f32_(forward.z);
	g_coop.sendToOthers(MessageType::PlayerKick, writer);
}

void endKick() {
	g_kicking = false;
	g_end = g_platformTime.frameStart();
}

//! The leg angles (degrees) through the kick
void legAngles(float phase, float & thigh, float & knee) {
	if(phase < 0.14f) {
		float t = phase / 0.14f;
		thigh = ThighBack * t;
		knee = KneeBent * t;
	} else if(phase < ImpactPhase) {
		float t = (phase - 0.14f) / (ImpactPhase - 0.14f);
		t = 1.f - (1.f - t) * (1.f - t); // fast out
		thigh = ThighBack + (ThighKick - ThighBack) * t;
		knee = KneeBent * (1.f - t);
	} else if(phase < 0.5f) {
		thigh = ThighKick;
		knee = 0.f;
	} else {
		float t = (phase - 0.5f) / 0.5f;
		t = t * t * (3.f - 2.f * t);
		thigh = ThighKick * (1.f - t);
		knee = KneeReturn * std::sin(t * glm::pi<float>());
	}
}

} // anonymous namespace

bool g_kickTestRequest = false;

void kickInit() {
	g_coop.onPlayerKick = handlePlayerKick;
}

float kickStamina() {
	return g_stamina;
}

float kickPhase() {
	if(!g_kicking) {
		return 0.f;
	}
	float phase = float(toMsf(g_platformTime.frameStart() - g_start)) / float(toMsf(KickDuration));
	return glm::clamp(phase, 0.f, 1.f);
}

float kickPhaseOf(const Entity & io) {
	if(&io == entities.player()) {
		return kickPhase();
	}
	if(io.coopPuppet) {
		return puppetKickPhase(io);
	}
	return 0.f;
}

void kickUpdate() {

	if(!entities.player() || !entities.player()->obj || ARXmenu.mode() != Mode_InGame) {
		return;
	}

	// The stamina comes back once the last kick is behind (not while kicking)
	if(!g_kicking && g_stamina < 1.f && g_platformTime.frameStart() - g_start >= StaminaDelay) {
		g_stamina = std::min(1.f, g_stamina + StaminaRegen * toMsf(g_platformTime.lastFrameDuration()) * 0.001f);
	}

	if(!g_kicking) {
		bool pressed = GInput->actionNowPressed(CONTROLS_CUST_KICK) || g_kickTestRequest;
		bool test = g_kickTestRequest;
		g_kickTestRequest = false;
		if(!pressed || (BLOCK_PLAYER_CONTROLS && !test)
		   || player.jumpphase != NotJumping || player.levitate || player.climbing || player.lifePool.current <= 0.f
		   || player.doingmagic || (player.Interface & (INTER_PLAYERBOOK | INTER_INVENTORYALL))
		   || rollActive() || g_platformTime.frameStart() - g_end < KickCooldown) {
			return;
		}
		if(g_stamina < StaminaCost - 0.001f && !test) {
			return; // out of breath: the bar in the HUD says so
		}
		g_stamina = std::max(0.f, g_stamina - StaminaCost);
		g_kicking = true;
		g_hitDone = false;
		g_start = g_platformTime.frameStart();
		g_direction = angleToVectorXZ(player.angle.getYaw());
		ARX_SOUND_PlaySFX(g_snd.WHOOSH, &player.pos, 0.9f);
		LogDebug("kick started");
		return;
	}

	if(player.lifePool.current <= 0.f) {
		endKick();
		return;
	}

	float phase = kickPhase();
	if(phase >= ImpactPhase && !g_hitDone) {
		g_hitDone = true;
		Vec3f from = player.basePosition();
		if(npcsAreMirrored()) {
			sendKick(from, g_direction); // the host shoves (the monsters and objects live there)
		} else {
			resolveKick(entities.player(), from, g_direction);
			sendKick(from, g_direction); // (the others hear it)
		}
	}
	if(phase >= 1.f) {
		endKick();
	}

}

void kickPose(const Entity & io, EERIE_3DOBJ * obj, Skeleton & skeleton) {

	// (first person too: the body is drawn, the boot comes up into the view with the dip)
	float phase = kickPhaseOf(io);
	if(phase <= 0.f || !obj) {
		return;
	}

	// The right leg's bones, looked up once per mesh
	struct Leg { VertexGroupId hip, knee; };
	static std::map<const EERIE_3DOBJ *, Leg> s_legs;
	auto it = s_legs.find(obj);
	if(it == s_legs.end()) {
		Leg leg { EERIE_OBJECT_GetGroup(obj, "right_hip"), EERIE_OBJECT_GetGroup(obj, "right_knee") };
		it = s_legs.emplace(obj, leg).first;
	}
	const Leg & leg = it->second;
	if(!leg.hip) {
		return;
	}

	float thigh, knee;
	legAngles(phase, thigh, knee);
	// In the bone's own frame (the object's axes at rest): the thigh swings about the sideways
	// axis (a negative angle about X sends it forward, measured with --kicktest), the knee bends
	// back about the same
	skeleton.bones[leg.hip].init.quat = skeleton.bones[leg.hip].init.quat
	                                    * glm::angleAxis(glm::radians(-thigh), Vec3f(1.f, 0.f, 0.f));
	if(leg.knee) {
		skeleton.bones[leg.knee].init.quat = skeleton.bones[leg.knee].init.quat
		                                     * glm::angleAxis(glm::radians(knee), Vec3f(1.f, 0.f, 0.f));
	}

}

void kickCameraEffect(Anglef & angle) {
	float phase = kickPhase();
	if(phase <= 0.f) {
		return;
	}
	// A dip that peaks at the impact and eases back
	float s = phase < ImpactPhase ? phase / ImpactPhase : 1.f - (phase - ImpactPhase) / (1.f - ImpactPhase);
	s = s * s * (3.f - 2.f * s);
	angle.setPitch(angle.getPitch() + ViewLunge * s);
}

void handlePlayerKick(PlayerId id, Reader & reader) {
	Vec3f from = reader.vec3<Vec3f>();
	Vec3f forward = reader.vec3<Vec3f>();
	Entity * puppet = puppetOf(id);
	if(puppet) {
		ARX_SOUND_PlaySFX(g_snd.WHOOSH, &puppet->pos, 0.9f);
	}
	if(!g_coop.isHost()) {
		return; // the monsters and objects are mirrored from the host here
	}
	LogInfo << "[coop] kick from player " << int(id) << " at " << int(from.x) << "," << int(from.y) << "," << int(from.z);
	PuppetActorScope actor(puppet);
	resolveKick(puppet ? puppet : entities.player(), from, forward);
}

} // namespace coop
