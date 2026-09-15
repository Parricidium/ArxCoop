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

#include "graphics/opengl/GLPostProcess.h"

#include <algorithm>

#include <glm/glm.hpp>

#include "graphics/opengl/GLShaderPipeline.h"
#include "graphics/opengl/GLShaderSources.h"
#include "io/log/Logger.h"

// SMAA lookup textures (Jorge Jimenez et al., MIT license, see smaa/LICENSE.txt)
#include "graphics/opengl/smaa/AreaTex.h"
#include "graphics/opengl/smaa/SearchTex.h"

GLPostProcess::GLPostProcess(GLShaderPipeline * pipeline)
	: m_pipeline(pipeline)
	, m_extractProgram(0)
	, m_blurProgram(0)
	, m_ssaoProgram(0)
	, m_finalProgram(0)
	, m_smaaEdgeProgram(0)
	, m_smaaWeightProgram(0)
	, m_smaaBlendProgram(0)
	, m_uSmaaEdgeMetrics(-1)
	, m_uSmaaWeightMetrics(-1)
	, m_uSmaaBlendMetrics(-1)
	, m_areaTexture(0)
	, m_searchTexture(0)
	, m_compositeFramebuffer(0)
	, m_compositeTexture(0)
	, m_edgesFramebuffer(0)
	, m_edgesTexture(0)
	, m_blendFramebuffer(0)
	, m_blendTexture(0)
	, m_uSsaoProjection(-1)
	, m_uSsaoInvSize(-1)
	, m_uSsaoRadius(-1)
	, m_uSsaoBias(-1)
	, m_uFinalAo(-1)
	, m_uFinalDebug(-1)
	, m_uExtractThreshold(-1)
	, m_uBlurDirection(-1)
	, m_uFinalBloom(-1)
	, m_uFinalFxaa(-1)
	, m_uFinalInvSize(-1)
	, m_vao(0)
	, m_width(0)
	, m_height(0)
	, m_samples(0)
	, m_sceneFramebuffer(0)
	, m_sceneColorBuffer(0)
	, m_sceneDepthBuffer(0)
	, m_resolveFramebuffer(0)
	, m_sceneTexture(0)
	, m_depthTexture(0)
	, m_bloomWidth(0)
	, m_bloomHeight(0)
	, m_inScene(false)
{
	m_bloomFramebuffer[0] = m_bloomFramebuffer[1] = 0;
	m_bloomTexture[0] = m_bloomTexture[1] = 0;
	m_aoFramebuffer[0] = m_aoFramebuffer[1] = 0;
	m_aoTexture[0] = m_aoTexture[1] = 0;
}

GLPostProcess::~GLPostProcess() {
	shutdown();
}

bool GLPostProcess::init() {

	shutdown();

	m_extractProgram = m_pipeline->buildProgram("post_extract", shadersources::post_vert, shadersources::post_extract_frag);
	m_blurProgram = m_pipeline->buildProgram("post_blur", shadersources::post_vert, shadersources::post_blur_frag);
	m_ssaoProgram = m_pipeline->buildProgram("post_ssao", shadersources::post_vert, shadersources::post_ssao_frag);
	m_finalProgram = m_pipeline->buildProgram("post_final", shadersources::post_vert, shadersources::post_final_frag);
	if(!m_extractProgram || !m_blurProgram || !m_ssaoProgram || !m_finalProgram) {
		LogWarning << "Post-processing shaders unavailable, post-processing disabled";
		shutdown();
		return false;
	}

	glUseProgram(m_extractProgram);
	glUniform1i(glGetUniformLocation(m_extractProgram, "u_scene"), 0);
	m_uExtractThreshold = glGetUniformLocation(m_extractProgram, "u_threshold");

	glUseProgram(m_blurProgram);
	glUniform1i(glGetUniformLocation(m_blurProgram, "u_source"), 0);
	m_uBlurDirection = glGetUniformLocation(m_blurProgram, "u_direction");

	glUseProgram(m_ssaoProgram);
	glUniform1i(glGetUniformLocation(m_ssaoProgram, "u_depth"), 0);
	m_uSsaoProjection = glGetUniformLocation(m_ssaoProgram, "u_projection");
	m_uSsaoInvSize = glGetUniformLocation(m_ssaoProgram, "u_invSize");
	m_uSsaoRadius = glGetUniformLocation(m_ssaoProgram, "u_radius");
	m_uSsaoBias = glGetUniformLocation(m_ssaoProgram, "u_bias");

	glUseProgram(m_finalProgram);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_scene"), 0);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_bloomTexture"), 1);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_aoTexture"), 2);
	m_uFinalAo = glGetUniformLocation(m_finalProgram, "u_ao");
	m_uFinalDebug = glGetUniformLocation(m_finalProgram, "u_debug");
	m_uFinalBloom = glGetUniformLocation(m_finalProgram, "u_bloom");
	m_uFinalFxaa = glGetUniformLocation(m_finalProgram, "u_fxaa");
	m_uFinalInvSize = glGetUniformLocation(m_finalProgram, "u_invSize");

	// An empty vertex array object: the full-screen triangle comes from gl_VertexID
	glGenVertexArrays(1, &m_vao);

	if(!initSmaa()) {
		LogWarning << "SMAA shaders unavailable, FXAA will stand in";
	}

	// The pipeline's program must be current again, or its uniform updates would land here
	m_pipeline->restoreAfterExternalDraw();

	return true;
}

