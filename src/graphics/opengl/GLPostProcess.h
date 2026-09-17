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

#ifndef ARX_GRAPHICS_OPENGL_GLPOSTPROCESS_H
#define ARX_GRAPHICS_OPENGL_GLPOSTPROCESS_H

#include <glm/glm.hpp>

#include "graphics/opengl/OpenGLUtil.h"

class GLShaderPipeline;

/*!
 * ArxModern: off-screen scene rendering and full-screen post-processing.
 *
 * Between \ref begin() and \ref end() the scene is rendered into a (multisampled) framebuffer
 * of the window size. \ref end() resolves it, extracts and blurs the bright parts for the bloom
 * and writes the final image (scene + bloom, anti-aliased with FXAA) to the window, so that
 * the HUD can be drawn on top as usual.
 */
class GLPostProcess {

public:

	struct Settings {
		float bloom = 0.f;     //!< bloom intensity, 0 = off
		float bloomThreshold = 0.6f;
		bool fxaa = false;
		bool smaa = false;     //!< SMAA 1x (takes precedence over fxaa)
		float ao = 0.f;        //!< ambient occlusion strength, 0 = off
		float aoRadius = 60.f; //!< world units
		float darkness = 0.f;  //!< 0..1, crushes the dark end of the image (the unlit places)
		float volumetric = 0.f; //!< 0..1, density of the volumetric haze (0 = off)
		float volumetricLight = 1.f; //!< strength of the light scattered by the haze
		//! ArxModern RT: 0 = off, 3 = traced occlusion and indirect light, 4 = also the static shadows (post_trace.frag)
		int traceMode = 0;
		float bounce = 1.f;      //!< strength of the traced indirect light
		float staticMix = 0.25f; //!< 0..1, how much of the original static lighting is kept in the traced static shadows
bool volumetricShadows = true; //!< trace the shadows of the light shafts (ray tracing only)
		int debugView = 0;     //!< 1 = ambient occlusion buffer, 2 = bloom buffer, 3 = the haze
	};

	explicit GLPostProcess(GLShaderPipeline * pipeline);
	~GLPostProcess();

	//! Compile the programs; returns false if post-processing cannot be used
	bool init();
	void shutdown();

	[[nodiscard]] bool isActive() const { return m_finalProgram != 0; }

	Settings & settings() { return m_settings; }

	//! Redirect rendering to the off-screen scene buffer (recreated on size change)
	void begin(int width, int height, int samples);
	//! Compose the final image into the window. No-op if begin() was not called.
	void end();

	[[nodiscard]] bool isInScene() const { return m_inScene; }
	//! Whether the haze program was built with the ray tracing code (init() again when this must change)
	[[nodiscard]] bool traced() const noexcept { return m_traced; }
	//! Whether the scene buffer carries the G-buffer of the traced lighting (init() again when this must change)
	[[nodiscard]] bool gbuffer() const noexcept { return m_gbuffer; }
	/*!
	 * The G-buffer outputs of the main program only mean something for the lit, opaque draws:
	 * the renderer switches them on around those and off around everything else (blended
	 * particles and decals, the water and reflection passes). No-op without a G-buffer.
	 */
	void setGBufferWrites(bool enable);

	/*!
	 * Copy the scene as rendered so far into the textures below (for passes that read the
	 * scene while drawing into it: the water). Rendering continues into the scene buffer.
	 */
	bool captureScene(bool depthOnly = false);
	[[nodiscard]] GLuint sceneTexture() const { return m_sceneTexture; }
	[[nodiscard]] GLuint depthTexture() const { return m_depthTexture; }
	[[nodiscard]] int width() const { return m_width; }
	[[nodiscard]] int height() const { return m_height; }

private:

	GLShaderPipeline * m_pipeline;
	Settings m_settings;

