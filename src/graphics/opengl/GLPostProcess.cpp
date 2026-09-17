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

#include <string>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

#include <glm/glm.hpp>

#include "core/GameTime.h"
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
	, m_uFinalDarkness(-1)
	, m_uFinalVolumetric(-1)
	, m_volumeProgram(0)
	, m_uVolumeProjection(-1)
	, m_uVolumeInvView(-1)
	, m_uVolumeCameraPos(-1)
	, m_uVolumeDensity(-1)
	, m_uVolumeLightScale(-1)
	, m_uVolumeTime(-1)
	, m_uVolumeLightCount(-1)
	, m_uVolumeLightPos(-1)
	, m_uVolumeLightColor(-1)
	, m_uVolumeShadows(-1)
	, m_uVolumeLightShadow(-1)
	, m_traced(false)
	, m_gbuffer(false)
	, m_gbufferWrites(false)
	, m_traceProgram(0)
	, m_traceBlurProgram(0)
	, m_uTraceProjection(-1)
	, m_uTraceInvView(-1)
	, m_uTracePrevViewProj(-1)
	, m_uTraceCameraPos(-1)
	, m_uTraceFullSize(-1)
	, m_uTraceFrame(-1)
	, m_uTraceReset(-1)
	, m_uTraceStatic(-1)
	, m_uTraceAoRadius(-1)
	, m_uTraceLightCount(-1)
	, m_uTraceDynamicLightCount(-1)
	, m_uTraceLightPos(-1)
	, m_uTraceLightColor(-1)
	, m_uTraceBlurDirection(-1)
	, m_uFinalTraceMode(-1)
	, m_uFinalBounce(-1)
	, m_uFinalStaticMix(-1)
	, m_uFinalTraceSize(-1)
	, m_uFinalProjection(-1)
	, m_uFinalFogRange(-1)
	, m_traceIndex(0)
	, m_traceFrame(0)
	, m_traceReset(true)
	, m_prevViewProj(1.f)
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
	m_volumeTexture[0] = m_volumeTexture[1] = 0;
	m_volumeFramebuffer[0] = m_volumeFramebuffer[1] = 0;
	m_aoTexture[0] = m_aoTexture[1] = 0;
	for(int i = 0; i < 3; i++) {
		m_gbufferBuffer[i] = m_gbufferTexture[i] = 0;
	}
	for(int i = 0; i < 2; i++) {
		m_traceFramebuffer[i] = m_traceBlurFramebuffer[i] = 0;
		m_traceTexture[i][0] = m_traceTexture[i][1] = 0;
		m_traceBlurTexture[i][0] = m_traceBlurTexture[i][1] = 0;
	}
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
	// The haze: with the ray tracing code when the pipeline traces (shadowed light shafts)
	m_traced = (m_pipeline->rayTracing() > 0);
	if(m_traced) {
		m_volumeProgram = m_pipeline->buildProgram("post_volume", shadersources::post_vert, shadersources::post_volume_frag,
		                                           "#version 430\n#define ARX_RT 1\n", "rt_common.glsl", shadersources::rt_common_glsl);
		if(!m_volumeProgram) {
			LogWarning << "Ray-traced haze shader unavailable, falling back to the plain one";
			m_traced = false;
		}
	}
	if(!m_volumeProgram) {
		m_volumeProgram = m_pipeline->buildProgram("post_volume", shadersources::post_vert, shadersources::post_volume_frag);
		if(!m_volumeProgram) {
			LogWarning << "Volumetric haze shader unavailable, no haze"; // the rest of the post-processing stays
		}
	}
	// The traced lighting: the main program then writes the G-buffer (GLShaderPipeline)
	m_gbuffer = m_pipeline->tracedLighting();
	if(m_gbuffer) {
		m_traceProgram = m_pipeline->buildProgram("post_trace", shadersources::post_vert, shadersources::post_trace_frag,
		                                          "#version 430\n#define ARX_RT 1\n", "rt_common.glsl", shadersources::rt_common_glsl);
		m_traceBlurProgram = m_pipeline->buildProgram("post_trace_blur", shadersources::post_vert, shadersources::post_trace_blur_frag);
		if(!m_traceProgram || !m_traceBlurProgram) {
			LogWarning << "Traced lighting shaders unavailable, traced lighting disabled";
			m_gbuffer = false;
		}
	}
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

	glUseProgram(m_volumeProgram); // 0 when unavailable: the locations below are then -1 and ignored
	glUniform1i(glGetUniformLocation(m_volumeProgram, "u_depth"), 0);
	if(m_traced) {
		glUniform1i(glGetUniformLocation(m_volumeProgram, "u_rtTextures"), 10); // bound by the pipeline
	}
	m_uVolumeProjection = glGetUniformLocation(m_volumeProgram, "u_projection");
	m_uVolumeInvView = glGetUniformLocation(m_volumeProgram, "u_invView");
	m_uVolumeCameraPos = glGetUniformLocation(m_volumeProgram, "u_cameraPos");
	m_uVolumeDensity = glGetUniformLocation(m_volumeProgram, "u_density");
	m_uVolumeLightScale = glGetUniformLocation(m_volumeProgram, "u_lightScale");
	m_uVolumeTime = glGetUniformLocation(m_volumeProgram, "u_time");
	m_uVolumeLightCount = glGetUniformLocation(m_volumeProgram, "u_lightCount");
	m_uVolumeLightPos = glGetUniformLocation(m_volumeProgram, "u_lightPos");
	m_uVolumeLightColor = glGetUniformLocation(m_volumeProgram, "u_lightColor");
	m_uVolumeShadows = glGetUniformLocation(m_volumeProgram, "u_shadows");
	m_uVolumeLightShadow = glGetUniformLocation(m_volumeProgram, "u_lightShadow");
	// The shadow cube maps of the main pipeline stay on units 4..7
	for(int i = 0; i < 4; i++) {
		glUniform1i(glGetUniformLocation(m_volumeProgram, ("u_shadow" + std::to_string(i)).c_str()), 4 + i);
	}

	if(m_gbuffer) {
		glUseProgram(m_traceProgram);
		glUniform1i(glGetUniformLocation(m_traceProgram, "u_depth"), 0);
		glUniform1i(glGetUniformLocation(m_traceProgram, "u_normal"), 1);
		glUniform1i(glGetUniformLocation(m_traceProgram, "u_historyA"), 12);
		glUniform1i(glGetUniformLocation(m_traceProgram, "u_historyB"), 13);
		glUniform1i(glGetUniformLocation(m_traceProgram, "u_rtTextures"), 10); // bound by the pipeline
		m_uTraceProjection = glGetUniformLocation(m_traceProgram, "u_projection");
		m_uTraceInvView = glGetUniformLocation(m_traceProgram, "u_invView");
		m_uTracePrevViewProj = glGetUniformLocation(m_traceProgram, "u_prevViewProj");
		m_uTraceCameraPos = glGetUniformLocation(m_traceProgram, "u_cameraPos");
		m_uTraceFullSize = glGetUniformLocation(m_traceProgram, "u_fullSize");
		m_uTraceFrame = glGetUniformLocation(m_traceProgram, "u_frame");
		m_uTraceReset = glGetUniformLocation(m_traceProgram, "u_reset");
		m_uTraceStatic = glGetUniformLocation(m_traceProgram, "u_static");
		m_uTraceAoRadius = glGetUniformLocation(m_traceProgram, "u_aoRadius");
		m_uTraceLightCount = glGetUniformLocation(m_traceProgram, "u_lightCount");
		m_uTraceDynamicLightCount = glGetUniformLocation(m_traceProgram, "u_dynamicLightCount");
		m_uTraceLightPos = glGetUniformLocation(m_traceProgram, "u_lightPos");
		m_uTraceLightColor = glGetUniformLocation(m_traceProgram, "u_lightColor");
		glUseProgram(m_traceBlurProgram);
		glUniform1i(glGetUniformLocation(m_traceBlurProgram, "u_sourceA"), 0);
		glUniform1i(glGetUniformLocation(m_traceBlurProgram, "u_sourceB"), 1);
		glUniform1i(glGetUniformLocation(m_traceBlurProgram, "u_normal"), 2);
		m_uTraceBlurDirection = glGetUniformLocation(m_traceBlurProgram, "u_direction");
		m_traceReset = true;
	}

	glUseProgram(m_finalProgram);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_scene"), 0);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_bloomTexture"), 1);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_aoTexture"), 2);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_volumeTexture"), 3);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_traceA"), 4);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_traceB"), 5);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_albedo"), 6);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_static"), 7);
	glUniform1i(glGetUniformLocation(m_finalProgram, "u_depth"), 8);
	m_uFinalTraceMode = glGetUniformLocation(m_finalProgram, "u_traceMode");
	m_uFinalBounce = glGetUniformLocation(m_finalProgram, "u_bounce");
	m_uFinalStaticMix = glGetUniformLocation(m_finalProgram, "u_staticMix");
	m_uFinalTraceSize = glGetUniformLocation(m_finalProgram, "u_traceSize");
	m_uFinalProjection = glGetUniformLocation(m_finalProgram, "u_projection");
	m_uFinalFogRange = glGetUniformLocation(m_finalProgram, "u_fogRange");
