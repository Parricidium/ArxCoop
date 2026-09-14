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

#include "physics/Cloth.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Renderer.h"
#include "graphics/RenderBatcher.h"
#include "graphics/Vertex.h"
#include "graphics/VertexBuffer.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/texture/Texture.h"
#include "io/log/Logger.h"
#include "math/Vector.h"
#include "platform/Time.h"
#include "scene/Tiles.h"
#include "util/String.h"

#ifdef ARX_HAVE_JOLT

#include "physics/PhysicsInternal.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

namespace physics {

namespace {

constexpr float CellSize = 32.f;          // units, target edge length of the cloth mesh
constexpr float MinHeight = 40.f;         // units, smaller patches are not worth a cloth
constexpr float MaxVertical = 0.35f;      // |normal.y| above this = an awning, a bed, a carpet: left alone (they sag)
constexpr float AttachBand = 0.6f;        // fraction of the height (from the top) where the cloth can be attached to the level
constexpr size_t MaxPolygons = 400;       // per patch
constexpr size_t MaxVertices = 4000;      // per patch (drawn with 16-bit indices)
constexpr float WindRange = 3500.f;       // units from the player within which the cloths move
constexpr float WindStrength = 2.2f;      // m/s^2
constexpr float Flutter = 1.4f;           // m/s^2, the faster flapping across the wind

struct ClothVertex {
	Vec2f uv;
	ColorRGBA color;
};

struct Cloth {
	JPH::BodyID id;
	TextureContainer * texture = nullptr;
	std::vector<ClothVertex> vertices;
	std::vector<Vec3f> positions; //!< simulated, world units
	std::vector<Vec3f> rest;      //!< as modelled (diagnostics)
	std::vector<Vec3f> normals;
	std::vector<unsigned short> indices;
	std::unique_ptr<VertexBuffer<SMY_VERTEX>> buffer;
	Vec3f centre = Vec3f(0.f);
	float radius = 0.f;
	float phase = 0.f;
	bool dirty = true;
};

std::vector<std::unique_ptr<Cloth>> g_cloths;
std::vector<EERIEPOLY *> g_hiddenPolygons;
std::unordered_map<std::string, bool> g_levelCorners; //!< corners of the non-cloth polygons (attachments)

bool clothTexture(const TextureContainer * tex) {
	if(!tex) {
		return false;
	}
	std::string name = util::toLowercase(std::string(tex->m_texName.basename()));
	return name.find("[fabric]") != std::string::npos || name.find("curtain") != std::string::npos
	       || name.find("tapestry") != std::string::npos || name.find("banner") != std::string::npos;
}

std::string quantize(const Vec3f & p) {
	return std::to_string(long(std::lround(p.x * 2.f))) + "," + std::to_string(long(std::lround(p.y * 2.f)))
	       + "," + std::to_string(long(std::lround(p.z * 2.f)));
}

//! Union-find over polygon indices
struct Groups {
	std::vector<size_t> parent;
	explicit Groups(size_t n) : parent(n) {
		for(size_t i = 0; i < n; i++) {
			parent[i] = i;
		}
	}
	size_t find(size_t i) {
		while(parent[i] != i) {
			parent[i] = parent[parent[i]];
			i = parent[i];
		}
		return i;
	}
	void unite(size_t a, size_t b) {
		a = find(a), b = find(b);
		if(a != b) {
			parent[a] = b;
		}
	}
};

//! The vertices of the subdivided patch, welded by position
struct PatchBuilder {
	std::unordered_map<std::string, unsigned short> lookup;
	std::vector<Vec3f> positions;
	std::vector<ClothVertex> attributes;
	std::vector<unsigned short> indices;
	unsigned short add(const Vec3f & p, const Vec2f & uv, ColorRGBA color) {
		std::string key = quantize(p);
		auto it = lookup.find(key);
		if(it != lookup.end()) {
			return it->second;
		}
		unsigned short index = (unsigned short)positions.size();
		lookup[key] = index;
		positions.push_back(p);
		attributes.push_back({ uv, color });
		return index;
	}
};

Vec3f mixColor(ColorRGBA a, ColorRGBA b, float t) {
	Color4f ca = Color4f::fromRGBA(a);
	Color4f cb = Color4f::fromRGBA(b);
	return Vec3f(glm::mix(ca.r, cb.r, t), glm::mix(ca.g, cb.g, t), glm::mix(ca.b, cb.b, t));
}

ColorRGBA toColor(const Vec3f & c) {
	return Color4f(c.x, c.y, c.z, 1.f).toRGBA();
}

/*!
 * Subdivide a polygon into a grid. A quad's vertices are ordered as a strip (0-1 / 2-3, the
 * triangles are 0,1,2 and 3,2,1), a triangle is a degenerate quad with its third vertex
 * repeated.
 */
void subdivide(PatchBuilder & builder, const EERIEPOLY & poly) {
	bool quad = (poly.type & POLY_QUAD) != 0;
	const TexturedVertex & v0 = poly.v[0];
	const TexturedVertex & v1 = poly.v[1];
	const TexturedVertex & v2 = poly.v[2];
	const TexturedVertex & v3 = quad ? poly.v[3] : poly.v[2];
	ColorRGBA c0 = poly.color[0], c1 = poly.color[1], c2 = poly.color[2], c3 = quad ? poly.color[3] : poly.color[2];
	float lu = std::max(glm::distance(v0.p, v1.p), glm::distance(v2.p, v3.p));
	float lv = std::max(glm::distance(v0.p, v2.p), glm::distance(v1.p, v3.p));
	int nu = std::clamp(int(std::ceil(lu / CellSize)), 1, 12);
	int nv = std::clamp(int(std::ceil(lv / CellSize)), 1, 12);
	std::vector<unsigned short> grid((nu + 1) * (nv + 1));
	for(int j = 0; j <= nv; j++) {
		float t = float(j) / float(nv);
		for(int i = 0; i <= nu; i++) {
			float s = float(i) / float(nu);
			Vec3f a = glm::mix(v0.p, v1.p, s);
			Vec3f b = glm::mix(v2.p, v3.p, s);
			Vec2f ua = glm::mix(v0.uv, v1.uv, s);
			Vec2f ub = glm::mix(v2.uv, v3.uv, s);
			Vec3f ca = mixColor(c0, c1, s);
			Vec3f cb = mixColor(c2, c3, s);
			grid[j * (nu + 1) + i] = builder.add(glm::mix(a, b, t), glm::mix(ua, ub, t), toColor(glm::mix(ca, cb, t)));
		}
	}
	for(int j = 0; j < nv; j++) {
		for(int i = 0; i < nu; i++) {
			unsigned short a = grid[j * (nu + 1) + i];
			unsigned short b = grid[j * (nu + 1) + i + 1];
			unsigned short c = grid[(j + 1) * (nu + 1) + i];
			unsigned short d = grid[(j + 1) * (nu + 1) + i + 1];
			// Same winding as the engine's quads: 0,1,2 and 3,2,1
			if(a != b && a != c && b != c) {
				builder.indices.insert(builder.indices.end(), { a, b, c });
			}
			if(d != c && d != b && c != b) {
				builder.indices.insert(builder.indices.end(), { d, c, b });
			}
		}
	}
}

void computeNormals(Cloth & cloth) {
	cloth.normals.assign(cloth.positions.size(), Vec3f(0.f));
	for(size_t i = 0; i + 2 < cloth.indices.size(); i += 3) {
		const Vec3f & a = cloth.positions[cloth.indices[i]];
		const Vec3f & b = cloth.positions[cloth.indices[i + 1]];
		const Vec3f & c = cloth.positions[cloth.indices[i + 2]];
		Vec3f n = glm::cross(b - a, c - a);
		cloth.normals[cloth.indices[i]] += n;
		cloth.normals[cloth.indices[i + 1]] += n;
		cloth.normals[cloth.indices[i + 2]] += n;
	}
	for(Vec3f & n : cloth.normals) {
		float length = glm::length(n);
		n = (length > 1e-6f) ? n / length : Vec3f(0.f, -1.f, 0.f);
	}
}

bool buildCloth(const std::vector<EERIEPOLY *> & polys, JPH::PhysicsSystem & world) {
	ARX_UNUSED(world);

	PatchBuilder builder;
	for(EERIEPOLY * poly : polys) {
		subdivide(builder, *poly);
		if(builder.positions.size() > MaxVertices) {
			LogInfo << "cloth: patch of " << polys.size() << " " << polys[0]->tex->m_texName << " rejected: too many vertices";
			return false;
		}
	}
	if(builder.positions.size() < 4 || builder.indices.size() < 3) {
		LogInfo << "cloth: patch of " << polys.size() << " " << polys[0]->tex->m_texName << " rejected: degenerate";
		return false;
	}

	// Pinned along the top edge (y points down) and wherever the upper part of the cloth meets
	// the rest of the level (a pole, a wall, a rod); the lower part is free to swing even where
	// the modeller welded it to the wall behind
	float minY = 1e9f, maxY = -1e9f;
	for(const Vec3f & p : builder.positions) {
		minY = std::min(minY, p.y);
		maxY = std::max(maxY, p.y);
	}
	if(maxY - minY < MinHeight) {
		Vec3f low(1e9f), high(-1e9f);
		for(const Vec3f & p : builder.positions) {
			low = glm::min(low, p);
			high = glm::max(high, p);
		}
		LogInfo << "cloth: patch of " << polys.size() << " " << polys[0]->tex->m_texName << " rejected: height " << (maxY - minY)
		        << " box " << (high.x - low.x) << "x" << (high.z - low.z) << " at " << low.x << " " << low.y << " " << low.z
		        << " ny " << polys[0]->norm.y << (polys[0]->type & POLY_TRANS ? " trans" : "") << (polys[0]->type & POLY_DOUBLESIDED ? " 2s" : "");
		return false;
	}
	float pinBand = std::max(6.f, (maxY - minY) * 0.06f);

	// A sloped cloth (an awning, a canopy) is a canvas stretched between its attachments:
	// its whole outline stays put, only the middle can billow
	float slope = 0.f;
	for(const EERIEPOLY * poly : polys) {
		slope += std::abs(poly->norm.y);
	}
	slope /= float(polys.size());
	std::vector<bool> boundary(builder.positions.size(), false);
	if(slope > MaxVertical * 0.45f) {
		std::unordered_map<unsigned int, int> edges;
		auto edgeKey = [](unsigned short a, unsigned short b) {
			unsigned int low = std::min(a, b), high = std::max(a, b);
			return (low << 16) | high;
		};
		for(size_t i = 0; i + 2 < builder.indices.size(); i += 3) {
			edges[edgeKey(builder.indices[i], builder.indices[i + 1])]++;
			edges[edgeKey(builder.indices[i + 1], builder.indices[i + 2])]++;
			edges[edgeKey(builder.indices[i + 2], builder.indices[i])]++;
		}
		for(const auto & entry : edges) {
			if(entry.second == 1) {
				boundary[entry.first >> 16] = true;
				boundary[entry.first & 0xffff] = true;
			}
		}
	}

	JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings();
	shared->mVertices.reserve(builder.positions.size());
	size_t pinned = 0;
	for(const Vec3f & p : builder.positions) {
		JPH::Vec3 q = toJolt(p);
		size_t index = size_t(&p - &builder.positions[0]);
		bool pin = (p.y - minY) <= pinBand || boundary[index]
		           || ((p.y - minY) <= (maxY - minY) * AttachBand && g_levelCorners.count(quantize(p)) != 0);
		shared->mVertices.push_back(JPH::SoftBodySharedSettings::Vertex(JPH::Float3(q.GetX(), q.GetY(), q.GetZ()),
		                                                                JPH::Float3(0.f, 0.f, 0.f), pin ? 0.f : 1.f));
		if(pin) {
			pinned++;
		}
	}
	if(pinned == 0 || pinned == builder.positions.size()) {
		LogInfo << "cloth: patch of " << polys.size() << " " << polys[0]->tex->m_texName << " rejected: " << pinned
		        << " of " << builder.positions.size() << " vertices pinned";
		return false;
	}
	for(size_t i = 0; i + 2 < builder.indices.size(); i += 3) {
		shared->AddFace(JPH::SoftBodySharedSettings::Face(builder.indices[i], builder.indices[i + 1], builder.indices[i + 2]));
	}
	// Stiff in stretch, softer in shear, quite free to bend: a hanging fabric
	JPH::SoftBodySharedSettings::VertexAttributes attributes(1e-5f, 5e-5f, 2e-3f);
	shared->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Distance);
	shared->Optimize();

