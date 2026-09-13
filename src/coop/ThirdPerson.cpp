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

#include "coop/ThirdPerson.h"

#include <algorithm>
#include <cmath>

#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Player.h"
#include "graphics/BaseGraphicsTypes.h"
#include "graphics/Math.h"
#include "input/Input.h"
#include "math/Angle.h"
#include "graphics/data/Mesh.h"
#include "physics/Collisions.h"
#include "scene/Tiles.h"
#include "platform/Time.h"

namespace coop {

namespace {

// Camera placement, in world units (the character is about 170 tall)
constexpr float Distance = 230.f;      //!< behind the pivot
constexpr float Side = 80.f;           //!< to the side, so the character sits on a third of the screen
constexpr float Height = 40.f;         //!< above the pivot
constexpr float PivotDown = 45.f;      //!< pivot below the eyes (chest), so the whole body is framed
constexpr float Probe = 14.f;          //!< radius kept free of walls around the camera
constexpr float MaxPitchDown = 74.9f;  //!< same limits as the first person mouse look
constexpr float MaxPitchUp = 301.f;

constexpr float ZoomMin = 0.5f;        //!< closest zoom factor (applied to Distance and Side)
constexpr float ZoomMax = 1.7f;
constexpr float ZoomStep = 1.12f;      //!< per wheel notch

bool g_thirdPerson = false;
bool g_orbit = false;
bool g_rightShoulder = true;
float g_zoom = 1.f;
Anglef g_orbitAngle;
float g_distance = 0.f; //!< current (smoothed) camera distance along the desired direction

//! Keeps a pitch inside the range the first person look allows (0..74.9 or 301..360).
float clampPitch(float pitch, float previous) {
	pitch = MAKEANGLE(pitch);
	if(pitch > MaxPitchDown && pitch < MaxPitchUp) {
		return previous < 180.f ? MaxPitchDown : MaxPitchUp;
	}
	return pitch;
}

} // anonymous namespace

bool thirdPersonActive() {
	return g_thirdPerson;
}

bool cameraOrbitActive() {
	return g_thirdPerson && g_orbit;
}

void thirdPersonHandleInput() {

	static bool loaded = false;
	if(!loaded) {
		loaded = true;
		g_thirdPerson = config.coop.thirdPerson;
		g_rightShoulder = config.coop.rightShoulder;
	}

	if(GInput->actionNowPressed(CONTROLS_CUST_THIRDPERSON)) {
		g_thirdPerson = !g_thirdPerson;
		g_orbit = false;
		g_distance = 0.f; // grows from the character: no first frame inside a wall
		config.coop.thirdPerson = g_thirdPerson;
		config.save();
	}

	if(!g_thirdPerson) {
		return;
	}

	if(GInput->actionNowPressed(CONTROLS_CUST_CAMERA_ORBIT)) {
		g_orbit = !g_orbit;
		if(g_orbit) {
			g_orbitAngle = player.angle;
			g_orbitAngle.setRoll(0.f);
		}
	}

	if(GInput->actionNowPressed(CONTROLS_CUST_SWAP_SHOULDER)) {
		g_rightShoulder = !g_rightShoulder;
		config.coop.rightShoulder = g_rightShoulder;
		config.save();
	}

	if(GInput->actionNowPressed(CONTROLS_CUST_CAMERA_ZOOM_IN)) {
		g_zoom = std::max(ZoomMin, g_zoom / ZoomStep);
	}
	if(GInput->actionNowPressed(CONTROLS_CUST_CAMERA_ZOOM_OUT)) {
		g_zoom = std::min(ZoomMax, g_zoom * ZoomStep);
	}

}

void cameraOrbitTurn(const Vec2f & rotation) {
	g_orbitAngle.setPitch(clampPitch(g_orbitAngle.getPitch() + rotation.y, g_orbitAngle.getPitch()));
	g_orbitAngle.setYaw(MAKEANGLE(g_orbitAngle.getYaw() - rotation.x));
}

void thirdPersonUpdateCamera() {

	Anglef angle = g_orbit ? g_orbitAngle : player.angle;
	angle.setRoll(0.f);

	// The camera orbits around the chest; the aim direction stays the player's
	Vec3f pivot = player.pos + Vec3f(0.f, PivotDown, 0.f);
	Vec3f front = angleToVector(angle);
	Vec3f right = angleToVectorXZ(angle.getYaw() + 90.f);
	Vec3f desired = pivot - front * (Distance * g_zoom) + right * (g_rightShoulder ? Side : -Side) * g_zoom
	                + Vec3f(0.f, -Height, 0.f);

	// Stop before the first wall between the head and the desired spot (a real ray test:
	// the engine's sphere checks only sample polygon vertices and miss big walls)
	Vec3f dir = desired - pivot;
	float wanted = glm::length(dir);
	dir /= wanted;
	float free = wanted;
	Vec3f probeEnd = pivot + dir * (wanted + Probe);
	for(auto tile : g_tiles->tilesAround((pivot + probeEnd) * 0.5f, wanted * 0.5f + Probe + 1.f)) {
		for(const EERIEPOLY & polygon : tile.polygons()) {
			if(polygon.type & (POLY_WATER | POLY_TRANS | POLY_NOCOL)) {
				continue;
			}
			Vec3f hit;
			if(RayCollidingPoly(pivot, probeEnd, polygon, &hit)) {
				free = std::min(free, std::max(0.f, glm::distance(pivot, hit) - Probe));
			}
		}
	}

	// Snap in when something blocks the view, ease back out
	float dt = toMsf(g_platformTime.lastFrameDuration()) * 0.001f;
	if(free < g_distance) {
		g_distance = free;
	} else {
		g_distance += (free - g_distance) * std::min(1.f, dt * 6.f);
	}

	g_playerCamera.m_pos = pivot + dir * g_distance;
	g_playerCameraStablePos = g_playerCamera.m_pos;
	g_playerCamera.angle = angle;

}

void thirdPersonTestSet(bool thirdPerson, bool orbit, bool rightShoulder, float orbitYawOffset) {
	g_thirdPerson = thirdPerson;
	g_orbit = orbit;
	g_rightShoulder = rightShoulder;
	g_orbitAngle = player.angle;
	g_orbitAngle.setRoll(0.f);
	g_orbitAngle.setYaw(MAKEANGLE(g_orbitAngle.getYaw() + orbitYawOffset));
	g_distance = 0.f;
}

} // namespace coop
