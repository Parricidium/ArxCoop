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

#include "coop/Spray.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

#include "animation/AnimationRender.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "graphics/Draw.h"
#include "graphics/Math.h"
#include "graphics/RenderBatcher.h"
#include "graphics/texture/TextureStage.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/image/Image.h"
#include "graphics/image/ImageEncode.h"
#include "graphics/texture/Texture.h"
#include "gui/Interface.h"
#include "gui/Notification.h"
#include "input/Input.h"
#include "io/fs/FilePath.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"
#include "platform/Time.h"
#include "scene/GameSound.h"
#include "scene/Light.h"
#include "scene/Tiles.h"
#include "util/String.h"

namespace coop {

namespace {

constexpr size_t SpraySize = 128;          //!< pixels: what travels and is painted
constexpr size_t SprayBorder = 2;          //!< transparent frame, so that the clamped texture ends cleanly
constexpr float SprayHalfWidth = 42.f;     //!< world units either side of the aimed point
constexpr float SprayRange = 330.f;        //!< how far the player can reach (about twice their height)
constexpr float SprayThickness = 14.f;     //!< world units either side of the surface a polygon may sit at
constexpr size_t MaxSpraysPerLevel = 48;   //!< safety net: one per player anyway (the new one replaces the old)
constexpr PlatformDuration SprayCooldown = 2s;

//! A player's image, as it travels (PNG with alpha) and as it is painted
struct SprayImage {
	std::vector<u8> encoded;
	TextureContainer * texture = nullptr;
};

//! A painted spray: the polygons it covers and where each shows it
struct Spray {
	std::string owner; //!< nickname: a player has one spray per level, the new one replaces the old
	std::vector<u8> encoded;
	TextureContainer * texture = nullptr;
	Vec3f pos = Vec3f(0.f);
	Vec3f normal = Vec3f(0.f);
	Vec3f right = Vec3f(0.f);
	Vec3f up = Vec3f(0.f);
	struct Piece {
		EERIEPOLY * polygon = nullptr;
		std::array<Vec2f, 4> uv;
	};
	std::vector<Piece> pieces;
};

std::map<PlayerId, SprayImage> g_remote;
SprayImage g_local;
std::string g_localError;
std::vector<Spray> g_sprays;
PlatformInstant g_lastSpray;

// Image helpers ----------------------------------------------------------------------------

bool toRGBA(Image & image) {

	if(!image.isValid()) {
		return false;
	}
	if(image.getFormat() == Image::Format_R8G8B8A8) {
		return true;
	}

	size_t channels = image.getNumChannels();
	Image::Format format = image.getFormat();
	bool bgr = (format == Image::Format_B8G8R8A8 || format == Image::Format_B8G8R8);
	bool hasAlpha = (format == Image::Format_L8A8 || format == Image::Format_R8G8B8A8 || format == Image::Format_B8G8R8A8);
	if(format != Image::Format_L8 && format != Image::Format_L8A8 && format != Image::Format_R8G8B8
	   && format != Image::Format_B8G8R8 && format != Image::Format_R8G8B8A8 && format != Image::Format_B8G8R8A8) {
		return false;
	}

	Image rgba;
	rgba.create(image.getWidth(), image.getHeight(), Image::Format_R8G8B8A8);
	const unsigned char * src = image.getData();
	unsigned char * dst = rgba.getData();
	size_t pixels = image.getWidth() * image.getHeight();
	for(size_t i = 0; i < pixels; i++, src += channels, dst += 4) {
		if(channels <= 2) {
			dst[0] = dst[1] = dst[2] = src[0];
			dst[3] = hasAlpha ? src[1] : 255;
		} else {
			dst[0] = src[bgr ? 2 : 0];
			dst[1] = src[1];
			dst[2] = src[bgr ? 0 : 2];
			dst[3] = hasAlpha ? src[3] : 255;
		}
	}
	image = rgba;

	return true;
}

//! Bilinear resize (RGBA), after averaging whole blocks for big reductions
Image resampleRGBA(const Image & source, size_t width, size_t height) {

	Image src = source;
	size_t factor = std::min(src.getWidth() / std::max(width, size_t(1)), src.getHeight() / std::max(height, size_t(1)));
	if(factor >= 2) {
		Image out;
		size_t w = src.getWidth() / factor, h = src.getHeight() / factor;
		out.create(w, h, Image::Format_R8G8B8A8);
		const unsigned char * s = src.getData();
		unsigned char * d = out.getData();
		size_t sw = src.getWidth();
		for(size_t y = 0; y < h; y++) {
			for(size_t x = 0; x < w; x++, d += 4) {
				u32 sum[4] = { 0, 0, 0, 0 };
				for(size_t yy = 0; yy < factor; yy++) {
					const unsigned char * row = s + ((y * factor + yy) * sw + x * factor) * 4;
					for(size_t xx = 0; xx < factor; xx++, row += 4) {
						// Weigh the colour by the alpha: transparent pixels must not tint the edges
						u32 a = row[3];
						sum[0] += row[0] * a;
						sum[1] += row[1] * a;
						sum[2] += row[2] * a;
						sum[3] += a;
					}
				}
				u32 n = u32(factor * factor);
				d[3] = u8(sum[3] / n);
				for(int c = 0; c < 3; c++) {
					d[c] = sum[3] ? u8(sum[c] / sum[3]) : 0;
				}
			}
		}
		src = out;
	}

	Image out;
	out.create(width, height, Image::Format_R8G8B8A8);
	const unsigned char * s = src.getData();
	unsigned char * d = out.getData();
	size_t sw = src.getWidth(), sh = src.getHeight();
	for(size_t y = 0; y < height; y++) {
		float fy = std::max(0.f, (float(y) + 0.5f) * float(sh) / float(height) - 0.5f);
		size_t y0 = std::min(size_t(fy), sh - 1);
		size_t y1 = std::min(y0 + 1, sh - 1);
		float ty = fy - float(y0);
		for(size_t x = 0; x < width; x++, d += 4) {
			float fx = std::max(0.f, (float(x) + 0.5f) * float(sw) / float(width) - 0.5f);
			size_t x0 = std::min(size_t(fx), sw - 1);
			size_t x1 = std::min(x0 + 1, sw - 1);
			float tx = fx - float(x0);
			const unsigned char * p00 = s + (y0 * sw + x0) * 4;
			const unsigned char * p10 = s + (y0 * sw + x1) * 4;
			const unsigned char * p01 = s + (y1 * sw + x0) * 4;
			const unsigned char * p11 = s + (y1 * sw + x1) * 4;
			for(size_t c = 0; c < 4; c++) {
				float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * tx;
				float bottom = float(p01[c]) + (float(p11[c]) - float(p01[c])) * tx;
				d[c] = u8(std::lround(top + (bottom - top) * ty));
			}
		}
	}

	return out;
}

//! The image centred in a square (transparent around), then SpraySize² with a transparent frame
Image squareSpray(const Image & source) {

	size_t w = source.getWidth(), h = source.getHeight();
	size_t side = std::max(w, h);
	Image square;
	square.create(side, side, Image::Format_R8G8B8A8);
	std::fill(square.getData(), square.getData() + side * side * 4, u8(0));
	size_t ox = (side - w) / 2, oy = (side - h) / 2;
	for(size_t y = 0; y < h; y++) {
		std::memcpy(square.getData() + ((oy + y) * side + ox) * 4, source.getData() + y * w * 4, w * 4);
	}

	Image out = resampleRGBA(square, SpraySize, SpraySize);
	unsigned char * d = out.getData();
	for(size_t y = 0; y < SpraySize; y++) {
		for(size_t x = 0; x < SpraySize; x++) {
			if(x < SprayBorder || y < SprayBorder || x >= SpraySize - SprayBorder || y >= SpraySize - SprayBorder) {
				d[(y * SpraySize + x) * 4 + 3] = 0;
			}
		}
	}
	return out;
}

bool setTextureImage(TextureContainer & tc, const Image & image) {
	delete tc.m_pTexture;
	tc.m_pTexture = GRenderer->createTexture();
	if(!tc.m_pTexture || !tc.m_pTexture->create(image, Texture::HasMipmaps)) {
		delete tc.m_pTexture;
		tc.m_pTexture = nullptr;
		return false;
	}
	tc.m_size = tc.m_pTexture->getSize();
	Vec2f storedSize = Vec2f(tc.m_pTexture->getStoredSize());
	tc.uv = Vec2f(tc.m_size) / storedSize;
	tc.hd = Vec2f(.5f, .5f) / storedSize;
	return true;
}

u32 hashBytes(const std::vector<u8> & data) {
	u32 h = 2166136261u;
	for(u8 b : data) {
		h = (h ^ b) * 16777619u;
	}
	return h;
}

//! The texture of an encoded spray (shared between the sprays of the same image)
TextureContainer * sprayTexture(const std::vector<u8> & encoded) {
	if(encoded.empty()) {
		return nullptr;
	}
	std::ostringstream name;
	name << "coop_spray_" << std::hex << hashBytes(encoded) << "_" << encoded.size();
	res::path path(name.str());
	TextureContainer * tc = TextureContainer::Find(path);
	if(tc && tc->m_pTexture) {
		return tc;
	}
	Image image;
	if(!image.load(reinterpret_cast<const char *>(encoded.data()), encoded.size(), "spray") || !toRGBA(image)
	   || image.getWidth() > 512 || image.getHeight() > 512) {
		return nullptr;
	}
	if(!tc) {
		tc = new TextureContainer(path, TextureContainer::NoColorKey);
	}
	if(!setTextureImage(*tc, image)) {
		return nullptr;
	}
	return tc;
}

//! Prepares (squares, shrinks, encodes) an image file for sending. Returns an error message or empty.
std::string prepareSpray(const fs::path & file, SprayImage & spray) {

	std::string data = fs::read(file);
	if(data.empty()) {
		return trs("coop_face_unreadable", "fichier illisible");
	}

	Image image;
	if(!image.load(data.data(), data.size(), file.string().c_str()) || !toRGBA(image)) {
		return trs("coop_face_bad_format", "format d'image non reconnu");
	}
	if(image.getWidth() < 4 || image.getHeight() < 4) {
		return trs("coop_face_bad_format", "format d'image non reconnu");
	}

	image = squareSpray(image);
	if(!encodeImagePng(image, spray.encoded)) {
		return trs("coop_face_encode_failed", "encodage impossible");
	}
	if(spray.encoded.size() > MaxSprayBytes) {
		return trs("coop_face_too_big", "image trop lourde");
	}
	spray.texture = nullptr;

	return std::string();
}

// Network ----------------------------------------------------------------------------------

void writeSpray(Writer & writer, PlayerId id, const SprayImage & spray) {
	writer.u8_(id);
	writer.u32_(u32(spray.encoded.size()));
	writer.bytes(spray.encoded.data(), spray.encoded.size());
}

void sendLocalSpray() {
	if(!g_coop.isActive() || g_coop.localId() == InvalidPlayerId) {
		return;
	}
	Writer writer;
	writeSpray(writer, g_coop.localId(), g_local);
	g_coop.sendToOthers(MessageType::PlayerSpray, writer);
}

//! Host: a new client needs every spray already known.
void sendKnownSpraysTo(PlayerId id) {
	if(!g_local.encoded.empty()) {
		Writer writer;
		writeSpray(writer, g_coop.localId(), g_local);
		g_coop.sendTo(id, MessageType::PlayerSpray, writer);
	}
	for(const auto & entry : g_remote) {
		if(entry.first == id || entry.second.encoded.empty()) {
			continue;
		}
		Writer writer;
		writeSpray(writer, entry.first, entry.second);
		g_coop.sendTo(id, MessageType::PlayerSpray, writer);
	}
}

// Painting -----------------------------------------------------------------------------------

//! Projects the image onto the polygons around \a pos, in the frame (right, up, normal)
void projectSpray(Spray & spray) {

	spray.pieces.clear();
	if(!g_tiles) {
		return;
	}

	float reach = SprayHalfWidth * 1.5f;
	for(auto tile : g_tiles->tilesAround(spray.pos, reach + 1.f)) {
		for(EERIEPOLY & polygon : tile.polygons()) {
			if(polygon.type & (POLY_WATER | POLY_TRANS)) {
				continue;
			}
			// Facing the same way, and in the plane of the aimed surface
			if(glm::dot(polygon.norm, spray.normal) < 0.5f && glm::dot(polygon.norm2, spray.normal) < 0.5f) {
				continue;
			}
			if(glm::abs(glm::dot(polygon.center - spray.pos, spray.normal)) > SprayThickness + polygon.area * 0.0005f
			   && glm::abs(glm::dot(polygon.center - spray.pos, spray.normal)) > SprayThickness * 3.f) {
				continue;
			}
			size_t nbvert = (polygon.type & POLY_QUAD) ? 4 : 3;
			Spray::Piece piece;
			piece.polygon = &polygon;
			Vec2f lo(1e9f), hi(-1e9f);
			bool inPlane = false;
			for(size_t k = 0; k < nbvert; k++) {
				Vec3f d = polygon.v[k].p - spray.pos;
				if(glm::abs(glm::dot(d, spray.normal)) <= SprayThickness) {
					inPlane = true;
				}
				Vec2f uv(glm::dot(d, spray.right) / (2.f * SprayHalfWidth) + 0.5f,
				         -glm::dot(d, spray.up) / (2.f * SprayHalfWidth) + 0.5f);
				piece.uv[k] = uv;
				lo = glm::min(lo, uv);
				hi = glm::max(hi, uv);
			}
			if(nbvert == 3) {
				piece.uv[3] = piece.uv[2];
			}
			if(!inPlane || hi.x < 0.f || hi.y < 0.f || lo.x > 1.f || lo.y > 1.f) {
				continue;
			}
			spray.pieces.push_back(piece);
		}
	}

}

void addSpray(Spray spray) {
	spray.texture = sprayTexture(spray.encoded);
	if(!spray.texture) {
		return;
	}
	projectSpray(spray);
	if(spray.pieces.empty()) {
		return;
	}
	// One per player: the previous one goes (like Counter-Strike)
	if(!spray.owner.empty()) {
		g_sprays.erase(std::remove_if(g_sprays.begin(), g_sprays.end(), [&spray](const Spray & other) {
			return other.owner == spray.owner;
		}), g_sprays.end());
	}
	while(g_sprays.size() >= MaxSpraysPerLevel) {
		g_sprays.erase(g_sprays.begin());
	}
	g_sprays.push_back(std::move(spray));
}

/*!
 * Where the player's spray would land: the nearest level polygon along the view, its normal
 * (facing the player) and the image axes (the camera's right and up, laid on the surface).
 */
bool aimSpray(Vec3f & pos, Vec3f & normal, Vec3f & right, Vec3f & up) {

	if(!g_tiles) {
		return false;
	}
	Vec3f from = g_camera ? g_camera->m_pos : player.pos;
	Vec3f dir = angleToVector(g_camera ? g_camera->angle : player.angle);
	Vec3f to = from + dir * SprayRange;
	float best = SprayRange;
	const EERIEPOLY * hitPolygon = nullptr;
	for(auto tile : g_tiles->tilesAround((from + to) * 0.5f, SprayRange * 0.5f + 1.f)) {
		for(const EERIEPOLY & polygon : tile.polygons()) {
			if(polygon.type & (POLY_WATER | POLY_TRANS | POLY_NOCOL)) {
				continue;
			}
			Vec3f hit;
			if(RayCollidingPoly(from, to, polygon, &hit)) {
				float dist = glm::distance(from, hit);
				if(dist < best) {
					best = dist;
					hitPolygon = &polygon;
					pos = hit;
				}
			}
		}
	}
	if(!hitPolygon) {
		return false;
	}

	normal = hitPolygon->norm;
	if(glm::dot(normal, dir) > 0.f) {
		normal = -normal;
	}
	if(arx::length2(normal) < 0.5f) {
		normal = -dir;
	}
	normal = glm::normalize(normal);

	// The camera's axes laid on the surface: the tag stands upright on a wall, and faces the
	// player on a floor or a ceiling
	const glm::mat4 & viewToWorld = g_preparedCamera.m_viewToWorld;
	Vec3f camRight = Vec3f(viewToWorld[0]);
	Vec3f camUp = -Vec3f(viewToWorld[1]); // (view y points down)
	right = camRight - normal * glm::dot(camRight, normal);
	if(arx::length2(right) < 0.05f) {
		right = camUp - normal * glm::dot(camUp, normal);
	}
	right = glm::normalize(right);
	up = glm::cross(normal, right);
	Vec3f wantedUp = camUp - normal * glm::dot(camUp, normal);
	if(arx::length2(wantedUp) < 0.05f) {
		wantedUp = -dir;
	}
	if(glm::dot(up, wantedUp) < 0.f) {
		up = -up;
	}

	return true;
}

//! The nickname a player's spray is filed under
std::string ownerName(PlayerId id) {
	const Player * who = g_coop.player(id);
	return who ? who->name : "player " + std::to_string(int(id));
}

void placeSpray(PlayerId owner, const std::vector<u8> & encoded, const Vec3f & pos, const Vec3f & normal,
                const Vec3f & right, const Vec3f & up) {
	if(encoded.empty()) {
		return;
	}
	Spray spray;
	spray.owner = ownerName(owner);
	spray.encoded = encoded;
	spray.pos = pos;
	spray.normal = normal;
	spray.right = right;
	spray.up = up;
	addSpray(std::move(spray));
	ARX_SOUND_PlaySFX(g_snd.TORCH_END, &pos, 1.6f);
}

void sendSpray(const Vec3f & pos, const Vec3f & normal, const Vec3f & right, const Vec3f & up) {
	if(!g_coop.isActive()) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u32_(g_currentArea.handleData());
	for(const Vec3f * v : { &pos, &normal, &right, &up }) {
		writer.f32_(v->x);
		writer.f32_(v->y);
		writer.f32_(v->z);
	}
	g_coop.sendToOthers(MessageType::SprayPlaced, writer);
}

} // anonymous namespace

void sprayInit() {

	// After facesInit(): both need to hear about players joining and leaving
	auto previousJoined = g_coop.onPlayerJoined;
	g_coop.onPlayerJoined = [previousJoined](PlayerId id) {
		if(previousJoined) {
			previousJoined(id);
		}
		if(id == g_coop.localId()) {
			if(!g_local.encoded.empty()) {
				sendLocalSpray();
			}
		} else if(g_coop.isHost()) {
			sendKnownSpraysTo(id);
		}
	};
	auto previousLeft = g_coop.onPlayerLeft;
	g_coop.onPlayerLeft = [previousLeft](PlayerId id) {
		if(previousLeft) {
			previousLeft(id);
		}
		g_remote.erase(id);
	};
	g_coop.onPlayerSpray = handlePlayerSpray;
	g_coop.onSprayPlaced = handleSprayPlaced;

	loadLocalSpray();

}

void sprayReset() {
	g_remote.clear();
}

fs::path spraysDirectory() {
	fs::path dir = fs::getUserDir() / "coop" / "sprays";
	if(!fs::is_directory(dir)) {
		fs::create_directories(dir);
	}
	return dir;
}

std::vector<std::string> availableSprays() {
	std::vector<std::string> files;
	fs::path dir = spraysDirectory();
	for(fs::directory_iterator it(dir); !it.end(); ++it) {
		if(!it.is_regular_file()) {
			continue;
		}
		std::string name = it.name();
		std::string ext = util::toLowercase(fs::path(name).ext());
		if(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
			files.push_back(std::move(name));
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

bool loadLocalSpray() {

	SprayImage spray;
	g_localError.clear();
	if(!config.coop.spray.empty()) {
		fs::path file = spraysDirectory() / config.coop.spray;
		g_localError = prepareSpray(file, spray);
		if(!g_localError.empty()) {
			LogWarning << "[coop] spray " << file << ": " << g_localError;
			spray = SprayImage();
		} else {
			LogInfo << "[coop] spray " << config.coop.spray << ": " << spray.encoded.size() << " bytes";
		}
	}

	g_local = std::move(spray);
	sendLocalSpray();

	return g_localError.empty();
}

const std::string & localSprayError() {
	return g_localError;
}

TextureContainer * localSprayPreview() {
	if(g_local.encoded.empty()) {
		return nullptr;
	}
	if(!g_local.texture) {
		g_local.texture = sprayTexture(g_local.encoded);
	}
	return g_local.texture;
}

void handlePlayerSpray(PlayerId id, Reader & reader) {

	u32 size = reader.u32_();
	if(size > MaxSprayBytes || size > reader.remaining()) {
		throw ReadError("bad spray size");
	}
	auto span = reader.rest();
	SprayImage spray;
	spray.encoded.assign(span.first, span.first + size);
	reader.skip(size);

	if(spray.encoded.empty()) {
		g_remote.erase(id);
		return;
	}
	Image check;
	if(!check.load(reinterpret_cast<const char *>(spray.encoded.data()), spray.encoded.size(), "spray")
	   || check.getWidth() > 512 || check.getHeight() > 512) {
		LogWarning << "[coop] ignoring unusable spray from player " << int(id);
		g_remote.erase(id);
		return;
	}

	LogInfo << "[coop] spray of player " << int(id) << ": " << size << " bytes";
	g_remote[id] = std::move(spray);

}

void handleSprayPlaced(PlayerId id, Reader & reader) {
	u32 area = reader.u32_();
	Vec3f pos = reader.vec3<Vec3f>();
	Vec3f normal = reader.vec3<Vec3f>();
	Vec3f right = reader.vec3<Vec3f>();
	Vec3f up = reader.vec3<Vec3f>();
	if(area != g_currentArea.handleData()) {
		return;
	}
	auto it = g_remote.find(id);
	if(it == g_remote.end()) {
		LogInfo << "[coop] spray placed by player " << int(id) << " but its image is not known yet";
		return;
	}
	placeSpray(id, it->second.encoded, pos, normal, right, up);
}

void sprayUpdate() {

	if(!g_coop.isActive() || g_coop.state() != State::InGame || ARXmenu.mode() != Mode_InGame
	   || !entities.player() || !entities.player()->obj) {
		return;
	}
	if(BLOCK_PLAYER_CONTROLS || !GInput->actionNowPressed(CONTROLS_CUST_SPRAY)) {
		return;
	}

	if(g_local.encoded.empty()) {
		notification_add(trs("coop_spray_none", "Aucun spray choisi (menu Personnalisation)"));
		return;
	}
	PlatformInstant now = platform::getTime();
	if(now - g_lastSpray < SprayCooldown) {
		return;
	}
	Vec3f pos, normal, right, up;
	if(!aimSpray(pos, normal, right, up)) {
		return;
	}
	g_lastSpray = now;
	placeSpray(g_coop.localId(), g_local.encoded, pos, normal, right, up);
	sendSpray(pos, normal, right, up);
	LogInfo << "[coop] spray at " << int(pos.x) << "," << int(pos.y) << "," << int(pos.z);

}

void spraysDraw() {

	if(g_sprays.empty()) {
		return;
	}

	// Drawn right away rather than through the render batcher: the batches are flushed at the
	// end of the frame, when every blended draw is treated as a soft particle and fades out
	// against the scene depth - a decal lying on the surface would vanish entirely
	UseRenderState state(render3D().depthWrite(false).depthOffset(8).blend(BlendSrcAlpha, BlendInvSrcAlpha));
	UseTextureState textureState(TextureStage::FilterLinear, TextureStage::WrapClamp);
	GRenderer->GetTextureStage(0)->setAlphaOp(TextureStage::OpModulate); // texture alpha x vertex alpha

	for(const Spray & spray : g_sprays) {
		if(!spray.texture) {
			continue;
		}
		GRenderer->SetTexture(0, spray.texture);
		for(const Spray::Piece & piece : spray.pieces) {
			EERIEPOLY * polygon = piece.polygon;
			size_t nbvert = (polygon->type & POLY_QUAD) ? 4 : 3;
			// Lit like the surface it is painted on (see PolyBoomDraw)
			Color3f lit[4] = { Color3f::white, Color3f::white, Color3f::white, Color3f::white };
			auto tile = g_tiles->getTile(polygon->center);
			if(tile.valid()) {
				ApplyTileLights(polygon, Vec2s(tile.x, tile.y));
				for(size_t i = 0; i < nbvert; i++) {
					Color4f c = Color4f::fromRGBA(polygon->color[i]);
					lit[i] = Color3f(c.r, c.g, c.b);
				}
			}
			TexturedVertex vertices[6];
			size_t count = 0;
			auto emit = [&](size_t i) {
				worldToClipSpace(polygon->v[i].p, vertices[count]);
				vertices[count].uv = piece.uv[i];
				vertices[count].color = Color4f(lit[i], 1.f).toRGBA();
				count++;
			};
			emit(0), emit(1), emit(2);
			if(nbvert == 4) {
				emit(1), emit(2), emit(3);
			}
			EERIEDRAWPRIM(Renderer::TriangleList, vertices, count, true);
		}
	}

	GRenderer->GetTextureStage(0)->setAlphaOp(TextureStage::OpSelectArg1);
	GRenderer->ResetTexture(0);

}

void spraysClear() {
	g_sprays.clear();
}

std::string serializeSprays() {
	if(g_sprays.empty()) {
		return std::string();
	}
	Writer writer;
	writer.u32_(2); // version (2: the owner's nickname)
	writer.u32_(u32(g_sprays.size()));
	for(const Spray & spray : g_sprays) {
		writer.string(spray.owner);
		writer.u32_(u32(spray.encoded.size()));
		writer.bytes(spray.encoded.data(), spray.encoded.size());
		for(const Vec3f * v : { &spray.pos, &spray.normal, &spray.right, &spray.up }) {
			writer.f32_(v->x);
			writer.f32_(v->y);
			writer.f32_(v->z);
		}
	}
	return std::string(reinterpret_cast<const char *>(writer.data().data()), writer.size());
}

void restoreSprays(std::string_view buffer) {
	g_sprays.clear();
	if(buffer.empty()) {
		return;
	}
	try {
		Reader reader(reinterpret_cast<const u8 *>(buffer.data()), buffer.size());
		u32 version = reader.u32_();
		if(version != 1 && version != 2) {
			return;
		}
		u32 count = std::min(reader.u32_(), u32(MaxSpraysPerLevel));
		for(u32 i = 0; i < count; i++) {
			std::string owner = (version >= 2) ? reader.string() : std::string();
			u32 size = reader.u32_();
			if(size > MaxSprayBytes || size > reader.remaining()) {
				throw ReadError("bad spray size");
			}
			Spray spray;
			spray.owner = owner;
			auto span = reader.rest();
			spray.encoded.assign(span.first, span.first + size);
			reader.skip(size);
			spray.pos = reader.vec3<Vec3f>();
			spray.normal = reader.vec3<Vec3f>();
			spray.right = reader.vec3<Vec3f>();
			spray.up = reader.vec3<Vec3f>();
			addSpray(std::move(spray));
		}
		LogInfo << "[coop] " << g_sprays.size() << " sprays restored";
	} catch(const ReadError & e) {
		LogWarning << "[coop] sprays block: " << e.what();
	}
}

bool sprayTestPlace() {
	if(g_local.encoded.empty()) {
		LogInfo << "[coop] test: no local spray";
		return false;
	}
	Vec3f pos, normal, right, up;
	if(!aimSpray(pos, normal, right, up)) {
		LogInfo << "[coop] test: nothing to spray on";
		return false;
	}
	placeSpray(g_coop.localId(), g_local.encoded, pos, normal, right, up);
	sendSpray(pos, normal, right, up);
	LogInfo << "[coop] test: sprayed at " << int(pos.x) << "," << int(pos.y) << "," << int(pos.z)
	        << " normal " << normal.x << "," << normal.y << "," << normal.z << " right " << right.x << "," << right.y << "," << right.z
	        << " up " << up.x << "," << up.y << "," << up.z << " pieces " << (g_sprays.empty() ? 0 : g_sprays.back().pieces.size());
	return true;
}

size_t sprayCount() {
	return g_sprays.size();
}

size_t sprayPieceCount() {
	size_t n = 0;
	for(const Spray & spray : g_sprays) {
		n += spray.pieces.size();
	}
	return n;
}

} // namespace coop
