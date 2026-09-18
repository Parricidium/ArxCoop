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
#include <string_view>
#include <unordered_map>

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
#include "gui/Speech.h"
#include "physics/Collisions.h"
#include "physics/LooseObjects.h"
#include "physics/Physics.h"
#include "physics/Ragdoll.h"
#include "platform/Time.h"
#include "scene/Interactive.h"
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
constexpr float PushForce = 150.f;        //!< the shove of a monster below the skill (world units of forced move, see NPC.cpp)
constexpr float KnockdownSkill = 70.f;    //!< Close combat from which a kick knocks a monster down (ragdoll)
constexpr float KnockdownSpeed = 6.f;     //!< m/s given to the ragdoll (4 to 5 m of flight in the open)
constexpr float KnockdownLift = 0.45f;    //!< upward share of it (the body flies rather than skids)
constexpr float ImmuneHeight = 200.f;     //!< world units: taller than this (a human is 180) = too big to knock down
constexpr float ImmuneRadius = 45.f;      //!< ... or wider than this (a human is 30)
constexpr float ImpactSpeed = 3.f;        //!< m/s of horizontal speed lost in one frame from which a ragdoll takes damage (a wall)
constexpr float ImpactDamage = 2.f;       //!< hit points per m/s lost beyond that
constexpr GameDuration ImpactGrace = std::chrono::milliseconds(150); //!< right after the kick the bodies settle: no damage yet
constexpr GameDuration KnockdownMax = std::chrono::seconds(12); //!< up (or dead) after this whatever the ragdoll does
constexpr GameDuration GetUpBlend = std::chrono::milliseconds(500);
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

//! A monster knocked down (host, or single player)
struct Knockdown {
	GameInstant since;
	float lastSpeed = 0.f; //!< horizontal speed of the pelvis last frame, m/s
	bool hadCollisions = true;
};
std::unordered_map<Entity *, Knockdown> g_down;

//! Monsters too big to be knocked down whatever their cylinder says, by class name
constexpr std::string_view ImmuneClasses[] = { "black_beast", "golem", "dragon", "worm", "akbaa", "demon" };

