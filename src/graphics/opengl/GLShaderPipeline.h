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

#ifndef ARX_GRAPHICS_OPENGL_GLSHADERPIPELINE_H
#define ARX_GRAPHICS_OPENGL_GLSHADERPIPELINE_H

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "graphics/Color.h"
#include "graphics/Renderer.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "graphics/texture/TextureStage.h"

class OpenGLRenderer;
class GLRayScene;
class GLShadowMaps;
class GLTexture;

/*!
 * ArxModern: programmable replacement for the fixed-function OpenGL pipeline.
 *
 * Reproduces what the legacy path configures through glTexEnv / glFog / matrix stack
 * with a single GLSL program ("legacy" shader), so that every draw call of the engine
 * keeps producing the same pixels while the rest of the renderer moves to shaders.
 *
 * The GLSL sources are loaded from the game resources (\c graph/shaders/legacy.vert
 * and \c graph/shaders/legacy.frag) so they can be overridden by mods; the built-in copies
 * are used when the files are missing.
 */
class GLShaderPipeline {

public:

	//! Generic vertex attribute locations shared by all vertex formats
	enum Attribute {
		AttribPosition = 0,
		AttribColor = 1,
		AttribTexCoord0 = 2,
		AttribTexCoord1 = 3,
		AttribTexCoord2 = 4,
		AttribNormal = 5,
		AttribWorldPos = 6,
		AttribCaster = 7,
		AttribCount = 8
	};

	//! Number of texture stages the shader can combine
	static constexpr size_t MaxStages = 3;

	explicit GLShaderPipeline(OpenGLRenderer * renderer);
	~GLShaderPipeline();

	//! Compile and bind the program. Returns false if the shader path cannot be used.
	bool init();
	void shutdown();

	[[nodiscard]] bool isActive() const { return m_program != 0; }

	//! Re-read the shader sources; returns false and keeps the old program on error.
	bool reload();

	// State mirrored from the renderer
	void setPretransformed(bool pretransformed) { m_pretransformed = pretransformed; }
	void setMatrices(const glm::mat4 & view, const glm::mat4 & projection);
	void setViewportSize(float width, float height);
	void setFogColor(Color color);
	void setFogParams(float fogStart, float fogEnd);
	void setLights(const RendererLight * lights, size_t dynamicCount, size_t count);
	void setPixelLighting(bool enable) { m_pixelLighting = enable; }
	void setNormalMap(GLTexture * normalMap, const MaterialParams & material) {
		m_normalMap = normalMap;
		m_material = material;
	}
	void setNormalMapStrength(float strength) { m_normalStrength = strength; m_normalStrengthDirty = true; }
	//! Depth of the parallax relief (0 = off) and strength of the specular highlights (0 = off)
	void setMaterialStrength(float parallax, float specular) {
		m_parallaxStrength = parallax;
		m_specularStrength = specular;
		m_materialStrengthDirty = true;
	}

	/*!
	 * Soft particles: from now on the blended, depth-tested draws without depth write fade
	 * against the scene depth in the given texture (0 = off).
	 */
	void setSoftDepth(GLuint depthTexture, int width, int height);
	
	//! Compile a program from graph/shaders/<name>.vert/.frag (or the given fallbacks); 0 on failure
	GLuint buildProgram(std::string_view name, std::string_view vertFallback, std::string_view fragFallback) {
		return build(name, vertFallback, fragFallback);
	}
	/*!
	 * Same, with a prelude prepended to both stages (the stage files then carry no #version):
	 * \c prelude + \c common (a shared source, itself overridable as graph/shaders/<commonName>) + the stage.
	 */
	GLuint buildProgram(std::string_view name, std::string_view vertFallback, std::string_view fragFallback,
	                    std::string_view prelude, std::string_view commonName, std::string_view commonFallback);
	
	//! Re-bind the main program and forget cached GL state after raw GL use (post-processing)
	void restoreAfterExternalDraw();

	[[nodiscard]] const glm::mat4 & projection() const { return m_projection; }
	[[nodiscard]] const glm::mat4 & view() const { return m_view; }
	[[nodiscard]] const glm::vec2 & fogRange() const { return m_fogRange; }
	[[nodiscard]] bool fogEnabled() const;
	[[nodiscard]] const std::vector<glm::vec4> & lightPositions() const { return m_lightPos; }
	[[nodiscard]] const std::vector<glm::vec4> & lightColors() const { return m_lightColor; }
	//! The cube maps rendered this frame: the first shadowedLightCount() lights have one (units 4..7)
	[[nodiscard]] size_t shadowedLightCount() const noexcept { return m_shadowedLights; }
	[[nodiscard]] GLuint shadowMapTexture(size_t light) const;
	//! Per light: whether its room is in view (RendererLight::inView)
	[[nodiscard]] const std::vector<bool> & lightsInView() const { return m_lightInView; }
	[[nodiscard]] size_t dynamicLightCount() const { return m_dynamicLightCount; }
	[[nodiscard]] const glm::vec3 & fogColor() const { return m_fogColor; }

	/*!
	 * While another program of ours is current (the water pass), the draws must not push the
	 * main program's uniforms: apply() is a no-op until the pass ends.
	 */
	void setExternalPass(bool external) { m_externalPass = external; }
	
