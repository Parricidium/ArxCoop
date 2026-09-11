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

#include "coop/Puppets.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "animation/Animation.h"
#include "coop/Protocol.h"
#include "coop/Replication.h"
#include "coop/Session.h"
#include "core/Application.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "game/Entity.h"
#include "game/EntityId.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Damage.h"
#include "game/Spells.h"
#include "game/magic/Spell.h"
#include "util/Number.h"
#include "game/Equipment.h"
#include "graphics/data/MeshManipulation.h"
#include "scene/LinkedObject.h"
#include "game/Inventory.h"
#include "game/Player.h"
#include "graphics/Math.h"
#include "gui/CinematicBorder.h"
#include "gui/Menu.h"
#include "gui/MenuPublic.h"
#include "gui/Interface.h"
#include "gui/Text.h"
#include "input/Input.h"
#include "gui/menu/MenuFader.h"
#include "io/Screenshot.h"
#include "io/log/Logger.h"
#include "io/resource/ResourcePath.h"
#include "math/Angle.h"
#include "math/Vector.h"
#include "physics/CollisionShapes.h"
#include "platform/Time.h"
#include "scene/Interactive.h"
#include "scene/ChangeLevel.h"
#include "scene/Object.h"
#include "core/Config.h"
#include "graphics/Renderer.h"
#include "gui/MenuWidgets.h"
#include "script/Script.h"