/*!
 * SMAA 1x: three programs sharing the SMAA source (smaa.glsl, overridable) behind a prelude
 * that selects GLSL 1.30 and the "high" preset, plus the two constant lookup textures.
 */
bool GLPostProcess::initSmaa() {

	static const char * const prelude = "#version 130\n"
		"#define SMAA_GLSL_3 1\n"
		"#define SMAA_PRESET_HIGH 1\n"
		"uniform vec4 u_rtMetrics; // 1/width, 1/height, width, height\n"
		"#define SMAA_RT_METRICS u_rtMetrics\n";

	m_smaaEdgeProgram = m_pipeline->buildProgram("smaa_edge", shadersources::smaa_edge_vert, shadersources::smaa_edge_frag,
	                                             prelude, "smaa.glsl", shadersources::smaa_glsl);
	m_smaaWeightProgram = m_pipeline->buildProgram("smaa_weight", shadersources::smaa_weight_vert, shadersources::smaa_weight_frag,
	                                               prelude, "smaa.glsl", shadersources::smaa_glsl);
	m_smaaBlendProgram = m_pipeline->buildProgram("smaa_blend", shadersources::smaa_blend_vert, shadersources::smaa_blend_frag,
	                                              prelude, "smaa.glsl", shadersources::smaa_glsl);
	if(!m_smaaEdgeProgram || !m_smaaWeightProgram || !m_smaaBlendProgram) {
		for(GLuint * program : { &m_smaaEdgeProgram, &m_smaaWeightProgram, &m_smaaBlendProgram }) {
			if(*program) {
				glDeleteProgram(*program);
				*program = 0;
			}
		}
		return false;
	}

	glUseProgram(m_smaaEdgeProgram);
	glUniform1i(glGetUniformLocation(m_smaaEdgeProgram, "u_scene"), 0);
	m_uSmaaEdgeMetrics = glGetUniformLocation(m_smaaEdgeProgram, "u_rtMetrics");
	glUseProgram(m_smaaWeightProgram);
	glUniform1i(glGetUniformLocation(m_smaaWeightProgram, "u_edges"), 0);
	glUniform1i(glGetUniformLocation(m_smaaWeightProgram, "u_area"), 1);
	glUniform1i(glGetUniformLocation(m_smaaWeightProgram, "u_search"), 2);
	m_uSmaaWeightMetrics = glGetUniformLocation(m_smaaWeightProgram, "u_rtMetrics");
	glUseProgram(m_smaaBlendProgram);
	glUniform1i(glGetUniformLocation(m_smaaBlendProgram, "u_scene"), 0);
	glUniform1i(glGetUniformLocation(m_smaaBlendProgram, "u_blend"), 1);
	m_uSmaaBlendMetrics = glGetUniformLocation(m_smaaBlendProgram, "u_rtMetrics");

	// The lookup textures: the area texture is filtered bilinearly, the search texture is not
	glGenTextures(1, &m_areaTexture);
	glBindTexture(GL_TEXTURE_2D, m_areaTexture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT, 0, GL_RG, GL_UNSIGNED_BYTE, areaTexBytes);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &m_searchTexture);
	glBindTexture(GL_TEXTURE_2D, m_searchTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, searchTexBytes);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);

	return true;
}

