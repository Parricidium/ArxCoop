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

#ifndef ARX_GRAPHICS_OPENGL_GLLAVA_H
#define ARX_GRAPHICS_OPENGL_GLLAVA_H

#include "graphics/opengl/OpenGLUtil.h"
#include "math/Types.h"

class GLShaderPipeline;
class GLPostProcess;

/*!
 * ArxModern: the lava pass (data/graph/shaders/lava.{vert,frag}).
 *
 * Replaces the engine's scrolling highlight overlay on lava polygons by a shader that reads
 * the scene rendered so far: glowing, pulsing veins over a darker crust, a slow heave of the
 * surface, and a heat haze over the pool (the polygons drawn a second time, raised, distort
 * what is seen through them). Needs the post-processing scene buffer; without it the engine's
 * overlay is drawn as before.
 */
class GLLava {

public:

	GLLava(GLShaderPipeline * pipeline, GLPostProcess * post);
	~GLLava();

	//! Compile the program; false if the pass cannot be used
	bool init();
	void shutdown();

	void setPost(GLPostProcess * post) { m_post = post; }
	void setStrength(float strength) { m_strength = strength; }

	/*!
	 * Bind the lava program with the scene captured as it is now.
	 * \return false if the pass is unavailable (the caller draws the lava the old way)
	 */
	bool begin(float time, const Vec3f & cameraPos);

	//! Switch between the surface (raise = 0) and the heat haze cap raised above the pool
	void setHaze(bool haze, float raise);

	//! Back to the main program
	void end();

private:

	GLShaderPipeline * m_pipeline;
	GLPostProcess * m_post;
	float m_strength;
	GLuint m_program;
	GLint m_uViewProj;
	GLint m_uView;
	GLint m_uInvSize;
	GLint m_uProjection;
	GLint m_uCameraPos;
	GLint m_uTime;
	GLint m_uStrength;
	GLint m_uFogEnabled;
	GLint m_uFogRange;
	GLint m_uHaze;
	GLint m_uRaise;

};

#endif // ARX_GRAPHICS_OPENGL_GLLAVA_H
