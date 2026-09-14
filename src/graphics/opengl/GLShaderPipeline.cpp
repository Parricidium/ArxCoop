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

#include "graphics/opengl/GLShaderPipeline.h"

#include <algorithm>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "graphics/opengl/GLShaderSources.h"
#include "graphics/opengl/GLShadowMaps.h"
#include "graphics/opengl/GLTexture.h"
#include "graphics/opengl/GLTextureStage.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"

static const char * const g_shaderDir = "graph/shaders";

GLShaderPipeline::GLShaderPipeline(OpenGLRenderer * renderer)
	: m_renderer(renderer)
	, m_program(0)
	, m_uMVP(-1)
	, m_uView(-1)
	, m_uTransform(-1)
	, m_uFogEnabled(-1)
	, m_uFogColor(-1)
	, m_uFogRange(-1)
	, m_uPixelLighting(-1)
	, m_uLightCount(-1)
	, m_uDynamicLightCount(-1)
	, m_uLightPos(-1)
	, m_uLightColor(-1)
	, m_uShadowCount(-1)
	, m_uNormalMapped(-1)
	, m_uNormalStrength(-1)
	, m_normalMap(nullptr)
	, m_glNormalMap(nullptr)
	, m_glNormalMapped(-1)
	, m_normalStrength(1.f)
	, m_normalStrengthDirty(true)
	, m_shadowProgram(0)
	, m_uShadowViewProj(-1)
	, m_uShadowTransform(-1)
	, m_uShadowLightPos(-1)
	, m_uShadowFallend(-1)
	, m_uShadowTextured(-1)
	, m_shadowPass(false)
	, m_externalPass(false)
	, m_glShadowTransform(-1)
	, m_glShadowTextured(-1)
	, m_shadowedLights(0)
	, m_glShadowCount(-1)
	, m_pretransformed(false)
	, m_glTransform(-1)
	, m_matricesDirty(true)
	, m_orthoDirty(true)
	, m_view(1.f)
	, m_projection(1.f)
	, m_ortho(1.f)
	, m_viewportWidth(1.f)
	, m_viewportHeight(1.f)
	, m_glFogEnabled(-1)
	, m_fogDirty(true)
	, m_fogColor(0.f)
	, m_fogRange(0.f, 1.f)
	, m_pixelLighting(false)
	, m_glPixelLighting(-1)
	, m_dynamicLightCount(0)
	, m_lightsDirty(true)
{
	for(GLint & location : m_uStages) {
		location = -1;
	}
}

GLShaderPipeline::~GLShaderPipeline() {
	shutdown();
}

std::string GLShaderPipeline::loadSource(std::string_view name, std::string_view fallback) {

	res::path file = res::path(g_shaderDir) / name;
	if(g_resources && g_resources->getFile(file)) {
		std::string source = g_resources->read(file);
		if(!source.empty()) {
			LogInfo << "Using shader override " << file;
			return source;
		}
	}

	return std::string(fallback);
}

GLuint GLShaderPipeline::compile(GLenum type, std::string_view name, std::string_view source) {

	GLuint shader = glCreateShader(type);
	if(!shader) {
		LogError << "Could not create shader object for " << name;
		return 0;
	}

	const GLchar * text = source.data();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &text, &length);
	glCompileShader(shader);

	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

	GLint logLength = 0;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
	if(logLength > 1) {
		std::vector<GLchar> infoLog(static_cast<size_t>(logLength));
		GLsizei written = 0;
		glGetShaderInfoLog(shader, logLength, &written, &infoLog[0]);
		if(status != GL_TRUE) {
			LogError << "Shader " << name << ": " << &infoLog[0];
		}
	}

	if(status != GL_TRUE) {
		glDeleteShader(shader);
		return 0;
	}

	return shader;
}

