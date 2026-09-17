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

#include "coop/Roll.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "animation/Animation.h"
#include "coop/Puppets.h"
#include "coop/ThirdPerson.h"
#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "graphics/Math.h"
#include "gui/Interface.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "platform/Time.h"
#include "scene/GameSound.h"

extern bool BLOCK_PLAYER_CONTROLS;
extern bool EXTERNALVIEW;

namespace coop {

namespace {

// Tunables (JD judges the feel: duration, distance, the invulnerability window)
constexpr PlatformDuration RollDuration = std::chrono::milliseconds(620);
constexpr PlatformDuration RollCooldown = std::chrono::milliseconds(250);  //!< after the roll ends
constexpr float RollSpeed = 26.f;        //!< in "run forward" units (10 = the normal run)
constexpr float RollSpeedEnd = 8.f;      //!< the burst eases down to this by the end
constexpr float InvulnerableFrom = 0.08f; //!< phase window without damage (the Dark Souls i-frames)
constexpr float InvulnerableTo = 0.62f;
constexpr float TumbleCentre = 62.f;     //!< height of the body's centre above the feet while balled up
constexpr float ViewDip = 32.f;          //!< degrees, first-person forward dip at mid-roll

bool g_rolling = false;
PlatformInstant g_start;
PlatformInstant g_end;
Vec3f g_direction(0.f);
float g_turn = 0.f; //!< degrees off the facing, see rollTurn()
bool g_invulnerable = false; //!< we set the flag (never clear one set by a script)
bool g_landed = false;

} // anonymous namespace

bool g_rollTestRequest = false;
float g_rollTestTurn = 0.f;

bool rollActive() {
	return g_rolling;
}

float rollPhase() {
	if(!g_rolling) {
		return 0.f;
	}
	float phase = float(toMsf(g_platformTime.frameStart() - g_start)) / float(toMsf(RollDuration));
	return glm::clamp(phase, 0.001f, 1.f);
}

float rollTurn() {
	return g_rolling ? g_turn : 0.f;
}

float rollDirectionYaw() {
	return g_rolling ? vectorToAngle(g_direction).getYaw() : 0.f;
}

static void endRoll() {
	if(g_invulnerable) {
		player.playerflags &= ~PLAYERFLAGS_INVULNERABILITY;
		g_invulnerable = false;
	}
	g_rolling = false;
	g_end = g_platformTime.frameStart();
}

void rollUpdate() {
	if(!g_rolling) {
		return;
	}
	if(!entities.player() || player.lifePool.current <= 0.f || BLOCK_PLAYER_CONTROLS) {
		endRoll();
		return;
	}
	float phase = rollPhase();
	bool window = phase >= InvulnerableFrom && phase <= InvulnerableTo;
	if(window && !g_invulnerable && !(player.playerflags & PLAYERFLAGS_INVULNERABILITY)) {
		player.playerflags |= PLAYERFLAGS_INVULNERABILITY;
		g_invulnerable = true;
	} else if(!window && g_invulnerable) {
		player.playerflags &= ~PLAYERFLAGS_INVULNERABILITY;
		g_invulnerable = false;
	}
	if(phase > 0.55f && !g_landed) {
		g_landed = true;
		ARX_NPC_NeedStepSound(entities.player(), player.basePosition(), ARX_NPC_AUDIBLE_VOLUME_DEFAULT * 1.3f,
		                      ARX_NPC_AUDIBLE_FACTOR_DEFAULT); // the body hits the ground out of the tumble
	}
	if(phase >= 1.f) {
		endRoll();
	}
}

bool rollMovement(bool forward, bool backward, bool left, bool right, float unit, Vec3f & tm) {

	if(!g_rolling) {
		bool pressed = GInput->actionNowPressed(CONTROLS_CUST_ROLL) || g_rollTestRequest;
		bool test = g_rollTestRequest;
		g_rollTestRequest = false;
		if(!pressed || !entities.player() || BLOCK_PLAYER_CONTROLS
		   || player.jumpphase != NotJumping || player.levitate || player.climbing || player.lifePool.current <= 0.f
		   || player.doingmagic || (player.Interface & (INTER_PLAYERBOOK | INTER_INVENTORYALL))
		   || g_platformTime.frameStart() - g_end < RollCooldown) {
			return false;
		}
		// Locked at the start, like a Dark Souls roll: the keys held, forward when none
		Vec3f dir(0.f);
		float yaw = player.angle.getYaw();
		if(forward) {
			dir += angleToVectorXZ(yaw);
		}
		if(backward) {
			dir += angleToVectorXZ_180offset(yaw);
		}
		if(left) {
			dir += angleToVectorXZ(yaw + 90.f);
		}
		if(right) {
			dir += angleToVectorXZ(yaw - 90.f);
		}
		if(test) {
			dir = angleToVectorXZ(yaw + g_rollTestTurn);
		}
		if(arx::length2(dir) < 0.01f) {
			dir = angleToVectorXZ(yaw);
		}
		g_direction = glm::normalize(dir);
		g_turn = AngleDifference(yaw, vectorToAngle(g_direction).getYaw());
		g_rolling = true;
		g_landed = false;
		g_start = g_platformTime.frameStart();
		ARX_SOUND_PlaySFX(g_snd.WHOOSH, &player.pos, 1.2f);
		LogDebug("roll started");
	}

	// The burst of speed, easing down through the roll
	float phase = rollPhase();
	float speed = RollSpeed + (RollSpeedEnd - RollSpeed) * phase * phase;
	tm = g_direction * (speed * unit);

	// Crouched into the ball: the engine's crouch animation (and cylinder), the puppets get it
	// through the animation layers; forward for the run animation blend on the others' side
	player.m_currentMovement &= ~(PLAYER_MOVE_WALK_FORWARD | PLAYER_MOVE_WALK_BACKWARD
	                              | PLAYER_MOVE_STRAFE_LEFT | PLAYER_MOVE_STRAFE_RIGHT | PLAYER_LEAN_LEFT | PLAYER_LEAN_RIGHT);
	player.m_currentMovement |= PLAYER_CROUCH;
	return true;
}

void rollImpulse(Vec3f & impulse) {
	if(!g_rolling || !entities.player()) {
		return;
	}
	// The run animation's pace is the unit (what the engine gives a running player per frame)
	float runScale = 1.25f / 1000;
	if(ANIM_HANDLE * run = entities.player()->anims[ANIM_RUN]; run && run->anims[0]) {
		Vec3f mv = GetAnimTotalTranslate(run, 0);
		AnimationDuration time = run->anims[0]->anim_time;
		if(toMsf(time) > 0.f) {
			runScale = glm::length(mv) / toMsf(time) * 0.0125f;
		}
	}
	float phase = rollPhase();
	float factor = (RollSpeed + (RollSpeedEnd - RollSpeed) * phase * phase) * 0.1f; // in "runs"
	impulse = g_direction * (runScale * factor);
}

void rollCameraEffect(Anglef & angle) {
	float phase = rollPhase();
	if(phase <= 0.f) {
		return;
	}
	// Dip along the roll's direction and come back (forward dip rolling forward, backward dip
	// rolling back, a lean rolling sideways): the first-person eye follows the crouch already,
	// this sells the tumble
	float s = std::sin(phase * glm::pi<float>());
	float turn = glm::radians(g_turn);
	angle.setPitch(angle.getPitch() + ViewDip * s * std::cos(turn));
	angle.setRoll(angle.getRoll() + ViewDip * 0.5f * s * std::sin(turn)
	              + 4.f * std::sin(phase * 2.f * glm::pi<float>()));
}

//! The roll shown on an entity: its phase and its direction in the world.
static bool shownRoll(const Entity & io, float & phase, Vec3f & direction) {
	if(&io == entities.player()) {
		if(!thirdPersonActive() && !EXTERNALVIEW) {
			return false; // first person: the eye follows the head vertex
		}
		phase = rollPhase();
		direction = g_direction;
	} else if(io.coopPuppet) {
		phase = puppetRollPhase(io);
		direction = angleToVectorXZ(puppetRollYaw(io));
	} else {
		return false;
	}
	return phase > 0.f;
}

glm::quat rollRotation(const Entity & io, const glm::quat & base) {
	float phase;
	Vec3f direction;
	if(!shownRoll(io, phase, direction)) {
		return base;
	}
	// One full roll over the move, quick in the middle, about the horizontal axis perpendicular
	// to the direction, turning so that the top of the body (up is -Y) leads the way: forward
	// roll going forward, backward roll going back, a shoulder roll going sideways. The facing
	// never changes (the player keeps looking where they look). Applied in the world frame, on
	// the left of the body's own rotation.
	float eased = phase < 0.5f ? 2.f * phase * phase : 1.f - std::pow(-2.f * phase + 2.f, 2.f) * 0.5f;
	Vec3f axis = Vec3f(-direction.z, 0.f, direction.x);
	if(arx::length2(axis) < 0.0001f) {
		return base;
	}
	return glm::angleAxis(glm::radians(360.f * eased), glm::normalize(axis)) * base;
}

void rollTumble(const Entity & io, const Anglef & angle, Vec3f & pos) {
	glm::quat base = QuatFromAngles(angle);
	glm::quat q = rollRotation(io, base);
	if(q == base) {
		return;
	}
	// The mesh pivots on the feet: move them so that the body's centre stays put
	Vec3f centre(0.f, -TumbleCentre, 0.f);
	pos = pos + centre - (q * centre);
}

float rollPhaseOf(const Entity & io) {
	if(&io == entities.player()) {
		return rollPhase();
	}
	if(io.coopPuppet) {
		return puppetRollPhase(io);
	}
	return 0.f;
}

} // namespace coop
