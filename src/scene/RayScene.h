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

#ifndef ARX_SCENE_RAYSCENE_H
#define ARX_SCENE_RAYSCENE_H

#include <cstddef>
#include <memory>
#include <vector>

#include "math/Types.h"
#include "platform/Platform.h"

class TextureContainer;

/*!
 * ArxModern ray tracing: the level geometry as a bounding volume hierarchy the shaders can walk
 * (data/graph/shaders/rt_common.glsl). Built once per level from the background polygons, in the
 * layouts the GPU buffers use (std430: everything is 16-byte vectors).
 *
 * A node is two vec4: (min.xyz, leftFirst) and (max.xyz, count). count > 0: a leaf holding the
 * triangles [leftFirst, leftFirst + count); count == 0: an inner node whose children are the
 * nodes leftFirst and leftFirst + 1 (the integers are stored as their bit patterns).
 *
 * A triangle is three vec4: v0, e1 = v1 - v0 (w = |e1 x e2|), e2 = v2 - v0. Its attributes are three
 * more: (uv0, uv1), (uv2, layer | flags << 16, color0), (color1, color2, 0, 0) with the colors
 * as packed RGBA bytes (the static lighting of the vertices, what the level draws in per-pixel
 * lighting mode) and layer the index of the texture in RaySceneData::textures.
 */
struct RaySceneData {

	enum Flags {
		AlphaCutout = 1 << 0, //!< the texture has an alpha channel: holes where alpha < 0.5
		Water       = 1 << 1,
		Lava        = 1 << 2,
		Glow        = 1 << 3, //!< drawn fullbright (POLY_GLOW)
		DoubleSided = 1 << 4
	};

	std::vector<Vec4f> nodes;      //!< 2 per node
	std::vector<Vec4f> triangles;  //!< 3 per triangle
	std::vector<Vec4f> attributes; //!< 3 per triangle
	std::vector<TextureContainer *> textures;

	[[nodiscard]] size_t nodeCount() const noexcept { return nodes.size() / 2; }
	[[nodiscard]] size_t triangleCount() const noexcept { return triangles.size() / 3; }

};

//! Build the hierarchy of the current level; null without a level
std::unique_ptr<RaySceneData> buildRayScene();

/*!
 * Incremented whenever the level geometry changes (level load): whoever keeps a RaySceneData
 * rebuilds it when the generation differs from the one it was built for.
 */
[[nodiscard]] size_t raySceneGeneration() noexcept;
void raySceneChanged() noexcept;

#endif // ARX_SCENE_RAYSCENE_H