GLuint GLShaderPipeline::build(std::string_view name, std::string_view vertFallback, std::string_view fragFallback) {

	std::string vertName = std::string(name) + ".vert";
	std::string fragName = std::string(name) + ".frag";

	std::string vertSource = loadSource(vertName, vertFallback);
	std::string fragSource = loadSource(fragName, fragFallback);

	GLuint vert = compile(GL_VERTEX_SHADER, vertName, vertSource);
	if(!vert) {
		return 0;
	}
	GLuint frag = compile(GL_FRAGMENT_SHADER, fragName, fragSource);
	if(!frag) {
		glDeleteShader(vert);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vert);
	glAttachShader(program, frag);

	glBindAttribLocation(program, AttribPosition, "a_position");
	glBindAttribLocation(program, AttribColor, "a_color");
	glBindAttribLocation(program, AttribTexCoord0, "a_texcoord0");
	glBindAttribLocation(program, AttribTexCoord1, "a_texcoord1");
	glBindAttribLocation(program, AttribTexCoord2, "a_texcoord2");
	glBindAttribLocation(program, AttribNormal, "a_normal");
	glBindAttribLocation(program, AttribWorldPos, "a_worldPos");
	glBindAttribLocation(program, AttribCaster, "a_caster");
	glBindFragDataLocation(program, 0, "fragColor");

	glLinkProgram(program);

	// The shader objects are no longer needed once linked
	glDetachShader(program, vert);
	glDetachShader(program, frag);
	glDeleteShader(vert);
	glDeleteShader(frag);

	GLint status = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	GLint logLength = 0;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
	if(logLength > 1) {
		std::vector<GLchar> infoLog(static_cast<size_t>(logLength));
		GLsizei written = 0;
		glGetProgramInfoLog(program, logLength, &written, &infoLog[0]);
		if(status != GL_TRUE) {
			LogError << "Program " << name << ": " << &infoLog[0];
		}
	}

	if(status != GL_TRUE) {
		glDeleteProgram(program);
		return 0;
	}

	return program;
}

bool GLShaderPipeline::init() {

	shutdown();

	GLuint program = build("legacy", shadersources::legacy_vert, shadersources::legacy_frag);
	if(!program) {
		return false;
	}
	m_program = program;

	m_shadowProgram = build("shadow", shadersources::shadow_vert, shadersources::shadow_frag);
	if(m_shadowProgram) {
		m_uShadowViewProj = glGetUniformLocation(m_shadowProgram, "u_lightViewProj");
		m_uShadowTransform = glGetUniformLocation(m_shadowProgram, "u_transform");
		m_uShadowLightPos = glGetUniformLocation(m_shadowProgram, "u_lightPos");
		m_uShadowFallend = glGetUniformLocation(m_shadowProgram, "u_lightFallend");
		m_uShadowTextured = glGetUniformLocation(m_shadowProgram, "u_textured");
		m_uShadowOwner = glGetUniformLocation(m_shadowProgram, "u_lightOwner");
		glUseProgram(m_shadowProgram);
		glUniform1i(glGetUniformLocation(m_shadowProgram, "u_texture0"), 0);
	} else {
		LogWarning << "Shadow shader unavailable, shadows disabled";
	}

	m_uMVP = glGetUniformLocation(m_program, "u_mvp");
	m_uView = glGetUniformLocation(m_program, "u_view");
	m_uTransform = glGetUniformLocation(m_program, "u_transform");
	m_uStages[0] = glGetUniformLocation(m_program, "u_stage0");
	m_uStages[1] = glGetUniformLocation(m_program, "u_stage1");
	m_uStages[2] = glGetUniformLocation(m_program, "u_stage2");
	m_uFogEnabled = glGetUniformLocation(m_program, "u_fogEnabled");
	m_uFogColor = glGetUniformLocation(m_program, "u_fogColor");
	m_uFogRange = glGetUniformLocation(m_program, "u_fogRange");
	m_uPixelLighting = glGetUniformLocation(m_program, "u_pixelLighting");
	m_uLightCount = glGetUniformLocation(m_program, "u_lightCount");
	m_uDynamicLightCount = glGetUniformLocation(m_program, "u_dynamicLightCount");
	m_uLightPos = glGetUniformLocation(m_program, "u_lightPos");
	m_uLightColor = glGetUniformLocation(m_program, "u_lightColor");
	m_uShadowCount = glGetUniformLocation(m_program, "u_shadowCount");
	m_uNormalMapped = glGetUniformLocation(m_program, "u_normalMapped");
	m_uNormalStrength = glGetUniformLocation(m_program, "u_normalStrength");

	glUseProgram(m_program);

	// Samplers always map to the texture unit of their stage
	glUniform1i(glGetUniformLocation(m_program, "u_texture0"), 0);
	glUniform1i(glGetUniformLocation(m_program, "u_texture1"), 1);
	glUniform1i(glGetUniformLocation(m_program, "u_texture2"), 2);
	// Shadow cube maps live on texture units 4..7 (0..2 are the texture stages)
	glUniform1i(glGetUniformLocation(m_program, "u_shadow0"), 4);
	glUniform1i(glGetUniformLocation(m_program, "u_shadow1"), 5);
	glUniform1i(glGetUniformLocation(m_program, "u_shadow2"), 6);
	glUniform1i(glGetUniformLocation(m_program, "u_shadow3"), 7);
	glUniform1i(glGetUniformLocation(m_program, "u_normalMap"), 3);

	resetCache();

	return true;
}