	/*!
	 * ArxModern RT (OpenGL 4.3): 0 = off, 1 = the level hierarchy is available to the passes
	 * (traced reflections), 2 = the main shader also traces the shadows of the dynamic lights
	 * through the level (the cube maps then only hold the entities), 3 = the main shader also
	 * writes a G-buffer for the traced lighting pass (occlusion and indirect light,
	 * GLPostProcess), 4 = that pass also traces the shadows of the static lights. Rebuilds the
	 * programs when the mode crosses on/off or the G-buffer threshold; returns false if
	 * unsupported (the mode stays off).
	 */
	bool setRayTracing(int mode);
	//! post_debug=rtshadow: the traced shadow factors instead of the scene
	void setRayTracingDebug(int mode);
	[[nodiscard]] int rayTracing() const noexcept { return m_rayTracing; }
	[[nodiscard]] bool tracedShadows() const noexcept { return m_rayTracing >= 2; }
	//! Whether the main program writes the G-buffer of the traced lighting (modes 3 and 4)
	[[nodiscard]] bool tracedLighting() const noexcept { return m_rayTracing >= 3; }
	//! Whether a pass of ours other than the main program is drawing (water, reflections, lava)
	[[nodiscard]] bool inExternalPass() const noexcept { return m_externalPass; }
	//! Whether the shadow cube maps are being rendered
	[[nodiscard]] bool inShadowPass() const noexcept { return m_shadowPass; }
	//! The level hierarchy, null when ray tracing is off or there is no level
	[[nodiscard]] GLRayScene * rayScene() const noexcept { return m_rayScene.get(); }
	//! Once per frame before the scene: (re)build and bind the hierarchy
	void prepareRayScene();

	//! Create the shadow maps (count = 0 disables them)
	bool initShadows(size_t count, int resolution);
	//! Render the cube maps of the first shadowed dynamic lights
	void renderShadowMaps(ShadowCasterDrawFunc drawCasters);

	//! Upload whatever changed since the last draw. Called right before each draw call.
	void apply();

private:

	struct StageState {
		int colorOp = 0;
		int alphaOp = 0;
	};

	OpenGLRenderer * m_renderer;
	GLuint m_program;

	int m_rayTracing;
	int m_rayDebug;
	std::unique_ptr<GLRayScene> m_rayScene;
	bool m_raySceneBound;

	// uniform locations
	GLint m_uMVP;
	GLint m_uView;
	GLint m_uTransform;
	GLint m_uStages[MaxStages];
	GLint m_uFogEnabled;
	GLint m_uFogColor;
	GLint m_uFogRange;
	GLint m_uPixelLighting;
	GLint m_uLightCount;
	GLint m_uDynamicLightCount;
	GLint m_uLightPos;
	GLint m_uLightColor;
	GLint m_uShadowCount;
	GLint m_uNormalMapped;
	GLint m_uNormalStrength;
	GLint m_uMaterial;
	GLint m_uSpecular;
	GLint m_uCameraPos;
	GLint m_uSoftMode;
	GLint m_uProjection;
	GLint m_uInvSize;
	GLTexture * m_normalMap;
	GLTexture * m_glNormalMap;
	int m_glNormalMapped;
	float m_normalStrength;
	bool m_normalStrengthDirty;
	MaterialParams m_material;
	MaterialParams m_glMaterial;
	bool m_glMaterialSet;
	float m_parallaxStrength;
	float m_specularStrength;
	bool m_materialStrengthDirty;
	GLuint m_softDepth;
	int m_glSoftMode;
	bool m_softDirty;
	int m_softWidth;
	int m_softHeight;
	
	// shadow pass program
	GLuint m_shadowProgram;
	GLint m_uShadowViewProj;
	GLint m_uShadowTransform;
	GLint m_uShadowLightPos;
	GLint m_uShadowFallend;
	GLint m_uShadowTextured;
	GLint m_uShadowOwner;
	std::unique_ptr<GLShadowMaps> m_shadows;
	bool m_shadowPass;
	bool m_externalPass;
	int m_glShadowTransform;
	int m_glShadowTextured;
	size_t m_shadowedLights; // cube maps rendered for this frame
	int m_glShadowCount;

	// cached values (what the GPU currently has)
	bool m_pretransformed;
	int m_glTransform;
	bool m_matricesDirty;
	bool m_orthoDirty;
	glm::mat4 m_view;
	glm::mat4 m_projection;
	glm::mat4 m_ortho;
	float m_viewportWidth;
	float m_viewportHeight;
	StageState m_glStages[MaxStages];
	int m_glFogEnabled;
	bool m_fogDirty;
	glm::vec3 m_fogColor;
	glm::vec2 m_fogRange;
	bool m_pixelLighting;
	int m_glPixelLighting;
	std::vector<glm::vec4> m_lightPos;
	std::vector<glm::vec4> m_lightColor;
	std::vector<bool> m_lightInView;
	std::vector<RendererLight> m_lights;
	size_t m_dynamicLightCount;
	bool m_lightsDirty;

	GLuint build(std::string_view name, std::string_view vertFallback, std::string_view fragFallback,
	             std::string_view prefixVert = std::string_view(), std::string_view prefixFrag = std::string_view());
	void applyShadowPass();
	GLuint compile(GLenum type, std::string_view name, std::string_view source);
	std::string loadSource(std::string_view name, std::string_view fallback);
	void resetCache();

};

#endif // ARX_GRAPHICS_OPENGL_GLSHADERPIPELINE_H