namespace coop {

static bool puppetsAllowed();

namespace {

constexpr size_t SyncedAnimLayers = 2;
constexpr PlatformDuration SendInterval = std::chrono::milliseconds(50); // 20 Hz
constexpr AnimationDuration MaxAnimDrift = std::chrono::milliseconds(150);
constexpr float PositionSmoothing = 15.f; // higher = snappier
constexpr float SnapDistance = 300.f;      // teleport instead of sliding beyond this

struct AnimState {
	std::string path;
	s64 ctime = 0; // microseconds
	u16 altidx = 0;
	u32 flags = 0;
};

struct TweakInfo {
	std::string file;
	std::string skinFrom;
	std::string skinTo;
	bool operator==(const TweakInfo & o) const { return file == o.file && skinFrom == o.skinFrom && skinTo == o.skinTo; }
};

struct EquipmentState {
	bool combat = false;
	TweakInfo helmet, armor, leggings;
	std::string weapon, shield;
	bool operator==(const EquipmentState & o) const {
		return combat == o.combat && helmet == o.helmet && armor == o.armor && leggings == o.leggings
		       && weapon == o.weapon && shield == o.shield;
	}
	bool operator!=(const EquipmentState & o) const { return !(*this == o); }
};

struct PlayerSnapshot {
	u32 area = 0;
	EquipmentState equipment;
	bool equipmentApplied = false;
	Vec3f pos = Vec3f(0.f);
	Anglef angle;
	AnimState layers[SyncedAnimLayers];
	bool visible = true;
	bool downed = false;
	PlatformInstant received;
};

std::map<PlayerId, PlayerSnapshot> g_remote;
PlatformInstant g_lastSend;

std::string puppetIdString(PlayerId id) {
	return EntityId("coop_player", EntityInstance(id + 1)).string();
}

Entity * findPuppet(PlayerId id) {
	return entities.getById(puppetIdString(id));
}

Entity * createPuppet(PlayerId id, const PlayerSnapshot & state) {

	Entity * io = new Entity("graph/obj3d/interactive/npc/coop_player/coop_player", EntityInstance(id + 1));
	arx_assert(io->idString() == puppetIdString(id));

	io->obj = loadObject("graph/obj3d/interactive/npc/human_base/human_base.teo", false).release();
	if(!io->obj) {
		LogError << "[coop] cannot load the puppet mesh";
		io->destroy();
		return nullptr;
	}

	io->coopPuppet = true;
	io->_npcdata = new IO_NPCDATA;
	io->ioflags = IO_NPC | IO_NOSAVE;
	io->gameFlags &= ~GFLAG_NEEDINIT; // no script to initialize
	io->_npcdata->lifePool.max = io->_npcdata->lifePool.current = 10.f;
	io->_npcdata->vvpos = -99999.f;
	io->armormaterial = "leather";
	io->show = SHOW_FLAG_IN_SCENE;

	io->pos = state.pos;
	io->angle = state.angle;
	io->initpos = state.pos;
	io->initangle = state.angle;
	EERIE_COLLISION_Cylinder_Create(io);
	io->requestRoomUpdate = true;

	ARX_INTERACTIVE_HideGore(io, false);

	LogInfo << "[coop] created puppet " << io->idString() << " for player " << int(id);

	return io;
}

/*!
 * The player plays a few first-person-only animations (arms in front of the camera).
 * Seen from the outside a puppet must play the matching third-person human animation.
 */
std::string thirdPersonAnim(const std::string & path) {

	static const struct { const char * first; const char * third; } table[] = {
		{ "player_wait_1st",                "human_normal_wait" },
		{ "player_wait_short",              "human_normal_wait" },
		{ "player_normal_run_test",         "human_normal_run" },
		{ "player_normal_run",              "human_normal_run" },
		{ "player_normal_run_backward_test", "human_normal_run_backward" },
		{ "player_normal_run_backward",     "human_normal_run_backward" },
		{ "player_normal_strafe_run_left",  "human_normal_strafe_run_left" },
		{ "player_normal_strafe_run_right", "human_normal_strafe_run_right" },
		{ "player_uturn_left_fight_test",   "human_fight_wait" },
		{ "player_uturn_right_fight_test",  "human_fight_wait" },
		{ "player_uturn_left_fight",        "human_fight_wait" },
		{ "player_uturn_right_fight",       "human_fight_wait" },
		{ "player_uturn_left",              "human_normal_uturn" },
		{ "player_uturn_right",             "human_normal_uturn" },
		{ "player_fight_walk_backward",     "human_fight_walk_backward" },
		{ "player_fight_walk",              "human_fight_walk" },
		{ "player_fight_strafe_left",       "human_fight_strafe_left" },
		{ "player_fight_strafe_right",      "human_fight_strafe_right" },
	};

	static const std::string prefix = "graph/obj3d/anims/npc/";
	if(path.compare(0, prefix.size(), prefix) != 0) {
		return path;
	}
	std::string name = path.substr(prefix.size());
	for(const auto & entry : table) {
		if(name == entry.first + std::string(".tea")) {
			return prefix + entry.third + ".tea";
		}
	}
	return path;
}

void applyAnim(AnimLayer & layer, const AnimState & state) {

	if(state.path.empty()) {
		if(layer.cur_anim) {
			layer = AnimLayer();
		}
		return;
	}

	std::string path = thirdPersonAnim(state.path);
	bool changed = !layer.cur_anim || layer.cur_anim->path.string() != path;
	if(changed) {
		ANIM_HANDLE * anim = EERIE_ANIMMANAGER_Load(path);
		if(!anim) {
			return;
		}
		ANIM_Set(layer, anim);
	}

	if(layer.cur_anim) {
		layer.altidx_cur = u16(std::min<size_t>(state.altidx, layer.cur_anim->anims.size() - 1));
	}
	layer.flags = AnimUseType::load(state.flags);

	AnimationDuration ctime = AnimationDuration::ofRaw(state.ctime);
	AnimationDuration drift = layer.ctime > ctime ? layer.ctime - ctime : ctime - layer.ctime;
	if(changed || drift > MaxAnimDrift) {
		layer.ctime = ctime;
	}

}

void applySnapshot(Entity & io, const PlayerSnapshot & state, bool justCreated) {

	if(justCreated || arx::distance2(io.pos, state.pos) > square(SnapDistance)) {
		io.pos = state.pos;
	} else {
		float dt = toMsf(g_gameTime.lastFrameDuration()) * 0.001f;
		float t = std::min(1.f, dt * PositionSmoothing);
		io.pos += (state.pos - io.pos) * t;
	}
	// NPCs are rendered with a yaw of (180 - angle) while the player entity uses its angle directly
	io.angle = Anglef(state.angle.getPitch(), MAKEANGLE(180.f - state.angle.getYaw()), state.angle.getRoll());
	io.requestRoomUpdate = true;
	io.show = state.visible ? SHOW_FLAG_IN_SCENE : SHOW_FLAG_HIDDEN;

	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		applyAnim(io.animlayer[i], state.layers[i]);
	}

}

void removePuppet(PlayerId id) {
	if(Entity * io = findPuppet(id)) {
		LogInfo << "[coop] removing puppet " << io->idString();
		io->destroy();
	}
}

void writeAnim(Writer & writer, const AnimLayer & layer) {
	if(layer.cur_anim) {
		writer.string(layer.cur_anim->path.string());
		writer.s64_(layer.ctime.value().count());
		writer.u16_(layer.altidx_cur);
		writer.u32_(u32(layer.flags));
	} else {
		writer.string("");
		writer.s64_(0);
		writer.u16_(0);
		writer.u32_(0);
	}
}

void readAnim(Reader & reader, AnimState & state) {
	state.path = reader.string();
	state.ctime = reader.s64_();
	state.altidx = reader.u16_();
	state.flags = reader.u32_();
}


// Equipment ---------------------------------------------------------------------------

TweakInfo tweakOf(EquipmentSlot slot) {
	TweakInfo info;
	if(Entity * item = entities.get(player.equiped[slot]); item && item->tweakerinfo) {
		info.file = item->tweakerinfo->filename.string();
		info.skinFrom = item->tweakerinfo->skintochange;
		info.skinTo = item->tweakerinfo->skinchangeto.string();
	}
	return info;
}

EquipmentState localEquipment() {
	EquipmentState state;
	state.combat = (player.Interface & INTER_COMBATMODE) != 0;
	state.helmet = tweakOf(EQUIP_SLOT_HELMET);
	state.armor = tweakOf(EQUIP_SLOT_ARMOR);
	state.leggings = tweakOf(EQUIP_SLOT_LEGGINGS);
	if(Entity * item = entities.get(player.equiped[EQUIP_SLOT_WEAPON])) {
		state.weapon = item->classPath().string();
	}
	if(Entity * item = entities.get(player.equiped[EQUIP_SLOT_SHIELD])) {
		state.shield = item->classPath().string();
	}
	return state;
}

void writeTweak(Writer & writer, const TweakInfo & info) {
	writer.string(info.file);
	writer.string(info.skinFrom);
	writer.string(info.skinTo);
}

void readTweak(Reader & reader, TweakInfo & info) {
	info.file = reader.string();
	info.skinFrom = reader.string();
	info.skinTo = reader.string();
}

EquipmentState g_lastSentEquipment;
PlatformInstant g_lastEquipmentSend;

void sendEquipmentIfNeeded(bool force) {
	EquipmentState state = localEquipment();
	PlatformInstant now = platform::getTime();
	if(!force && state == g_lastSentEquipment && now - g_lastEquipmentSend < std::chrono::seconds(5)) {
		return;
	}
	g_lastSentEquipment = state;
	g_lastEquipmentSend = now;
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.bool_(state.combat);
	writeTweak(writer, state.helmet);
	writeTweak(writer, state.armor);
	writeTweak(writer, state.leggings);
	writer.string(state.weapon);
	writer.string(state.shield);
	g_coop.sendToOthers(MessageType::PlayerEquipment, writer);
}

void handlePlayerEquipment(PlayerId id, Reader & reader) {
	PlayerSnapshot & snap = g_remote[id];
	EquipmentState state;
	state.combat = reader.bool_();
	readTweak(reader, state.helmet);
	readTweak(reader, state.armor);
	readTweak(reader, state.leggings);
	state.weapon = reader.string();
	state.shield = reader.string();
	if(state != snap.equipment) {
		snap.equipment = state;
		snap.equipmentApplied = false;
	}
}

void applyTweakTo(Entity * io, const TweakInfo & info, TweakType type, std::string_view selection) {
	if(info.file.empty() && info.skinTo.empty()) {
		return;
	}
	IO_TWEAKER_INFO tweak;
	tweak.filename = res::path::load(info.file);
	tweak.skintochange = info.skinFrom;
	tweak.skinchangeto = res::path::load(info.skinTo);
	ARX_EQUIPMENT_ApplyTweak(io, tweak, type, selection);
}

//! Creates a display-only item linked to the puppet (destroyed with it or when replaced).
void attachPuppetItem(Entity & puppet, const std::string & classPath, std::string_view puppetVertex,
                      std::string_view itemVertex) {
	if(classPath.empty()) {
		return;
	}
	Entity * item = AddItem(res::path::load(classPath), -1, IO_IMMEDIATELOAD | NO_ON_LOAD);
	if(!item || !item->obj) {
		return;
	}
	item->ioflags |= IO_NOSAVE | IO_NO_COLLISIONS;
	item->gameFlags &= ~GFLAG_INTERACTIVITY;
	item->coopPuppet = true; // never part of the shared world
	linkEntities(puppet, puppetVertex, *item, itemVertex);
}

void applyEquipment(Entity & io, const EquipmentState & state) {

	// Fresh body, then the armor pieces, then what the hands hold
	std::vector<Entity *> displayItems;
	for(Entity & entity : entities) {
		if(entity.coopPuppet && (entity.ioflags & IO_ITEM) && entity.owner() == &io) {
			displayItems.push_back(&entity);
		}
	}
	for(Entity * item : displayItems) {
		item->destroy(); // unlinks itself from our mesh
	}
	delete io.obj;
	io.obj = loadObject("graph/obj3d/interactive/npc/human_base/human_base.teo", false).release();
	if(!io.obj) {
		return;
	}
	applyTweakTo(&io, state.helmet, TWEAK_HEAD, "head");
	applyTweakTo(&io, state.armor, TWEAK_TORSO, "chest");
	applyTweakTo(&io, state.leggings, TWEAK_LEGS, "leggings");
	if(!state.weapon.empty()) {
		attachPuppetItem(io, state.weapon, state.combat ? "primary_attach" : "weapon_attach", "primary_attach");
	}
	if(!state.shield.empty()) {
		attachPuppetItem(io, state.shield, "shield_attach", "shield_attach");
	}
	ARX_INTERACTIVE_HideGore(&io, false);
	EERIE_Object_Precompute_Fast_Access(io.obj);
	EERIE_COLLISION_Cylinder_Create(&io);
	io.requestRoomUpdate = true;

}

void handlePlayerState(PlayerId id, Reader & reader) {

	PlayerSnapshot & state = g_remote[id];
	state.area = reader.u32_();
	state.pos.x = reader.f32_();
	state.pos.y = reader.f32_();
	state.pos.z = reader.f32_();
	float pitch = reader.f32_();
	float yaw = reader.f32_();
	float roll = reader.f32_();
	state.angle = Anglef(pitch, yaw, roll);
	state.visible = reader.bool_();
	state.downed = reader.bool_();
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		readAnim(reader, state.layers[i]);
	}
	state.received = platform::getTime();

}


