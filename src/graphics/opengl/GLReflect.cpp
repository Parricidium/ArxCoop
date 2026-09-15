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

#include "graphics/opengl/GLReflect.h"

#include <algorithm>

#include <glm/gtc/type_ptr.hpp>

#include "graphics/opengl/GLPostProcess.h"
#include "graphics/opengl/GLRayScene.h"
#include "graphics/opengl/GLShaderPipeline.h"
#include "graphics/opengl/GLShaderSources.h"
#include "graphics/opengl/GLTexture.h"
#include "io/log/Logger.h"

// Texture units of the scene copies: above the shadow cube maps (4..7) of the main program
static const GLenum ReflectSceneUnit = GL_TEXTURE8;
static const GLenum ReflectDepthUnit = GL_TEXTURE9;
static const GLenum ReflectRayTexturesUnit = GL_TEXTURE10; // bound by the pipeline (GLShaderPipeline::prepareRayScene)

// Dynamic lights passed to the traced shading (MAX_RT_LIGHTS in reflect_rt.frag)
static const size_t MaxRayLights = 32;

GLReflect::GLReflect(GLShaderPipeline * pipeline, GLPostProcess * post)
	: m_pipeline(pipeline)
	, m_post(post)
	, m_strength(1.f)
	, m_program(0)
	, m_uViewProj(-1)
	, m_uView(-1)
	, m_uProj(-1)
	, m_uInvSize(-1)
	, m_uProjection(-1)
	, m_uStrength(-1)
	, m_uFogEnabled(-1)
	, m_uFogRange(-1)
	, m_uMaterial(-1)
	, m_uNormalMapped(-1)
	, m_traced(false)
	, m_shadowRays(true)
	, m_debugPrimary(false)
	, m_uFogColor(-1)
	, m_uCameraPos(-1)
	, m_uMode(-1)
	, m_uShadowRays(-1)
	, m_uLightCount(-1)
	, m_uLightPos(-1)
	, m_uLightColor(-1)
{ }

void GLReflect::setTraced(bool traced, bool shadowRays, bool debugPrimary) {
	m_traced = traced && GLRayScene::supported();
	m_shadowRays = shadowRays;
	m_debugPrimary = debugPrimary;
}

GLReflect::~GLReflect() {
	shutdown();
}

bool GLReflect::init() {

	shutdown();

	if(m_traced) {
		m_program = m_pipeline->buildProgram("reflect_rt", shadersources::reflect_rt_vert, shadersources::reflect_rt_frag,
		                                     "#version 430\n", "rt_common.glsl", shadersources::rt_common_glsl);
		if(!m_program) {
			LogWarning << "Ray-traced reflection shader unavailable, falling back to screen-space reflections";
			m_traced = false;
		}
	}
	if(!m_program) {
		m_program = m_pipeline->buildProgram("reflect", shadersources::reflect_vert, shadersources::reflect_frag);
	}
	if(!m_program) {
		LogWarning << "Reflection shader unavailable, no screen-space reflections";
		return false;
	}

	m_uViewProj = glGetUniformLocation(m_program, "u_viewProj");
	m_uView = glGetUniformLocation(m_program, "u_view");
	m_uProj = glGetUniformLocation(m_program, "u_proj");
	m_uInvSize = glGetUniformLocation(m_program, "u_invSize");
	m_uProjection = glGetUniformLocation(m_program, "u_projection");
	m_uStrength = glGetUniformLocation(m_program, "u_strength");
	m_uFogEnabled = glGetUniformLocation(m_program, "u_fogEnabled");
	m_uFogRange = glGetUniformLocation(m_program, "u_fogRange");
	m_uMaterial = glGetUniformLocation(m_program, "u_material");
	m_uNormalMapped = glGetUniformLocation(m_program, "u_normalMapped");
	m_uFogColor = glGetUniformLocation(m_program, "u_fogColor");
	m_uCameraPos = glGetUniformLocation(m_program, "u_cameraPos");
	m_uMode = glGetUniformLocation(m_program, "u_mode");
	m_uShadowRays = glGetUniformLocation(m_program, "u_shadowRays");
	m_uLightCount = glGetUniformLocation(m_program, "u_lightCount");
	m_uLightPos = glGetUniformLocation(m_program, "u_lightPos");
	m_uLightColor = glGetUniformLocation(m_program, "u_lightColor");

	glUseProgram(m_program);
	if(m_traced) {
		glUniform1i(glGetUniformLocation(m_program, "u_rtTextures"), int(ReflectRayTexturesUnit - GL_TEXTURE0));
	}
	glUniform1i(glGetUniformLocation(m_program, "u_texture0"), 0);
	glUniform1i(glGetUniformLocation(m_program, "u_normalMap"), 3);
	glUniform1i(glGetUniformLocation(m_program, "u_scene"), int(ReflectSceneUnit - GL_TEXTURE0));
	glUniform1i(glGetUniformLocation(m_program, "u_depth"), int(ReflectDepthUnit - GL_TEXTURE0));
	m_pipeline->restoreAfterExternalDraw();

	return true;
}

