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

#include "graphics/opengl/GLRipples.h"

#include <algorithm>
#include <cmath>

#include "graphics/opengl/GLShaderPipeline.h"
#include "graphics/opengl/GLShaderSources.h"
#include "io/log/Logger.h"

namespace {

constexpr int MapSize = 512;            //!< texels per side
constexpr float TexelSize = 4.f;        //!< world units per texel: a 2048-unit window around the camera
constexpr float StepRate = 60.f;        //!< simulation steps per second
constexpr float WaveSpeed = 0.42f;      //!< c² in texels² per step² (must stay below 0.5)
constexpr float Damping = 0.992f;       //!< velocity kept per step
constexpr int MaxStepsPerFrame = 4;
constexpr int MaxSourcesPerStep = 32;   //!< u_sources[] size in the shader

} // anonymous namespace

GLRipples::GLRipples(GLShaderPipeline * pipeline)
	: m_pipeline(pipeline)
	, m_program(0)
	, m_vao(0)
	, m_texture { 0, 0 }
	, m_framebuffer { 0, 0 }
	, m_current(0)
	, m_origin(0.f)
	, m_started(false)
	, m_lastTime(0.f)
	, m_accumulated(0.f)
	, m_uShift(-1)
	, m_uInvSize(-1)
	, m_uWindow(-1)
	, m_uSpeed(-1)
	, m_uDamping(-1)
	, m_uSourceCount(-1)
	, m_uSources(-1)
{ }

GLRipples::~GLRipples() {
	shutdown();
}

bool GLRipples::init() {

	shutdown();

	m_program = m_pipeline->buildProgram("ripple_update", shadersources::post_vert, shadersources::ripple_update_frag);
	if(!m_program) {
		return false;
	}

	glUseProgram(m_program);
	glUniform1i(glGetUniformLocation(m_program, "u_previous"), 0);
	m_uShift = glGetUniformLocation(m_program, "u_shift");
	m_uInvSize = glGetUniformLocation(m_program, "u_invSize");
	m_uWindow = glGetUniformLocation(m_program, "u_window");
	m_uSpeed = glGetUniformLocation(m_program, "u_speed");
	m_uDamping = glGetUniformLocation(m_program, "u_damping");
	m_uSourceCount = glGetUniformLocation(m_program, "u_sourceCount");
	m_uSources = glGetUniformLocation(m_program, "u_sources");

	glGenVertexArrays(1, &m_vao);

	bool ok = true;
	for(int i = 0; i < 2; i++) {
		glGenTextures(1, &m_texture[i]);
		glBindTexture(GL_TEXTURE_2D, m_texture[i]);
		// 16-bit normalised (0.5 = flat): a float format sampled as zeros in the water pass here
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, MapSize, MapSize, 0, GL_RGBA, GL_UNSIGNED_SHORT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // (the shader fades the edges out itself)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &m_framebuffer[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture[i], 0);
		if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LogWarning << "Water ripples: incomplete framebuffer";
			ok = false;
		}
		glClearColor(0.5f, 0.5f, 0.f, 1.f); // (flat water)
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	m_pipeline->restoreAfterExternalDraw();

	if(!ok) {
		shutdown();
		return false;
	}

	m_started = false;
	LogInfo << "Water ripples: " << MapSize << "x" << MapSize << " map, " << int(MapSize * TexelSize) << " units around the camera";
	return true;
}

void GLRipples::shutdown() {
	if(m_program) {
		glDeleteProgram(m_program);
		m_program = 0;
	}
	for(int i = 0; i < 2; i++) {
		if(m_framebuffer[i]) {
			glDeleteFramebuffers(1, &m_framebuffer[i]);
			m_framebuffer[i] = 0;
		}
		if(m_texture[i]) {
			glDeleteTextures(1, &m_texture[i]);
			m_texture[i] = 0;
		}
	}
	if(m_vao) {
		glDeleteVertexArrays(1, &m_vao);
		m_vao = 0;
	}
}