bool GLShaderPipeline::reload() {

	if(!m_program) {
		return init();
	}

	GLuint old = m_program;
	GLuint oldShadow = m_shadowProgram;
	std::unique_ptr<GLShadowMaps> shadows = std::move(m_shadows);
	m_program = 0;
	m_shadowProgram = 0;

	bool ok = init();
	if(!ok) {
		m_program = old;
		m_shadowProgram = oldShadow;
		glUseProgram(m_program);
		resetCache();
	} else {
		glDeleteProgram(old);
		if(oldShadow) {
			glDeleteProgram(oldShadow);
		}
	}

	// The cube maps do not depend on the programs
	m_shadows = std::move(shadows);

	return ok;
}

void GLShaderPipeline::shutdown() {

	m_shadows.reset();

	if(m_shadowProgram) {
		glDeleteProgram(m_shadowProgram);
		m_shadowProgram = 0;
	}

	if(m_program) {
		glUseProgram(0);
		glDeleteProgram(m_program);
		m_program = 0;
	}

}

void GLShaderPipeline::resetCache() {

	m_glTransform = -1;
	m_matricesDirty = true;
	m_orthoDirty = true;
	for(StageState & stage : m_glStages) {
		stage.colorOp = -1;
		stage.alphaOp = -1;
	}
	m_glFogEnabled = -1;
	m_fogDirty = true;
	m_glPixelLighting = -1;
	m_lightsDirty = true;
	m_glShadowCount = -1;
	m_glShadowTransform = -1;
	m_glShadowTextured = -1;
	m_glNormalMap = nullptr;
	m_glNormalMapped = -1;
	m_normalStrengthDirty = true;

}

bool GLShaderPipeline::initShadows(size_t count, int resolution) {

	m_shadows.reset();
	m_shadowedLights = 0;
	m_glShadowCount = -1;

	if(count == 0 || !m_shadowProgram) {
		return false;
	}

	auto shadows = std::make_unique<GLShadowMaps>();
	if(!shadows->init(count, resolution)) {
		return false;
	}
	m_shadows = std::move(shadows);

	return true;
}

