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

#include "graphics/opengl/GLWater.h"

#include <vector>
#include <algorithm>

#include "graphics/effects/WaterRipples.h"
#include "graphics/opengl/GLRipples.h"

#include <algorithm>

#include <glm/gtc/type_ptr.hpp>

#include "graphics/opengl/GLPostProcess.h"
#include "graphics/opengl/GLRayScene.h"
#include "graphics/opengl/GLShaderPipeline.h"
#include "graphics/opengl/GLShaderSources.h"
#include "io/log/Logger.h"

// Texture units of the scene copies: above the shadow cube maps (4..7) of the main program
static const GLenum SceneUnit = GL_TEXTURE8;
static const GLenum DepthUnit = GL_TEXTURE9;
static const GLenum RippleUnit = GL_TEXTURE11; // (0..2 the texture stages, 3 the normal map, 4..7 shadow maps, 8..10 scene, depth and ray tracing textures)

GLWater::GLWater(GLShaderPipeline * pipeline, GLPostProcess * post)
	: m_pipeline(pipeline)
	, m_post(post)
	, m_strength(1.f)
	, m_reflection(0.f)
	, m_traced(false)
	, m_program(0)
	, m_uViewProj(-1)
	, m_uView(-1)
	, m_uProj(-1)
	, m_uReflection(-1)
	, m_uFogColor(-1)
	, m_uDynamicLightCount(-1)
	, m_uInvSize(-1)
	, m_uProjection(-1)
	, m_uCameraPos(-1)
	, m_uTime(-1)
	, m_uStrength(-1)
	, m_uFogEnabled(-1)
	, m_uFogRange(-1)
	, m_uLightCount(-1)
	, m_uLightPos(-1)
	, m_uLightColor(-1)
	, m_ripplesWanted(false)
	, m_uRippleWindow(-1)
	, m_uRippleStrength(-1)
{ }

GLWater::~GLWater() {
	shutdown();
}

bool GLWater::init() {

	shutdown();

	m_traced = (m_pipeline->rayTracing() > 0);
	if(m_traced) {
		m_program = m_pipeline->buildProgram("water", shadersources::water_vert, shadersources::water_frag,
		                                     "#version 430\n#define ARX_RT 1\n", "rt_common.glsl", shadersources::rt_common_glsl);
		if(!m_program) {
			LogWarning << "Ray-traced water shader unavailable, falling back to the screen-space one";
			m_traced = false;
		}
	}
	if(!m_program) {
		m_program = m_pipeline->buildProgram("water", shadersources::water_vert, shadersources::water_frag);
	}
	if(!m_program) {
		LogWarning << "Water shader unavailable, keeping the original water";
		return false;
	}

	m_uViewProj = glGetUniformLocation(m_program, "u_viewProj");
	m_uView = glGetUniformLocation(m_program, "u_view");
	m_uProj = glGetUniformLocation(m_program, "u_proj");
	m_uReflection = glGetUniformLocation(m_program, "u_reflection");
	m_uFogColor = glGetUniformLocation(m_program, "u_fogColor");
	m_uDynamicLightCount = glGetUniformLocation(m_program, "u_dynamicLightCount");
	m_uInvSize = glGetUniformLocation(m_program, "u_invSize");
	m_uProjection = glGetUniformLocation(m_program, "u_projection");
	m_uCameraPos = glGetUniformLocation(m_program, "u_cameraPos");
	m_uTime = glGetUniformLocation(m_program, "u_time");
	m_uStrength = glGetUniformLocation(m_program, "u_strength");
	m_uFogEnabled = glGetUniformLocation(m_program, "u_fogEnabled");
	m_uFogRange = glGetUniformLocation(m_program, "u_fogRange");
	m_uLightCount = glGetUniformLocation(m_program, "u_lightCount");
	m_uLightPos = glGetUniformLocation(m_program, "u_lightPos");
	m_uLightColor = glGetUniformLocation(m_program, "u_lightColor");
	m_uRippleWindow = glGetUniformLocation(m_program, "u_rippleWindow");
	m_uRippleStrength = glGetUniformLocation(m_program, "u_rippleStrength");

	glUseProgram(m_program);
	glUniform1i(glGetUniformLocation(m_program, "u_ripples"), int(RippleUnit - GL_TEXTURE0));
glUniform1i(glGetUniformLocation(m_program, "u_enviro"), 0);
	glUniform1i(glGetUniformLocation(m_program, "u_scene"), int(SceneUnit - GL_TEXTURE0));
	glUniform1i(glGetUniformLocation(m_program, "u_depth"), int(DepthUnit - GL_TEXTURE0));
	if(m_traced) {
		glUniform1i(glGetUniformLocation(m_program, "u_rtTextures"), 10); // bound by the pipeline
	}
	m_pipeline->restoreAfterExternalDraw();

	return true;
}