// Downed players and revival -----------------------------------------------------------

constexpr float ReviveDistance = 220.f;
constexpr float ReviveSeconds = 4.f;
PlayerId g_reviveTarget = InvalidPlayerId;
float g_reviveProgress = 0.f;
bool g_testHoldRevive = false; //!< --coop-test: pretend the mouse button is held

bool isLocalDowned() {
	return g_coop.isActive() && g_coop.state() == State::InGame && player.lifePool.current <= 0.f;
}

//! The downed puppet we are looking at from close by, if any.
PlayerId lookedAtDownedPuppet() {
	if(!entities.player() || isLocalDowned()) {
		return InvalidPlayerId;
	}
	Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	for(const auto & entry : g_remote) {
		if(!entry.second.downed) {
			continue;
		}
		const Entity * io = findPuppet(entry.first);
		if(!io || io->show != SHOW_FLAG_IN_SCENE) {
			continue;
		}
		Vec3f to = io->pos - entities.player()->pos;
		float dist = glm::length(to);
		if(dist > ReviveDistance || dist < 1.f) {
			continue;
		}
		to.y = 0.f;
		if(glm::length(to) > 1.f && glm::dot(glm::normalize(to), forward) > 0.7f) {
			return entry.first;
		}
	}
	return InvalidPlayerId;
}

void reviveUpdate() {
	PlayerId target = lookedAtDownedPuppet();
	{
		static PlatformInstant lastLog; // TODO(dev) remove
		bool anyDowned = false;
		for(const auto & entry : g_remote) {
			anyDowned = anyDowned || entry.second.downed;
		}
		if(anyDowned && platform::getTime() - lastLog > std::chrono::seconds(3)) {
			lastLog = platform::getTime();
			for(const auto & entry : g_remote) {
				const Entity * io = findPuppet(entry.first);
				if(!io || !entry.second.downed) {
					continue;
				}
				Vec3f to = io->pos - entities.player()->pos;
				Vec3f flat(to.x, 0.f, to.z);
				float dot = glm::length(flat) > 1.f ? glm::dot(glm::normalize(flat), angleToVectorXZ(player.angle.getYaw())) : 0.f;
				LogInfo << "[coop] revive check: " << io->idString() << " dist " << int(glm::length(to)) << " dot " << dot
				        << " mouse " << GInput->getMouseButtonRepeat(Mouse::Button_0) << " block " << BLOCK_PLAYER_CONTROLS
				        << " target " << int(target) << " progress " << g_reviveProgress;
			}
		}
	}
	bool holding = target != InvalidPlayerId && !BLOCK_PLAYER_CONTROLS
	               && (GInput->getMouseButtonRepeat(Mouse::Button_0) || g_testHoldRevive);
	if(!holding || target != g_reviveTarget) {
		g_reviveTarget = target;
		g_reviveProgress = 0.f;
		return;
	}
	g_reviveProgress += toMsf(g_platformTime.lastFrameDuration()) * 0.001f / ReviveSeconds;
	if(g_reviveProgress >= 1.f) {
		g_reviveProgress = 0.f;
		g_reviveTarget = InvalidPlayerId;
		Writer writer;
		writer.u8_(target);
		if(g_coop.isHost()) {
			g_coop.sendTo(target, MessageType::Revive, Writer());
		} else {
			g_coop.sendToHost(MessageType::Revive, writer);
		}
		LogInfo << "[coop] revived player " << int(target);
	}
}

