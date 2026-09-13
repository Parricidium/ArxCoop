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

#ifndef ARX_GRAPHICS_IMAGE_IMAGEENCODE_H
#define ARX_GRAPHICS_IMAGE_IMAGEENCODE_H

#include <vector>

class Image;

/*!
 * Encodes an RGB (or L8 / RGBA) image as a baseline JPEG in memory.
 * \param quality 1 (smallest) to 100 (best)
 * \return false if the image format is not supported
 */
bool encodeImageJpeg(const Image & image, int quality, std::vector<unsigned char> & out);

//! Encodes an L8 / RGB / RGBA image as a PNG in memory.
bool encodeImagePng(const Image & image, std::vector<unsigned char> & out);

#endif // ARX_GRAPHICS_IMAGE_IMAGEENCODE_H
