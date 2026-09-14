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

#include "graphics/texture/NormalMap.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "graphics/image/Image.h"

namespace {

//! Height field with wrapping access
struct HeightField {

	size_t width;
	size_t height;
	std::vector<float> data;

	HeightField(size_t w, size_t h) : width(w), height(h), data(w * h, 0.f) { }

	float at(ptrdiff_t x, ptrdiff_t y) const {
		x = ((x % ptrdiff_t(width)) + ptrdiff_t(width)) % ptrdiff_t(width);
		y = ((y % ptrdiff_t(height)) + ptrdiff_t(height)) % ptrdiff_t(height);
		return data[size_t(y) * width + size_t(x)];
	}

	float & ref(size_t x, size_t y) {
		return data[y * width + x];
	}

};

//! Separable box blur of the given radius (wrapping), returns a new field
HeightField blur(const HeightField & in, int radius) {

	if(radius <= 0) {
		return in;
	}

	HeightField tmp(in.width, in.height);
	float norm = 1.f / float(2 * radius + 1);
	for(size_t y = 0; y < in.height; y++) {
		for(size_t x = 0; x < in.width; x++) {
			float sum = 0.f;
			for(int k = -radius; k <= radius; k++) {
				sum += in.at(ptrdiff_t(x) + k, ptrdiff_t(y));
			}
			tmp.ref(x, y) = sum * norm;
		}
	}

	HeightField out(in.width, in.height);
	for(size_t y = 0; y < in.height; y++) {
		for(size_t x = 0; x < in.width; x++) {
			float sum = 0.f;
			for(int k = -radius; k <= radius; k++) {
				sum += tmp.at(ptrdiff_t(x), ptrdiff_t(y) + k);
			}
			out.ref(x, y) = sum * norm;
		}
	}

	return out;
}

//! Sobel gradient of a height field at (x, y), wrapping
void gradient(const HeightField & h, size_t x, size_t y, float & gx, float & gy) {
	ptrdiff_t px = ptrdiff_t(x), py = ptrdiff_t(y);
	gx = (h.at(px + 1, py - 1) + 2.f * h.at(px + 1, py) + h.at(px + 1, py + 1))
	   - (h.at(px - 1, py - 1) + 2.f * h.at(px - 1, py) + h.at(px - 1, py + 1));
	gy = (h.at(px - 1, py + 1) + 2.f * h.at(px, py + 1) + h.at(px + 1, py + 1))
	   - (h.at(px - 1, py - 1) + 2.f * h.at(px, py - 1) + h.at(px + 1, py - 1));
	gx *= 0.125f;
	gy *= 0.125f;
}

} // anonymous namespace

bool generateNormalMap(const Image & diffuse, Image & out, float strength) {

	if(!diffuse.isValid() || diffuse.getWidth() < 2 || diffuse.getHeight() < 2) {
		return false;
	}

	size_t width = diffuse.getWidth();
	size_t height = diffuse.getHeight();
	size_t channels = diffuse.getNumChannels();
	const unsigned char * src = diffuse.getData();
	Image::Format format = diffuse.getFormat();

	// Height from luminance, in [0, 1]
	HeightField base(width, height);
	for(size_t y = 0; y < height; y++) {
		for(size_t x = 0; x < width; x++) {
			const unsigned char * p = src + (y * width + x) * channels;
			float luma;
			switch(format) {
				case Image::Format_L8:
				case Image::Format_A8:
				case Image::Format_L8A8: {
					luma = float(p[0]);
					break;
				}
				case Image::Format_B8G8R8:
				case Image::Format_B8G8R8A8: {
					luma = 0.114f * float(p[0]) + 0.587f * float(p[1]) + 0.299f * float(p[2]);
					break;
				}
				default: {
					luma = 0.299f * float(p[0]) + 0.587f * float(p[1]) + 0.114f * float(p[2]);
					break;
				}
			}
			base.ref(x, y) = luma / 255.f;
		}
	}

	// Two scales: the grain (lightly smoothed) and the shapes (strongly smoothed)
	int fineRadius = std::max(1, int(std::min(width, height)) / 128);
	int coarseRadius = std::max(3, int(std::min(width, height)) / 32);
	HeightField fine = blur(base, fineRadius);
	HeightField coarse = blur(base, coarseRadius);

	// Slopes are measured per texel: scale so that a full-range luminance step over a
	// few texels gives a clearly tilted normal, more for the coarse shapes
	float fineScale = 6.f * strength;
	float coarseScale = 14.f * strength;

	out.create(width, height, Image::Format_R8G8B8);
	unsigned char * dst = out.getData();
	for(size_t y = 0; y < height; y++) {
		for(size_t x = 0; x < width; x++) {
			float fx, fy, cx, cy;
			gradient(fine, x, y, fx, fy);
			gradient(coarse, x, y, cx, cy);
			float gx = fx * fineScale + cx * coarseScale;
			float gy = fy * fineScale + cy * coarseScale;
			// Tangent-space normal: bright = raised, so the normal tilts away from the slope
			float nx = -gx, ny = -gy, nz = 1.f;
			float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			nx /= len, ny /= len, nz /= len;
			unsigned char * p = dst + (y * width + x) * 3;
			p[0] = (unsigned char)std::clamp(int(std::lround((nx * 0.5f + 0.5f) * 255.f)), 0, 255);
			p[1] = (unsigned char)std::clamp(int(std::lround((ny * 0.5f + 0.5f) * 255.f)), 0, 255);
			p[2] = (unsigned char)std::clamp(int(std::lround((nz * 0.5f + 0.5f) * 255.f)), 0, 255);
		}
	}

	return true;
}
