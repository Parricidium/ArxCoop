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

#ifndef ARX_GRAPHICS_OPENGL_GLRIPPLES_H
#define ARX_GRAPHICS_OPENGL_GLRIPPLES_H

#include <vector>

#include "graphics/effects/WaterRipples.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "math/Types.h"
#include "math/Vector.h"

class GLShaderPipeline;

/*!
 * ArxModern: the water ripple simulation (data/graph/shaders/ripple_update.frag).
 *
 * A height field (red = height, green = velocity) covering a square window of the level
 * around the camera, stepped at a fixed rate with a 2D wave equation; the things moving in
 * the water (WaterRipples.h) push the surface down where they are. The water shader reads
 * the slopes of the map through the window (origin, size) to bend its normals.
 */
class GLRipples {

public:

	explicit GLRipples(GLShaderPipeline * pipeline);
	~GLRipples();

	//! Compile the program and create the maps; false if the simulation cannot run
	bool init();
	void shutdown();

	/*!
	 * Advance the simulation to \a time (seconds), the window following \a cameraPos, with the
	 * sources reported since the last frame. Restores the framebuffer and viewport it found.
	 */
	void update(float time, const Vec3f & cameraPos, const std::vector<RippleSource> & sources,
	            GLuint restoreFramebuffer, int viewportWidth, int viewportHeight);

	[[nodiscard]] bool ready() const noexcept { return m_program != 0; }
	[[nodiscard]] GLuint texture() const noexcept { return m_texture[m_current]; }
	//! (origin x, origin z, size, texel) in world units, what the water shader needs
	[[nodiscard]] Vec4f window() const noexcept;

private:

	void step(const std::vector<RippleSource> & sources, size_t first, size_t count, Vec2f shift);

	GLShaderPipeline * m_pipeline;
	GLuint m_program;
	GLuint m_vao;
	GLuint m_texture[2];
	GLuint m_framebuffer[2];
	int m_current;
	Vec2f m_origin;
	bool m_started;
	float m_lastTime;
	float m_accumulated;

	GLint m_uShift;
	GLint m_uInvSize;
	GLint m_uWindow;
	GLint m_uSpeed;
	GLint m_uDamping;
	GLint m_uSourceCount;
	GLint m_uSources;

};

#endif // ARX_GRAPHICS_OPENGL_GLRIPPLES_H
