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

#include "graphics/opengl/GLLava.h"

#include <glm/gtc/type_ptr.hpp>

#include "graphics/opengl/GLPostProcess.h"
#include "graphics/opengl/GLShaderPipeline.h"
#include "graphics/opengl/GLShaderSources.h"
#include "io/log/Logger.h"

// Texture units of the scene copies: above the shadow cube maps (4..7) of the main program
static const GLenum LavaSceneUnit = GL_TEXTURE8;
static const GLenum LavaDepthUnit = GL_TEXTURE9;

GLLava::GLLava(GLShaderPipeline * pipeline, GLPostProcess * post)
	: m_pipeline(pipeline)
	, m_post(post)
	, m_strength(1.f)
	, m_program(0)
	, m_uViewProj(-1)
	, m_uView(-1)
	, m_uInvSize(-1)
	, m_uProjection(-1)
	, m_uCameraPos(-1)
	, m_uTime(-1)
	, m_uStrength(-1)
	, m_uFogEnabled(-1)
	, m_uFogRange(-1)
	, m_uHaze(-1)
	, m_uRaise(-1)
{ }

GLLava::~GLLava() {
	shutdown();
}

bool GLLava::init() {

	shutdown();

	m_program = m_pipeline->buildProgram("lava", shadersources::lava_vert, shadersources::lava_frag);
	if(!m_program) {
		LogWarning << "Lava shader unavailable, keeping the original lava";
		return false;
	}

	m_uViewProj = glGetUniformLocation(m_program, "u_viewProj");
	m_uView = glGetUniformLocation(m_program, "u_view");
	m_uInvSize = glGetUniformLocation(m_program, "u_invSize");
	m_uProjection = glGetUniformLocation(m_program, "u_projection");
	m_uCameraPos = glGetUniformLocation(m_program, "u_cameraPos");
	m_uTime = glGetUniformLocation(m_program, "u_time");
	m_uStrength = glGetUniformLocation(m_program, "u_strength");
	m_uFogEnabled = glGetUniformLocation(m_program, "u_fogEnabled");
	m_uFogRange = glGetUniformLocation(m_program, "u_fogRange");
	m_uHaze = glGetUniformLocation(m_program, "u_haze");
	m_uRaise = glGetUniformLocation(m_program, "u_raise");

	glUseProgram(m_program);
	glUniform1i(glGetUniformLocation(m_program, "u_enviro"), 0);
	glUniform1i(glGetUniformLocation(m_program, "u_scene"), int(LavaSceneUnit - GL_TEXTURE0));
	glUniform1i(glGetUniformLocation(m_program, "u_depth"), int(LavaDepthUnit - GL_TEXTURE0));
	m_pipeline->restoreAfterExternalDraw();

	return true;
}

void GLLava::shutdown() {
	if(m_program) {
		glDeleteProgram(m_program);
		m_program = 0;
	}
}

bool GLLava::begin(float time, const Vec3f & cameraPos) {

	if(!m_program || m_strength <= 0.f || !m_post || !m_post->isInScene()) {
		return false;
	}
	if(!m_post->captureScene()) {
		return false;
	}

	glUseProgram(m_program);

	const glm::mat4 & proj = m_pipeline->projection();
	glm::mat4 viewProj = proj * m_pipeline->view();
	glUniformMatrix4fv(m_uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
	glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(m_pipeline->view()));
	glUniform2f(m_uInvSize, 1.f / float(m_post->width()), 1.f / float(m_post->height()));
	glUniform4f(m_uProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
	glUniform3f(m_uCameraPos, cameraPos.x, cameraPos.y, cameraPos.z);
	glUniform1f(m_uTime, time);
	glUniform1f(m_uStrength, m_strength);
	glUniform1i(m_uFogEnabled, m_pipeline->fogEnabled() ? 1 : 0);
	glUniform2fv(m_uFogRange, 1, glm::value_ptr(m_pipeline->fogRange()));

	glUniform1i(m_uHaze, 0);
	glUniform1f(m_uRaise, 0.f);

	glActiveTexture(LavaSceneUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->sceneTexture());
	glActiveTexture(LavaDepthUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->depthTexture());
	glActiveTexture(GL_TEXTURE0);

	m_pipeline->setExternalPass(true);

	return true;
}

void GLLava::setHaze(bool haze, float raise) {
	if(haze && m_post) {
		// The haze shows the surface as the pass just drew it
		m_post->captureScene();
	}
	glUniform1i(m_uHaze, haze ? 1 : 0);
	glUniform1f(m_uRaise, raise);
}

void GLLava::end() {

	glActiveTexture(LavaSceneUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(LavaDepthUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

	m_pipeline->setExternalPass(false);
	m_pipeline->restoreAfterExternalDraw();
}
