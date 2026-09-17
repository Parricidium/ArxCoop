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
/* Based on:
===========================================================================
ARX FATALIS GPL Source Code
Copyright (C) 1999-2010 Arkane Studios SA, a ZeniMax Media company.

This file is part of the Arx Fatalis GPL Source Code ('Arx Fatalis Source Code'). 

Arx Fatalis Source Code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public 
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Arx Fatalis Source Code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied 
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with Arx Fatalis Source Code.  If not, see 
<http://www.gnu.org/licenses/>.

In addition, the Arx Fatalis Source Code is also subject to certain additional terms. You should have received a copy of these 
additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Arx 
Fatalis Source Code. If not, please request a copy in writing from Arkane Studios at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing Arkane Studios, c/o 
ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/
// Initial Code: Cyril Meynier
//
// Copyright (c) 1999-2001 ARKANE Studios SA. All rights reserved

#include "graphics/effects/Decal.h"

#include <array>
#include <type_traits>
#include <unordered_map>

#include "animation/AnimationRender.h"

#include "core/Application.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "core/TimeTypes.h"

#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "game/Spells.h"

#include "graphics/Draw.h"
#include "graphics/GlobalFog.h"
#include "graphics/Math.h"
#include "graphics/data/Mesh.h"
#include "graphics/particle/ParticleEffects.h"
#include "graphics/particle/ParticleTextures.h"
#include "graphics/texture/TextureStage.h"
#include "platform/profiler/Profiler.h"
#include "io/log/Logger.h"

#include "scene/Light.h"
#include "scene/Interactive.h"
#include "scene/Tiles.h"

#include "util/Range.h"


enum DecalType : u8 {
	ScorchMarkDecal,
	BloodDecal,
	WaterDecal
};

struct Decal {
	
	std::array<Vec2f, 4> uv = { Vec3f(0.f), Vec3f(0.f), Vec3f(0.f), Vec3f(0.f) };
	Color3f rgb;
	DecalType type = DecalType(0);
	bool fastdecay = false;
	bool footprint = false; //!< ArxModern: a bloody footprint (nothing to step in)
	TextureContainer * material = nullptr;
	EERIEPOLY * polygon = nullptr;
	ShortGameDuration elapsed;
	ShortGameDuration duration;
	
};

static_assert(std::is_trivially_copyable_v<Decal>);

static const size_t MAX_POLYBOOM = 4000;
static std::vector<Decal> g_decals;

struct BloodyFeet {
	float strength = 0.f; //!< how much blood is left on them (a footprint takes a fifth)
	Color3f rgb = Color3f::red;
	Vec3f lastStep = Vec3f(0.f);
	bool right = false;   //!< the foot of the next print
};

std::unordered_map<const Entity *, BloodyFeet> g_bloodyFeet;

static const float BOOM_RADIUS = 420.f;

size_t PolyBoomCount() {
	return g_decals.size();
}

void PolyBoomClear() {
	g_bloodyFeet.clear();
g_decals.clear();
}

void PolyBoomAddScorch(const Vec3f & poss) {
	
	for(auto tile : g_tiles->tilesAround(g_tiles->getTile(poss), 3))  {
		for(EERIEPOLY & polygon : tile.polygons()) {
			
			if((polygon.type & POLY_TRANS) && !(polygon.type & POLY_WATER)) {
				continue;
			}
			
			size_t nbvert = (polygon.type & POLY_QUAD) ? 4 : 3;
			
			float temp_uv1[4];
			
			bool dod = true;
			for(size_t k = 0; k < nbvert; k++) {
				float ddd = fdist(polygon.v[k].p, poss);
				if(ddd > BOOM_RADIUS) {
					dod = false;
					break;
				} else {
					temp_uv1[k] = 0.5f - ddd * (0.5f / BOOM_RADIUS);
				}
			}
			if(!dod) {
				continue;
			}
			
			if(g_decals.size() >= MAX_POLYBOOM) {
				continue;
			}
			
			Decal & decal = g_decals.emplace_back();
			decal.type = ScorchMarkDecal;
			decal.polygon = &polygon;
			decal.material = g_particleTextures.boom;
			decal.duration = 10s;
			decal.rgb = Color3f::black;
			for(size_t k = 0; k < nbvert; k++) {
				decal.uv[k] = Vec2f(temp_uv1[k]);
			}
			
		}
	}
	
}

void PolyBoomAddSplat(const Sphere & sp, const Color3f & col, long flags) {

	bool footprint = (flags & 4) != 0;
	if(g_decals.size() > (MAX_POLYBOOM / 4) - 30 || (g_decals.size() > 250 && sp.radius < 10 && !footprint)) {
		return;
	}

	float splatsize = 90;
	float size = std::min(sp.radius, 40.f) * 0.75f;
	switch(config.video.levelOfDetail) {
		case 2: {
			if(g_decals.size() > 160 && !footprint) { // (a few footprints always fit)
				return;
			}
			splatsize = 90;
			size *= 1.f;
			break;
		}
		case 1: {
			if(g_decals.size() > 60 && !footprint) {
				return;
			}
			splatsize = 60;
			size *= 0.5f;
			break;
		}
		default: {
			if(g_decals.size() > 10 && !footprint) {
				return;
			}
			splatsize = 30;
			size *= 0.25f;
		}
	}
	
	float py;
	if(!CheckInPoly(sp.origin + Vec3f(0.f, -40, 0.f), &py)) {
		return;
	}
	if(flags & 1) {
		py = sp.origin.y;
	}
	
	EERIEPOLY TheoricalSplat;
	TheoricalSplat.type = POLY_QUAD;
	TheoricalSplat.v[0].p = sp.origin + Vec3f(-splatsize, 0.f, -splatsize);
	TheoricalSplat.v[1].p = sp.origin + Vec3f(-splatsize, 0.f, splatsize);
	TheoricalSplat.v[2].p = sp.origin + Vec3f(splatsize, 0.f, splatsize);
	TheoricalSplat.v[3].p = sp.origin + Vec3f(splatsize, 0.f, -splatsize);
	
	Vec3f RealSplatStart = toXZ(sp.origin) + toXZ(-size);

	if(!footprint) { // (a footprint does not hurry the stains it comes from)
		for(Decal & decal : g_decals) {
			decal.fastdecay = true;
		}
	}
	if(footprint) {
		splatsize = 16.f;
	}

	for(auto tile : g_tiles->tilesAround(g_tiles->getTile(sp.origin), 3)) {
		for(EERIEPOLY & polygon : tile.intersectingPolygons()) {
			
			if((flags & 2) && !(polygon.type & POLY_WATER)) {
				continue;
			}
			
			if((polygon.type & POLY_TRANS) && !(polygon.type & POLY_WATER)) {
				continue;
			}
			
			size_t nbvert = (polygon.type & POLY_QUAD) ? 4 : 3;
			
			bool oki = false;
			for(size_t k = 0; k < nbvert; k++) {
				Vec3f p = polygon.v[k].p;
				if(glm::abs(p.y - py) >= 100.f) {
					continue;
				}
				Vec3f midpoint = (p + polygon.center) * 0.5f;
				if(PointIn2DPolyXZ(&TheoricalSplat, p.x, p.z) ||
				   PointIn2DPolyXZ(&TheoricalSplat, midpoint.x, midpoint.z)) {
					oki = true;
					break;
				}
			}
			
			if(!oki && PointIn2DPolyXZ(&TheoricalSplat, polygon.center.x, polygon.center.z)
			   && glm::abs(polygon.center.y - py) < 100.f) {
				oki = true;
			}
			
			if(!oki || g_decals.size() >= MAX_POLYBOOM) {
				continue;
			}
			if(footprint && !PointIn2DPolyXZ(&TheoricalSplat, polygon.center.x, polygon.center.z)) {
				continue; // (a print stays on the polygons it is really on: fine meshes would take dozens)
			}
			
			Decal & decal = g_decals.emplace_back();
			decal.footprint = footprint;
			
			if(flags & 2) {
				decal.type = WaterDecal;
				decal.material = g_particleTextures.water_splat[Random::get(0, 2)];
				decal.duration = 1500ms;
			} else if(footprint) {
				decal.type = BloodDecal;
				decal.material = g_particleTextures.bloodsplat[Random::get(0, 5)];
				decal.duration = 9s;
			} else {
				decal.type = BloodDecal;
				decal.material = g_particleTextures.bloodsplat[Random::get(0, 5)];
				decal.duration = 400ms * size * (config.video.bloodTrails ? 2 : 1); // (stains last a while longer with the trails)
			}

			decal.polygon = &polygon;
			decal.rgb = col;
			
			for(size_t k = 0; k < nbvert; k++) {
				float vdiff = glm::abs(polygon.v[k].p.y - py);
				decal.uv[k] = getXZ(polygon.v[k].p - RealSplatStart) / (size * 2.f);
				decal.uv[k].x += vdiff / (size * 2.f) * (decal.uv[k].x < 0.5f ? -1.f : 1.f);
				decal.uv[k].y += vdiff / (size * 2.f) * (decal.uv[k].y < 0.5f ? -1.f : 1.f);
			}
			
		}
	}
	
}

// Blood trails (ArxModern) ----------------------------------------------------------------

namespace {

//! Freshness (1 = just spilled, 0 = none) of the blood under \a pos, and its colour
float bloodUnder(const Vec3f & pos, Color3f & rgb) {
	float fresh = 0.f;
	for(const Decal & decal : g_decals) {
		if(decal.type != BloodDecal || decal.footprint || !decal.polygon || decal.duration <= 0) {
			continue;
		}
		if(!closerThan(getXZ(decal.polygon->center), getXZ(pos), 80.f) || glm::abs(decal.polygon->center.y - pos.y) > 80.f) {
			continue;
		}
		if(!PointIn2DPolyXZ(decal.polygon, pos.x, pos.z) && !closerThan(getXZ(decal.polygon->center), getXZ(pos), 25.f)) {
			continue;
		}
		float t = 1.f - decal.elapsed / decal.duration;
		if(t > fresh) {
			fresh = t;
			rgb = decal.rgb;
		}
	}
	return fresh;
}

} // anonymous namespace

void PolyBoomFootstep(Entity * io, const Vec3f & pos) {

	if(!io || !config.video.bloodTrails || (io->ioflags & IO_ITEM)) {
		return;
	}

	BloodyFeet & feet = g_bloodyFeet[io];
	Color3f rgb;
	float fresh = bloodUnder(pos, rgb);

	if(fresh > 0.25f) {
		// Stepping in it: the feet take its colour, the freshest of what is there
		if(fresh > feet.strength) {
			feet.strength = fresh;
			feet.rgb = rgb;
		}
		feet.lastStep = pos;
		return;
	}
	if(feet.strength <= 0.05f) {
		feet.lastStep = pos;
		return;
	}

	// A print beside the path (alternating feet), fading with what is left on them
	Vec2f dir = getXZ(pos - feet.lastStep);
	if(arx::length2(dir) < 1.f) {
		return; // (not walking: the same spot again)
	}
	dir = glm::normalize(dir);
	Vec2f side(-dir.y, dir.x);
	float offset = (feet.right ? 7.f : -7.f) * (io == entities.player() ? 1.f : 0.8f);
	feet.right = !feet.right;
	Vec3f at = pos + Vec3f(side.x * offset, 0.f, side.y * offset);
	Sphere print(at, 14.f + 8.f * feet.strength);
	PolyBoomAddSplat(print, feet.rgb * (0.35f + 0.65f * feet.strength), 4);
	feet.strength -= 0.2f;
	feet.lastStep = pos;

}

void PolyBoomDraw() {

	ARX_PROFILE_FUNC();
	
	ShortGameDuration delta = g_gameTime.lastFrameDuration();
	
	for(Decal & decal : g_decals) {
		arx_assume(decal.elapsed <= ShortGameDuration::max() / 2);
		decal.elapsed += delta * (decal.fastdecay ? 3 : 1);
	}
	
	util::unordered_remove_if(g_decals, [](const Decal & decal) {
		return decal.elapsed >= decal.duration;
	});
	
	for(const Decal & decal : g_decals) {
		
		arx_assume(decal.duration > 0 && decal.duration <= ShortGameDuration::max() / 2);
		arx_assume(decal.elapsed >= 0 && decal.elapsed < decal.duration);
		
		float t = 1.f - decal.elapsed / decal.duration;
		
		size_t nbvert = (decal.polygon->type & POLY_QUAD) ? 4 : 3;
		
		std::array<TexturedVertexUntransformed, 4> vertices;
		
		// Co-op mod (RT): lit like the floor it lies on. Decals are drawn unlit, and the blood
		// blend, (1 - a) * (decal + floor), shows the decal's own red on a black floor: with the
		// deep shadows of the modern renderer, blood glowed in the dark. The polygon's vertex
		// lighting (static + dynamic lights) scales the decal's colour.
		Color3f lit[4] = { Color3f::white, Color3f::white, Color3f::white, Color3f::white };
		{
			EERIEPOLY * polygon = const_cast<EERIEPOLY *>(decal.polygon);
			auto tile = g_tiles->getTile(polygon->center);
			if(tile.valid()) {
				ApplyTileLights(polygon, Vec2s(tile.x, tile.y));
				for(size_t i = 0; i < nbvert; i++) {
					Color4f c = Color4f::fromRGBA(polygon->color[i]);
					lit[i] = Color3f(c.r, c.g, c.b);
				}
			}
		}

		RenderMaterial mat;
		mat.setDepthTest(true);
		mat.setDepthBias(8);
		mat.setLayer(RenderMaterial::Decal);
		mat.setWrapMode(TextureStage::WrapClamp);
		
		switch(decal.type) {
			
			case ScorchMarkDecal: {
				
				ColorRGBA color = (player.m_improve ? Color3f::red * (t * 0.4f) : Color3f::gray(t * 0.8f)).toRGB();
				for(size_t i = 0; i < nbvert; i++) {
					vertices[i].p = decal.polygon->v[i].p;
					vertices[i].uv = decal.uv[i];
					vertices[i].color = color;
				}
				
				mat.setBlendType(player.m_improve ? RenderMaterial::Additive : RenderMaterial::Subtractive);
				
				break;
			}
			
			case BloodDecal: {
				
				// (a footprint stays a red mark rather than the black of fresh blood: lighter blend)
				float alpha = decal.footprint ? glm::clamp(t * 0.7f, 0.f, 0.7f) : glm::clamp(t * 1.5f, 0.f, 1.f);
				for(size_t i = 0; i < nbvert; i++) {
					vertices[i].p = decal.polygon->v[i].p;
					vertices[i].uv = (decal.uv[i] - 0.5f) * std::max(1.f, t * 2.f - 0.5f) + 0.5f;
					Color3f rgb(decal.rgb.r * lit[i].r * t, decal.rgb.g * lit[i].g * t, decal.rgb.b * lit[i].b * t);
					vertices[i].color = Color4f(rgb, alpha).toRGBA();
				}

				mat.setBlendType(RenderMaterial::Subtractive2);
				
				break;
			}
			
			case WaterDecal: {
				
				bool cullXlow = true, cullXhigh = true, cullYlow = true, cullYhigh = true;
				for(size_t i = 0; i < nbvert; i++) {
					vertices[i].p = decal.polygon->v[i].p;
					vertices[i].uv = (decal.uv[i] - 0.5f) * std::max(1.f, t * 2.f - 0.5f) + 0.5f;
					float f = t * 0.5f;
					vertices[i].color = Color3f(decal.rgb.r * lit[i].r * f, decal.rgb.g * lit[i].g * f, decal.rgb.b * lit[i].b * f).toRGB();
cullXlow = cullXlow && vertices[i].uv.x < 0.f;
					cullXhigh = cullXhigh && vertices[i].uv.x > 1.f;
					cullYlow = cullYlow && vertices[i].uv.y < 0.f;
					cullYhigh = cullYhigh && vertices[i].uv.y > 1.f;
				}
				
				if(cullXlow || cullXhigh || cullYlow || cullYhigh) {
					continue;
				}
				
				mat.setBlendType(RenderMaterial::Screen);
				
				break;
			}
			
			default: arx_unreachable();
			
		}
		
		mat.setTexture(decal.material);
		
		drawTriangle(mat, vertices.data());
		if(nbvert == 4) {
			drawTriangle(mat, vertices.data() + 1);
		}
		
	}
	
}
