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

#ifndef ARX_GRAPHICS_OPENGL_GLRAYSCENE_H
#define ARX_GRAPHICS_OPENGL_GLRAYSCENE_H

#include <cstddef>

#include "graphics/opengl/OpenGLUtil.h"

/*!
 * ArxModern ray tracing: the level's bounding volume hierarchy (scene/RayScene.h) as shader
 * storage buffers, plus a small array texture holding every level texture (128x128, mipmapped)
 * so that a shader can shade whatever a ray hits. Needs OpenGL 4.3 (shader storage buffers).
 *
 * Buffer bindings (see data/graph/shaders/rt_common.glsl): 1 = nodes, 2 = triangles,
 * 3 = attributes. The texture array goes on the unit given to bind().
 */
class GLRayScene {

public:

	GLRayScene();
	~GLRayScene();

	//! Whether the context can do this at all
	[[nodiscard]] static bool supported();

	/*!
	 * Rebuild the buffers if the level changed since the last call.
	 * \return false when there is no level geometry to trace
	 */
	bool update();

	//! Bind the buffers and the texture array (on the given texture unit) for the current program
	void bind(GLenum textureUnit);
	void unbind(GLenum textureUnit);

	void release();

	[[nodiscard]] size_t triangleCount() const noexcept { return m_triangles; }
	//! The level generation the buffers hold (0 = nothing yet)
	[[nodiscard]] size_t generation() const noexcept { return m_triangles ? m_generation : 0; }

private:

	GLuint m_nodes;
	GLuint m_tris;
	GLuint m_attrs;
	GLuint m_textures;
	size_t m_generation;
	size_t m_triangles;

};

#endif // ARX_GRAPHICS_OPENGL_GLRAYSCENE_H