Vec4f GLRipples::window() const noexcept {
	return Vec4f(m_origin.x, m_origin.y, float(MapSize) * TexelSize, TexelSize);
}

void GLRipples::step(const std::vector<RippleSource> & sources, size_t first, size_t count, Vec2f shift) {

	int next = 1 - m_current;
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer[next]);
	glBindTexture(GL_TEXTURE_2D, m_texture[m_current]);

	glUniform2f(m_uShift, shift.x, shift.y);
	Vec4f w = window();
	glUniform4f(m_uWindow, w.x, w.y, w.z, w.w);

	int n = int(std::min<size_t>(count, MaxSourcesPerStep));
	glUniform1i(m_uSourceCount, n);
	if(n > 0) {
		float data[MaxSourcesPerStep * 4];
		for(int i = 0; i < n; i++) {
			const RippleSource & s = sources[first + size_t(i)];
			data[i * 4 + 0] = s.pos.x;
			data[i * 4 + 1] = s.pos.y;
			data[i * 4 + 2] = s.radius;
			data[i * 4 + 3] = s.strength;
		}
		glUniform4fv(m_uSources, n, data);
	}

	glDrawArrays(GL_TRIANGLES, 0, 3);
	m_current = next;
}

void GLRipples::update(float time, const Vec3f & cameraPos, const std::vector<RippleSource> & sources,
                       GLuint restoreFramebuffer, int viewportWidth, int viewportHeight) {

	if(!m_program) {
		return;
	}

	// The window follows the camera on the texel grid: the old state is read shifted
	Vec2f origin(std::floor(cameraPos.x / TexelSize) * TexelSize, std::floor(cameraPos.z / TexelSize) * TexelSize);
	Vec2f shift(0.f);
	if(!m_started) {
		m_started = true;
		m_lastTime = time;
		m_accumulated = 0.f;
	} else {
		shift = (origin - m_origin) / TexelSize;
		float dt = std::clamp(time - m_lastTime, 0.f, 0.25f);
		m_lastTime = time;
		m_accumulated += dt;
	}
	m_origin = origin;

	int steps = std::min(int(m_accumulated * StepRate), MaxStepsPerFrame);
	if(steps <= 0 && sources.empty() && shift == Vec2f(0.f)) {
		return;
	}
	m_accumulated = std::max(m_accumulated - float(steps) / StepRate, 0.f);
	if(steps <= 0) {
		steps = 1; // a shift or new sources: at least carry them into the map
	}

	// The engine caches its GL state: everything touched here is put back as found
	GLint activeUnit = GL_TEXTURE0, vao = 0, texture0 = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeUnit);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
	GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	
	glUseProgram(m_program);
	glBindVertexArray(m_vao);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, MapSize, MapSize);
	glUniform2f(m_uInvSize, 1.f / float(MapSize), 1.f / float(MapSize));
	glUniform1f(m_uSpeed, WaveSpeed);
	glUniform1f(m_uDamping, Damping);

	// The sources of the frame go into the first step (and the next ones when there are many)
	size_t given = 0;
	for(int i = 0; i < steps; i++) {
		size_t count = (given < sources.size()) ? std::min<size_t>(sources.size() - given, MaxSourcesPerStep) : 0;
		step(sources, given, count, (i == 0) ? shift : Vec2f(0.f));
		given += count;
	}

	
	glBindTexture(GL_TEXTURE_2D, GLuint(texture0));
	glActiveTexture(GLenum(activeUnit));
	glBindVertexArray(GLuint(vao));
	if(depthTest) glEnable(GL_DEPTH_TEST);
	if(blend) glEnable(GL_BLEND);
	if(cull) glEnable(GL_CULL_FACE);
	if(scissor) glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, restoreFramebuffer);
	glViewport(0, 0, viewportWidth, viewportHeight);
	m_pipeline->restoreAfterExternalDraw();
}
