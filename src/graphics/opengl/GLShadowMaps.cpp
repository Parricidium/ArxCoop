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

#include "graphics/opengl/GLShadowMaps.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include "io/log/Logger.h"

GLShadowMaps::GLShadowMaps()
	: m_resolution(0)
	, m_framebuffer(0)
	, m_depthBuffer(0)
{ }

GLShadowMaps::~GLShadowMaps() {
	shutdown();
}

bool GLShadowMaps::init(size_t count, int resolution) {

	shutdown();

	count = std::min(count, MaxLights);
	if(count == 0) {
		return false;
	}
	m_resolution = std::clamp(resolution, 64, 4096);

	m_textures.resize(count, 0);
	glGenTextures(GLsizei(count), m_textures.data());
	for(GLuint texture : m_textures) {
		glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
		for(int face = 0; face < 6; face++) {
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_R16F, m_resolution, m_resolution, 0,
			             GL_RED, GL_FLOAT, nullptr);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

	glGenRenderbuffers(1, &m_depthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_resolution, m_resolution);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &m_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthBuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, m_textures[0], 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if(status != GL_FRAMEBUFFER_COMPLETE) {
		LogWarning << "Shadow map framebuffer incomplete (status " << status << "), shadows disabled";
		shutdown();
		return false;
	}

	LogInfo << "Shadow maps: " << count << " x 6 x " << m_resolution << "x" << m_resolution;

	return true;
}

void GLShadowMaps::shutdown() {

	if(m_framebuffer) {
		glDeleteFramebuffers(1, &m_framebuffer);
		m_framebuffer = 0;
	}
	if(m_depthBuffer) {
		glDeleteRenderbuffers(1, &m_depthBuffer);
		m_depthBuffer = 0;
	}
	if(!m_textures.empty()) {
		glDeleteTextures(GLsizei(m_textures.size()), m_textures.data());
		m_textures.clear();
	}
	m_resolution = 0;

}

glm::mat4 GLShadowMaps::beginFace(size_t light, int face, const glm::vec3 & lightPos, float range) {

	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
	                       m_textures[light], 0);
	glViewport(0, 0, m_resolution, m_resolution);

	// Beyond the light's range everything is lit, so clear to "farther than range"
	glClearColor(2.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Standard OpenGL cube map face orientations
	static const glm::vec3 directions[6] = {
		glm::vec3( 1.f,  0.f,  0.f), glm::vec3(-1.f,  0.f,  0.f),
		glm::vec3( 0.f,  1.f,  0.f), glm::vec3( 0.f, -1.f,  0.f),
		glm::vec3( 0.f,  0.f,  1.f), glm::vec3( 0.f,  0.f, -1.f)
	};
	static const glm::vec3 ups[6] = {
		glm::vec3(0.f, -1.f,  0.f), glm::vec3(0.f, -1.f,  0.f),
		glm::vec3(0.f,  0.f,  1.f), glm::vec3(0.f,  0.f, -1.f),
		glm::vec3(0.f, -1.f,  0.f), glm::vec3(0.f, -1.f,  0.f)
	};

	glm::mat4 view = glm::lookAt(lightPos, lightPos + directions[face], ups[face]);
	glm::mat4 projection = glm::perspective(glm::half_pi<float>(), 1.f, 1.f, range);

	return projection * view;
}

void GLShadowMaps::end() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
