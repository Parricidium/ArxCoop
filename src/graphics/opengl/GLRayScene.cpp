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

#include "graphics/opengl/GLRayScene.h"

#include <algorithm>
#include <vector>

#include "graphics/data/TextureContainer.h"
#include "graphics/opengl/GLTexture.h"
#include "io/log/Logger.h"
#include "scene/RayScene.h"

// Every level texture is resampled to this size for the hit shading: reflections are blurred
// anyway, and it keeps the array small (a level uses a few hundred textures)
static const GLsizei RayTextureSize = 128;

// Buffer binding points, matching rt_common.glsl
static const GLuint NodesBinding = 1;
static const GLuint TrianglesBinding = 2;
static const GLuint AttributesBinding = 3;

GLRayScene::GLRayScene()
	: m_nodes(0)
	, m_tris(0)
	, m_attrs(0)
	, m_textures(0)
	, m_generation(0)
	, m_triangles(0)
{ }

GLRayScene::~GLRayScene() {
	release();
}

bool GLRayScene::supported() {
	#if ARX_HAVE_EPOXY
	return epoxy_is_desktop_gl() && epoxy_gl_version() >= 43;
	#else
	return GLEW_VERSION_4_3 != 0;
	#endif
}

void GLRayScene::release() {
	if(m_nodes) {
		glDeleteBuffers(1, &m_nodes);
		glDeleteBuffers(1, &m_tris);
		glDeleteBuffers(1, &m_attrs);
		m_nodes = m_tris = m_attrs = 0;
	}
	if(m_textures) {
		glDeleteTextures(1, &m_textures);
		m_textures = 0;
	}
	m_generation = 0;
	m_triangles = 0;
}

static void uploadBuffer(GLuint & buffer, const std::vector<Vec4f> & data) {
	if(!buffer) {
		glGenBuffers(1, &buffer);
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
	glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(data.size() * sizeof(Vec4f)), data.data(), GL_STATIC_DRAW);
}

/*!
 * Box-resample an RGBA8 image to RayTextureSize², reading the texture back from the GL: the
 * containers do not keep their pixels once uploaded.
 */
static bool resampleTexture(TextureContainer * container, std::vector<u8> & out) {

	GLTexture * texture = static_cast<GLTexture *>(container->m_pTexture);
	if(!texture || !texture->id()) {
		return false;
	}
	Vec2i size = texture->getStoredSize();
	if(size.x <= 0 || size.y <= 0) {
		return false;
	}

	std::vector<u8> pixels(size_t(size.x) * size_t(size.y) * 4);
	glBindTexture(GL_TEXTURE_2D, texture->id());
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	out.resize(size_t(RayTextureSize) * size_t(RayTextureSize) * 4);
	for(GLsizei y = 0; y < RayTextureSize; y++) {
		int y0 = int(y) * size.y / RayTextureSize;
		int y1 = std::max(y0 + 1, int((y + 1) * size.y / RayTextureSize));
		for(GLsizei x = 0; x < RayTextureSize; x++) {
			int x0 = int(x) * size.x / RayTextureSize;
			int x1 = std::max(x0 + 1, int((x + 1) * size.x / RayTextureSize));
			u32 sum[4] = { 0, 0, 0, 0 };
			u32 n = 0;
			for(int sy = y0; sy < y1; sy++) {
				for(int sx = x0; sx < x1; sx++) {
					const u8 * p = &pixels[(size_t(sy) * size_t(size.x) + size_t(sx)) * 4];
					sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], sum[3] += p[3];
					n++;
				}
			}
			u8 * o = &out[(size_t(y) * size_t(RayTextureSize) + size_t(x)) * 4];
			for(int c = 0; c < 4; c++) {
				o[c] = u8(sum[c] / n);
			}
		}
	}

	return true;
}

bool GLRayScene::update() {

	if(m_generation == raySceneGeneration()) {
		return m_triangles > 0;
	}
	m_generation = raySceneGeneration();
	m_triangles = 0;

	std::unique_ptr<RaySceneData> data = buildRayScene();
	if(!data) {
		return false;
	}

	uploadBuffer(m_nodes, data->nodes);
	uploadBuffer(m_tris, data->triangles);
	uploadBuffer(m_attrs, data->attributes);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	GLint maxLayers = 256;
	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
	GLsizei layers = GLsizei(std::min(data->textures.size(), size_t(maxLayers)));
	if(size_t(layers) < data->textures.size()) {
		LogWarning << "Ray tracing: " << data->textures.size() << " level textures, only " << layers
		           << " fit in an array texture";
	}

	if(!m_textures) {
		glGenTextures(1, &m_textures);
	}
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_textures);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, RayTextureSize, RayTextureSize, std::max(layers, 1),
	             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	std::vector<u8> resampled;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for(GLsizei layer = 0; layer < layers; layer++) {
		if(resampleTexture(data->textures[size_t(layer)], resampled)) {
			glBindTexture(GL_TEXTURE_2D_ARRAY, m_textures);
			glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, RayTextureSize, RayTextureSize, 1,
			                GL_RGBA, GL_UNSIGNED_BYTE, resampled.data());
		}
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_textures);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	m_triangles = data->triangleCount();

	return true;
}

void GLRayScene::bind(GLenum textureUnit) {
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, NodesBinding, m_nodes);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TrianglesBinding, m_tris);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, AttributesBinding, m_attrs);
	glActiveTexture(textureUnit);
	glBindTexture(GL_TEXTURE_2D_ARRAY, m_textures);
	glActiveTexture(GL_TEXTURE0);
}

void GLRayScene::unbind(GLenum textureUnit) {
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, NodesBinding, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TrianglesBinding, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, AttributesBinding, 0);
	glActiveTexture(textureUnit);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glActiveTexture(GL_TEXTURE0);
}