void GLReflect::shutdown() {
	if(m_program) {
		glDeleteProgram(m_program);
		m_program = 0;
	}
}

bool GLReflect::begin() {

	if(!m_program || m_strength <= 0.f || !m_post || !m_post->isInScene()) {
		return false;
	}
	if(!m_post->captureScene()) {
		return false;
	}
	if(m_traced && (!m_pipeline->rayScene() || !m_pipeline->rayScene()->generation())) {
		return false; // no level hierarchy (bound by the pipeline at the start of the frame)
	}

	glUseProgram(m_program);

	const glm::mat4 & proj = m_pipeline->projection();
	glm::mat4 viewProj = proj * m_pipeline->view();
	glUniformMatrix4fv(m_uViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
	glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(m_pipeline->view()));
	glUniformMatrix4fv(m_uProj, 1, GL_FALSE, glm::value_ptr(proj));
	glUniform2f(m_uInvSize, 1.f / float(m_post->width()), 1.f / float(m_post->height()));
	glUniform4f(m_uProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
	glUniform1f(m_uStrength, m_strength);
	glUniform1i(m_uFogEnabled, m_pipeline->fogEnabled() ? 1 : 0);
	glUniform2fv(m_uFogRange, 1, glm::value_ptr(m_pipeline->fogRange()));

	if(m_traced) {
		glUniform3fv(m_uFogColor, 1, glm::value_ptr(m_pipeline->fogColor()));
		glm::vec3 cameraPos(glm::inverse(m_pipeline->view())[3]);
		glUniform3fv(m_uCameraPos, 1, glm::value_ptr(cameraPos));
		glUniform1i(m_uMode, m_debugPrimary ? 1 : 0);
		glUniform1i(m_uShadowRays, m_shadowRays ? 1 : 0);
		const std::vector<glm::vec4> & lightPos = m_pipeline->lightPositions();
		GLsizei lights = GLsizei(std::min({ m_pipeline->dynamicLightCount(), lightPos.size(), MaxRayLights }));
		glUniform1i(m_uLightCount, lights);
		if(lights > 0) {
			glUniform4fv(m_uLightPos, lights, glm::value_ptr(lightPos[0]));
			glUniform4fv(m_uLightColor, lights, glm::value_ptr(m_pipeline->lightColors()[0]));
		}
	}

	glActiveTexture(ReflectSceneUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->sceneTexture());
	glActiveTexture(ReflectDepthUnit);
	glBindTexture(GL_TEXTURE_2D, m_post->depthTexture());
	glActiveTexture(GL_TEXTURE0);

	m_pipeline->setExternalPass(true);

	return true;
}

void GLReflect::setMaterial(GLTexture * normalMap, const MaterialParams & material) {
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, normalMap ? normalMap->id() : 0);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(m_uNormalMapped, normalMap ? 1 : 0);
	glUniform4f(m_uMaterial, material.parallax, material.gloss, material.metal, material.generated ? 1.f : 0.f);
}

void GLReflect::end() {

	glActiveTexture(ReflectSceneUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(ReflectDepthUnit);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

	m_pipeline->setExternalPass(false);
	m_pipeline->restoreAfterExternalDraw();
}