void GLShaderPipeline::renderShadowMaps(ShadowCasterDrawFunc drawCasters) {

	m_shadowedLights = 0;
	m_glShadowCount = -1; // upload the (possibly changed) count on the next draw

	if(!m_shadows || !m_program || !m_shadowProgram) {
		return;
	}

	size_t count = std::min({ m_shadows->count(), m_dynamicLightCount, m_lights.size() });
	if(count == 0) {
		return;
	}

	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	if(scissor) {
		glDisable(GL_SCISSOR_TEST);
	}

	// The scene may be rendering off-screen (post-processing): come back to that target
	GLint sceneFramebuffer = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sceneFramebuffer);

	m_shadowPass = true;
	glUseProgram(m_shadowProgram);
	m_glShadowTransform = -1;
	m_glShadowTextured = -1;

	for(size_t i = 0; i < count; i++) {
		const RendererLight & light = m_lights[i];
		glm::vec3 pos(light.pos.x, light.pos.y, light.pos.z);
		glUniform3fv(m_uShadowLightPos, 1, &pos.x);
		glUniform1f(m_uShadowFallend, light.fallend);
		glUniform1f(m_uShadowOwner, float(light.owner));
		for(int face = 0; face < 6; face++) {
			glm::mat4 viewProj = m_shadows->beginFace(i, face, pos, light.fallend);
			glUniformMatrix4fv(m_uShadowViewProj, 1, GL_FALSE, glm::value_ptr(viewProj));
			drawCasters(light);
		}
	}
	m_shadows->end();
	glBindFramebuffer(GL_FRAMEBUFFER, GLuint(sceneFramebuffer));
	m_shadowedLights = count;
	

	glUseProgram(m_program);
	m_shadowPass = false;

	// The main program samples the cube maps from units 4..7
	for(size_t i = 0; i < count; i++) {
		glActiveTexture(GL_TEXTURE4 + GLenum(i));
		glBindTexture(GL_TEXTURE_CUBE_MAP, m_shadows->texture(i));
	}
	glActiveTexture(GL_TEXTURE0);

	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}

}

void GLShaderPipeline::restoreAfterExternalDraw() {
	if(m_program) {
		glUseProgram(m_program);
	}
	resetCache();
	m_renderer->forgetTextureBindings();
}

void GLShaderPipeline::applyShadowPass() {

	int transform = m_pretransformed ? 0 : 1;
	if(transform != m_glShadowTransform) {
		glUniform1i(m_uShadowTransform, transform);
		m_glShadowTransform = transform;
	}

	int textured = m_renderer->GetTextureStage(0)->hasTexture() ? 1 : 0;
	if(textured != m_glShadowTextured) {
		glUniform1i(m_uShadowTextured, textured);
		m_glShadowTextured = textured;
	}

}

void GLShaderPipeline::setLights(const RendererLight * lights, size_t dynamicCount, size_t count) {

	count = std::min(count, Renderer::MaxPixelLights);
	m_dynamicLightCount = std::min(dynamicCount, count);
	m_lights.assign(lights, lights + count);

	m_lightPos.resize(count);
	m_lightColor.resize(count);
	for(size_t i = 0; i < count; i++) {
		const RendererLight & light = lights[i];
		m_lightPos[i] = glm::vec4(light.pos.x, light.pos.y, light.pos.z, light.fallstart);
		m_lightColor[i] = glm::vec4(light.color.r, light.color.g, light.color.b, light.fallend);
	}
	m_lightsDirty = true;

}

void GLShaderPipeline::setMatrices(const glm::mat4 & view, const glm::mat4 & projection) {
	m_view = view;
	m_projection = projection;
	m_matricesDirty = true;
}

void GLShaderPipeline::setViewportSize(float width, float height) {
	if(m_viewportWidth != width || m_viewportHeight != height) {
		m_viewportWidth = width;
		m_viewportHeight = height;
		m_orthoDirty = true;
	}
}

bool GLShaderPipeline::fogEnabled() const {
	return m_renderer->getRenderState().getFog();
}

void GLShaderPipeline::setFogColor(Color color) {
	Color4f c(color);
	m_fogColor = glm::vec3(c.r, c.g, c.b);
	m_fogDirty = true;
}

void GLShaderPipeline::setFogParams(float fogStart, float fogEnd) {
	m_fogRange = glm::vec2(fogStart, fogEnd);
	m_fogDirty = true;
}

