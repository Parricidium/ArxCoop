/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
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

#include "graphics/opengl/GLVertexBuffer.h"

static GLArrayClientState glArrayClientState = GL_NoArray;
static const void * glArrayClientStateRef = nullptr;
static int glArrayClientStateTexCount = 1;
static GLuint glBoundBuffer = GL_NONE;
static bool glArrayAttribMode = false;

std::vector<GLushort> glShortIndexBuffer;
std::vector<GLuint> glIntIndexBuffer;

void setVertexArrayAttribMode(bool attribs) {
	glArrayAttribMode = attribs;
}

void setVertexArrayLighting(const void * normal, int normalSize, const void * worldPos, size_t stride,
                            const void * caster) {
	
	static bool normalEnabled = false;
	static bool worldPosEnabled = false;
	static bool casterEnabled = false;
	
	if(!glArrayAttribMode) {
		return;
	}
	
	if(normal) {
		if(!normalEnabled) {
			glEnableVertexAttribArray(GLShaderPipeline::AttribNormal);
			normalEnabled = true;
		}
		glVertexAttribPointer(GLShaderPipeline::AttribNormal, normalSize, GL_FLOAT, GL_FALSE, GLsizei(stride), normal);
	} else if(normalEnabled) {
		glDisableVertexAttribArray(GLShaderPipeline::AttribNormal);
		normalEnabled = false;
	}
	
	if(worldPos) {
		if(!worldPosEnabled) {
			glEnableVertexAttribArray(GLShaderPipeline::AttribWorldPos);
			worldPosEnabled = true;
		}
		glVertexAttribPointer(GLShaderPipeline::AttribWorldPos, 3, GL_FLOAT, GL_FALSE, GLsizei(stride), worldPos);
	} else if(worldPosEnabled) {
		glDisableVertexAttribArray(GLShaderPipeline::AttribWorldPos);
		worldPosEnabled = false;
	}
	
	if(caster) {
		if(!casterEnabled) {
			glEnableVertexAttribArray(GLShaderPipeline::AttribCaster);
			casterEnabled = true;
		}
		glVertexAttribPointer(GLShaderPipeline::AttribCaster, 1, GL_FLOAT, GL_FALSE, GLsizei(stride), caster);
	} else if(casterEnabled) {
		glDisableVertexAttribArray(GLShaderPipeline::AttribCaster);
		casterEnabled = false;
	}
	
}

void setVertexArrayTexCoord(int index, const void * coord, size_t stride) {
	
	if(glArrayAttribMode) {
		GLuint attrib = GLShaderPipeline::AttribTexCoord0 + GLuint(index);
		if(index > 0) {
			glEnableVertexAttribArray(attrib);
		}
		glVertexAttribPointer(attrib, 2, GL_FLOAT, GL_FALSE, GLsizei(stride), coord);
		return;
	}
	
	glClientActiveTexture(GL_TEXTURE0 + index);
	if(index > 0) {
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	glTexCoordPointer(2, GL_FLOAT, stride, coord);
	
}

bool switchVertexArray(GLArrayClientState type, const void * ref, int texcount) {

	if(glArrayClientState == type && glArrayClientStateRef == ref) {
		return false;
	}

	if(glArrayClientState != type) {
		arx_assert(texcount >= 1);
		if(type == GL_SMY_VERTEX3 || type == GL_NoArray) {
			setVertexArrayLighting(nullptr, 0, nullptr, 0);
		}
		for(int i = texcount; i < glArrayClientStateTexCount; i++) {
			if(glArrayAttribMode) {
				glDisableVertexAttribArray(GLShaderPipeline::AttribTexCoord0 + GLuint(i));
				continue;
			}
			glClientActiveTexture(GL_TEXTURE0 + i);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
		glArrayClientStateTexCount = texcount;
	}

	glArrayClientState = type;
	glArrayClientStateRef = ref;

	return true;
}

void clearVertexArray(const void * ref) {
	if(glArrayClientStateRef == ref) {
		glArrayClientStateRef = nullptr;
	}
}

void bindBuffer(GLuint buffer) {
	if(buffer != glBoundBuffer) {
		glBoundBuffer = buffer;
		glBindBuffer(GL_ARRAY_BUFFER, glBoundBuffer);
	}
}

void unbindBuffer(GLuint buffer) {
	if(buffer == glBoundBuffer) {
		bindBuffer(GL_NONE);
	}
}
