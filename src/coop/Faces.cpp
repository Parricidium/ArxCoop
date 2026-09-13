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

#include "coop/Faces.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

#include "coop/Session.h"
#include "coop/Text.h"
#include "core/Config.h"
#include "game/Player.h"
#include "graphics/Renderer.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/image/Image.h"
#include "graphics/image/ImageEncode.h"
#include "graphics/texture/Texture.h"
#include "io/fs/FilePath.h"
#include "io/fs/Filesystem.h"
#include "io/fs/SystemPaths.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"
#include "util/String.h"

namespace coop {

namespace {

constexpr size_t VariantCount = 4;

//! Where the face sits in the 128x128 hero head textures, per variant (none for the leather hood).
struct FaceRect {
	int x, y, w, h;
};
constexpr FaceRect FaceRects[VariantCount] = {
	// The painted faces are slightly turned: the nose / mouth line sits at u = 100 (checked against
	// the mesh UVs), the eyes are centred a bit left of it, so the rectangles are centred at u = 98.
	{ 68, 10, 60, 86 }, // bare head
	{ 70, 30, 56, 62 }, // chainmail coif
	{ 70, 30, 56, 62 }, // mithril coif
	{ 0, 0, 0, 0 },     // leather hood: only the eyes show, keep the skin's
};
constexpr size_t HeadTextureSize = 128;
constexpr size_t ComposeScale = 4; //!< composited textures are 512x512 so the photo stays sharp
constexpr size_t PhotoWidth = size_t(FaceRects[0].w) * ComposeScale;  // 240
constexpr size_t PhotoHeight = size_t(FaceRects[0].h) * ComposeScale; // 344
constexpr size_t MaxTextureSize = 512;
constexpr size_t MaxReceivedSize = 1024;

struct Face {
	FaceMode mode = FaceMode::None;
	std::vector<u8> encoded; //!< JPEG, what travels on the network
	Image image;             //!< decoded, RGB
	u8 builtSkin = 0xFF;     //!< the skin \ref textures were composed for
	TextureContainer * textures[VariantCount] = { nullptr, nullptr, nullptr, nullptr };
};

std::map<PlayerId, Face> g_remote;
Face g_local;
std::string g_localError;

// Image helpers ----------------------------------------------------------------------------

//! Converts any of the loader's formats to packed RGB.
bool toRGB(Image & image) {

	if(!image.isValid()) {
		return false;
	}
	if(image.getFormat() == Image::Format_R8G8B8) {
		return true;
	}
	if(image.getFormat() == Image::Format_B8G8R8) {
		return image.convertTo(Image::Format_R8G8B8);
	}

	size_t channels = image.getNumChannels();
	bool bgr = image.getFormat() == Image::Format_B8G8R8A8;
	if(image.getFormat() != Image::Format_L8 && image.getFormat() != Image::Format_L8A8
	   && image.getFormat() != Image::Format_R8G8B8A8 && !bgr) {
		return false;
	}

	Image rgb;
	rgb.create(image.getWidth(), image.getHeight(), Image::Format_R8G8B8);
	const unsigned char * src = image.getData();
	unsigned char * dst = rgb.getData();
	size_t pixels = image.getWidth() * image.getHeight();
	for(size_t i = 0; i < pixels; i++, src += channels, dst += 3) {
		if(channels <= 2) {
			dst[0] = dst[1] = dst[2] = src[0];
		} else {
			dst[0] = src[bgr ? 2 : 0];
			dst[1] = src[1];
			dst[2] = src[bgr ? 0 : 2];
		}
	}
	image = rgb;

	return true;
}

//! Averages  factor x factor blocks (cheap pre-filter before a big downscale).
Image boxDownscale(const Image & src, size_t factor) {
	Image out;
	size_t ch = src.getNumChannels();
	size_t w = std::max(size_t(1), src.getWidth() / factor);
	size_t h = std::max(size_t(1), src.getHeight() / factor);
	out.create(w, h, src.getFormat());
	const unsigned char * s = src.getData();
	unsigned char * d = out.getData();
	size_t stride = src.getWidth() * ch;
	for(size_t y = 0; y < h; y++) {
		for(size_t x = 0; x < w; x++, d += ch) {
			unsigned sum[4] = { 0, 0, 0, 0 };
			for(size_t j = 0; j < factor; j++) {
				const unsigned char * row = s + (y * factor + j) * stride + x * factor * ch;
				for(size_t i = 0; i < factor; i++, row += ch) {
					for(size_t c = 0; c < ch; c++) {
						sum[c] += row[c];
					}
				}
			}
			unsigned count = unsigned(factor * factor);
			for(size_t c = 0; c < ch; c++) {
				d[c] = u8((sum[c] + count / 2) / count);
			}
		}
	}
	return out;
}

//! Bilinear resize of an RGB or RGBA image.
Image resample(const Image & source, size_t width, size_t height) {

	Image src = source;
	size_t factor = std::min(src.getWidth() / std::max(width, size_t(1)),
	                         src.getHeight() / std::max(height, size_t(1)));
	if(factor >= 2) {
		src = boxDownscale(src, factor);
	}

	Image out;
	size_t ch = src.getNumChannels();
	out.create(width, height, src.getFormat());
	const unsigned char * s = src.getData();
	unsigned char * d = out.getData();
	size_t sw = src.getWidth(), sh = src.getHeight();
	for(size_t y = 0; y < height; y++) {
		float fy = std::max(0.f, (float(y) + 0.5f) * float(sh) / float(height) - 0.5f);
		size_t y0 = std::min(size_t(fy), sh - 1);
		size_t y1 = std::min(y0 + 1, sh - 1);
		float ty = fy - float(y0);
		for(size_t x = 0; x < width; x++, d += ch) {
			float fx = std::max(0.f, (float(x) + 0.5f) * float(sw) / float(width) - 0.5f);
			size_t x0 = std::min(size_t(fx), sw - 1);
			size_t x1 = std::min(x0 + 1, sw - 1);
			float tx = fx - float(x0);
			const unsigned char * p00 = s + (y0 * sw + x0) * ch;
			const unsigned char * p10 = s + (y0 * sw + x1) * ch;
			const unsigned char * p01 = s + (y1 * sw + x0) * ch;
			const unsigned char * p11 = s + (y1 * sw + x1) * ch;
			for(size_t c = 0; c < ch; c++) {
				float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * tx;
				float bottom = float(p01[c]) + (float(p11[c]) - float(p01[c])) * tx;
				d[c] = u8(std::lround(top + (bottom - top) * ty));
			}
		}
	}

	return out;
}

//! Largest centered crop with the given aspect ratio.
Image centerCrop(const Image & src, size_t aspectW, size_t aspectH) {
	size_t w = src.getWidth(), h = src.getHeight();
	size_t cw = w, ch = h;
	if(w * aspectH > h * aspectW) {
		cw = std::max(size_t(1), h * aspectW / aspectH);
	} else {
		ch = std::max(size_t(1), w * aspectH / aspectW);
	}
	if(cw == w && ch == h) {
		return src;
	}
	Image out;
	out.create(cw, ch, Image::Format_R8G8B8);
	out.copy(src, 0, 0, (w - cw) / 2, (h - ch) / 2, cw, ch);
	return out;
}

//! Loads one of the game's textures (any of the extensions the engine accepts).
bool loadResourceImage(const res::path & name, Image & image, bool * colorKeyed = nullptr) {
	static const char * const extensions[] = { "png", "jpg", "jpeg", "bmp", "tga" };
	for(const char * ext : extensions) {
		res::path file = name;
		file.set_ext(ext);
		if(g_resources->getFile(file) && image.load(file) && toRGB(image)) {
			if(colorKeyed) {
				*colorKeyed = std::string_view(ext) == "bmp"; // see TextureContainer::LoadFile
			}
			return true;
		}
	}
	return false;
}

/*!
 * The engine makes pure black transparent in its .bmp textures, and the hairstyles of the
 * hero heads rely on it (skin 2 has whole hair strands cut out that way). A composed texture
 * must keep those holes, and so must a full skin sent as JPEG, where black is only nearly black.
 */
void applyHairColorKey(Image & image, u8 threshold) {
	if(image.getFormat() != Image::Format_R8G8B8) {
		return;
	}
	unsigned char * p = image.getData();
	size_t pixels = image.getWidth() * image.getHeight();
	for(size_t i = 0; i < pixels; i++, p += 3) {
		if(p[0] < threshold && p[1] < threshold && p[2] < threshold) {
			p[0] = p[1] = p[2] = 0;
		}
	}
	image.applyColorKeyToAlpha(Color::black, true);
}

/*!
 * Pastes the photo over the face area of a head texture, blended through an elliptic
 * mask so the skin's hair, forehead and jaw line frame it.
 */
Image composePhoto(const Image & head, const Image & photo, const FaceRect & rect) {

	size_t size = HeadTextureSize * ComposeScale;
	Image out = resample(head, size, size);
	size_t ch = out.getNumChannels();

	size_t fx = size_t(rect.x) * ComposeScale, fy = size_t(rect.y) * ComposeScale;
	size_t fw = size_t(rect.w) * ComposeScale, fh = size_t(rect.h) * ComposeScale;
	Image face = resample(photo, fw, fh);

	constexpr float Feather = 0.2f; // fraction of the radius over which the photo fades out
	for(size_t y = 0; y < fh; y++) {
		float ny = ((float(y) + 0.5f) - float(fh) * 0.5f) / (float(fh) * 0.5f);
		for(size_t x = 0; x < fw; x++) {
			float nx = ((float(x) + 0.5f) - float(fw) * 0.5f) / (float(fw) * 0.5f);
			float r = std::sqrt(nx * nx + ny * ny);
			float alpha = std::clamp((1.f - r) / Feather, 0.f, 1.f);
			if(alpha <= 0.f) {
				continue;
			}
			unsigned char * dst = out.getData() + ((fy + y) * size + fx + x) * ch;
			const unsigned char * src = face.getData() + (y * fw + x) * 3;
			for(size_t c = 0; c < 3; c++) {
				dst[c] = u8(std::lround(float(dst[c]) + (float(src[c]) - float(dst[c])) * alpha));
			}
			if(ch == 4) {
				dst[3] = u8(std::lround(float(dst[3]) + (255.f - float(dst[3])) * alpha)); // the photo is opaque
			}
		}
	}

	return out;
}

//! The head texture for one variant of a face, or an invalid image to keep the skin's.
Image composeVariant(const Face & face, u8 skin, size_t variant) {

	if(face.mode == FaceMode::Texture) {
		if(variant != 0) {
			return Image();
		}
		Image skin = face.image;
		applyHairColorKey(skin, 12); // JPEG noise around the black holes
		return skin;
	}

	const FaceRect & rect = FaceRects[variant];
	if(face.mode != FaceMode::Photo || rect.w == 0) {
		return Image();
	}

	res::path names[VariantCount];
	ARX_PLAYER_SkinTextures(skin, names[0], names[1], names[2], names[3]);
	Image head;
	bool keyed = false;
	if(names[variant].empty() || !loadResourceImage(names[variant], head, &keyed)) {
		LogWarning << "[coop] cannot load head texture " << names[variant];
		return Image();
	}
	if(keyed) {
		applyHairColorKey(head, 1);
	}

	return composePhoto(head, face.image, rect);
}

// Textures ---------------------------------------------------------------------------------

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

//! (Re)builds the four head textures of a face for the given skin, under  prefix_N names.
void buildTextures(Face & face, u8 skin, const std::string & prefix) {
	if(face.builtSkin == skin) {
		return;
	}
	face.builtSkin = skin;
	for(size_t variant = 0; variant < VariantCount; variant++) {
		face.textures[variant] = nullptr;
		Image image = composeVariant(face, skin, variant);
		if(!image.isValid()) {
			continue;
		}
		res::path name = prefix + "_" + std::to_string(variant);
		TextureContainer * tc = TextureContainer::Find(name);
		if(!tc) {
			tc = new TextureContainer(name, 0);
		}
		if(setTextureImage(*tc, image)) {
			face.textures[variant] = tc;
		}
	}
}

// Decoding / encoding ----------------------------------------------------------------------

//! Decodes a received (or freshly encoded) face and checks it is reasonable.
bool decodeFace(Face & face, const char * source) {
	face.builtSkin = 0xFF;
	if(!face.image.load(reinterpret_cast<const char *>(face.encoded.data()), face.encoded.size(), source)
	   || !toRGB(face.image)) {
		return false;
	}
	if(face.image.getWidth() > MaxReceivedSize || face.image.getHeight() > MaxReceivedSize) {
		return false;
	}
	return true;
}

//! Prepares (crops, shrinks, encodes) an image file for sending. Returns an error message or empty.
std::string prepareFace(const fs::path & file, Face & face) {

	std::string data = fs::read(file);
	if(data.empty()) {
		return trs("coop_face_unreadable", "fichier illisible");
	}

	Image image;
	if(!image.load(data.data(), data.size(), file.string().c_str()) || !toRGB(image)) {
		return trs("coop_face_bad_format", "format d'image non reconnu");
	}

	bool texture = std::string_view(file.basename()).find("_skin") != std::string_view::npos;
	if(texture) {
		image = centerCrop(image, 1, 1);
		if(image.getWidth() > MaxTextureSize) {
			image = resample(image, MaxTextureSize, MaxTextureSize);
		}
	} else {
		image = centerCrop(image, size_t(FaceRects[0].w), size_t(FaceRects[0].h));
		if(image.getWidth() != PhotoWidth || image.getHeight() != PhotoHeight) {
			image = resample(image, PhotoWidth, PhotoHeight);
		}
	}

	face.mode = texture ? FaceMode::Texture : FaceMode::Photo;
	if(!encodeImageJpeg(image, texture ? 92 : 88, face.encoded)) {
		return trs("coop_face_encode_failed", "encodage impossible");
	}
	if(face.encoded.size() > MaxFaceBytes) {
		return trs("coop_face_too_big", "image trop lourde");
	}
	// Keep exactly what the others will see
	if(!decodeFace(face, file.string().c_str())) {
		return trs("coop_face_encode_failed", "encodage impossible");
	}

	return std::string();
}

// Network ----------------------------------------------------------------------------------

void writeFace(Writer & writer, PlayerId id, const Face & face) {
	writer.u8_(id);
	writer.u8_(u8(face.mode));
	writer.u32_(u32(face.encoded.size()));
	writer.bytes(face.encoded.data(), face.encoded.size());
}

void sendLocalFace() {
	if(!g_coop.isActive() || g_coop.localId() == InvalidPlayerId) {
		return;
	}
	Writer writer;
	writeFace(writer, g_coop.localId(), g_local);
	g_coop.sendToOthers(MessageType::PlayerFace, writer);
}

//! Host: a new client needs every face already known.
void sendKnownFacesTo(PlayerId id) {
	if(g_local.mode != FaceMode::None) {
		Writer writer;
		writeFace(writer, g_coop.localId(), g_local);
		g_coop.sendTo(id, MessageType::PlayerFace, writer);
	}
	for(const auto & entry : g_remote) {
		if(entry.first == id || entry.second.mode == FaceMode::None) {
			continue;
		}
		Writer writer;
		writeFace(writer, entry.first, entry.second);
		g_coop.sendTo(id, MessageType::PlayerFace, writer);
	}
}

} // anonymous namespace

void facesInit() {

	g_coop.onPlayerJoined = [](PlayerId id) {
		if(id == g_coop.localId()) {
			if(g_local.mode != FaceMode::None) {
				sendLocalFace();
			}
		} else if(g_coop.isHost()) {
			sendKnownFacesTo(id);
		}
	};

	g_coop.onPlayerLeft = [](PlayerId id) {
		g_remote.erase(id);
	};

	loadLocalFace();

}

void facesReset() {
	g_remote.clear();
}

/*!
 * A player's own menu picture from <user dir>/coop/<base>.{png,jpg,jpeg,bmp}: PNG transparency is
 * kept, a .bmp gets the engine's black colour key. \a size != 0 forces the pixel size (the menu
 * window takes its dimensions from its textures).
 */
namespace { bool savePng(const Image & image, const fs::path & file); }

TextureContainer * customMenuImage(const char * base, Vec2i size, size_t maxSize) {

	static const char * const extensions[] = { "png", "jpg", "jpeg", "bmp" };
	fs::path dir = fs::getUserDir() / "coop";
	for(const char * ext : extensions) {
		fs::path file = dir / (std::string(base) + "." + ext);
		std::string data = fs::read(file);
		if(data.empty()) {
			continue;
		}
		Image image;
		if(!image.load(data.data(), data.size(), file.string().c_str())) {
			LogWarning << "[coop] cannot read " << file;
			continue;
		}
		if(image.hasAlpha()) {
			if(image.getFormat() != Image::Format_R8G8B8A8 && !image.convertTo(Image::Format_R8G8B8A8)) {
				LogWarning << "[coop] unsupported image format " << file;
				continue;
			}
		} else if(!toRGB(image)) {
			LogWarning << "[coop] unsupported image format " << file;
			continue;
		} else if(std::string_view(ext) == "bmp") {
			applyHairColorKey(image, 1); // black = transparent, like the game's own .bmp textures
		}
		if(size != Vec2i(0)) {
			if(image.getWidth() != size_t(size.x) || image.getHeight() != size_t(size.y)) {
				image = resample(image, size_t(size.x), size_t(size.y));
			}
		} else if(image.getWidth() > maxSize || image.getHeight() > maxSize) {
			float scale = std::min(float(maxSize) / float(image.getWidth()), float(maxSize) / float(image.getHeight()));
			image = resample(image, std::max<size_t>(1, size_t(float(image.getWidth()) * scale)),
			                 std::max<size_t>(1, size_t(float(image.getHeight()) * scale)));
		}
		res::path textureName = std::string("coop_") + base;
		TextureContainer * tc = TextureContainer::Find(textureName);
		if(!tc) {
			tc = new TextureContainer(textureName, TextureContainer::NoColorKey | TextureContainer::NoMipmap);
		}
		if(!setTextureImage(*tc, image)) {
			LogWarning << "[coop] cannot upload " << file;
			return nullptr;
		}
		LogInfo << "[coop] custom menu image: " << file;
		return tc;
	}
	return nullptr;
}

TextureContainer * customMenuBackground() {
	return customMenuImage("menu_background", Vec2i(0), 2048);
}

TextureContainer * customMenuPanel() {
	return customMenuImage("menu_panel", Vec2i(321, 419), 0);
}

TextureContainer * customMenuBorder() {
	return customMenuImage("menu_border", Vec2i(322, 424), 0);
}

//! Copies the game's own menu pictures next to where the custom ones go, as a starting point.
void exportMenuTemplates() {
	fs::path dir = fs::getUserDir() / "coop" / "menu_modeles";
	if(!fs::is_directory(dir) && !fs::create_directories(dir)) {
		return;
	}
	static const struct { const char * resource; const char * file; } templates[] = {
		{ "graph/interface/menus/menu_main_background", "menu_background.png" },
		{ "graph/interface/menus/menu_console_background", "menu_panel.png" },
		{ "graph/interface/menus/menu_console_background_border", "menu_border.png" },
	};
	for(const auto & entry : templates) {
		Image image;
		if(loadResourceImage(entry.resource, image)) {
			savePng(image, dir / entry.file);
		}
	}
}

fs::path facesDirectory() {
	fs::path dir = fs::getUserDir() / "coop" / "faces";
	if(!fs::is_directory(dir)) {
		fs::create_directories(dir);
	}
	return dir;
}

std::vector<std::string> availableFaces() {
	std::vector<std::string> files;
	fs::path dir = facesDirectory();
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

// Templates ---------------------------------------------------------------------------------

namespace {

void plot(Image & image, long x, long y, u8 r, u8 g, u8 b) {
	if(x < 0 || y < 0 || size_t(x) >= image.getWidth() || size_t(y) >= image.getHeight()) {
		return;
	}
	unsigned char * p = image.getData() + (size_t(y) * image.getWidth() + size_t(x)) * 3;
	p[0] = r, p[1] = g, p[2] = b;
}

//! Outlines the face rectangle (red) and the blend ellipse (green) of a 512x512 head texture.
void drawFaceZones(Image & image, const FaceRect & rect) {
	long x0 = rect.x * long(ComposeScale), y0 = rect.y * long(ComposeScale);
	long w = rect.w * long(ComposeScale), h = rect.h * long(ComposeScale);
	for(long i = 0; i < w; i++) {
		plot(image, x0 + i, y0, 255, 0, 0), plot(image, x0 + i, y0 + h - 1, 255, 0, 0);
	}
	for(long j = 0; j < h; j++) {
		plot(image, x0, y0 + j, 255, 0, 0), plot(image, x0 + w - 1, y0 + j, 255, 0, 0);
	}
	// The photo is fully opaque inside r < 1 - Feather (see composePhoto): mark both limits
	constexpr float Inner = 0.8f;
	for(long j = 0; j < h; j++) {
		for(long i = 0; i < w; i++) {
			float nx = ((float(i) + 0.5f) - float(w) * 0.5f) / (float(w) * 0.5f);
			float ny = ((float(j) + 0.5f) - float(h) * 0.5f) / (float(h) * 0.5f);
			float r = std::sqrt(nx * nx + ny * ny);
			if(std::abs(r - 1.f) < 0.01f) {
				plot(image, x0 + i, y0 + j, 0, 255, 0);
			} else if(std::abs(r - Inner) < 0.01f) {
				plot(image, x0 + i, y0 + j, 0, 160, 0);
			}
		}
	}
}

bool savePng(const Image & image, const fs::path & file) {
	std::vector<u8> png;
	if(!encodeImagePng(image, png)) {
		return false;
	}
	return fs::write(file, std::string_view(reinterpret_cast<const char *>(png.data()), png.size()));
}

} // anonymous namespace

fs::path exportFaceTemplates() {

	exportMenuTemplates();

	fs::path dir = facesDirectory() / "modeles";
	if(!fs::is_directory(dir) && !fs::create_directories(dir)) {
		return fs::path();
	}

	size_t size = HeadTextureSize * ComposeScale;
	bool ok = true;
	for(u8 skin = 0; skin < 4; skin++) {
		res::path names[VariantCount];
		ARX_PLAYER_SkinTextures(skin, names[0], names[1], names[2], names[3]);
		Image head;
		if(!loadResourceImage(names[0], head)) {
			ok = false;
			continue;
		}
		Image big = resample(head, size, size);
		std::string base = "tete_hero_" + std::to_string(int(skin) + 1);
		ok = savePng(big, dir / (base + "_skin.png")) && ok;
		drawFaceZones(big, FaceRects[0]);
		ok = savePng(big, dir / (base + "_zones.png")) && ok;
		if(skin == 0) {
			// Photo framing guide: the face area at the size photos are scaled to, with the blend ellipse
			Image guide;
			guide.create(PhotoWidth, PhotoHeight, Image::Format_R8G8B8);
			guide.copy(big, 0, 0, size_t(FaceRects[0].x) * ComposeScale, size_t(FaceRects[0].y) * ComposeScale,
			           PhotoWidth, PhotoHeight);
			ok = savePng(guide, dir / "cadrage_photo.png") && ok;
		}
	}

	if(!ok) {
		LogWarning << "[coop] could not write every face template to " << dir;
	}

	return dir;
}

bool loadLocalFace() {

	Face face;
	g_localError.clear();
	if(!config.coop.face.empty()) {
		fs::path file = facesDirectory() / config.coop.face;
		g_localError = prepareFace(file, face);
		if(!g_localError.empty()) {
			LogWarning << "[coop] face " << file << ": " << g_localError;
			face = Face();
		} else {
			LogInfo << "[coop] face " << config.coop.face << ": " << face.encoded.size() << " bytes"
			        << (face.mode == FaceMode::Texture ? " (full texture)" : "");
		}
	}

	g_local = std::move(face);
	sendLocalFace();
	applyLocalFaceToHero();

	return g_localError.empty();
}

const std::string & localFaceError() {
	return g_localError;
}

TextureContainer * localFacePreview() {
	if(g_local.mode == FaceMode::None) {
		return nullptr;
	}
	buildTextures(g_local, player.skin, "coop_face_local");
	return g_local.textures[0];
}

void handlePlayerFace(PlayerId id, Reader & reader) {

	Face face;
	face.mode = FaceMode(reader.u8_());
	u32 size = reader.u32_();
	if(size > MaxFaceBytes || size > reader.remaining()) {
		throw ReadError("bad face size");
	}
	auto span = reader.rest();
	face.encoded.assign(span.first, span.first + size);
	reader.skip(size);

	if(face.mode == FaceMode::None) {
		g_remote.erase(id);
		return;
	}
	if((face.mode != FaceMode::Photo && face.mode != FaceMode::Texture) || !decodeFace(face, "face")) {
		LogWarning << "[coop] ignoring unusable face from player " << int(id);
		g_remote.erase(id);
		return;
	}

	LogInfo << "[coop] face of player " << int(id) << ": " << size << " bytes";
	g_remote[id] = std::move(face);

}

TextureContainer * playerFaceTexture(PlayerId id, size_t variant, u8 skin) {
	auto it = g_remote.find(id);
	if(it == g_remote.end() || variant >= VariantCount) {
		return nullptr;
	}
	buildTextures(it->second, skin, "coop_face_" + std::to_string(int(id)));
	return it->second.textures[variant];
}

void applyLocalFaceToHero() {

	if(g_local.mode == FaceMode::None) {
		return;
	}

	static const char * const shared[VariantCount] = {
		"graph/obj3d/textures/npc_human_base_hero_head",
		"graph/obj3d/textures/npc_human_chainmail_hero_head",
		"graph/obj3d/textures/npc_human_chainmail_mithril_hero_head",
		"graph/obj3d/textures/npc_human_leather_hero_head",
	};
	for(size_t variant = 0; variant < VariantCount; variant++) {
		TextureContainer * tc = TextureContainer::Find(shared[variant]);
		if(!tc) {
			continue;
		}
		Image image = composeVariant(g_local, player.skin, variant);
		if(image.isValid()) {
			setTextureImage(*tc, image);
		}
	}

}

} // namespace coop
