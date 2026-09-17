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

#include "coop/Qol.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "coop/Puppets.h"
#include "coop/Replication.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "physics/CollisionShapes.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/Localisation.h"
#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Inventory.h"
#include "math/Random.h"
#include "script/Script.h"
#include "scene/Object.h"
#include "physics/Physics.h"
#include "game/Item.h"
#include "game/Player.h"
#include "graphics/Draw.h"
#include "graphics/DrawLine.h"
#include "graphics/Math.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/font/Font.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/Notification.h"
#include "gui/Text.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "io/resource/ResourcePath.h"
#include "platform/Time.h"
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "scene/Tiles.h"

namespace coop {

namespace {

constexpr PlatformDuration MarkerLifetime = std::chrono::seconds(12);
constexpr PlatformDuration MarkerCooldown = std::chrono::milliseconds(700);
constexpr float MarkerRange = 3000.f;
constexpr float BleedOutSeconds = 120.f;
constexpr PlatformDuration AutosaveInterval = std::chrono::minutes(3);

struct Marker {
	PlayerId owner;
	u32 area;
	Vec3f pos;
	PlatformInstant placed;
};
std::vector<Marker> g_markers;
PlatformInstant g_lastMarkerSent;

std::map<PlayerId, u16> g_latencies;
std::map<PlayerId, bool> g_wasDowned;
PlatformInstant g_downedSince;
bool g_bledOut = false;
PlatformInstant g_lastAutosave;

struct LeftSpot {
	u32 area;
	Vec3f pos;
	float yaw;
};
std::map<std::string, LeftSpot> g_leftSpots; //!< Host: by nickname

bool inGame() {
	return g_coop.isActive() && g_coop.state() == State::InGame && ARXmenu.mode() == Mode_InGame
	       && entities.player() && entities.player()->obj;
}

std::string nameOf(PlayerId id) {
	const Player * who = g_coop.player(id);
	return who ? who->name : std::string("?");
}

//! First wall along the view direction (or the entity under the cursor), for placing a marker.
Vec3f aimedPoint() {
	if(FlyingOverIO && FlyingOverIO->show == SHOW_FLAG_IN_SCENE) {
		return FlyingOverIO->pos + Vec3f(0.f, -40.f, 0.f);
	}
	Vec3f from = g_camera ? g_camera->m_pos : player.pos;
	Vec3f dir = angleToVector(g_camera ? g_camera->angle : player.angle);
	Vec3f to = from + dir * MarkerRange;
	float best = MarkerRange;
	for(auto tile : g_tiles->tilesAround((from + to) * 0.5f, MarkerRange * 0.5f + 1.f)) {
		for(const EERIEPOLY & polygon : tile.polygons()) {
			if(polygon.type & (POLY_WATER | POLY_TRANS | POLY_NOCOL)) {
				continue;
			}
			Vec3f hit;
			if(RayCollidingPoly(from, to, polygon, &hit)) {
				best = std::min(best, glm::distance(from, hit));
			}
		}
	}
	return from + dir * std::max(0.f, best - 10.f);
}

void placeMarker(PlayerId owner, u32 area, const Vec3f & pos) {
	g_markers.erase(std::remove_if(g_markers.begin(), g_markers.end(), [owner](const Marker & m) {
		return m.owner == owner;
	}), g_markers.end());
	g_markers.push_back(Marker{ owner, area, pos, platform::getTime() });
	LogInfo << "[coop] marker from " << nameOf(owner) << " at " << int(pos.x) << "," << int(pos.y) << "," << int(pos.z);
	if(area == g_currentArea.handleData()) {
		ARX_SOUND_PlaySFX(g_snd.WHOOSH, &pos, 1.6f);
	}
	if(owner != g_coop.localId()) {
		notification_add(nameOf(owner) + trs("coop_ping_here", " : par ici !"));
	}
}

void sendMarker() {
	PlatformInstant now = platform::getTime();
	if(now - g_lastMarkerSent < MarkerCooldown) {
		return;
	}
	g_lastMarkerSent = now;
	Vec3f pos = aimedPoint();
	placeMarker(g_coop.localId(), g_currentArea.handleData(), pos);
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u32_(g_currentArea.handleData());
	writer.f32_(pos.x);
	writer.f32_(pos.y);
	writer.f32_(pos.z);
	g_coop.sendToOthers(MessageType::PlayerMarker, writer);
}

Color playerColor(PlayerId id) {
	static const Color colors[4] = { Color(255, 210, 90), Color(90, 200, 255), Color(120, 255, 120), Color(255, 120, 220) };
	return colors[id % 4];
}

std::string distanceLabel(const Vec3f & pos) {
	float meters = glm::distance(pos, player.pos) / 100.f;
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.0f m", double(meters));
	return buffer;
}

constexpr Color MarkerColor = Color(235, 40, 40);

//! Screen-space ring (a polyline), \a thickness pixels wide.
void drawRing(const Vec2f & center, float radius, float thickness, Color color) {
	UseRenderState state(render2D());
	constexpr int Segments = 48;
	for(float r = radius; r < radius + thickness; r += 0.5f) {
		Vec2f previous = center + Vec2f(r, 0.f);
		for(int i = 1; i <= Segments; i++) {
			float a = float(i) * 2.f * glm::pi<float>() / float(Segments);
			Vec2f next = center + Vec2f(std::cos(a) * r, std::sin(a) * r);
			drawLine(previous, next, 0.01f, color);
			previous = next;
		}
	}
}

//! Arrow at the screen edge pointing to an off-screen point; returns false when it is on screen.
void drawEdgeArrow(const Vec3f & target, const std::string & label, Color color) {
	Vec4f p = worldToClipSpace(target);
	Vec2f center = Vec2f(g_size.center());
	Vec2f screen = (p.w > 0.f) ? Vec2f(p) / p.w : center - (Vec2f(p) - center); // behind: mirror
	bool onScreen = p.w > 0.f && screen.x >= 0.f && screen.x <= float(g_size.width())
	                && screen.y >= 0.f && screen.y <= float(g_size.height());
	if(onScreen) {
		return;
	}
	Vec2f dir = screen - center;
	if(p.w <= 0.f) {
		dir = -dir; // behind the camera: point the other way
	}
	if(glm::length(dir) < 1.f) {
		dir = Vec2f(0.f, 1.f);
	}
	dir = glm::normalize(dir);
	float margin = 40.f;
	Vec2f half = Vec2f(float(g_size.width()) * 0.5f - margin, float(g_size.height()) * 0.5f - margin);
	float scale = std::min(half.x / std::max(std::abs(dir.x), 0.001f), half.y / std::max(std::abs(dir.y), 0.001f));
	Vec2f tip = center + dir * scale;
	Vec2f side(-dir.y, dir.x);
	Vec2f base = tip - dir * 22.f;
	UseRenderState state(render2D());
	drawLine(base + side * 11.f, tip, 0.01f, color);
	drawLine(base - side * 11.f, tip, 0.01f, color);
	drawLine(base + side * 11.f, base - side * 11.f, 0.01f, color);
	Font::TextSize size = hFontInGame->getTextSize(label);
	Vec2f textPos = base - dir * 16.f - Vec2f(float(size.width()) * 0.5f, float(size.height()) * 0.5f);
	textPos.x = glm::clamp(textPos.x, 4.f, float(g_size.width()) - float(size.width()) - 4.f);
	textPos.y = glm::clamp(textPos.y, 4.f, float(g_size.height()) - float(size.height()) - 4.f);
	hFontInGame->draw(Vec2i(textPos), label, color);
}

} // anonymous namespace

// Latency -----------------------------------------------------------------------------------

u16 latencyOf(PlayerId id) {
	if(id == g_coop.localId()) {
		return g_coop.isClient() ? g_coop.ownLatency() : 0;
	}
	if(g_coop.isHost()) {
		return g_coop.measuredLatency(id);
	}
	auto it = g_latencies.find(id);
	return it != g_latencies.end() ? it->second : 0;
}

void handleLatencies(Reader & reader) {
	u8 count = reader.u8_();
	for(u8 i = 0; i < count; i++) {
		PlayerId id = reader.u8_();
		g_latencies[id] = reader.u16_();
	}
}

// Markers -----------------------------------------------------------------------------------

void handlePlayerMarker(PlayerId id, Reader & reader) {
	u32 area = reader.u32_();
	Vec3f pos = reader.vec3<Vec3f>();
	placeMarker(id, area, pos);
}

// Giving items -------------------------------------------------------------------------------

bool giveItemToPuppet(Entity & item, const Entity & puppet) {
	PlayerId to = puppetOwner(puppet);
	if(to == InvalidPlayerId || !(item.ioflags & IO_ITEM) || !item._itemdata) {
		return false;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u8_(to);
	writer.string(item.classPath().string());
	writer.raw<s16>(s16(std::max(1, int(item._itemdata->count))));
	writer.f32_(item.durability);
	writer.f32_(item.max_durability);
	writer.raw<s16>(item.poisonous);
	writer.raw<s16>(item.poisonous_count);
	writeItemState(writer, captureItemState(item)); // what its scripts made of it: a scroll's spell, an enchantment, its name
	if(g_coop.isHost()) {
		g_coop.sendTo(to, MessageType::GiveItem, writer);
	} else {
		g_coop.sendToHost(MessageType::GiveItem, writer);
	}
	notification_add(trs("coop_given_to", "Donn\xC3\xA9 \xC3\xA0 ") + nameOf(to));
	ARX_SOUND_PlayInterface(g_snd.INVSTD);
	itemHandedOver(item); // (hidden, not destroyed: its number stays taken, see Replication.h)
	return true;
}

void handleGiveItem(PlayerId from, Reader & reader) {
	PlayerId sender = reader.u8_();
	PlayerId to = reader.u8_();
	if(g_coop.isHost()) {
		sender = from;
	}
	std::string classPath = reader.string();
	s16 count = reader.raw<s16>();
	float durability = reader.f32_();
	float maxDurability = reader.f32_();
	s16 poisonous = reader.raw<s16>();
	s16 poisonousCount = reader.raw<s16>();
	ItemState state = readItemState(reader);
	if(g_coop.isHost() && to != g_coop.localId()) {
		if(g_coop.player(to)) {
			Writer writer;
			writer.u8_(sender);
			writer.u8_(to);
			writer.string(classPath);
			writer.raw<s16>(count);
			writer.f32_(durability);
			writer.f32_(maxDurability);
			writer.raw<s16>(poisonous);
			writer.raw<s16>(poisonousCount);
			writeItemState(writer, state);
			g_coop.sendTo(to, MessageType::GiveItem, writer);
		}
		return;
	}
	if(to != g_coop.localId() || !entities.player()) {
		return;
	}
	res::path path = res::path::load(classPath);
	Entity * item = nullptr;
	bool fresh = true;
	if(state.present && state.instance > 0) {
		std::string id = EntityId(path.filename(), state.instance).string();
		if(Entity * existing = entities.getById(id)) {
			// Our hidden copy of the world item the giver had taken (hideTakenItem): it is the
			// item, with its instance script and its INIT already done
			if((existing->ioflags & IO_ITEM) && existing->_itemdata && (existing->ioflags & IO_NOSAVE)
			   && existing->show == SHOW_FLAG_MEGAHIDE && !existing->coopPuppet && !existing->coopProxy) {
				item = existing;
				fresh = false;
				item->ioflags &= ~IO_NOSAVE; // ours now: its later drop must be shared again
				item->show = SHOW_FLAG_IN_SCENE;
			}
		} else {
			// Same id as the original: its instance script keeps resolving on a later give / drop,
			// and the others' stale copy under that id is found again when we drop it
			item = AddItem(path, state.instance, IO_IMMEDIATELOAD);
		}
	}
	if(!item) {
		item = AddItem(path, -1, IO_IMMEDIATELOAD);
	}
	if(!item || !item->_itemdata) {
		LogWarning << "[coop] cannot create the given item " << classPath;
		return;
	}
	if(fresh) {
		initItemCopy(*item, state); // AddItem() only sends LOAD: without INIT the copy had no type, stats, name nor spell
		if(!ValidIOAddress(item)) {
			return;
		}
	} else {
		applyItemState(state, *item);
	}
	item->_itemdata->count = std::max<s16>(1, count);
	item->durability = durability;
	item->max_durability = maxDurability;
	item->poisonous = poisonous;
	item->poisonous_count = poisonousCount;
	giveToPlayer(item); // (inventory full: giveToPlayer() itself puts it at our feet)
	notification_add(nameOf(sender) + trs("coop_gives_you", " vous donne : ") + std::string(getLocalised(item->locname, item->className())));
	LogInfo << "[coop] received " << classPath << " x" << count << " from " << nameOf(sender);
	ARX_SOUND_PlayInterface(g_snd.INVSTD);
}

// Giving gold ---------------------------------------------------------------------------------

bool giveGoldToPlayer(PlayerId to, long amount) {
	if(to == InvalidPlayerId || to == g_coop.localId() || amount <= 0 || !g_coop.player(to)) {
		return false;
	}
	if(player.gold < amount) {
		notification_add(trs("coop_not_enough_gold", "Pas assez d'or"));
		return false;
	}
	ARX_PLAYER_AddGold(-amount);
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u8_(to);
	writer.u32_(u32(amount));
	if(g_coop.isHost()) {
		g_coop.sendTo(to, MessageType::GiveGold, writer);
	} else {
		g_coop.sendToHost(MessageType::GiveGold, writer);
	}
	notification_add(std::to_string(amount) + trs("coop_gold_given_to", " pi\xC3\xA8" "ces d'or donn\xC3\xA9" "es \xC3\xA0 ") + nameOf(to));
	ARX_SOUND_PlayInterface(g_snd.GOLD);
	LogInfo << "[coop] gave " << amount << " gold to " << nameOf(to);
	return true;
}

void handleGiveGold(PlayerId from, Reader & reader) {
	PlayerId sender = reader.u8_();
	PlayerId to = reader.u8_();
	u32 amount = reader.u32_();
	if(g_coop.isHost()) {
		sender = from;
	}
	if(g_coop.isHost() && to != g_coop.localId()) {
		if(g_coop.player(to)) {
			Writer writer;
			writer.u8_(sender);
			writer.u8_(to);
			writer.u32_(amount);
			g_coop.sendTo(to, MessageType::GiveGold, writer);
		}
		return;
	}
	if(to != g_coop.localId() || amount == 0 || amount > 1000000) {
		return;
	}
	ARX_PLAYER_AddGold(long(amount));
	notification_add(nameOf(sender) + trs("coop_gives_you_gold", " vous donne ") + std::to_string(amount) + trs("coop_gold_pieces", " pi\xC3\xA8" "ces d'or"));
	ARX_SOUND_PlayInterface(g_snd.GOLD);
	LogInfo << "[coop] received " << amount << " gold from " << nameOf(sender);
}

// Downed players -----------------------------------------------------------------------------

float bleedOutSecondsLeft() {
	if(!localPlayerDownedRaw()) {
		return 0.f;
	}
	float elapsed = toMsf(platform::getTime() - g_downedSince) * 0.001f;
	return std::max(0.f, BleedOutSeconds - elapsed);
}

bool bledOut() {
	return g_bledOut;
}

// Reconnection spots ---------------------------------------------------------------------------

void rememberLeavingPlayer(PlayerId id) {
	if(!g_coop.isHost()) {
		return;
	}
	for(const TeammateInfo & mate : teammates()) {
		if(mate.id == id) {
			g_leftSpots[mate.name] = LeftSpot{ mate.area, mate.pos, mate.yaw };
			LogInfo << "[coop] remembering where " << mate.name << " left";
			return;
		}
	}
}

bool restoreRejoiningPlayer(PlayerId id) {
	if(!g_coop.isHost()) {
		return false;
	}
	const Player * who = g_coop.player(id);
	if(!who) {
		return false;
	}
	auto it = g_leftSpots.find(who->name);
	if(it == g_leftSpots.end() || it->second.area != g_currentArea.handleData()) {
		return false;
	}
	Writer writer;
	writer.u8_(id);
	writer.u32_(it->second.area);
	writer.f32_(it->second.pos.x);
	writer.f32_(it->second.pos.y);
	writer.f32_(it->second.pos.z);
	writer.f32_(it->second.yaw);
	g_coop.sendTo(id, MessageType::TeleportPlayer, writer);
	LogInfo << "[coop] " << who->name << " is back: sent to where it left";
	g_leftSpots.erase(it);
	return true;
}

// Frame hooks ---------------------------------------------------------------------------------

void qolTestPing() {
	if(inGame()) {
		sendMarker();
	}
}

void qolInit() {
	g_coop.onPlayerMarker = handlePlayerMarker;
	auto previousLeft = g_coop.onPlayerLeft;
	g_coop.onPlayerLeft = [previousLeft](PlayerId id) {
		rememberLeavingPlayer(id);
		if(previousLeft) {
			previousLeft(id);
		}
	};
}

void qolReset() {
	g_markers.clear();
	g_latencies.clear();
	g_wasDowned.clear();
	g_bledOut = false;
}

void qolUpdate() {

	if(!inGame()) {
		g_bledOut = false;
		return;
	}

	PlatformInstant now = platform::getTime();

	// Ping marker
	if(!BLOCK_PLAYER_CONTROLS && GInput->actionNowPressed(CONTROLS_CUST_PING)) {
		sendMarker();
	}
	g_markers.erase(std::remove_if(g_markers.begin(), g_markers.end(), [now](const Marker & m) {
		return now - m.placed > MarkerLifetime;
	}), g_markers.end());

	// Teammates going down / getting up
	for(const TeammateInfo & mate : teammates()) {
		bool & was = g_wasDowned[mate.id];
		if(mate.downed && !was) {
			notification_add(mate.name + trs("coop_is_down", " est \xC3\xA0 terre !"));
			ARX_SOUND_PlayInterface(g_snd.PLAYER_HEART_BEAT);
		} else if(!mate.downed && was) {
			notification_add(mate.name + trs("coop_is_up", " est debout"));
		}
		was = mate.downed;
	}

	// Our own bleed-out
	bool downed = localPlayerDownedRaw();
	static bool wasDowned = false;
	if(downed && !wasDowned) {
		g_downedSince = now;
		g_bledOut = false;
	}
	wasDowned = downed;
	if(downed && !g_bledOut && bleedOutSecondsLeft() <= 0.f) {
		g_bledOut = true;
		LogInfo << "[coop] bled out";
	}
	if(!downed) {
		g_bledOut = false;
	}

	// Potion of life on a downed teammate: instant revival
	if(!BLOCK_PLAYER_CONTROLS && GInput->actionNowPressed(CONTROLS_CUST_DRINKPOTIONLIFE)) {
		PlayerId target = lookedAtDownedTeammate();
		if(target != InvalidPlayerId) {
			if(Entity * potion = getInventoryItemWithLowestDurability("potion_life")) {
				if(potion->_itemdata && potion->_itemdata->count > 1) {
					potion->_itemdata->count--;
				} else {
					potion->destroy();
				}
				reviveTeammate(target);
				notification_add(trs("coop_potion_given", "Potion de vie donn\xC3\xA9" "e \xC3\xA0 ") + nameOf(target));
				ARX_SOUND_PlayInterface(g_snd.INVSTD);
			} else {
				notification_add(trs("coop_no_potion", "Pas de potion de vie"));
			}
		}
	}

	// Clients: keep our character fresh on disk for a reconnection
	if(g_coop.isClient() && now - g_lastAutosave > AutosaveInterval) {
		g_lastAutosave = now;
		saveMyCharacter("autosave");
	}

}

void qolDraw3D() {
	// Markers are drawn as a screen-space reticle in qolDraw2D
}

void qolDraw2D() {

	if(!inGame()) {
		return;
	}

	// Markers: a small double ring at the spot with the distance above (edge arrow when off screen)
	PlatformInstant now = platform::getTime();
	for(const Marker & marker : g_markers) {
		if(marker.area != g_currentArea.handleData()) {
			continue;
		}
		std::string label = distanceLabel(marker.pos);
		Vec4f p = worldToClipSpace(marker.pos + Vec3f(0.f, -20.f, 0.f));
		if(p.w > 0.f) {
			Vec2f screen = Vec2f(p) / p.w;
			if(screen.x >= 0.f && screen.x <= float(g_size.width()) && screen.y >= 0.f && screen.y <= float(g_size.height())) {
				float age = toMsf(now - marker.placed) * 0.001f;
				float s = std::max(1.f, float(g_size.height()) / 720.f);
				float pulse = 0.85f + 0.15f * std::sin(age * 5.f);
				Color color = MarkerColor * pulse;
				drawRing(screen, 14.f * s, 2.f * s, color);
				drawRing(screen, 7.f * s, 2.f * s, color);
				Font::TextSize size = hFontInGame->getTextSize(label);
				Vec2f textPos = screen - Vec2f(float(size.width()) * 0.5f, 18.f * s + float(size.height()));
				hFontInGame->draw(Vec2i(textPos), label, color);
				continue;
			}
		}
		drawEdgeArrow(marker.pos, label, MarkerColor);
	}

	// Off-screen teammates
	for(const TeammateInfo & mate : teammates()) {
		if(!mate.here) {
			continue;
		}
		Vec3f head = mate.pos + Vec3f(0.f, -150.f, 0.f);
		std::string label = mate.name + " - " + distanceLabel(mate.pos);
		if(mate.downed) {
			label += " (" + trs("coop_hud_downed", "\xC3\xA0 terre") + ")";
		}
		drawEdgeArrow(head, label, mate.downed ? Color(255, 90, 90) : playerColor(mate.id));
	}

	// Frozen while a teammate talks
	if(dialogueHold()) {
		std::string text = nameOf(teammateInDialogue()) + trs("coop_in_dialogue", " discute avec quelqu'un...");
		Font::TextSize size = hFontInGame->getTextSize(text);
		Vec2f pos(float(g_size.center().x) - float(size.width()) * 0.5f, float(g_size.height()) * 0.12f);
		UseRenderState state(render2D());
		EERIEDrawFill2DRectDegrad(pos - Vec2f(8.f, 4.f), pos + Vec2f(float(size.width()) + 8.f, float(size.height()) + 4.f),
		                          0.01f, Color(0, 0, 0, 140), Color(0, 0, 0, 140));
		hFontInGame->draw(Vec2i(pos), text, Color(232, 204, 142));
	}

	// Our own countdown while down
	if(localPlayerDownedRaw() && !allPlayersDowned()) {
		char text[96];
		std::string format = trs("coop_downed_countdown", "\xC3\x80 terre - un co\xC3\xA9quipier peut vous relever (%d s)");
		if(format.find("%d") == std::string::npos || format.find('%', format.find("%d") + 2) != std::string::npos) {
			format = "\xC3\x80 terre (%d s)"; // a translation must keep exactly one %d
		}
		std::snprintf(text, sizeof(text), format.c_str(), int(bleedOutSecondsLeft()));
		Font::TextSize size = hFontInGame->getTextSize(text);
		Vec2f pos(float(g_size.center().x) - float(size.width()) * 0.5f, float(g_size.height()) * 0.62f);
		UseRenderState state(render2D());
		EERIEDrawFill2DRectDegrad(pos - Vec2f(8.f, 4.f), pos + Vec2f(float(size.width()) + 8.f, float(size.height()) + 4.f),
		                          0.01f, Color(0, 0, 0, 140), Color(0, 0, 0, 140));
		hFontInGame->draw(Vec2i(pos), text, Color(255, 120, 120));
	}

}

// Stuck arrows -------------------------------------------------------------------------

constexpr float ArrowBreakChance = 0.3f; //!< arrows that break on the wall instead of sticking (JD: no free arrows forever)
static const char * const StuckArrowVar = "\xA7" "coop_stuck"; //!< local script variable marking a stuck arrow item

//! The entity angle rendering an item with rotation \a q: the engine draws items with
//! toQuaternion(pitch, 270 - yaw, roll) = Rz(-roll) * Rx(pitch) * Ry(270 - yaw), a Z-X-Y decomposition.
static Anglef itemAngleOf(const glm::quat & q) {
	glm::mat3 m = glm::mat3_cast(q); // m[column][row]
	float r21 = glm::clamp(m[1][2], -1.f, 1.f);
	float b = std::asin(r21);
	float a, c;
	if(std::abs(r21) < 0.9999f) {
		a = std::atan2(-m[1][0], m[1][1]);
		c = std::atan2(-m[0][2], m[2][2]);
	} else {
		a = std::atan2(m[0][1], m[0][0]); // pointing straight up or down: the roll carries the yaw
		c = 0.f;
	}
	return Anglef(glm::degrees(b), MAKEANGLE(270.f - glm::degrees(c)), -glm::degrees(a));
}

void arrowLanded(const Vec3f & position, const glm::quat & quat) {
	if(!arrowobj || arrowobj->vertexlist.size() < 2) {
		return;
	}
	if(Random::getf() < ArrowBreakChance) {
		LogInfo << "[coop] my arrow broke";
		return;
	}
	Entity * item = AddItem("graph/obj3d/interactive/items/weapons/arrows/arrows", -1, IO_IMMEDIATELOAD);
	if(!item || !item->_itemdata) {
		LogWarning << "[coop] my arrow stuck but no quiver item could be created";
		return;
	}
	initItemCopy(*item, ItemState()); // a fresh class item: INIT / INITEND like every other AddItem() of the engine
	if(!ValidIOAddress(item)) {
		return;
	}
	item->durability = 1.f; // one arrow
	item->_itemdata->count = 1;
	SETVarValueLong(item->m_variables, StuckArrowVar, 1);
	// The projectile drew the arrow with its attach vertex on its position: the entity's pivot goes
	// where the projectile put the mesh's origin, with the projectile's rotation
	VertexId attach = getNamedVertex(arrowobj.get(), "attach");
	if(!attach) {
		attach = arrowobj->origin;
	}
	Vec3f pos = position + quat * (arrowobj->vertexlist[arrowobj->origin].v - arrowobj->vertexlist[attach].v);
	item->show = SHOW_FLAG_IN_SCENE;
	item->pos = item->lastpos = item->initpos = pos;
	item->angle = item->initangle = itemAngleOf(quat);
	item->requestRoomUpdate = true;
	{
		// (self-check of the decomposition against the item render path: yaw drawn as 270 - yaw)
		Anglef render = item->angle;
		render.setYaw(MAKEANGLE(270.f - render.getYaw()));
		float agreement = std::abs(glm::dot(toQuaternion(render), quat));
		if(agreement < 0.999f) {
			LogWarning << "[coop] stuck arrow orientation off: agreement " << agreement;
		}
	}
	dressStuckArrow(*item);
	itemDropped(*item); // (the others get it where and how it is)
	LogInfo << "[coop] my arrow stuck: " << item->idString() << " at " << int(pos.x) << "," << int(pos.y) << "," << int(pos.z);
}

void dressStuckArrow(Entity & item) {
	if(item.className() != "arrows" || GETVarValueLong(item.m_variables, StuckArrowVar) != 1 || !item.usemesh.empty()) {
		return;
	}
	ARX_INTERACTIVE_USEMESH(&item, "weapons/arrow/arrow.teo");
	if(item.obj && item.obj->pbox) {
		item.obj->pbox->active = 0; // stuck: no physics
	}
	item.locname = trs("coop_arrow_stuck", "Fl\xC3\xA8" "che (ramasser)");
}

void undressStuckArrow(Entity & item) {
	if(item.className() != "arrows" || GETVarValueLong(item.m_variables, StuckArrowVar) != 1) {
		return;
	}
	SETVarValueLong(item.m_variables, StuckArrowVar, 0);
	item.locname = "description_arrows";
	if(!item.usemesh.empty()) {
		item.usemesh.clear();
		delete item.obj;
		item.obj = loadObject(item.classPath() + ".teo").release();
		EERIE_COLLISION_Cylinder_Create(&item);
	}
}

} // namespace coop
