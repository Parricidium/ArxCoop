/*
 * Copyright 2026 ArxCoop contributors
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

// Single translation unit holding the (public domain) stb_image_write implementation.

#include "graphics/image/ImageEncode.h"

#ifdef _MSC_VER
#pragma warning(disable: 4505) // unreferenced static functions (we only use the JPEG writer)
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "graphics/image/stb_image_write.h"

#include "graphics/image/Image.h"

namespace {

void appendChunk(void * context, void * data, int size) {
	auto * out = static_cast<std::vector<unsigned char> *>(context);
	const unsigned char * bytes = static_cast<const unsigned char *>(data);
	out->insert(out->end(), bytes, bytes + size);
}

int channelCount(const Image & image) {
	switch(image.getFormat()) {
		case Image::Format_L8:       return 1;
		case Image::Format_R8G8B8:   return 3;
		case Image::Format_R8G8B8A8: return 4;
		default: return 0;
	}
}

} // anonymous namespace

bool encodeImageJpeg(const Image & image, int quality, std::vector<unsigned char> & out) {
	int channels = channelCount(image);
	if(!channels) {
		return false;
	}
	out.clear();
	return stbi_write_jpg_to_func(appendChunk, &out, int(image.getWidth()), int(image.getHeight()),
	                              channels, image.getData(), quality) != 0;
}

bool encodeImagePng(const Image & image, std::vector<unsigned char> & out) {
	int channels = channelCount(image);
	if(!channels) {
		return false;
	}
	out.clear();
	return stbi_write_png_to_func(appendChunk, &out, int(image.getWidth()), int(image.getHeight()),
	                              channels, image.getData(), int(image.getWidth()) * channels) != 0;
}