	JPH::SoftBodyCreationSettings settings(shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), LayerMoving);
	settings.mUpdatePosition = false;
	settings.mVertexRadius = 0.015f;
	settings.mLinearDamping = 0.25f;
	settings.mFriction = 0.5f;
	settings.mNumIterations = 6;
	settings.mFacesDoubleSided = true;
	settings.mUserData = makeUserData(KindCloth, g_cloths.size());
	JPH::Body * body = system()->GetBodyInterface().CreateSoftBody(settings);
	if(!body) {
		return false;
	}
	system()->GetBodyInterface().AddBody(body->GetID(), JPH::EActivation::Activate);

	auto cloth = std::make_unique<Cloth>();
	cloth->id = body->GetID();
	cloth->texture = polys[0]->tex;
	cloth->vertices = builder.attributes;
	cloth->positions = builder.positions;
	cloth->rest = builder.positions;
	cloth->indices = builder.indices;
	cloth->phase = float(g_cloths.size()) * 1.7f;
	Vec3f low(1e9f), high(-1e9f);
	for(const Vec3f & p : builder.positions) {
		low = glm::min(low, p);
		high = glm::max(high, p);
	}
	cloth->centre = (low + high) * 0.5f;
	cloth->radius = glm::distance(low, high) * 0.5f;
	computeNormals(*cloth);
	g_cloths.push_back(std::move(cloth));