void reviveLocalPlayer() {
	if(!isLocalDowned()) {
		return;
	}
	player.lifePool.current = player.lifePool.max * 0.5f;
	player.DeadTime = 0;
	player.m_paralysed = false;
	BLOCK_PLAYER_CONTROLS = false;
	player.Interface = INTER_LIFE_MANA | INTER_MINIBACK | INTER_MINIBOOK;
	HERO_SHOW_1ST = -1;
	if(entities.player()) {
		entities.player()->animlayer[0].cur_anim = nullptr; // back to the normal stance
	}
	LogInfo << "[coop] back on my feet";
}

int g_applyingRemoteSpell = 0;

// Spells ------------------------------------------------------------------------------

//! Translates an entity id as seen by another player into our own entity.
Entity * mapRemoteEntity(PlayerId from, const std::string & id) {
	if(id.empty()) {
		return nullptr;
	}
	if(id == "player") {
		return findPuppet(from);
	}
	if(id.compare(0, 12, "coop_player_") == 0) {
		int instance = util::toInt(std::string_view(id).substr(12)).value_or(0);
		PlayerId owner = PlayerId(instance - 1);
		if(owner == g_coop.localId()) {
			return entities.player();
		}
		return findPuppet(owner);
	}
	return entities.getById(id);
}

void handleSpellCast(PlayerId from, Reader & reader) {
	u32 spell = reader.u32_();
	float level = reader.f32_();
	u32 flags = reader.u32_();
	std::string targetId = reader.string();
	s64 duration = reader.s64_();
	Entity * caster = findPuppet(from);
	if(!caster || !puppetsAllowed()) {
		return;
	}
	Entity * target = mapRemoteEntity(from, targetId);
	LogInfo << "[coop] player " << int(from) << " casts spell " << spell << " level " << level;
	g_applyingRemoteSpell++;
	ARX_SPELLS_Launch(SpellType(spell), *caster,
	                  SpellcastFlags::load(flags) | SPELLCAST_FLAG_NOCHECKCANCAST | SPELLCAST_FLAG_NOMANA
	                  | SPELLCAST_FLAG_NOANIM,
	                  long(level), target, GameDuration::ofRaw(duration));
	g_applyingRemoteSpell--;
}

// NPC mirroring ------------------------------------------------------------------------

struct NpcSnapshot {
	Vec3f pos = Vec3f(0.f);
	float yaw = 0.f;
	float life = 0.f;
	u8 show = 0;
	bool dead = false;
	AnimState layers[SyncedAnimLayers];
	PlatformInstant sent;
};

std::map<std::string, NpcSnapshot> g_npcSent; //!< Host: last state sent per NPC
PlatformInstant g_lastNpcSend;
constexpr PlatformDuration NpcSendInterval = std::chrono::milliseconds(100); // 10 Hz
constexpr PlatformDuration NpcKeyframeInterval = std::chrono::seconds(5);

bool isMirroredNpc(const Entity & io) {
	return (io.ioflags & IO_NPC) && !io.coopPuppet && !io.coopProxy && &io != entities.player()
	       && io.show != SHOW_FLAG_IN_INVENTORY && io.show != SHOW_FLAG_MEGAHIDE;
}

void writeNpc(Writer & writer, const Entity & io, NpcSnapshot & snap) {
	writer.string(io.idString());
	writer.f32_(io.pos.x);
	writer.f32_(io.pos.y);
	writer.f32_(io.pos.z);
	writer.f32_(io.angle.getYaw());
	writer.f32_(io._npcdata->lifePool.current);
	writer.u8_(u8(io.show));
	writer.bool_(io.mainevent == SM_DEAD);
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		writeAnim(writer, io.animlayer[i]);
	}
	snap.pos = io.pos;
	snap.yaw = io.angle.getYaw();
	snap.life = io._npcdata->lifePool.current;
	snap.show = u8(io.show);
	snap.dead = io.mainevent == SM_DEAD;
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		const AnimLayer & layer = io.animlayer[i];
		snap.layers[i].path = layer.cur_anim ? layer.cur_anim->path.string() : std::string();
		snap.layers[i].flags = u32(layer.flags);
		snap.layers[i].altidx = layer.altidx_cur;
	}
}

bool npcChanged(const Entity & io, const NpcSnapshot & snap) {
	if(arx::distance2(io.pos, snap.pos) > square(2.f) || glm::abs(io.angle.getYaw() - snap.yaw) > 1.f) {
		return true;
	}
	if(io._npcdata->lifePool.current != snap.life || u8(io.show) != snap.show
	   || (io.mainevent == SM_DEAD) != snap.dead) {
		return true;
	}
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		const AnimLayer & layer = io.animlayer[i];
		std::string path = layer.cur_anim ? layer.cur_anim->path.string() : std::string();
		if(path != snap.layers[i].path || u32(layer.flags) != snap.layers[i].flags
		   || layer.altidx_cur != snap.layers[i].altidx) {
			return true;
		}
	}
	return false;
}