//! Too big (or too special) to be thrown around: it takes the short shove instead
bool immuneToKnockdown(const Entity & npc) {
	if(std::abs(npc.physics.cyl.height) > ImmuneHeight || npc.physics.cyl.radius > ImmuneRadius) {
		return true;
	}
	std::string_view path = npc.classPath().string();
	for(std::string_view name : ImmuneClasses) {
		if(path.find(name) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

//! Only a monster in a fight is thrown: townsfolk and unaware creatures just stagger
bool fighting(const Entity & npc) {
	return npc._npcdata && (npc._npcdata->behavior & (BEHAVIOUR_FIGHT | BEHAVIOUR_DISTANT | BEHAVIOUR_MAGIC | BEHAVIOUR_FLEE));
}

//! In a scripted dialogue (a cinematic line, not a battle cry): nothing touches it
bool talking(const Entity & npc) {
	const Speech * speech = getSpeechForEntity(npc);
	return speech && (speech->cine.type != ARX_CINE_SPEECH_NONE || (speech->flags & ARX_SPEECH_FLAG_UNBREAKABLE));
}

void startKnockdown(Entity & npc) {
	Knockdown & down = g_down[&npc];
	down.since = g_gameTime.now();
	down.lastSpeed = 0.f;
	down.hadCollisions = !(npc.ioflags & IO_NO_COLLISIONS);
	npc.ioflags |= IO_NO_COLLISIONS; // (the body lies on the floor: the standing cylinder must not block)
}

//! How far the NPC's standing cylinder is from a valid spot at  pos (0 = fine, > 0 = that deep in something)
float standingClearance(Entity & npc, const Vec3f & pos) {
	Cylinder cyl(pos, npc.physics.cyl.radius, npc.physics.cyl.height);
	float anything = CheckAnythingInCylinder(cyl, &npc, CFLAG_NPC | CFLAG_JUST_TEST | CFLAG_NO_INTERCOL);
	return anything < 0.f ? -anything : 0.f;
}

/*!
 * A body that came to rest against a wall gets up with its cylinder inside the wall (JD's
 * screenshot): if the resting spot is blocked, the nearest clear spot around is used.
 */
void standSomewhereClear(Entity & npc) {
	constexpr float Blocked = 30.f; // units the cylinder would have to rise: more than a floor bump
	if(standingClearance(npc, npc.pos) <= Blocked) {
		return;
	}
	Vec3f best = npc.pos;
	float bestScore = 1e9f;
	for(float radius : { 30.f, 60.f, 90.f, 130.f, 180.f }) {
		for(int k = 0; k < 12; k++) {
			float angle = float(k) * (2.f * glm::pi<float>() / 12.f);
			Vec3f candidate = npc.pos + Vec3f(std::cos(angle) * radius, 0.f, std::sin(angle) * radius);
			float clearance = standingClearance(npc, candidate);
			if(clearance <= Blocked) {
				float score = radius + clearance; // the nearest clear spot, the clearest among equals
				if(score < bestScore) {
					bestScore = score;
					best = candidate;
				}
			}
		}
		if(bestScore < 1e9f) {
			break;
		}
	}
	if(bestScore < 1e9f) {
		LogInfo << "[coop] kick: " << npc.idString() << " was in a wall, stands " << int(glm::distance(best, npc.pos)) << " units away";
		ARX_INTERACTIVE_Teleport(&npc, best, false);
	}
}

void endKnockdown(Entity & npc, bool getUp) {
	auto it = g_down.find(&npc);
	if(it == g_down.end()) {
		return;
	}
	if(getUp) {
		physics::endRagdoll(npc, GetUpBlend); // (keeps the lying pose for the blend)
		if(it->second.hadCollisions) {
			npc.ioflags &= ~IO_NO_COLLISIONS;
		}
		standSomewhereClear(npc);
		LogInfo << "[coop] kick: " << npc.idString() << " gets up";
	}
	g_down.erase(it);
}

//! Host / single player, every frame: the monsters on the floor
void knockdownUpdate() {
	for(auto it = g_down.begin(); it != g_down.end(); ) {
		Entity * npc = it->first;
		if(!ValidIOAddress(npc) || !(npc->ioflags & IO_NPC)) {
			it = g_down.erase(it);
			continue;
		}
		Knockdown & down = it->second;
		++it;
		if(IsDeadNPC(*npc)) {
			endKnockdown(*npc, false); // (the ragdoll stays as the corpse)
			continue;
		}
		if(!physics::hasRagdoll(*npc)) {
			endKnockdown(*npc, true);
			continue;
		}
		// Hitting something hard: the horizontal speed the pelvis loses in one frame beyond a
		// threshold hurts (a wall); landing on the floor loses vertical speed, which is free
		Vec3f velocity = physics::ragdollVelocity(*npc);
		float speed = glm::length(Vec2f(velocity.x, velocity.z));
		float lost = down.lastSpeed - speed;
		down.lastSpeed = speed;
		GameDuration age = g_gameTime.now() - down.since;
		if(lost > ImpactSpeed && age > ImpactGrace) {
			float damage = (lost - ImpactSpeed) * ImpactDamage;
			Vec3f at = npc->pos;
			damageNpc(*npc, damage, nullptr, nullptr, DAMAGE_TYPE_GENERIC, &at);
			LogInfo << "[coop] kick: " << npc->idString() << " hits something at " << lost << " m/s, " << damage << " damage";
			if(IsDeadNPC(*npc)) {
				endKnockdown(*npc, false);
				continue;
			}
		}
		if(age > GameDuration(std::chrono::milliseconds(700)) && physics::ragdollResting(*npc)) {
			endKnockdown(*npc, true);
		} else if(age > KnockdownMax) {
			// Still falling: a bottomless pit, the monster is gone
			LogInfo << "[coop] kick: " << npc->idString() << " never landed, dies";
			damageNpc(*npc, 100000.f, nullptr, nullptr, DAMAGE_TYPE_GENERIC, nullptr);
			endKnockdown(*npc, false);
		}
	}
}
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

	if(!targets.npc) {
		// (diagnostics: the nearest monster and why it was out of reach)
		for(Entity & io : entities) {
			if((io.ioflags & IO_NPC) && &io != entities.player() && !io.coopPuppet && !IsDeadNPC(io) && closerThan(io.pos, from, 300.f)) {
				Vec3f to = io.pos - from;
				LogInfo << "[coop] kick: nothing in front; " << io.idString() << " is " << int(glm::length(Vec2f(to.x, to.z)))
				        << " away, dy " << int(to.y) << ", facing " << glm::dot(glm::normalize(Vec3f(to.x, 0.f, to.z)), forward)
				        << ", show " << int(io.show) << ", nocol " << ((io.ioflags & IO_NO_COLLISIONS) ? 1 : 0);
			}
		}
	}

	if(Entity * npc = targets.npc) {
		Vec3f hit = npc->pos + Vec3f(0.f, -std::abs(npc->physics.cyl.height) * 0.5f, 0.f);
		if(talking(*npc)) {
			// Mid-line: a dull thud, nothing else
			ARX_SOUND_PlayCollision("flesh", "stone", 0.6f, 1.f, hit, kicker);
			LogInfo << "[coop] kick: " << npc->idString() << " is talking, untouched";
			return;
		}
		// The kicker's skill: a client's travels with its stats (PlayerStats), ours is at hand
		const PlayerStats * stats = actingPlayerStats();
		float skill = stats ? stats->closeCombat : player.m_skillFull.closeCombat;
		bool immune = immuneToKnockdown(*npc);
		bool knocked = false;
		if(skill >= KnockdownSkill && !immune && fighting(*npc) && !g_down.count(npc) && !physics::hasRagdoll(*npc)) {
			// Its own obstacle cylinder must be gone from the physics world before the bodies
			// are thrown, or they start inside it and lose their speed at once
			startKnockdown(*npc);
			physics::syncObstacles();
			knocked = physics::knockDown(*npc, glm::normalize(forward + Vec3f(0.f, -KnockdownLift, 0.f)), KnockdownSpeed, hit); // (y down: -y is up)
			if(!knocked) {
				if(g_down[npc].hadCollisions) {
					npc->ioflags &= ~IO_NO_COLLISIONS;
				}
				g_down.erase(npc);
			}
		}
		if(!knocked) {
			// Thrown off balance: the engine's forced move (what a weapon blow does), a metre or two
			npc->forcedmove += forward * (immune ? PushForce * 0.3f : PushForce);
		}
		ARX_SOUND_PlayCollision("flesh", immune ? "stone" : "flesh", 1.f, 1.f, hit, kicker);
		// No damage: the hit event alone, for the stagger and the anger
		damageNpc(*npc, 0.f, entities.player(), nullptr, DAMAGE_TYPE_GENERIC, &hit);
		LogInfo << "[coop] kick: " << npc->idString() << (knocked ? " knocked down" : immune ? " too big, nudged" : " shoved")
		        << " (close combat " << skill << ")";
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

	if(!npcsAreMirrored() && !g_down.empty()) {
		knockdownUpdate();
	}

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

bool isKnockedDown(const Entity & io) {
	return !g_down.empty() && g_down.count(const_cast<Entity *>(&io)) != 0;
}

size_t knockedDownCount() {
	return g_down.size();
}

float kickTestStandClear(Entity & npc) {
	Vec3f before = npc.pos;
	LogInfo << "[coop] test: clearance at " << int(before.x) << "," << int(before.y) << "," << int(before.z) << " = " << standingClearance(npc, before);
	standSomewhereClear(npc);
	return glm::distance(npc.pos, before);
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