void GLPostProcess::shutdown() {

	destroyBuffers();

	if(m_vao) {
		glDeleteVertexArrays(1, &m_vao);
		m_vao = 0;
	}
	for(GLuint * program : { &m_extractProgram, &m_blurProgram, &m_ssaoProgram, &m_finalProgram,
	                         &m_smaaEdgeProgram, &m_smaaWeightProgram, &m_smaaBlendProgram }) {
		if(*program) {
			glDeleteProgram(*program);
			*program = 0;
		}
	}
	for(GLuint * tex : { &m_areaTexture, &m_searchTexture }) {
		if(*tex) {
			glDeleteTextures(1, tex);
			*tex = 0;
		}
	}

	m_inScene = false;
}

static GLuint createColorTexture(int width, int height) {
	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return texture;
}

static bool framebufferComplete(const char * what) {
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if(status != GL_FRAMEBUFFER_COMPLETE) {
		LogWarning << "Post-processing " << what << " framebuffer incomplete (status " << status << ")";
		return false;
	}
	return true;
}

bool GLPostProcess::createBuffers(int width, int height, int samples) {

	destroyBuffers();

	m_width = width;
	m_height = height;
	m_samples = samples;

	// Scene target, multisampled when the window is
	glGenRenderbuffers(1, &m_sceneColorBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, m_sceneColorBuffer);
	if(samples > 1) {
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
	} else {
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	}
	glGenRenderbuffers(1, &m_sceneDepthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, m_sceneDepthBuffer);
	if(samples > 1) {
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
	} else {
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	}
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &m_sceneFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_sceneColorBuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_sceneDepthBuffer);
	bool ok = framebufferComplete("scene");

	// Resolved copy (color and depth)
	m_sceneTexture = createColorTexture(width, height);
	glGenTextures(1, &m_depthTexture);
	glBindTexture(GL_TEXTURE_2D, m_depthTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers(1, &m_resolveFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneTexture, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depthTexture, 0);
	ok = framebufferComplete("resolve") && ok;

	// SMAA: the composed image, its edges and the blending weights, full size
	if(m_smaaEdgeProgram) {
		struct Target { GLuint * fb; GLuint * tex; const char * what; };
		for(Target target : { Target { &m_compositeFramebuffer, &m_compositeTexture, "composite" },
		                      Target { &m_edgesFramebuffer, &m_edgesTexture, "edges" },
		                      Target { &m_blendFramebuffer, &m_blendTexture, "blend" } }) {
			*target.tex = createColorTexture(width, height);
			glGenFramebuffers(1, target.fb);
			glBindFramebuffer(GL_FRAMEBUFFER, *target.fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *target.tex, 0);
			ok = framebufferComplete(target.what) && ok;
		}
	}

	// Ambient occlusion ping-pong at half resolution
	for(int i = 0; i < 2; i++) {
		m_aoTexture[i] = createColorTexture(std::max(width / 2, 1), std::max(height / 2, 1));
		glGenFramebuffers(1, &m_aoFramebuffer[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_aoFramebuffer[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_aoTexture[i], 0);
		ok = framebufferComplete("ao") && ok;
	}

	// Bloom ping-pong at half resolution
	m_bloomWidth = std::max(width / 2, 1);
	m_bloomHeight = std::max(height / 2, 1);
	for(int i = 0; i < 2; i++) {
		m_bloomTexture[i] = createColorTexture(m_bloomWidth, m_bloomHeight);
		glGenFramebuffers(1, &m_bloomFramebuffer[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFramebuffer[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bloomTexture[i], 0);
		ok = framebufferComplete("bloom") && ok;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if(!ok) {
		destroyBuffers();
		return false;
	}

	LogInfo << "Post-processing buffers: " << width << "x" << height << ", " << samples << " samples";

	return true;
}

void GLPostProcess::destroyBuffers() {

	for(GLuint * fb : { &m_sceneFramebuffer, &m_resolveFramebuffer, &m_bloomFramebuffer[0], &m_bloomFramebuffer[1],
	                    &m_aoFramebuffer[0], &m_aoFramebuffer[1], &m_compositeFramebuffer, &m_edgesFramebuffer,
	                    &m_blendFramebuffer }) {
		if(*fb) {
			glDeleteFramebuffers(1, fb);
			*fb = 0;
		}
	}
	for(GLuint * rb : { &m_sceneColorBuffer, &m_sceneDepthBuffer }) {
		if(*rb) {
			glDeleteRenderbuffers(1, rb);
			*rb = 0;
		}
	}
	for(GLuint * tex : { &m_sceneTexture, &m_depthTexture, &m_bloomTexture[0], &m_bloomTexture[1],
	                     &m_aoTexture[0], &m_aoTexture[1], &m_compositeTexture, &m_edgesTexture, &m_blendTexture }) {
		if(*tex) {
			glDeleteTextures(1, tex);
			*tex = 0;
		}
	}
	m_width = m_height = m_samples = 0;

}

void GLPostProcess::begin(int width, int height, int samples) {

	if(!isActive() || m_inScene || width <= 0 || height <= 0) {
		return;
	}

	if(width != m_width || height != m_height || samples != m_samples) {
		if(!createBuffers(width, height, samples)) {
			return;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFramebuffer);
	m_inScene = true;

	// The engine only clears (and draws) inside its viewport, which can be a letterbox band:
	// the window outside of it is black, so make the scene buffer black there too.
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean depthMask = GL_TRUE;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	if(scissor) {
		glDisable(GL_SCISSOR_TEST);
	}
	glDepthMask(GL_TRUE);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDepthMask(depthMask);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

}

void GLPostProcess::drawFullscreen() {
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool GLPostProcess::captureScene(bool depthOnly) {

	if(!m_inScene) {
		return false;
	}

	// The scissor test restricts glBlitFramebuffer too
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	if(scissor) {
		glDisable(GL_SCISSOR_TEST);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFramebuffer);
	if(!depthOnly) {
		glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFramebuffer);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}

	return true;
}

void GLPostProcess::end() {

	if(!m_inScene) {
		return;
	}
	m_inScene = false;

	// Full-screen passes: no depth, no blending, no alpha test, no scissor, no culling.
	// (The scissor test also restricts glBlitFramebuffer, so it goes first.)
	GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean alphaTest = glIsEnabled(GL_ALPHA_TEST);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean cull = glIsEnabled(GL_CULL_FACE);
	GLboolean multisample = glIsEnabled(GL_MULTISAMPLE);
	GLboolean depthMask = GL_TRUE;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_MULTISAMPLE);

	// 1. Resolve the scene into a texture
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_sceneFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFramebuffer);
	glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	bool ao = m_settings.ao > 0.f;
	if(ao) {
		glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}

	glBindVertexArray(m_vao);
	glActiveTexture(GL_TEXTURE0);

	if(ao) {
		// Ambient occlusion from the depth buffer, half size, then blurred
		const glm::mat4 & proj = m_pipeline->projection();
		glViewport(0, 0, m_bloomWidth, m_bloomHeight);
		glBindFramebuffer(GL_FRAMEBUFFER, m_aoFramebuffer[0]);
		glUseProgram(m_ssaoProgram);
		glUniform4f(m_uSsaoProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
		glUniform2f(m_uSsaoInvSize, 1.f / float(m_width), 1.f / float(m_height));
		glUniform1f(m_uSsaoRadius, m_settings.aoRadius);
		glUniform1f(m_uSsaoBias, 1.5f);
		glBindTexture(GL_TEXTURE_2D, m_depthTexture);
		drawFullscreen();
		glUseProgram(m_blurProgram);
		glBindFramebuffer(GL_FRAMEBUFFER, m_aoFramebuffer[1]);
		glUniform2f(m_uBlurDirection, 1.f / float(m_bloomWidth), 0.f);
		glBindTexture(GL_TEXTURE_2D, m_aoTexture[0]);
		drawFullscreen();
		glBindFramebuffer(GL_FRAMEBUFFER, m_aoFramebuffer[0]);
		glUniform2f(m_uBlurDirection, 0.f, 1.f / float(m_bloomHeight));
		glBindTexture(GL_TEXTURE_2D, m_aoTexture[1]);
		drawFullscreen();
	}

	bool bloom = m_settings.bloom > 0.f;
	if(bloom) {
		// 2. Bright parts, half size
		glViewport(0, 0, m_bloomWidth, m_bloomHeight);
		glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFramebuffer[0]);
		glUseProgram(m_extractProgram);
		glUniform1f(m_uExtractThreshold, m_settings.bloomThreshold);
		glBindTexture(GL_TEXTURE_2D, m_sceneTexture);
		drawFullscreen();
		// 3. Blur, two iterations of horizontal + vertical
		glUseProgram(m_blurProgram);
		for(int i = 0; i < 2; i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFramebuffer[1]);
			glUniform2f(m_uBlurDirection, 1.f / float(m_bloomWidth), 0.f);
			glBindTexture(GL_TEXTURE_2D, m_bloomTexture[0]);
			drawFullscreen();
			glBindFramebuffer(GL_FRAMEBUFFER, m_bloomFramebuffer[0]);
			glUniform2f(m_uBlurDirection, 0.f, 1.f / float(m_bloomHeight));
			glBindTexture(GL_TEXTURE_2D, m_bloomTexture[1]);
			drawFullscreen();
		}
	}

	// 4. Final image to the window, or to the composite buffer that SMAA then filters
	bool smaa = m_settings.smaa && m_smaaEdgeProgram && m_compositeFramebuffer && m_settings.debugView == 0;
	static int loggedSmaa = -1;
	if(int(smaa) != loggedSmaa) {
		loggedSmaa = int(smaa);
		LogInfo << "Post-processing: SMAA " << (smaa ? "on" : "off");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, smaa ? m_compositeFramebuffer : 0);
	glViewport(0, 0, m_width, m_height);
	glUseProgram(m_finalProgram);
	glUniform1f(m_uFinalBloom, bloom ? m_settings.bloom : 0.f);
	glUniform1i(m_uFinalFxaa, (m_settings.fxaa && !smaa) ? 1 : 0);
	glUniform1f(m_uFinalAo, ao ? m_settings.ao : 0.f);
	glUniform1i(m_uFinalDebug, m_settings.debugView);
	glUniform2f(m_uFinalInvSize, 1.f / float(m_width), 1.f / float(m_height));
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ao ? m_aoTexture[0] : 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, bloom ? m_bloomTexture[0] : 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_sceneTexture);
	drawFullscreen();

	if(smaa) {
		// 5. SMAA: edges of the composed image, blending weights, then the blended image to the window.
		// The edge and weight passes discard the pixels they do not touch: clear them first.
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glBindFramebuffer(GL_FRAMEBUFFER, m_edgesFramebuffer);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(m_smaaEdgeProgram);
		glUniform4f(m_uSmaaEdgeMetrics, 1.f / float(m_width), 1.f / float(m_height), float(m_width), float(m_height));
		glBindTexture(GL_TEXTURE_2D, m_compositeTexture);
		drawFullscreen();
		glBindFramebuffer(GL_FRAMEBUFFER, m_blendFramebuffer);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(m_smaaWeightProgram);
		glUniform4f(m_uSmaaWeightMetrics, 1.f / float(m_width), 1.f / float(m_height), float(m_width), float(m_height));
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, m_searchTexture);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_areaTexture);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_edgesTexture);
		drawFullscreen();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glUseProgram(m_smaaBlendProgram);
		glUniform4f(m_uSmaaBlendMetrics, 1.f / float(m_width), 1.f / float(m_height), float(m_width), float(m_height));
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_blendTexture);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_compositeTexture);
		drawFullscreen();
	}

	// The HUD is drawn next into the window: give it a clean depth buffer
	glDepthMask(GL_TRUE);
	glClear(GL_DEPTH_BUFFER_BIT);
	glDepthMask(depthMask);

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);

	// Restore the state the engine believes is set
	if(depthTest) glEnable(GL_DEPTH_TEST);
	if(blend) glEnable(GL_BLEND);
	if(alphaTest) glEnable(GL_ALPHA_TEST);
	if(scissor) glEnable(GL_SCISSOR_TEST);
	if(cull) glEnable(GL_CULL_FACE);
	if(multisample) glEnable(GL_MULTISAMPLE);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	m_pipeline->restoreAfterExternalDraw();

}
