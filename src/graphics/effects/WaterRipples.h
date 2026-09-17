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

#ifndef ARX_GRAPHICS_EFFECTS_WATERRIPPLES_H
#define ARX_GRAPHICS_EFFECTS_WATERRIPPLES_H

/*!
 * ArxModern: what disturbs the water this frame. The game code reports splashes and wading
 * (NPC.cpp, ParticleEffects.cpp, Projectile.cpp); the renderer's ripple simulation
 * (GLRipples.cpp) takes the list once per frame and spreads the rings from there. Nothing to
 * synchronise in co-op: every machine sees the same things move in the water.
 */

#include <vector>

#include "math/Vector.h"

class Entity;

struct RippleSource {
	Vec2f pos;      //!< world x, z
	float radius;   //!< world units
	float strength; //!< how deep the surface is pushed (a splash ~ 6, a wading step ~ 1)
};

//! A disturbance of the water surface at \a pos (world), for the ripple simulation.
void addRippleSource(const Vec3f & pos, float radius, float strength);

//! Per frame, for an entity in the water: a wake where it moved since the last frame.
void rippleWake(const Entity & io);

//! The renderer takes the sources of this frame (the list is emptied).
std::vector<RippleSource> takeRippleSources();

//! Level change: forget the wakes' last positions.
void clearRippleSources();

#endif // ARX_GRAPHICS_EFFECTS_WATERRIPPLES_H