void GLShaderPipeline::apply() {

	if(!m_program) {
		return;
	}

	if(m_shadowPass) {
		applyShadowPass();
		return;
	}
	if(m_externalPass) {
		return;
	}

	// Transform

	int transform = m_pretransformed ? 0 : 1;
	if(transform != m_glTransform) {
		glUniform1i(m_uTransform, transform);
		m_glTransform = transform;
		m_matricesDirty = true;
	}

	if(m_matricesDirty || (m_pretransformed && m_orthoDirty)) {
		if(m_pretransformed) {
			if(m_orthoDirty) {
				// Same mapping as OpenGLRenderer::disableTransform():
				// [0, width] x [0, height] to [-1, 1] x [-1, 1], flipped y, half-pixel offset
				m_ortho = glm::translate(glm::mat4(1.f), glm::vec3(-1.f, 1.f, 0.f));
				m_ortho = glm::scale(m_ortho, glm::vec3(2.f / m_viewportWidth, -2.f / m_viewportHeight, 1.f));
				m_ortho = glm::translate(m_ortho, glm::vec3(0.5f, 0.5f, 0.f));
				m_orthoDirty = false;
			}
			glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, glm::value_ptr(m_ortho));
		} else {
			glm::mat4 mvp = m_projection * m_view;
			glUniformMatrix4fv(m_uMVP, 1, GL_FALSE, glm::value_ptr(mvp));
			glUniformMatrix4fv(m_uView, 1, GL_FALSE, glm::value_ptr(m_view));
		}
		m_matricesDirty = false;
	}

	// Texture stages

	size_t stageCount = std::min(MaxStages, m_renderer->getTextureStageCount());
	for(size_t i = 0; i < stageCount; i++) {
		GLTextureStage * stage = m_renderer->GetTextureStage(i);
		int colorOp = 0;
		int alphaOp = 0;
		if(stage->hasTexture()) {
			colorOp = int(stage->getColorOp());
			alphaOp = int(stage->getAlphaOp());
		}
		if(colorOp != m_glStages[i].colorOp || alphaOp != m_glStages[i].alphaOp) {
			glUniform2i(m_uStages[i], colorOp, alphaOp);
			m_glStages[i].colorOp = colorOp;
			m_glStages[i].alphaOp = alphaOp;
		}
	}

	// Fog

	int fogEnabled = m_renderer->getRenderState().getFog() ? 1 : 0;
	if(fogEnabled != m_glFogEnabled) {
		glUniform1i(m_uFogEnabled, fogEnabled);
		m_glFogEnabled = fogEnabled;
	}
	if(m_fogDirty) {
		glUniform3fv(m_uFogColor, 1, glm::value_ptr(m_fogColor));
		glUniform2fv(m_uFogRange, 1, glm::value_ptr(m_fogRange));
		m_fogDirty = false;
	}

	// Per-pixel lighting

	int pixelLighting = (m_pixelLighting && !m_pretransformed) ? 1 : 0;
	if(pixelLighting != m_glPixelLighting) {
		glUniform1i(m_uPixelLighting, pixelLighting);
		m_glPixelLighting = pixelLighting;
	}
	// Normal map of the current material, on texture unit 3 (the shader only uses it when lighting)
	GLTexture * normalMap = (m_normalStrength > 0.f) ? m_normalMap : nullptr;
	if(normalMap != m_glNormalMap) {
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, normalMap ? normalMap->id() : 0);
		if(normalMap) {
			normalMap->apply(m_renderer->GetTextureStage(0));
		}
		glActiveTexture(GL_TEXTURE0);
		m_glNormalMap = normalMap;
	}
	int normalMapped = normalMap ? 1 : 0;
	if(normalMapped != m_glNormalMapped) {
		glUniform1i(m_uNormalMapped, normalMapped);
		m_glNormalMapped = normalMapped;
	}
	if(m_normalStrengthDirty) {
		glUniform1f(m_uNormalStrength, m_normalStrength);
		m_normalStrengthDirty = false;
	}
	
	int shadowCount = int(m_shadowedLights);
	if(shadowCount != m_glShadowCount) {
		glUniform1i(m_uShadowCount, shadowCount);
		m_glShadowCount = shadowCount;
	}
	if(m_lightsDirty) {
		GLsizei count = GLsizei(m_lightPos.size());
		glUniform1i(m_uLightCount, count);
		glUniform1i(m_uDynamicLightCount, GLsizei(m_dynamicLightCount));
		if(count > 0) {
			glUniform4fv(m_uLightPos, count, glm::value_ptr(m_lightPos[0]));
			glUniform4fv(m_uLightColor, count, glm::value_ptr(m_lightColor[0]));
		}
		m_lightsDirty = false;
	}

}
