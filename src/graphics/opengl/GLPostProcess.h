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
		float ao = 0.f;        //!< ambient occlusion strength, 0 = off
		float aoRadius = 60.f; //!< world units
		int debugView = 0;     //!< 1 = ambient occlusion buffer, 2 = bloom buffer
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
	GLint m_uSsaoProjection;
	GLint m_uSsaoInvSize;
	GLint m_uSsaoRadius;
	GLint m_uSsaoBias;
	GLint m_uFinalAo;
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
	void destroyBuffers();
	void drawFullscreen();

};

#endif // ARX_GRAPHICS_OPENGL_GLPOSTPROCESS_H