	for(EERIEPOLY * poly : polys) {
		poly->type |= POLY_HIDE;
		g_hiddenPolygons.push_back(poly);
	}

	return true;
}

//! Copy the simulated vertices out of the body
void fetchPositions(Cloth & cloth) {
	JPH::PhysicsSystem * world = system();
	if(!world) {
		return;
	}
	JPH::BodyLockRead lock(world->GetBodyLockInterface(), cloth.id);
	if(!lock.Succeeded() || !lock.GetBody().IsSoftBody()) {
		return;
	}
	const JPH::Body & body = lock.GetBody();
	const JPH::SoftBodyMotionProperties * motion = static_cast<const JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
	const JPH::Array<JPH::SoftBodyVertex> & vertices = motion->GetVertices();
	JPH::RMat44 transform = body.GetCenterOfMassTransform();
	size_t count = std::min(vertices.size(), cloth.positions.size());
	for(size_t i = 0; i < count; i++) {
		cloth.positions[i] = fromJolt(JPH::Vec3(transform * vertices[i].mPosition));
	}
	computeNormals(cloth);
	cloth.dirty = true;
}

} // anonymous namespace

bool isClothPolygon(const EERIEPOLY & poly) {
	if(!config.video.physics || (poly.type & (POLY_WATER | POLY_LAVA | POLY_IGNORE | POLY_NODRAW | POLY_TRANS))) {
		return false;
	}
	if(std::abs(poly.norm.y) > MaxVertical || !clothTexture(poly.tex)) {
		return false;
	}
	return true;
}