m_uFinalVolumetric = glGetUniformLocation(m_finalProgram, "u_volumetric");
	m_uFinalAo = glGetUniformLocation(m_finalProgram, "u_ao");
	m_uFinalDarkness = glGetUniformLocation(m_finalProgram, "u_darkness");
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
	// (the renderer keeps GL_UNPACK_ALIGNMENT at 1 for the whole game: leave it alone, the
	// engine's own textures are tightly packed and a 4-byte alignment made the driver read past
	// the end of RGB images whose width is not a multiple of 4 - the cinematic tiles crashed)
	glGenTextures(1, &m_areaTexture);
	glBindTexture(GL_TEXTURE_2D, m_areaTexture);
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
	glBindTexture(GL_TEXTURE_2D, 0);

	return true;
}

void GLPostProcess::shutdown() {

	destroyBuffers();

	if(m_vao) {
		glDeleteVertexArrays(1, &m_vao);
		m_vao = 0;
	}
	for(GLuint * program : { &m_extractProgram, &m_blurProgram, &m_ssaoProgram, &m_finalProgram, &m_volumeProgram,
	                         &m_smaaEdgeProgram, &m_smaaWeightProgram, &m_smaaBlendProgram,
	                         &m_traceProgram, &m_traceBlurProgram }) {
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

	// The G-buffer of the traced lighting: three more targets next to the scene colour
	if(m_gbuffer) {
		for(int i = 0; i < 3; i++) {
			glGenRenderbuffers(1, &m_gbufferBuffer[i]);
			glBindRenderbuffer(GL_RENDERBUFFER, m_gbufferBuffer[i]);
			if(samples > 1) {
				glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
			} else {
				glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
			}
		}
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	glGenFramebuffers(1, &m_sceneFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_sceneColorBuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_sceneDepthBuffer);
	if(m_gbuffer) {
		for(int i = 0; i < 3; i++) {
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1 + GLenum(i), GL_RENDERBUFFER, m_gbufferBuffer[i]);
		}
	}
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
	if(m_gbuffer) {
		for(int i = 0; i < 3; i++) {
			m_gbufferTexture[i] = createColorTexture(width, height);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1 + GLenum(i), GL_TEXTURE_2D, m_gbufferTexture[i], 0);
		}
	}
	ok = framebufferComplete("resolve") && ok;

	// Traced lighting at half size: the accumulated outputs (history ping-pong) and their blur.
	// 16-bit normalised textures: a float format is sampled as zeros by the water pass on some
	// setups (GLRipples.cpp), the values are scaled instead.
	if(m_gbuffer) {
		int tw = std::max(width / 2, 1);
		int th = std::max(height / 2, 1);
		struct Target { GLuint * fb; GLuint * tex; const char * what; };
		for(Target target : { Target { &m_traceFramebuffer[0], m_traceTexture[0], "trace" },
		                      Target { &m_traceFramebuffer[1], m_traceTexture[1], "trace" },
		                      Target { &m_traceBlurFramebuffer[0], m_traceBlurTexture[0], "trace blur" },
		                      Target { &m_traceBlurFramebuffer[1], m_traceBlurTexture[1], "trace blur" } }) {
			glGenFramebuffers(1, target.fb);
			glBindFramebuffer(GL_FRAMEBUFFER, *target.fb);
			for(int i = 0; i < 2; i++) {
				glGenTextures(1, &target.tex[i]);
				glBindTexture(GL_TEXTURE_2D, target.tex[i]);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, tw, th, 0, GL_RGBA, GL_UNSIGNED_SHORT, nullptr);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + GLenum(i), GL_TEXTURE_2D, target.tex[i], 0);
			}
			const GLenum both[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			glDrawBuffers(2, both);
			ok = framebufferComplete(target.what) && ok;
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		m_traceReset = true;
	}

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

	// Volumetric haze ping-pong at half resolution
	for(int i = 0; i < 2; i++) {
		m_volumeTexture[i] = createColorTexture(std::max(width / 2, 1), std::max(height / 2, 1));
		glGenFramebuffers(1, &m_volumeFramebuffer[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, m_volumeFramebuffer[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_volumeTexture[i], 0);
		ok = framebufferComplete("volume") && ok;
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
	                    &m_aoFramebuffer[0], &m_aoFramebuffer[1], &m_volumeFramebuffer[0], &m_volumeFramebuffer[1],
	                    &m_compositeFramebuffer, &m_edgesFramebuffer,
	                    &m_blendFramebuffer, &m_traceFramebuffer[0], &m_traceFramebuffer[1],
	                    &m_traceBlurFramebuffer[0], &m_traceBlurFramebuffer[1] }) {
		if(*fb) {
			glDeleteFramebuffers(1, fb);
			*fb = 0;
		}
	}
	for(GLuint * rb : { &m_sceneColorBuffer, &m_sceneDepthBuffer, &m_gbufferBuffer[0], &m_gbufferBuffer[1], &m_gbufferBuffer[2] }) {
		if(*rb) {
			glDeleteRenderbuffers(1, rb);
			*rb = 0;
		}
	}
	for(GLuint * tex : { &m_sceneTexture, &m_depthTexture, &m_bloomTexture[0], &m_bloomTexture[1],
	                     &m_aoTexture[0], &m_aoTexture[1], &m_volumeTexture[0], &m_volumeTexture[1],
	                     &m_compositeTexture, &m_edgesTexture, &m_blendTexture,
	                     &m_gbufferTexture[0], &m_gbufferTexture[1], &m_gbufferTexture[2],
	                     &m_traceTexture[0][0], &m_traceTexture[0][1], &m_traceTexture[1][0], &m_traceTexture[1][1],
	                     &m_traceBlurTexture[0][0], &m_traceBlurTexture[0][1], &m_traceBlurTexture[1][0], &m_traceBlurTexture[1][1] }) {
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
	m_gbufferWrites = false;

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
	if(m_gbuffer) {
		// The G-buffer starts as "nothing lit here" (alpha 0), the scene colour as opaque black
		const GLenum targets[3] = { GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
		glDrawBuffers(3, targets);
		glClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		const GLenum scene = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &scene);
	}
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDepthMask(depthMask);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

}

void GLPostProcess::setGBufferWrites(bool enable) {
	if(!m_gbuffer || !m_inScene || enable == m_gbufferWrites) {
		return;
	}
	m_gbufferWrites = enable;
	static const GLenum all[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	glDrawBuffers(enable ? 4 : 1, all);
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
	bool traced = m_gbuffer && m_settings.traceMode >= 3 && m_traceProgram != 0;
	bool ao = m_settings.ao > 0.f && !traced; // the traced pass supplies the occlusion instead
	bool haze = m_settings.volumetric > 0.f && m_volumeProgram != 0;
	if(ao || haze || traced) {
		glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}
	if(traced) {
		// The G-buffer too, one target at a time (a blit writes every draw buffer of the target)
		for(int i = 0; i < 3; i++) {
			glReadBuffer(GL_COLOR_ATTACHMENT1 + GLenum(i));
			glDrawBuffer(GL_COLOR_ATTACHMENT1 + GLenum(i));
			glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
	}

	glBindVertexArray(m_vao);
	glActiveTexture(GL_TEXTURE0);

	if(traced) {
		// One ray per pixel at half size, accumulated over the frames, then blurred along the surfaces
		const glm::mat4 & proj = m_pipeline->projection();
		const glm::mat4 & view = m_pipeline->view();
		glm::mat4 invView = glm::inverse(view);
		glm::vec3 cameraPos(invView[3]);
		int next = m_traceIndex ^ 1;
		glViewport(0, 0, m_bloomWidth, m_bloomHeight);
		glBindFramebuffer(GL_FRAMEBUFFER, m_traceFramebuffer[next]);
		glUseProgram(m_traceProgram);
		glUniform4f(m_uTraceProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
		glUniformMatrix4fv(m_uTraceInvView, 1, GL_FALSE, glm::value_ptr(invView));
		glUniformMatrix4fv(m_uTracePrevViewProj, 1, GL_FALSE, glm::value_ptr(m_prevViewProj));
		glUniform3fv(m_uTraceCameraPos, 1, glm::value_ptr(cameraPos));
		glUniform2i(m_uTraceFullSize, m_width, m_height);
		glUniform1i(m_uTraceFrame, m_traceFrame);
		glUniform1i(m_uTraceReset, m_traceReset ? 1 : 0);
		glUniform1i(m_uTraceStatic, (m_settings.traceMode >= 4) ? 1 : 0);
		glUniform1f(m_uTraceAoRadius, std::max(m_settings.aoRadius * 2.f, 60.f));
		const std::vector<glm::vec4> & lightPos = m_pipeline->lightPositions();
		GLsizei lights = GLsizei(std::min(lightPos.size(), size_t(128)));
		glUniform1i(m_uTraceLightCount, lights);
		glUniform1i(m_uTraceDynamicLightCount, GLint(std::min(m_pipeline->dynamicLightCount(), size_t(lights))));
		if(lights > 0) {
			glUniform4fv(m_uTraceLightPos, lights, glm::value_ptr(lightPos[0]));
			glUniform4fv(m_uTraceLightColor, lights, glm::value_ptr(m_pipeline->lightColors()[0]));
		}
		glActiveTexture(GL_TEXTURE13);
		glBindTexture(GL_TEXTURE_2D, m_traceTexture[m_traceIndex][1]);
		glActiveTexture(GL_TEXTURE12);
		glBindTexture(GL_TEXTURE_2D, m_traceTexture[m_traceIndex][0]);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_gbufferTexture[0]);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_depthTexture);
		drawFullscreen();
		m_traceIndex = next;
		m_traceFrame++;
		m_traceReset = false;
		m_prevViewProj = proj * view;
		// Blur: horizontal into the first pair, vertical into the second
		glUseProgram(m_traceBlurProgram);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, m_gbufferTexture[0]);
		for(int pass = 0; pass < 2; pass++) {
			glBindFramebuffer(GL_FRAMEBUFFER, m_traceBlurFramebuffer[pass]);
			if(pass == 0) {
				glUniform2f(m_uTraceBlurDirection, 1.f / float(m_bloomWidth), 0.f);
			} else {
				glUniform2f(m_uTraceBlurDirection, 0.f, 1.f / float(m_bloomHeight));
			}
			const GLuint * source = (pass == 0) ? m_traceTexture[m_traceIndex] : m_traceBlurTexture[0];
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, source[1]);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, source[0]);
			drawFullscreen();
		}
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
	}

	if(ao) {
		// Ambient occlusion from the depth buffer, half size, then blurred
		const glm::mat4 & proj = m_pipeline->projection();
		glViewport(0, 0, m_bloomWidth, m_bloomHeight);
		glBindFramebuffer(GL_FRAMEBUFFER, m_aoFramebuffer[0]);
		glUseProgram(m_ssaoProgram);
		glUniform4f(m_uSsaoProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
		glUniform2f(m_uSsaoInvSize, 1.f / float(m_width), 1.f / float(m_height));
		glUniform1f(m_uSsaoRadius, m_settings.aoRadius);
		glUniform1f(m_uSsaoBias, 2.5f); // world units: the floor tiles of the levels meet with small steps
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

	if(haze) {
		// The volumetric haze along the rays, half size, then blurred (the alpha holds the transmittance)
		const glm::mat4 & proj = m_pipeline->projection();
		glm::mat4 invView = glm::inverse(m_pipeline->view());
		glm::vec3 cameraPos(invView[3]);
		glViewport(0, 0, m_bloomWidth, m_bloomHeight);
		glBindFramebuffer(GL_FRAMEBUFFER, m_volumeFramebuffer[0]);
		glUseProgram(m_volumeProgram);
		glUniform4f(m_uVolumeProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
		glUniformMatrix4fv(m_uVolumeInvView, 1, GL_FALSE, glm::value_ptr(invView));
		glUniform3fv(m_uVolumeCameraPos, 1, glm::value_ptr(cameraPos));
		glUniform1f(m_uVolumeDensity, m_settings.volumetric);
		glUniform1f(m_uVolumeLightScale, m_settings.volumetricLight);
		glUniform1f(m_uVolumeTime, float(toMsi(g_gameTime.now())) * 0.001f);
		glUniform1i(m_uVolumeShadows, (m_traced && m_settings.volumetricShadows) ? 1 : 0);
		// Only the lights whose room is in view (RendererLight::inView)
		const std::vector<glm::vec4> & lightPos = m_pipeline->lightPositions();
		const std::vector<glm::vec4> & lightColor = m_pipeline->lightColors();
		const std::vector<bool> & inView = m_pipeline->lightsInView();
		static std::vector<glm::vec4> hazePos, hazeColor;
		static std::vector<GLint> hazeShadow;
		hazePos.clear();
		hazeColor.clear();
		hazeShadow.clear();
		size_t shadowed = std::min(m_pipeline->shadowedLightCount(), size_t(4));
		for(size_t i = 0; i < lightPos.size() && hazePos.size() < 128; i++) {
			if(i >= inView.size() || inView[i]) {
				hazePos.push_back(lightPos[i]);
				hazeColor.push_back(lightColor[i]);
				hazeShadow.push_back((i < shadowed) ? GLint(i) : -1); // its cube map, if it has one
			}
		}
		GLsizei lights = GLsizei(hazePos.size());
		glUniform1i(m_uVolumeLightCount, lights);
		if(lights > 0) {
			glUniform4fv(m_uVolumeLightPos, lights, glm::value_ptr(hazePos[0]));
			glUniform4fv(m_uVolumeLightColor, lights, glm::value_ptr(hazeColor[0]));
			glUniform1iv(m_uVolumeLightShadow, lights, hazeShadow.data());
		}
		for(size_t i = 0; i < 4; i++) {
			glActiveTexture(GL_TEXTURE4 + GLenum(i));
			glBindTexture(GL_TEXTURE_CUBE_MAP, (i < shadowed) ? m_pipeline->shadowMapTexture(i) : 0);
		}
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_depthTexture);
		drawFullscreen();
		glUseProgram(m_blurProgram);
		glBindFramebuffer(GL_FRAMEBUFFER, m_volumeFramebuffer[1]);
		glUniform2f(m_uBlurDirection, 1.f / float(m_bloomWidth), 0.f);
		glBindTexture(GL_TEXTURE_2D, m_volumeTexture[0]);
		drawFullscreen();
		glBindFramebuffer(GL_FRAMEBUFFER, m_volumeFramebuffer[0]);
		glUniform2f(m_uBlurDirection, 0.f, 1.f / float(m_bloomHeight));
		glBindTexture(GL_TEXTURE_2D, m_volumeTexture[1]);
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
	glUniform1f(m_uFinalDarkness, m_settings.darkness);
	glUniform1i(m_uFinalVolumetric, haze ? 1 : 0);
	glUniform1i(m_uFinalDebug, m_settings.debugView);
	glUniform2f(m_uFinalInvSize, 1.f / float(m_width), 1.f / float(m_height));
	glUniform1i(m_uFinalTraceMode, traced ? m_settings.traceMode : 0);
	if(traced) {
		const glm::mat4 & proj = m_pipeline->projection();
		glUniform1f(m_uFinalAo, m_settings.ao); // the traced occlusion, at the option's strength
		glUniform1f(m_uFinalBounce, m_settings.bounce);
		glUniform1f(m_uFinalStaticMix, m_settings.staticMix);
		glUniform2i(m_uFinalTraceSize, m_bloomWidth, m_bloomHeight);
		glUniform4f(m_uFinalProjection, proj[0][0], proj[1][1], proj[2][2], -proj[3][2]);
		const glm::vec2 & fog = m_pipeline->fogRange();
		glUniform2f(m_uFinalFogRange, fog.x, m_pipeline->fogEnabled() ? fog.y : 0.f);
		glActiveTexture(GL_TEXTURE8);
		glBindTexture(GL_TEXTURE_2D, m_depthTexture);
		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_2D, m_gbufferTexture[2]);
		glActiveTexture(GL_TEXTURE6);
		glBindTexture(GL_TEXTURE_2D, m_gbufferTexture[1]);
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, m_traceBlurTexture[1][1]);
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, m_traceBlurTexture[1][0]);
	}
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, haze ? m_volumeTexture[0] : 0);
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
	if(traced) {
		for(GLenum unit : { GL_TEXTURE4, GL_TEXTURE5, GL_TEXTURE6, GL_TEXTURE7, GL_TEXTURE8, GL_TEXTURE12, GL_TEXTURE13 }) {
			glActiveTexture(unit);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}
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
