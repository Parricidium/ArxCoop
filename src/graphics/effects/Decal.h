/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
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
/* Based on:
===========================================================================
ARX FATALIS GPL Source Code
Copyright (C) 1999-2010 Arkane Studios SA, a ZeniMax Media company.

This file is part of the Arx Fatalis GPL Source Code ('Arx Fatalis Source Code'). 

Arx Fatalis Source Code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Arx Fatalis Source Code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Arx Fatalis Source Code.  If not, see 
<http://www.gnu.org/licenses/>.

In addition, the Arx Fatalis Source Code is also subject to certain additional terms. You should have received a copy of these 
additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Arx 
Fatalis Source Code. If not, please request a copy in writing from Arkane Studios at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing Arkane Studios, c/o 
ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/
// Code: Cyril Meynier
//
// Copyright (c) 1999 ARKANE Studios SA. All rights reserved

#ifndef ARX_GRAPHICS_EFFECTS_DECAL_H
#define ARX_GRAPHICS_EFFECTS_DECAL_H

#include <stddef.h>

#include "graphics/BaseGraphicsTypes.h"
#include "graphics/Color.h"
#include "math/Types.h"

class Entity;

void PolyBoomClear();
size_t PolyBoomCount();

void PolyBoomAddScorch(const Vec3f & pos);
//! \a flags: 1 = at the sphere's height, 2 = on water, 4 = a footprint (small, short, leaves the others alone)
void PolyBoomAddSplat(const Sphere & sp, const Color3f & col, long flags);

/*!
 * ArxModern: blood trails. A step of \a io at \a pos (ARX_NPC_NeedStepSound): stepping in a
 * fresh blood stain wets the feet, the next steps leave shrinking red footprints. Each machine
 * does it from the positions it sees: nothing to synchronise in co-op.
 */
void PolyBoomFootstep(Entity * io, const Vec3f & pos);

void PolyBoomDraw();

#endif // ARX_GRAPHICS_EFFECTS_DECAL_H