void createCloths() {

	JPH::PhysicsSystem * world = system();
	if(!world || !g_tiles) {
		return;
	}
	PlatformInstant start = platform::getTime();

	// The candidate polygons, and which of them share corners
	std::vector<EERIEPOLY *> candidates;
	g_levelCorners.clear();
	std::unordered_map<std::string, int> skipped;
	for(auto tile : g_tiles->tiles()) {
		for(EERIEPOLY & poly : tile.polygons()) {
			if(isClothPolygon(poly)) {
				candidates.push_back(&poly);
			} else if(clothTexture(poly.tex)) {
				skipped[std::string(poly.tex->m_texName.string()) + ((poly.type & POLY_TRANS) ? " (trans)" : "")
				        + " ny=" + std::to_string(int(std::abs(poly.norm.y) * 100.f))]++;
			} else if(!(poly.type & (POLY_WATER | POLY_LAVA | POLY_IGNORE | POLY_NODRAW))) {
				size_t count = (poly.type & POLY_QUAD) ? 4 : 3;
				for(size_t k = 0; k < count; k++) {
					g_levelCorners[quantize(poly.v[k].p)] = true;
				}
			}
		}
	}
	for(const auto & entry : skipped) {
		LogInfo << "cloth: " << entry.second << " fabric polygons skipped: " << entry.first;
	}
	if(candidates.empty()) {
		return;
	}
	Groups groups(candidates.size());
	std::unordered_map<std::string, size_t> corners;
	for(size_t i = 0; i < candidates.size(); i++) {
		size_t count = (candidates[i]->type & POLY_QUAD) ? 4 : 3;
		for(size_t k = 0; k < count; k++) {
			std::string key = quantize(candidates[i]->v[k].p);
			auto it = corners.find(key);
			if(it == corners.end()) {
				corners[key] = i;
			} else if(candidates[it->second]->tex == candidates[i]->tex) {
				groups.unite(it->second, i);
			}
		}
	}
	std::unordered_map<size_t, std::vector<EERIEPOLY *>> patches;
	for(size_t i = 0; i < candidates.size(); i++) {
		patches[groups.find(i)].push_back(candidates[i]);
	}

	size_t built = 0;
	for(auto & entry : patches) {
		if(entry.second.size() > MaxPolygons) {
			LogInfo << "cloth: patch of " << entry.second.size() << " " << entry.second[0]->tex->m_texName << " rejected: too many polygons";
			continue;
		}
		if(buildCloth(entry.second, *world)) {
			built++;
			LogInfo << "cloth: " << entry.second.size() << " polygons of " << entry.second[0]->tex->m_texName << " at "
			        << g_cloths.back()->centre.x << " " << g_cloths.back()->centre.y << " " << g_cloths.back()->centre.z;
		}
	}

	g_levelCorners.clear();
	if(built) {
		LogInfo << "Jolt: " << built << " cloths from " << candidates.size() << " fabric polygons in "
		        << toMsi(platform::getTime() - start) << " ms";
	}
}

