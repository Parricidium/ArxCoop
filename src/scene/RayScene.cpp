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

#include "scene/RayScene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

#include "graphics/GraphicsTypes.h"
#include "math/Vector.h"
#include "core/TimeTypes.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/texture/Texture.h"
#include "io/log/Logger.h"
#include "platform/Time.h"
#include "scene/Tiles.h"

namespace {

// Build tunables
const size_t LeafTriangles = 4;  // a leaf holds at most this many triangles
const int Bins = 12;             // SAH split candidates per axis
const int MaxDepth = 48;

struct BuildTriangle {
	Vec3f v[3];
	Vec3f centroid;
	Vec3f min, max;
	Vec2f uv[3];
	u32 color[3];
	u32 layerFlags;
};

struct Bounds {
	Vec3f min = Vec3f(std::numeric_limits<float>::max());
	Vec3f max = Vec3f(-std::numeric_limits<float>::max());
	void add(const Vec3f & p) { min = glm::min(min, p); max = glm::max(max, p); }
	void add(const Bounds & b) { min = glm::min(min, b.min); max = glm::max(max, b.max); }
	[[nodiscard]] float area() const {
		if(max.x < min.x) {
			return 0.f;
		}
		Vec3f d = max - min;
		return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
	}
};

float asFloat(u32 bits) {
	float f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

struct Builder {

	std::vector<BuildTriangle> & tris;
	std::vector<u32> order; // triangle indices, permuted by the splits
	std::vector<Vec4f> nodes;
	size_t leaves = 0;
	int depth = 0;

	explicit Builder(std::vector<BuildTriangle> & triangles) : tris(triangles) {
		order.resize(tris.size());
		for(size_t i = 0; i < order.size(); i++) {
			order[i] = u32(i);
		}
	}

	Bounds boundsOf(size_t first, size_t count) const {
		Bounds b;
		for(size_t i = first; i < first + count; i++) {
			b.min = glm::min(b.min, tris[order[i]].min);
			b.max = glm::max(b.max, tris[order[i]].max);
		}
		return b;
	}

	// Fill the node at index (already allocated) with the triangles [first, first + count)
	void build(size_t index, size_t first, size_t count, int level) {
		
		depth = std::max(depth, level);
		
		Bounds bounds = boundsOf(first, count);
		
		if(count <= LeafTriangles || level >= MaxDepth) {
			leaf(index, bounds, first, count);
			return;
		}
		
		// Binned SAH: the centroid bounds are what the bins span
		Bounds centroids;
		for(size_t i = first; i < first + count; i++) {
			centroids.add(tris[order[i]].centroid);
		}
		Vec3f extent = centroids.max - centroids.min;
		
		float bestCost = std::numeric_limits<float>::max();
		int bestAxis = -1;
		int bestBin = -1;
		for(int axis = 0; axis < 3; axis++) {
			if(extent[axis] <= 1e-3f) {
				continue;
			}
			std::array<Bounds, Bins> binBounds;
			std::array<size_t, Bins> binCount = { };
			float scale = float(Bins) / extent[axis];
			for(size_t i = first; i < first + count; i++) {
				const BuildTriangle & t = tris[order[i]];
				int bin = std::min(Bins - 1, int((t.centroid[axis] - centroids.min[axis]) * scale));
				binCount[size_t(bin)]++;
				binBounds[size_t(bin)].min = glm::min(binBounds[size_t(bin)].min, t.min);
				binBounds[size_t(bin)].max = glm::max(binBounds[size_t(bin)].max, t.max);
			}
			// Sweep from the right to get the cost of every split in one pass each way
			std::array<float, Bins> rightArea;
			std::array<size_t, Bins> rightCount;
			Bounds right;
			size_t rightN = 0;
			for(int b = Bins - 1; b > 0; b--) {
				if(binCount[size_t(b)]) {
					right.add(binBounds[size_t(b)]);
				}
				rightN += binCount[size_t(b)];
				rightArea[size_t(b)] = right.area();
				rightCount[size_t(b)] = rightN;
			}
			Bounds left;
			size_t leftN = 0;
			for(int b = 0; b < Bins - 1; b++) {
				if(binCount[size_t(b)]) {
					left.add(binBounds[size_t(b)]);
				}
				leftN += binCount[size_t(b)];
				if(leftN == 0 || rightCount[size_t(b + 1)] == 0) {
					continue;
				}
				float cost = left.area() * float(leftN) + rightArea[size_t(b + 1)] * float(rightCount[size_t(b + 1)]);
				if(cost < bestCost) {
					bestCost = cost;
					bestAxis = axis;
					bestBin = b;
				}
			}
		}
		
		size_t mid;
		if(bestAxis < 0 || bestCost >= bounds.area() * float(count)) {
			// No split is worth it (or all centroids coincide): split in the middle along the
			// longest axis unless the leaf is small enough
			if(count <= LeafTriangles * 2) {
				leaf(index, bounds, first, count);
				return;
			}
			Vec3f d = bounds.max - bounds.min;
			int axis = (d.x > d.y) ? ((d.x > d.z) ? 0 : 2) : ((d.y > d.z) ? 1 : 2);
			std::nth_element(order.begin() + long(first), order.begin() + long(first + count / 2),
			                 order.begin() + long(first + count),
			                 [&](u32 a, u32 b) { return tris[a].centroid[axis] < tris[b].centroid[axis]; });
			mid = first + count / 2;
		} else {
			float split = centroids.min[bestAxis] + extent[bestAxis] * float(bestBin + 1) / float(Bins);
			auto it = std::partition(order.begin() + long(first), order.begin() + long(first + count),
			                         [&](u32 a) { return tris[a].centroid[bestAxis] < split; });
			mid = size_t(it - order.begin());
			if(mid == first || mid == first + count) {
				mid = first + count / 2; // rounding put everything on one side
			}
		}
		
		// Both children are allocated together so that they are adjacent
		size_t left = nodes.size() / 2;
		nodes.resize(nodes.size() + 4);
		nodes[index * 2] = Vec4f(bounds.min, asFloat(u32(left)));
		nodes[index * 2 + 1] = Vec4f(bounds.max, asFloat(0));
		build(left, first, mid - first, level + 1);
		build(left + 1, mid, first + count - mid, level + 1);
	}
	
	void leaf(size_t index, const Bounds & bounds, size_t first, size_t count) {
		nodes[index * 2] = Vec4f(bounds.min, asFloat(u32(first)));
		nodes[index * 2 + 1] = Vec4f(bounds.max, asFloat(u32(count)));
		leaves++;
	}

};

} // anonymous namespace

static size_t g_raySceneGeneration = 1;

size_t raySceneGeneration() noexcept {
	return g_raySceneGeneration;
}

void raySceneChanged() noexcept {
	g_raySceneGeneration++;
}

std::unique_ptr<RaySceneData> buildRayScene() {

	if(!g_tiles) {
		return nullptr;
	}

	PlatformInstant start = platform::getTime();

	auto data = std::make_unique<RaySceneData>();
	std::vector<BuildTriangle> tris;
	std::unordered_map<TextureContainer *, u32> layers;

	for(auto tile : g_tiles->tiles()) {
		for(const EERIEPOLY & poly : tile.polygons()) {
			if(!poly.tex || (poly.type & (POLY_NODRAW | POLY_HIDE | POLY_IGNORE | POLY_TRANS))) {
				continue;
			}
			u32 layer;
			auto it = layers.find(poly.tex);
			if(it == layers.end()) {
				layer = u32(data->textures.size());
				layers.emplace(poly.tex, layer);
				data->textures.push_back(poly.tex);
			} else {
				layer = it->second;
			}
			u32 flags = 0;
			if(poly.tex->m_pTexture && poly.tex->m_pTexture->hasAlpha()) {
				flags |= RaySceneData::AlphaCutout;
			}
			if(poly.type & POLY_WATER) {
				flags |= RaySceneData::Water;
			}
			if(poly.type & POLY_LAVA) {
				flags |= RaySceneData::Lava;
			}
			if(poly.type & POLY_GLOW) {
				flags |= RaySceneData::Glow;
			}
			if(poly.type & POLY_DOUBLESIDED) {
				flags |= RaySceneData::DoubleSided;
			}
			// A quad is the strip (0, 1, 2), (3, 2, 1) like the room index buffers
			int corners[2][3] = { { 0, 1, 2 }, { 3, 2, 1 } };
			int count = (poly.type & POLY_QUAD) ? 2 : 1;
			for(int t = 0; t < count; t++) {
				// Wound so that cross(e1, e2) is the polygon's normal, which faces the open space:
				// the shadow rays ignore the back faces (a light inside a sconce or a log pile
				// must still light the room)
				Vec3f normal = (t == 0) ? poly.norm : poly.norm2;
				Vec3f e1 = poly.v[corners[t][1]].p - poly.v[corners[t][0]].p;
				Vec3f e2 = poly.v[corners[t][2]].p - poly.v[corners[t][0]].p;
				if(glm::dot(glm::cross(e1, e2), normal) < 0.f) {
					std::swap(corners[t][1], corners[t][2]);
				}
				BuildTriangle tri;
				Bounds b;
				for(int k = 0; k < 3; k++) {
					const TexturedVertex & v = poly.v[corners[t][k]];
					tri.v[k] = v.p;
					tri.uv[k] = v.uv;
					tri.color[k] = v.color.t;
					b.add(v.p);
				}
				if(arx::length2(glm::cross(tri.v[1] - tri.v[0], tri.v[2] - tri.v[0])) < 1e-4f) {
					continue; // degenerate
				}
				tri.min = b.min;
				tri.max = b.max;
				tri.centroid = (tri.v[0] + tri.v[1] + tri.v[2]) / 3.f;
				tri.layerFlags = layer | (flags << 16);
				tris.push_back(tri);
			}
		}
	}

	if(tris.empty()) {
		return nullptr;
	}

	Builder builder(tris);
	builder.nodes.resize(2);
	builder.build(0, 0, tris.size(), 0);
	data->nodes = std::move(builder.nodes);

	data->triangles.reserve(tris.size() * 3);
	data->attributes.reserve(tris.size() * 3);
	for(u32 index : builder.order) {
		const BuildTriangle & t = tris[index];
		Vec3f e1 = t.v[1] - t.v[0];
		Vec3f e2 = t.v[2] - t.v[0];
		data->triangles.emplace_back(t.v[0], 0.f);
		data->triangles.emplace_back(e1, glm::length(glm::cross(e1, e2))); // w: |normal| (grazing test)
		data->triangles.emplace_back(e2, 0.f);
		data->attributes.emplace_back(t.uv[0].x, t.uv[0].y, t.uv[1].x, t.uv[1].y);
		data->attributes.emplace_back(t.uv[2].x, t.uv[2].y, asFloat(t.layerFlags), asFloat(t.color[0]));
		data->attributes.emplace_back(asFloat(t.color[1]), asFloat(t.color[2]), 0.f, 0.f);
	}

	std::string names;
	for(size_t i = 0; i < data->textures.size(); i++) {
		names += (i ? ", " : "") + std::to_string(i) + "=" + std::string(data->textures[i]->m_texName.basename());
	}
	LogInfo << "Ray tracing: textures: " << names;
	
	LogInfo << "Ray tracing scene: " << data->triangleCount() << " triangles, " << data->nodeCount()
	        << " nodes (" << builder.leaves << " leaves, depth " << builder.depth << "), "
	        << data->textures.size() << " textures, built in "
	        << toMsf(platform::getTime() - start) << " ms";

	return data;
}