	GLuint m_extractProgram;
	GLuint m_blurProgram;
	GLuint m_ssaoProgram;
	GLuint m_finalProgram;
	GLuint m_smaaEdgeProgram;
	GLuint m_smaaWeightProgram;
	GLuint m_smaaBlendProgram;
	GLint m_uSmaaEdgeMetrics;
	GLint m_uSmaaWeightMetrics;
	GLint m_uSmaaBlendMetrics;
	GLuint m_areaTexture;   //!< SMAA lookup textures (constant)
	GLuint m_searchTexture;
	GLuint m_compositeFramebuffer; //!< the composed image when SMAA runs after it
	GLuint m_compositeTexture;
	GLuint m_edgesFramebuffer;
	GLuint m_edgesTexture;
	GLuint m_blendFramebuffer;
	GLuint m_blendTexture;
	GLint m_uSsaoProjection;
	GLint m_uSsaoInvSize;
	GLint m_uSsaoRadius;
	GLint m_uSsaoBias;
	GLint m_uFinalAo;
	GLint m_uFinalDarkness;
	GLint m_uFinalVolumetric;
	GLuint m_volumeProgram;
	GLuint m_volumeTexture[2];
	GLuint m_volumeFramebuffer[2];
	GLint m_uVolumeProjection;
	GLint m_uVolumeInvView;
	GLint m_uVolumeCameraPos;
	GLint m_uVolumeDensity;
	GLint m_uVolumeLightScale;
	GLint m_uVolumeTime;
	GLint m_uVolumeLightCount;
	GLint m_uVolumeLightPos;
	GLint m_uVolumeLightColor;
	GLint m_uVolumeShadows;
	GLint m_uVolumeLightShadow;
	bool m_traced;
	// Traced lighting (post_trace.frag): the G-buffer written with the scene, the accumulated
	// outputs (two textures, ping-pong for the history) and their blurred copies
	bool m_gbuffer;
	bool m_gbufferWrites;
	GLuint m_gbufferBuffer[3];  //!< normal, albedo, static light (multisampled like the scene)
	GLuint m_gbufferTexture[3]; //!< their resolved copies
	GLuint m_traceProgram;
	GLuint m_traceBlurProgram;
	GLint m_uTraceProjection;
	GLint m_uTraceInvView;
	GLint m_uTracePrevViewProj;
	GLint m_uTraceCameraPos;
	GLint m_uTraceFullSize;
	GLint m_uTraceFrame;
	GLint m_uTraceReset;
	GLint m_uTraceStatic;
	GLint m_uTraceAoRadius;
	GLint m_uTraceLightCount;
	GLint m_uTraceDynamicLightCount;
	GLint m_uTraceLightPos;
	GLint m_uTraceLightColor;
	GLint m_uTraceBlurDirection;
	GLint m_uFinalTraceMode;
	GLint m_uFinalBounce;
	GLint m_uFinalStaticMix;
	GLint m_uFinalTraceSize;
	GLint m_uFinalProjection;
	GLint m_uFinalFogRange;
	GLuint m_traceFramebuffer[2];
	GLuint m_traceTexture[2][2];     //!< [history][A, B]
	GLuint m_traceBlurFramebuffer[2];
	GLuint m_traceBlurTexture[2][2]; //!< [pass][A, B]
	int m_traceIndex;
	int m_traceFrame;
	bool m_traceReset;
	glm::mat4 m_prevViewProj;
	GLint m_uFinalDebug;
GLint m_uExtractThreshold;
	GLint m_uBlurDirection;
	GLint m_uFinalBloom;
	GLint m_uFinalFxaa;
	GLint m_uFinalInvSize;

	GLuint m_vao;

	int m_width;
	int m_height;
	int m_samples;
	GLuint m_sceneFramebuffer;   // multisampled (or not) render target
	GLuint m_sceneColorBuffer;
	GLuint m_sceneDepthBuffer;
	GLuint m_resolveFramebuffer; // single-sampled copy, sampled by the passes
	GLuint m_sceneTexture;
	GLuint m_depthTexture;
	GLuint m_aoFramebuffer[2];
	GLuint m_aoTexture[2];
	GLuint m_bloomFramebuffer[2];
	GLuint m_bloomTexture[2];
	int m_bloomWidth;
	int m_bloomHeight;

	bool m_inScene;

	bool createBuffers(int width, int height, int samples);
	bool initSmaa();
	void destroyBuffers();
	void drawFullscreen();

};

#endif // ARX_GRAPHICS_OPENGL_GLPOSTPROCESS_H
