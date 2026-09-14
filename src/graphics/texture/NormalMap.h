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

#ifndef ARX_GRAPHICS_TEXTURE_NORMALMAP_H
#define ARX_GRAPHICS_TEXTURE_NORMALMAP_H

class Image;

/*!
 * ArxModern: derive a material map from a diffuse texture.
 *
 * Dark texels are treated as recessed and bright ones as raised (height from luminance),
 * smoothed at two scales so that both the fine grain and the larger shapes (stones, planks)
 * get a slope. Edges wrap so that tiling textures stay seamless.
 *
 * The result is an R8G8B8A8 image of the same size: the tangent-space normal's x and y in
 * red and green (128, 128 = flat), the height in blue (255 = the raised surface, for parallax
 * mapping) and the glossiness in alpha (gloss scaled by the local brightness: on a metal
 * texture the dark parts are dirt and rust).
 *
 * \param diffuse  the colour texture
 * \param out      receives the material map
 * \param strength overall slope scale, 1 = default
 * \param gloss    glossiness of the material, 0..1
 * \return false if the input is not a usable image
 */
bool generateNormalMap(const Image & diffuse, Image & out, float strength = 1.f, float gloss = 0.2f);

#endif // ARX_GRAPHICS_TEXTURE_NORMALMAP_H
