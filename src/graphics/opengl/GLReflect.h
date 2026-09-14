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

#ifndef ARX_GRAPHICS_OPENGL_GLREFLECT_H
#define ARX_GRAPHICS_OPENGL_GLREFLECT_H

#include "graphics/opengl/OpenGLUtil.h"
#include "graphics/texture/Material.h"

class GLShaderPipeline;
class GLPostProcess;
class GLTexture;

/*!
 * ArxModern: screen-space reflections (data/graph/shaders/reflect.{vert,frag}).
 *
 * The glossy level polygons are drawn a second time after the opaque scene, blending in what
 * the scene rendered so far shows along their reflected rays (ray marched through the depth
 * buffer). Needs the post-processing scene buffer for the scene colour and depth.
 */
class GLReflect {

public:

	GLReflect(GLShaderPipeline * pipeline, GLPostProcess * post);
	~GLReflect();

	//! Compile the program; false if the pass cannot be used
	bool init();
	void shutdown();

	void setPost(GLPostProcess * post) { m_post = post; }
	void setStrength(float strength) { m_strength = strength; }

	/*!
	 * Bind the reflection program with the scene captured as it is now.
	 * \return false if the pass is unavailable (nothing to draw then)
	 */
	bool begin();

	//! The material of the polygons drawn next (its material map on unit 3)
	void setMaterial(GLTexture * normalMap, const MaterialParams & material);

	//! Back to the main program
	void end();

private:

	GLShaderPipeline * m_pipeline;
	GLPostProcess * m_post;
	float m_strength;
	GLuint m_program;
	GLint m_uViewProj;
	GLint m_uView;
	GLint m_uProj;
	GLint m_uInvSize;
	GLint m_uProjection;
	GLint m_uStrength;
	GLint m_uFogEnabled;
	GLint m_uFogRange;
	GLint m_uMaterial;
	GLint m_uNormalMapped;

};

#endif // ARX_GRAPHICS_OPENGL_GLREFLECT_H