void updateCloths() {

	JPH::PhysicsSystem * world = system();
	if(!world || g_cloths.empty() || std::getenv("ARX_CLOTH_NOUPDATE")) {
		return;
	}
	float dt = toMsf(g_gameTime.lastFrameDuration()) * 0.001f;
	GameInstant now = g_gameTime.now();
	float time = float(toMsi(now)) * 0.001f;
	JPH::BodyInterface & bodies = world->GetBodyInterface();

	for(std::unique_ptr<Cloth> & cloth : g_cloths) {
		if(!closerThan(cloth->centre, player.pos, WindRange + cloth->radius)) {
			continue;
		}
		// A slowly turning breeze with gusts, and a faster flutter across it, a little different
		// for each cloth
		float gust = 0.35f + 0.65f * std::abs(std::sin(time * 0.6f + cloth->phase) * std::sin(time * 0.23f + cloth->phase * 0.7f));
		float angle = time * 0.15f + cloth->phase;
		JPH::Vec3 wind(std::cos(angle), 0.f, std::sin(angle));
		JPH::Vec3 across(-wind.GetZ(), 0.f, wind.GetX());
		wind = wind * (WindStrength * gust) + across * (Flutter * std::sin(time * 2.7f + cloth->phase * 3.f) * gust);
		wind *= dt;
		{
			// (the lock must be released before the body interface is used again below)
			JPH::BodyLockWrite lock(world->GetBodyLockInterface(), cloth->id);
			if(lock.Succeeded() && lock.GetBody().IsSoftBody()) {
				JPH::Body & body = lock.GetBody();
				JPH::SoftBodyMotionProperties * motion = static_cast<JPH::SoftBodyMotionProperties *>(body.GetMotionProperties());
				for(JPH::SoftBodyVertex & vertex : motion->GetVertices()) {
					if(vertex.mInvMass > 0.f) {
						vertex.mVelocity += wind;
					}
				}
			}
		}
		if(!bodies.IsActive(cloth->id)) {
			bodies.ActivateBody(cloth->id);
		}
		fetchPositions(*cloth);
	}

	// ARX_CLOTH_LOG=1: how far each cloth has moved from its modelled shape, every 2 seconds
	static GameInstant lastLog;
	if(std::getenv("ARX_CLOTH_LOG") && now - lastLog > 2s) {
		lastLog = now;
		for(std::unique_ptr<Cloth> & cloth : g_cloths) {
			float most = 0.f;
			for(size_t i = 0; i < cloth->positions.size() && i < cloth->rest.size(); i++) {
				most = std::max(most, glm::distance(cloth->positions[i], cloth->rest[i]));
			}
			LogInfo << "cloth " << cloth->texture->m_texName.basename() << " at " << cloth->centre.x << " " << cloth->centre.y
			        << " " << cloth->centre.z << ": max displacement " << most << " units";
		}
	}
}

