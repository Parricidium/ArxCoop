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

#ifndef ARX_PHYSICS_CLOTH_H
#define ARX_PHYSICS_CLOTH_H

#include <cstddef>

#include "math/Types.h"

struct EERIEPOLY;

/*!
 * ArxModern: the hanging cloths of the levels (banners, curtains, tapestries) as soft bodies.
 *
 * The level polygons textured with a [fabric] material that hang vertically are taken out of
 * the static level (hidden from the room draw, left out of the collision mesh) and rebuilt
 * as a cloth: the polygons are subdivided into a finer mesh, pinned along their top edge and
 * simulated by Jolt with a light wind. They collide with the level, the fixed objects and the
 * characters walking into them. Drawn every frame from the simulated vertices.
 */
namespace physics {

//! true for a level polygon that becomes a cloth (must be excluded from the level mesh)
bool isClothPolygon(const EERIEPOLY & poly);

//! Build the cloths of the loaded level (after the level mesh)
void createCloths();

//! After a simulation step: wind, waking the cloths near the player
void updateCloths();

//! Draw the cloths (during the opaque pass of the level)
void renderCloths();

//! Remove every cloth (level unload); the polygons are shown again
void clearCloths();

//! Number of cloths
size_t clothCount();

//! Centre and radius of the largest cloth (tests); false if there is none
bool largestCloth(Vec3f & centre, float & radius);

} // namespace physics

#endif // ARX_PHYSICS_CLOTH_H
