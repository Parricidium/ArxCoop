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

#include "graphics/effects/WaterRipples.h"

#include <algorithm>
#include <unordered_map>

#include "core/Config.h"
#include "game/Entity.h"
#include "math/Vector.h"

namespace {

constexpr size_t MaxSources = 64; //!< per frame (the shader takes 32 per step, the rest waits)

std::vector<RippleSource> g_sources;
std::unordered_map<const Entity *, Vec3f> g_lastWaterPos; //!< where each wading entity was last frame

} // anonymous namespace

void addRippleSource(const Vec3f & pos, float radius, float strength) {
	if(!config.video.waterRipples || config.video.water <= 0.f || radius <= 0.f || strength <= 0.f) {
		return;
	}
	if(g_sources.size() >= MaxSources) {
		g_sources.erase(g_sources.begin());
	}
	g_sources.push_back({ Vec2f(pos.x, pos.z), radius, strength });
}

void rippleWake(const Entity & io) {
	if(!config.video.waterRipples || config.video.water <= 0.f) {
		return;
	}
	auto it = g_lastWaterPos.find(&io);
	if(it == g_lastWaterPos.end()) {
		g_lastWaterPos[&io] = io.pos;
		return;
	}
	float moved = glm::distance(Vec2f(io.pos.x, io.pos.z), Vec2f(it->second.x, it->second.z));
	it->second = io.pos;
	if(moved < 0.5f) {
		return;
	}
	// A bigger wake for a bigger body, deeper with the speed (a frame of running ~ 8 units)
	float size = io.physics.cyl.radius > 0.f ? io.physics.cyl.radius : 20.f;
	addRippleSource(io.pos, std::clamp(size * 1.2f, 15.f, 60.f), std::min(moved * 0.15f, 1.5f));
}

std::vector<RippleSource> takeRippleSources() {
	std::vector<RippleSource> taken;
	taken.swap(g_sources);
	return taken;
}

void clearRippleSources() {
	g_sources.clear();
	g_lastWaterPos.clear();
}