void GLWater::shutdown() {
	m_ripples.reset();
if(m_program) {
		glDeleteProgram(m_program);
		m_program = 0;
	}
}

bool GLWater::begin(float time, const Vec3f & cameraPos) {

	if(!m_program || m_strength <= 0.f || !m_post || !m_post->isInScene()) {
		return false;
	}
	if(!m_post->captureScene()) {
		return false;
	}

	// Ripples: the simulation steps before the surface is drawn (it renders to its own maps
	// and puts the scene framebuffer back)
	if(m_ripplesWanted && !m_ripples) {
		m_ripples = std::make_unique<GLRipples>(m_pipeline);
		if(!m_ripples->init()) {
			m_ripples.reset();
			m_ripplesWanted = false; // (no second try every frame)
		}
	} else if(!m_ripplesWanted && m_ripples) {
		m_ripples.reset();
	}
	if(m_ripples) {
		GLint framebuffer = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		GLint viewport[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, viewport);
		m_ripples->update(time, cameraPos, takeRippleSources(), GLuint(framebuffer), viewport[2], viewport[3]);
	} else {
		takeRippleSources(); // (dropped)
	}

	glUseProgram(m_program);
	if(m_ripples) {
		Vec4f w = m_ripples->window();
		glUniform4f(m_uRippleWindow, w.x, w.y, w.z, w.w);
		glUniform1f(m_uRippleStrength, 1.f);
		glActiveTexture(RippleUnit);
		glBindTexture(GL_TEXTURE_2D, m_ripples->texture());
	} else {
		glUniform4f(m_uRippleWindow, 0.f, 0.f, 0.f, 0.f);
		glUniform1f(m_uRippleStrength, 0.f);
	}

	const glm::mat4 & proj = m_pipeline->projection();
	glm::mat4 viewProj = proj * m_pipeline->view();
	glUniformMatrix4fv(m_uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
	glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(m_pipeline->view()));
	glUniformMatrix4fv(m_uProj, 1, GL_FALSE, glm::value_ptr(proj));
	glUniform1f(m_uReflection, (m_traced && (!m_pipeline->rayScene() || !m_pipeline->rayScene()->generation())) ? 0.f : m_reflection);
	glUniform3fv(m_uFogColor, 1, glm::value_ptr(m_pipeline->fogColor()));
	glUniform2f(m_uInvSize, 1.f / float(m_post->width()), 1.f / float(m_post->height()));
	glUniform4f(m_uProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
	glUniform3f(m_uCameraPos, cameraPos.x, cameraPos.y, cameraPos.z);
	glUniform1f(m_uTime, time);
	glUniform1f(m_uStrength, m_strength);
	glUniform1i(m_uFogEnabled, m_pipeline->fogEnabled() ? 1 : 0);
	glUniform2fv(m_uFogRange, 1, glm::value_ptr(m_pipeline->fogRange()));

	const std::vector<glm::vec4> & lightPos = m_pipeline->lightPositions();
	const std::vector<glm::vec4> & lightColor = m_pipeline->lightColors();
	GLsizei count = GLsizei(lightPos.size());
	glUniform1i(m_uLightCount, count);
	glUniform1i(m_uDynamicLightCount, GLsizei(std::min(m_pipeline->dynamicLightCount(), lightPos.size())));
	if(count > 0) {
		glUniform4fv(m_uLightPos, count, glm::value_ptr(lightPos[0]));
		glUniform4fv(m_uLightColor, count, glm::value_ptr(lightColor[0]));
	}

	glActiveTexture(SceneUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->sceneTexture());
	glActiveTexture(DepthUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->depthTexture());
	glActiveTexture(GL_TEXTURE0);
	m_pipeline->setExternalPass(true);

	return true;
}

void GLWater::end() {

	glActiveTexture(RippleUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(SceneUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
glActiveTexture(DepthUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

	m_pipeline->setExternalPass(false);
	m_pipeline->restoreAfterExternalDraw();
}
