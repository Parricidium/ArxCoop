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

#ifndef ARX_GRAPHICS_TEXTURE_MATERIAL_H
#define ARX_GRAPHICS_TEXTURE_MATERIAL_H

/*!
 * ArxModern: how the shader treats a texture's surface (see TextureContainer::m_material).
 * The values come from the material tag of the texture name ([metal], [stone], ...).
 */
struct MaterialParams {
	float parallax = 1.f;   //!< depth of the relief for parallax mapping, 0 = flat
	float gloss = 0.2f;     //!< glossiness scale of the specular highlights, 0 = matte
	float metal = 0.f;      //!< 1 = highlights take the colour of the surface
	bool generated = true;  //!< the map is a generated one (normal + height + gloss), not a plain normal map
};

#endif // ARX_GRAPHICS_TEXTURE_MATERIAL_H