size_t g_npcStatesApplied = 0;
size_t g_npcStatesSent = 0;
PlatformInstant g_lastNpcStatsLog;

void logNpcStats() {
	PlatformInstant now = platform::getTime();
	if(now - g_lastNpcStatsLog > std::chrono::seconds(5)) {
		g_lastNpcStatsLog = now;
		if(g_npcStatesSent || g_npcStatesApplied) {
			LogInfo << "[coop] npc sync: sent " << g_npcStatesSent << ", applied " << g_npcStatesApplied << " in the last 5s";
			Logger::flush();
		}
		g_npcStatesSent = g_npcStatesApplied = 0;
	}
}

//! Client: last state received for each NPC; applied every frame since animations move NPCs locally.
struct NpcTarget {
	Vec3f pos;
	float yaw;
	bool fresh; //!< true until applied once (animations, life, show are applied once per update)
	float life;
	u8 show;
	bool dead;
	AnimState layers[SyncedAnimLayers];
};
std::map<std::string, NpcTarget> g_npcTargets;
size_t g_npcLogBudget = 0;

void applyNpcState(Reader & reader) {
	u16 count = reader.u16_();
	g_npcStatesApplied += count;
	logNpcStats();
	for(u16 n = 0; n < count; n++) {
		std::string id = reader.string();
		NpcTarget & target = g_npcTargets[id];
		target.pos = reader.vec3<Vec3f>();
		target.yaw = reader.f32_();
		target.life = reader.f32_();
		target.show = reader.u8_();
		target.dead = reader.bool_();
		for(size_t i = 0; i < SyncedAnimLayers; i++) {
			readAnim(reader, target.layers[i]);
		}
		target.fresh = true;
		if(g_npcLogBudget > 0) {
			g_npcLogBudget--;
			LogInfo << "[coop] npc state " << id << " " << int(target.pos.x) << "," << int(target.pos.y) << "," << int(target.pos.z)
			        << " life " << target.life << " anim " << target.layers[0].path;
		}
	}
}

void npcMirrorFrame() {
	static PlatformInstant lastBudget;
	PlatformInstant now = platform::getTime();
	if(now - lastBudget > std::chrono::seconds(20)) {
		lastBudget = now;
		g_npcLogBudget = 5; // TODO(dev) remove: a few received states every 20 s
	}
	float dt = toMsf(g_gameTime.lastFrameDuration()) * 0.001f;
	for(auto & entry : g_npcTargets) {
		NpcTarget & target = entry.second;
		Entity * io = entities.getById(entry.first);
		if(!io || !(io->ioflags & IO_NPC) || io->coopPuppet) {
			static std::set<std::string> reported; // TODO(dev) remove
			if(reported.insert(entry.first).second) {
				LogWarning << "[coop] npc state for " << entry.first << ": " << (io ? "not an npc here" : "no such entity here");
			}
			continue;
		}
		if(target.fresh && io->show != SHOW_FLAG_IN_SCENE) {
			static std::set<std::string> reported; // TODO(dev) remove
			if(reported.insert(entry.first).second) {
				LogWarning << "[coop] npc " << entry.first << " has show " << int(io->show) << " here, host says " << int(target.show);
			}
		}
		if(target.fresh || arx::distance2(io->pos, target.pos) > square(SnapDistance)) {
			if(target.fresh && arx::distance2(io->pos, target.pos) <= square(SnapDistance)) {
				io->pos += (target.pos - io->pos) * std::min(1.f, dt * PositionSmoothing);
			} else {
				io->pos = target.pos;
			}
		} else {
			io->pos += (target.pos - io->pos) * std::min(1.f, dt * PositionSmoothing);
		}
		io->angle.setYaw(target.yaw);
		io->requestRoomUpdate = true;
		if(target.fresh) {
			target.fresh = false;
			io->_npcdata->lifePool.current = target.life;
			if(target.dead && io->mainevent != SM_DEAD) {
				io->mainevent = SM_DEAD;
			}
			if(io->show != EntityShowState(target.show)
			   && (target.show == SHOW_FLAG_IN_SCENE || target.show == SHOW_FLAG_HIDDEN)) {
				io->show = EntityShowState(target.show);
			}
			for(size_t i = 0; i < SyncedAnimLayers; i++) {
				applyAnim(io->animlayer[i], target.layers[i]);
			}
		}
	}
}

} // anonymous namespace

void puppetsInit() {
	g_coop.onPlayerState = handlePlayerState;
	g_coop.onPlayerEquipment = handlePlayerEquipment;
	g_coop.onSpellCast = handleSpellCast;
	g_coop.onNpcState = [](Reader & reader) {
		if(npcsAreMirrored()) {
			applyNpcState(reader);
		}
	};
	g_coop.onRevive = [](PlayerId from, Reader & reader) {
		if(g_coop.isHost()) {
			PlayerId target = reader.u8_();
			if(target == g_coop.localId()) {
				reviveLocalPlayer();
			} else {
				g_coop.sendTo(target, MessageType::Revive, Writer());
			}
			ARX_UNUSED(from);
		} else {
			reviveLocalPlayer();
		}
	};
}

bool localPlayerDowned() {
	return isLocalDowned();
}

void spellCast(unsigned spell, float level, unsigned flags, const Entity * target, long long durationUs) {
	if(!puppetsAllowed() || g_applyingRemoteSpell > 0 || (flags & SPELLCAST_FLAG_PRECAST)) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u32_(spell);
	writer.f32_(level);
	writer.u32_(flags);
	std::string targetId;
	if(target == entities.player()) {
		targetId = "player";
	} else if(target) {
		targetId = target->idString();
	}
	writer.string(targetId);
	writer.s64_(durationUs);
	g_coop.sendToOthers(MessageType::SpellCast, writer);
}