void renderCloths() {

	if(g_cloths.empty() || !g_camera || std::getenv("ARX_CLOTH_NORENDER")) {
		return;
	}
	const Vec3f & camPos = g_camera->m_pos;
	float depth = g_camera->cdepth;

	for(std::unique_ptr<Cloth> & cloth : g_cloths) {
		if(!cloth->texture || !closerThan(cloth->centre, camPos, depth + cloth->radius)) {
			continue;
		}
		if(!cloth->buffer) {
			cloth->buffer = GRenderer->createVertexBuffer(cloth->positions.size(), Renderer::Dynamic);
			cloth->dirty = true;
		}
		if(cloth->dirty) {
			SMY_VERTEX * vertices = cloth->buffer->lock(DiscardBuffer);
			for(size_t i = 0; i < cloth->positions.size(); i++) {
				vertices[i].p = cloth->positions[i];
				vertices[i].color = cloth->vertices[i].color;
				vertices[i].uv = cloth->vertices[i].uv;
				vertices[i].normal = cloth->normals[i];
			}
			cloth->buffer->unlock();
			cloth->dirty = false;
		}
		GRenderer->SetTexture(0, cloth->texture);
		RenderState state = render3D().cull(false); // both sides show
		state.setAlphaCutout(cloth->texture->m_pTexture && cloth->texture->m_pTexture->hasAlpha());
		UseRenderState useState(state);
		GRenderer->GetTextureStage(0)->setColorOp(TextureStage::OpModulate);
		cloth->buffer->drawIndexed(Renderer::TriangleList, cloth->positions.size(), 0, cloth->indices.data(),
		                           cloth->indices.size());
	}
}

void clearCloths() {
	if(JPH::PhysicsSystem * world = system()) {
		JPH::BodyInterface & bodies = world->GetBodyInterface();
		for(std::unique_ptr<Cloth> & cloth : g_cloths) {
			bodies.RemoveBody(cloth->id);
			bodies.DestroyBody(cloth->id);
		}
	}
	g_cloths.clear();
	for(EERIEPOLY * poly : g_hiddenPolygons) {
		poly->type &= ~POLY_HIDE;
	}
	g_hiddenPolygons.clear();
	g_levelCorners.clear();
}

size_t clothCount() {
	return g_cloths.size();
}

bool largestCloth(Vec3f & centre, float & radius) {
	const Cloth * best = nullptr;
	for(const std::unique_ptr<Cloth> & cloth : g_cloths) {
		if(!best || cloth->radius > best->radius) {
			best = cloth.get();
		}
	}
	if(!best) {
		return false;
	}
	centre = best->centre;
	radius = best->radius;
	return true;
}

} // namespace physics

#else // ARX_HAVE_JOLT

namespace physics {

bool isClothPolygon(const EERIEPOLY & poly) { ARX_UNUSED(poly); return false; }
void createCloths() { }
void updateCloths() { }
void renderCloths() { }
void clearCloths() { }
size_t clothCount() { return 0; }
bool largestCloth(Vec3f & centre, float & radius) { ARX_UNUSED(centre), ARX_UNUSED(radius); return false; }

} // namespace physics

#endif // ARX_HAVE_JOLT
