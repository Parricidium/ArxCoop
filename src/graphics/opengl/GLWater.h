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

#ifndef ARX_GRAPHICS_OPENGL_GLWATER_H
#define ARX_GRAPHICS_OPENGL_GLWATER_H

#include "graphics/opengl/OpenGLUtil.h"
#include "math/Types.h"

class GLShaderPipeline;
class GLPostProcess;

/*!
 * ArxModern: the water surface pass (data/graph/shaders/water.{vert,frag}).
 *
 * Replaces the engine's scrolling highlight overlay on water polygons by a shader that reads
 * the scene rendered so far (refraction through animated waves, depth tint, soft banks) and
 * adds the specular trails of the scene's lights. Needs the post-processing scene buffer for
 * the scene colour and depth; without it the engine's overlay is drawn as before.
 */
class GLWater {

public:

	GLWater(GLShaderPipeline * pipeline, GLPostProcess * post);
	~GLWater();

	//! Compile the program; false if the pass cannot be used
	bool init();
	void shutdown();

	void setPost(GLPostProcess * post) { m_post = post; }
	void setStrength(float strength) { m_strength = strength; }
	//! Strength of the mirrored scene (0 = off); traced through the level when the pipeline ray traces
	void setReflection(float reflection) { m_reflection = reflection; }
	//! Whether the program was built with the ray tracing code (init() again when this must change)
	[[nodiscard]] bool traced() const noexcept { return m_traced; }

	/*!
	 * Bind the water program with the scene captured as it is now.
	 * \return false if the pass is unavailable (the caller draws the water the old way)
	 */
	bool begin(float time, const Vec3f & cameraPos);

	//! Back to the main program
	void end();

private:

	GLShaderPipeline * m_pipeline;
	GLPostProcess * m_post;
	float m_strength;
	float m_reflection;
	bool m_traced;
	GLuint m_program;
	GLint m_uViewProj;
	GLint m_uView;
	GLint m_uProj;
	GLint m_uReflection;
	GLint m_uFogColor;
	GLint m_uDynamicLightCount;
	GLint m_uInvSize;
	GLint m_uProjection;
	GLint m_uCameraPos;
	GLint m_uTime;
	GLint m_uStrength;
	GLint m_uFogEnabled;
	GLint m_uFogRange;
	GLint m_uLightCount;
	GLint m_uLightPos;
	GLint m_uLightColor;

};

#endif // ARX_GRAPHICS_OPENGL_GLWATER_H