bool allPlayersDowned() {
	if(!isLocalDowned()) {
		return false;
	}
	for(const auto & entry : g_remote) {
		if(!entry.second.downed) {
			return false;
		}
	}
	return true;
}

bool npcsAreMirrored() {
	return g_coop.isClient() && g_coop.state() == State::InGame && isPlaythroughStarted();
}

void npcSyncUpdate() {
	if(npcsAreMirrored()) {
		npcMirrorFrame();
	} else if(!g_npcTargets.empty()) {
		g_npcTargets.clear();
	}
	npcSyncSend();
}

void npcSyncSend() {

	if(!g_coop.isHost() || g_coop.state() != State::InGame || g_coop.players().size() < 2) {
		return;
	}
	PlatformInstant now = platform::getTime();
	if(now - g_lastNpcSend < NpcSendInterval) {
		return;
	}
	g_lastNpcSend = now;

	Writer body;
	u16 count = 0;
	for(Entity & io : entities.inScene(IO_NPC)) {
		if(!isMirroredNpc(io)) {
			continue;
		}
		NpcSnapshot & snap = g_npcSent[io.idString()];
		bool keyframe = now - snap.sent > NpcKeyframeInterval;
		if(!keyframe && !npcChanged(io, snap)) {
			continue;
		}
		writeNpc(body, io, snap);
		snap.sent = now;
		if(++count >= 500) {
			break;
		}
	}
	if(count == 0) {
		return;
	}
	Writer message;
	message.u16_(count);
	message.bytes(body.data().data(), body.size());
	g_coop.broadcast(MessageType::NpcState, message);
	g_npcStatesSent += count;
	logNpcStats();

}

Vec3f nearestPlayerEyePos(const Vec3f & from) {
	Vec3f best = player.pos;
	if(!g_coop.isHost()) {
		return best;
	}
	float bestDist = arx::distance2(from, best);
	for(const auto & entry : g_remote) {
		const Entity * io = findPuppet(entry.first);
		if(!io || io->show != SHOW_FLAG_IN_SCENE || !entry.second.visible || entry.second.downed) {
			continue;
		}
		Vec3f eye = io->pos + player.baseOffset();
		float dist = arx::distance2(from, eye);
		if(dist < bestDist) {
			bestDist = dist;
			best = eye;
		}
	}
	return best;
}

PlayerId puppetOwner(const Entity & io) {
	if(!io.coopPuppet) {
		return InvalidPlayerId;
	}
	for(const auto & entry : g_remote) {
		if(puppetIdString(entry.first) == io.idString()) {
			return entry.first;
		}
	}
	return InvalidPlayerId;
}

void puppetsReset() {
	for(auto & entry : g_remote) {
		removePuppet(entry.first);
	}
	g_remote.clear();
	g_npcTargets.clear();
}

//! Puppets only make sense while actually playing (not in the menu's background level or the intro).
static bool puppetsAllowed() {
	return g_coop.isActive() && g_coop.state() == State::InGame && ARXmenu.mode() == Mode_InGame
	       && entities.player() && entities.player()->obj;
}

void puppetsSendLocalState() {

	if(!puppetsAllowed()) {
		return;
	}

	PlatformInstant now = platform::getTime();
	if(now - g_lastSend < SendInterval) {
		return;
	}
	g_lastSend = now;

	const Entity & io = *entities.player();

	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u32_(g_currentArea.handleData());
	writer.f32_(io.pos.x);
	writer.f32_(io.pos.y);
	writer.f32_(io.pos.z);
	writer.f32_(io.angle.getPitch());
	writer.f32_(io.angle.getYaw());
	writer.f32_(io.angle.getRoll());
	writer.bool_(io.show == SHOW_FLAG_IN_SCENE);
	writer.bool_(player.lifePool.current <= 0.f);
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		writeAnim(writer, io.animlayer[i]);
	}

	g_coop.sendToOthers(MessageType::PlayerState, writer);
	sendEquipmentIfNeeded(false);

}

void puppetsUpdate() {

	if(puppetsAllowed()) {
		reviveUpdate();
	}

	if(!puppetsAllowed()) {
		if(!g_remote.empty()) {
			puppetsReset();
		}
		return;
	}

	for(auto it = g_remote.begin(); it != g_remote.end(); ) {

		PlayerId id = it->first;
		const PlayerSnapshot & state = it->second;

		bool present = g_coop.player(id) != nullptr && id != g_coop.localId();
		if(!present) {
			removePuppet(id);
			it = g_remote.erase(it);
			continue;
		}

		if(state.area != g_currentArea.handleData()) {
			// The other player is in another level: nothing to show here
			removePuppet(id);
			++it;
			continue;
		}

		Entity * io = findPuppet(id);
		bool created = false;
		if(!io) {
			io = createPuppet(id, state);
			created = true;
			if(!io) {
				++it;
				continue;
			}
		}
		applySnapshot(*io, state, created);
		if(!state.equipmentApplied) {
			it->second.equipmentApplied = true;
			applyEquipment(*io, state.equipment);
		}

		++it;
	}

}

bool g_puppetsTestMode = false;

