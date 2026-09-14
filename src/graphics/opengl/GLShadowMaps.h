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

#ifndef ARX_GRAPHICS_OPENGL_GLSHADOWMAPS_H
#define ARX_GRAPHICS_OPENGL_GLSHADOWMAPS_H

#include <stddef.h>
#include <vector>

#include <glm/glm.hpp>

#include "graphics/opengl/OpenGLUtil.h"

/*!
 * ArxModern: omnidirectional shadow maps for point lights.
 *
 * One cube map per shadowed light, each face holding the distance from the light
 * (normalised by the light's range) as rendered by the shadow shader. The caller renders
 * the six faces with \ref beginFace() / \ref end() and samples the cubes in the lighting
 * shader with the direction from the light to the fragment.
 */
class GLShadowMaps {

public:

	static constexpr size_t MaxLights = 4;

	GLShadowMaps();
	~GLShadowMaps();

	//! (Re)create the cube maps; count is clamped to MaxLights. Returns false on failure.
	bool init(size_t count, int resolution);
	void shutdown();

	[[nodiscard]] size_t count() const { return m_textures.size(); }
	[[nodiscard]] int resolution() const { return m_resolution; }
	[[nodiscard]] GLuint texture(size_t light) const { return m_textures[light]; }

	/*!
	 * Bind one cube face of one light as the render target, clear it and return the
	 * view-projection matrix to use for it. The projection covers 90 degrees up to \a range.
	 */
	glm::mat4 beginFace(size_t light, int face, const glm::vec3 & lightPos, float range);

	//! Unbind the framebuffer; the caller restores its own viewport
	void end();

private:

	int m_resolution;
	GLuint m_framebuffer;
	GLuint m_depthBuffer;
	std::vector<GLuint> m_textures;

};

#endif // ARX_GRAPHICS_OPENGL_GLSHADOWMAPS_H