void puppetsTestUpdate() {

	if(!g_puppetsTestMode || !g_coop.isActive()) {
		return;
	}

	static bool creationSkipped = false;
	static bool creationSkippedDeathDone = false;
	static bool lootTaken = false;
	static bool equipDone = false;
	static bool spellDone = false;
	static bool equipChecked = false;
	static bool lootDropped = false;
	static bool playing = false;
	static PlatformInstant start;
	static int step = 0;
	PlatformInstant now = platform::getTime();

	// Lobby: the host starts as soon as a second player is there
	if(ARXmenu.mode() == Mode_MainMenu && g_coop.isHost() && g_coop.state() == State::Lobby
	   && g_coop.players().size() >= 2) {
		LogInfo << "[coop] test: starting the game";
		g_coop.startGame();
		return;
	}

	// Character creation: accept the average hero
	if(ARXmenu.mode() == Mode_CharacterCreation) {
		if(!creationSkipped) {
			LogInfo << "[coop] test: skipping character creation";
			ARX_PLAYER_MakeAverageHero();
			MenuFader_start(Fade_In, Mode_InGame);
			creationSkipped = true;
		}
		return;
	}

	// Only act once we are in the actual co-op playthrough (not the menu's background level)
	if(ARXmenu.mode() != Mode_InGame || !entities.player() || g_coop.state() != State::InGame || !creationSkipped) {
		return; // paused (menu, cinematic...): keep our place in the scenario
	}

	if(!playing) {
		playing = true;
		start = now;
		step = 0;
		player.playerflags |= PLAYERFLAGS_INVULNERABILITY; // the goblin must not end the test early
	}
	PlatformDuration elapsed = now - start;

	if(g_coop.isHost()) {
		player.playerflags |= PLAYERFLAGS_INVULNERABILITY; // scripts clear it after the intro
	}

	// Goblin tracking (NPC sync check)
	{
		static PlatformInstant lastGoblinLog;
		if(step >= 3 && now - lastGoblinLog > std::chrono::seconds(5)) {
			lastGoblinLog = now;
			if(const Entity * goblin = entities.getById("goblin_base_0006")) {
				LogInfo << "[coop] test: goblin pos " << int(goblin->pos.x) << "," << int(goblin->pos.y) << "," << int(goblin->pos.z)
				        << " life " << goblin->_npcdata->lifePool.current
				        << " anim0 " << (goblin->animlayer[0].cur_anim ? goblin->animlayer[0].cur_anim->path.filename() : "none")
				        << " playerlife " << player.lifePool.current;
				Logger::flush();
			}
		}
	}

	if(step == 0 && elapsed > std::chrono::seconds(30)) {
		// Clients step aside from the shared spawn point and face the host's puppet
		if(g_coop.isClient()) {
			Vec3f spawn = entities.player()->pos; // feet position, unlike player.pos (eyes)
			Vec3f side = angleToVectorXZ(player.angle.getYaw()) * 60.f;
			ARX_INTERACTIVE_Teleport(entities.player(), spawn + side);
			Vec3f dir = spawn - entities.player()->pos;
			float yaw = glm::degrees(std::atan2(-dir.x, dir.z)); // inverse of angleToVectorXZ()
			player.angle.setYaw(MAKEANGLE(yaw));
			player.desiredangle = player.angle;
			LogInfo << "[coop] test: stepped aside, facing " << yaw;
		}
		step = 1;
	} else if(step == 1 && elapsed > std::chrono::seconds(34)) {
		GetSnapShot();
		LogInfo << "[coop] test: snapshot taken, player at " << player.pos.x << "," << player.pos.y << "," << player.pos.z
		        << " yaw " << player.angle.getYaw();
		for(const auto & entry : g_remote) {
			const PlayerSnapshot & s = entry.second;
			LogInfo << "[coop] test: remote " << int(entry.first) << " area " << s.area << " pos " << s.pos.x << "," << s.pos.y << "," << s.pos.z
			        << " anim0 " << s.layers[0].path << " visible " << s.visible;
			if(const Entity * io = findPuppet(entry.first)) {
				LogInfo << "[coop] test: puppet " << io->idString() << " pos " << io->pos.x << "," << io->pos.y << "," << io->pos.z
				        << " show " << int(io->show) << " treat " << bool(io->gameFlags & GFLAG_ISINTREATZONE)
				        << " bbox2D " << io->bbox2D.min.x << "," << io->bbox2D.min.y << "-" << io->bbox2D.max.x << "," << io->bbox2D.max.y
				        << " anim0 " << (io->animlayer[0].cur_anim ? io->animlayer[0].cur_anim->path.string() : "none")
				        << " room " << io->room << " cyl " << io->physics.cyl.radius << "/" << io->physics.cyl.height;
			}
		}
		if(g_coop.isHost()) {
			// List the nearby interactive entities, handy to pick test targets
			for(const Entity & io : entities.inScene(IO_FIX | IO_NPC | IO_ITEM)) {
				float dist = glm::distance(io.pos, entities.player()->pos);
				if(dist < 900.f && &io != entities.player()) {
					LogInfo << "[coop] test: nearby " << io.idString() << " dist " << int(dist)
					        << (io.script.valid ? " script" : "") << " show " << int(io.show);
				}
			}
		}
		Logger::flush();
		step = 2;
	} else if(step == 2 && elapsed > std::chrono::seconds(40)) {
		// World sync check: the client pulls the lever next to the cell...
		if(g_coop.isClient()) {
			if(Entity * lever = entities.getById("lever_0011")) {
				LogInfo << "[coop] test: client pulls " << lever->idString();
				SendIOScriptEvent(entities.player(), lever, SM_ACTION);
			}
		}
		step = 3;
	} else if(step >= 3 && elapsed > std::chrono::seconds(52) && g_coop.isClient() && !lootTaken) {
		lootTaken = true;
		if(Entity * item = entities.getById("food_mushroom_0009")) {
			LogInfo << "[coop] test: client takes " << item->idString();
			giveToPlayer(item);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(100) && g_coop.isClient() && !equipDone) {
		equipDone = true;
		if(Entity * item = AddItem("graph/obj3d/interactive/items/armor/legging_leather/legging_leather", -1, IO_IMMEDIATELOAD)) {
			SendInitScriptEvent(item);
			giveToPlayer(item);
			ARX_EQUIPMENT_Equip(entities.player(), item);
			LogInfo << "[coop] test: client equips " << item->idString();
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(105) && g_coop.isClient() && !spellDone) {
		spellDone = true;
		LogInfo << "[coop] test: client casts magic missile";
		ARX_SPELLS_Launch(SPELL_MAGIC_MISSILE, *entities.player(), SPELLCAST_FLAG_NOCHECKCANCAST | SPELLCAST_FLAG_NOMANA,
		                  3, nullptr, GameDuration::ofRaw(-1));
	} else if(step >= 3 && elapsed > std::chrono::seconds(110) && !equipChecked) {
		equipChecked = true;
		LogInfo << "[coop] test: my player mesh tweaked=" << (entities.player()->tweaky != nullptr)
		        << " faces " << entities.player()->obj->facelist.size();
		for(const auto & entry : g_remote) {
			if(const Entity * io = findPuppet(entry.first)) {
				LogInfo << "[coop] test: puppet " << io->idString() << " tweaked=" << (io->tweaky != nullptr)
				        << " faces " << io->obj->facelist.size() << " weapon " << entry.second.equipment.weapon
				        << " leggings " << entry.second.equipment.leggings.file;
			}
		}
		Logger::flush();
	} else if(step >= 3 && elapsed > std::chrono::seconds(75) && g_coop.isClient() && !lootDropped) {
		lootDropped = true;
		if(Entity * item = entities.getById("food_mushroom_0009")) {
			LogInfo << "[coop] test: client drops " << item->idString();
			removeFromInventories(item);
			item->pos = entities.player()->pos + angleToVectorXZ(player.angle.getYaw()) * 60.f;
			item->show = SHOW_FLAG_IN_SCENE;
			coop::itemDropped(*item);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(60) && g_coop.isClient() && player.lifePool.current > 0.f
	          && !creationSkippedDeathDone) {
		creationSkippedDeathDone = true;
		LogInfo << "[coop] test: client goes down";
		player.playerflags &= ~PLAYERFLAGS_INVULNERABILITY;
		damagePlayer(9999.f, 0, nullptr);
	} else if(step >= 3 && elapsed > std::chrono::seconds(90) && g_coop.isHost() && !g_testHoldRevive) {
		for(const auto & entry : g_remote) {
			if(const Entity * io = findPuppet(entry.first)) {
				// Stand in front of the downed teammate, facing it, and hold the button
				Vec3f dir = glm::normalize(Vec3f(io->pos.x - entities.player()->pos.x, 0.f, io->pos.z - entities.player()->pos.z));
				ARX_INTERACTIVE_Teleport(entities.player(), io->pos - dir * 120.f);
				player.angle.setYaw(MAKEANGLE(glm::degrees(std::atan2(-dir.x, dir.z))));
				player.desiredangle = player.angle;
				g_testHoldRevive = true;
				LogInfo << "[coop] test: host holds the button on " << io->idString() << " (downed=" << entry.second.downed << ")";
			}
		}
	} else if(step == 3 && elapsed > std::chrono::seconds(g_coop.isHost() ? 115 : 46)) {
		// ... and everyone should see the portcullis open
		if(Entity * gate = entities.getById("porticullis_0013")) {
			LogInfo << "[coop] test: porticullis_0013 anim0="
			        << (gate->animlayer[0].cur_anim ? gate->animlayer[0].cur_anim->path.string() : "none")
			        << " open=" << GETVarValueLong(gate->m_variables, "§open")
			        << " collision=" << !(gate->ioflags & IO_NO_COLLISIONS);
		}
		if(Entity * lever = entities.getById("lever_0011")) {
			LogInfo << "[coop] test: lever_0011 position=" << GETVarValueLong(lever->m_variables, "§position")
			        << " anim0=" << (lever->animlayer[0].cur_anim ? lever->animlayer[0].cur_anim->path.string() : "none");
		}
		Logger::flush();
		step = 4;
	} else if(step == 4 && elapsed > std::chrono::seconds(122) && g_coop.isHost()) {
		LogInfo << "[coop] test: host quicksaves";
		GRenderer->getSnapshot(savegame_thumbnail, config.interface.thumbnailSize.x, config.interface.thumbnailSize.y);
		ARX_QuickSave();
		step = 5;
	} else if(step == 5 && elapsed > std::chrono::seconds(128) && g_coop.isHost()) {
		// Level change: validated once (clients load the host's new level); without a real
		// arrival marker it puts everyone outside the map, so it is not part of the routine test.
		step = 6;
	} else if(step == 6 && elapsed > std::chrono::seconds(150) && g_coop.isHost()) {
		for(const auto & entry : g_remote) {
			LogInfo << "[coop] test: remote " << int(entry.first) << " area " << entry.second.area
			        << " puppet " << (findPuppet(entry.first) ? "present" : "absent");
		}
		Logger::flush();
		step = 7;
	} else if(((step == 4 && !g_coop.isHost()) || step == 7) && elapsed > std::chrono::seconds(g_coop.isHost() ? 200 : 190)) {
		mainApp->quit();
		step = 8;
	}

}

void puppetsDrawNames() {

	if(!g_coop.isActive()) {
		return;
	}

	for(const auto & entry : g_remote) {
		const Entity * io = findPuppet(entry.first);
		if(!io || io->show != SHOW_FLAG_IN_SCENE || !(io->gameFlags & GFLAG_ISINTREATZONE)) {
			continue;
		}
		const Player * player = g_coop.player(entry.first);
		if(!player) {
			continue;
		}
		Vec3f pos = io->pos + Vec3f(0.f, io->physics.cyl.height - 20.f, 0.f);
		std::string label = player->name;
		if(entry.second.downed) {
			pos = io->pos + Vec3f(0.f, -60.f, 0.f);
			if(g_reviveTarget == entry.first && g_reviveProgress > 0.f) {
				label += " - r\xC3\xA9" "animation " + std::to_string(int(g_reviveProgress * 100.f)) + " %";
			} else {
				label += " - \xC3\xA0 terre (maintenir clic gauche)";
			}
		}
		drawTextAt(hFontInGame, pos, label, entry.second.downed ? Color(255, 90, 90) : Color(232, 204, 142));
	}

}

} // namespace coop
