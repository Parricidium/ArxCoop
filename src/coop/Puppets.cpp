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
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "animation/Animation.h"
#include "animation/Skeleton.h"
#include "coop/Admin.h"
#include "coop/Faces.h"
#include "coop/Protocol.h"
#include "coop/Qol.h"
#include "coop/Replication.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "coop/ThirdPerson.h"
#include "physics/Ragdoll.h"
#include "core/Application.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "game/Entity.h"
#include "game/EntityId.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Damage.h"
#include "game/npc/Dismemberment.h"
#include "game/Spells.h"
#include "game/magic/Spell.h"
#include "game/magic/spells/SpellsLvl06.h"
#include "util/Number.h"
#include "game/Equipment.h"
#include "graphics/data/MeshManipulation.h"
#include "scene/LinkedObject.h"
#include "game/Inventory.h"
#include "game/Player.h"
#include "graphics/data/TextureContainer.h"
#include "graphics/Math.h"
#include "gui/CinematicBorder.h"
#include "gui/Menu.h"
#include "gui/MenuPublic.h"
#include "gui/CinematicBorder.h"
#include "gui/Interface.h"
#include "graphics/DrawLine.h"
#include "graphics/Renderer.h"
#include "graphics/font/Font.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "gui/Speech.h"
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
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "scene/ChangeLevel.h"
#include "scene/Object.h"
#include "core/Config.h"
#include "graphics/Renderer.h"
#include "gui/MenuWidgets.h"
#include "physics/Physics.h"
#include "script/Script.h"
#include "script/ScriptEvent.h"

extern Entity * LASTSPAWNED;

namespace coop {

static bool puppetsAllowed();

namespace {

constexpr size_t SyncedAnimLayers = 4; //!< all of them: 2 = talking head, 3 = leaning
constexpr PlatformDuration SendInterval = std::chrono::milliseconds(50); // 20 Hz
constexpr AnimationDuration MaxAnimDrift = std::chrono::milliseconds(150);
constexpr float PositionSmoothing = 15.f; // higher = snappier
constexpr float TorchPitch = -90.0f; //!< hip torch orientation, see hipTorchRotation()
constexpr float TorchYaw = 25.0f;
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
	u8 skin = 0;
	bool combat = false;
	TweakInfo helmet, armor, leggings;
	std::string weapon, shield;
	std::string torch; //!< class of the lit torch / lamp, empty when none
	bool operator==(const EquipmentState & o) const {
		return skin == o.skin && combat == o.combat && helmet == o.helmet && armor == o.armor && leggings == o.leggings
		       && weapon == o.weapon && shield == o.shield && torch == o.torch;
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
	float life = 1.f;   // ratio
	float hunger = 1.f; // ratio (1 = full)
	bool inDialogue = false; //!< locked in a cinematic dialogue with an NPC
	float ignition = 0.f;    //!< on fire (the engine's Entity::ignition of the player)
	PlatformInstant received;
};

std::map<PlayerId, PlayerSnapshot> g_remote;
PlatformInstant g_lastSend;


Entity * findPuppet(PlayerId id) {
	return entities.getById(puppetIdString(id));
}

//! Same spine bending as the player's own mesh (see ARX_PLAYER_Manage_Visual), from the look pitch.
void applyLookPitch(Entity & io, float pitch, bool combat) {
	if(!io._npcdata) {
		return;
	}
	if(!io._npcdata->ex_rotate) {
		static const char * const groups[] = { "head", "neck", "chest", "belt", "left_shoulder", "right_shoulder" };
		auto * rotate = new EERIE_EXTRA_ROTATE();
		for(size_t i = 0; i < std::size(groups) && i < rotate->group_number.size(); i++) {
			rotate->group_number[i] = EERIE_OBJECT_GetGroup(io.obj, groups[i]);
		}
		for(Anglef & rotation : rotate->group_rotate) {
			rotation = Anglef();
		}
		io._npcdata->ex_rotate = rotate;
	}
	float v = pitch;
	if(v > 160.f) {
		v = -(360.f - v);
	}
	EERIE_EXTRA_ROTATE & rotate = *io._npcdata->ex_rotate;
	if(combat) {
		rotate.group_rotate[0] = Anglef(v * 0.1f, 0.f, 0.f); // Head
		rotate.group_rotate[1] = Anglef(v * 0.1f, 0.f, 0.f); // Neck
		rotate.group_rotate[2] = Anglef(v * 0.4f, 0.f, 0.f); // Chest
		rotate.group_rotate[3] = Anglef(v * 0.4f, 0.f, 0.f); // Belt
	} else {
		for(size_t i = 0; i < 4; i++) {
			rotate.group_rotate[i] = Anglef(v * 0.25f, 0.f, 0.f);
		}
	}
	rotate.group_rotate[4] = Anglef();
	rotate.group_rotate[5] = Anglef();
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
	// NPCs are rendered with a yaw of (180 - angle) while the player entity uses its angle directly.
	// The look pitch is not a body tilt: it bends the spine like ARX_PLAYER_Manage_Visual() does.
	io.angle = Anglef(0.f, MAKEANGLE(180.f - state.angle.getYaw()), state.angle.getRoll());
	applyLookPitch(io, state.angle.getPitch(), state.equipment.combat);
	io.requestRoomUpdate = true;
	io.show = state.visible ? SHOW_FLAG_IN_SCENE : SHOW_FLAG_HIDDEN;

	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		applyAnim(io.animlayer[i], state.layers[i]);
	}

	// Burning follows the player, not what set the puppet alight here (a puppet stayed on fire
	// forever once a flame had touched it - JD's friend, 16/09): the flames go out when its
	// player's do, and light and crackle with them
	if(state.ignition > 0.f) {
		io.ignition = state.ignition;
	} else if(io.ignition > 0.f) {
		io.ignition = 0.f;
		ManageIgnition_2(io); // releases the light and the sound (ManageIgnition() no longer runs for it)
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
	state.skin = player.skin;
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
	if(player.torch) {
		state.torch = player.torch->classPath().string();
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
	writer.u8_(state.skin);
	writer.bool_(state.combat);
	writeTweak(writer, state.helmet);
	writeTweak(writer, state.armor);
	writeTweak(writer, state.leggings);
	writer.string(state.weapon);
	writer.string(state.shield);
	writer.string(state.torch);
	g_coop.sendToOthers(MessageType::PlayerEquipment, writer);
}

void handlePlayerEquipment(PlayerId id, Reader & reader) {
	PlayerSnapshot & snap = g_remote[id];
	EquipmentState state;
	state.skin = reader.u8_();
	state.combat = reader.bool_();
	readTweak(reader, state.helmet);
	readTweak(reader, state.armor);
	readTweak(reader, state.leggings);
	state.weapon = reader.string();
	state.shield = reader.string();
	state.torch = reader.string();
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


/*!
 * The engine gives the player its chosen face by overwriting the shared "hero head" texture
 * data, so every human mesh using those textures (our puppets included) would show the local
 * player's face. Point the puppet's mesh at the texture files of its own player's skin instead.
 */
void applySkin(EERIE_3DOBJ & obj, u8 skin, PlayerId owner) {
	res::path replacements[4];
	ARX_PLAYER_SkinTextures(skin, replacements[0], replacements[1], replacements[2], replacements[3]);
	const char * const shared[4] = {
		"graph/obj3d/textures/npc_human_base_hero_head",
		"graph/obj3d/textures/npc_human_chainmail_hero_head",
		"graph/obj3d/textures/npc_human_chainmail_mithril_hero_head",
		"graph/obj3d/textures/npc_human_leather_hero_head",
	};
	for(TextureContainer * & tc : obj.materials) {
		if(!tc) {
			continue;
		}
		for(size_t i = 0; i < 4; i++) {
			if(replacements[i].empty() || tc->m_texName != res::path(shared[i])) {
				continue;
			}
			// A custom face composited over that skin, if the player chose one
			if(TextureContainer * face = playerFaceTexture(owner, i, skin)) {
				tc = face;
				break;
			}
			// A private copy of the skin's texture file, under a name the engine never overwrites
			res::path name = replacements[i].string() + "_coopskin";
			TextureContainer * variant = TextureContainer::Find(name);
			if(!variant) {
				variant = new TextureContainer(name, 0);
				if(!variant->LoadFile(replacements[i])) {
					LogWarning << "[coop] cannot load skin texture " << replacements[i];
					delete variant;
					variant = nullptr;
				}
			}
			if(variant) {
				tc = variant;
			}
			break;
		}
	}
}

//! Creates a display-only item linked to the puppet (destroyed with it or when replaced).
Entity * attachPuppetItem(Entity & puppet, const std::string & classPath, std::string_view puppetVertex,
                          std::string_view itemVertex, const glm::quat & rotation = quat_identity()) {
	if(classPath.empty()) {
		return nullptr;
	}
	Entity * item = AddItem(res::path::load(classPath), -1, IO_IMMEDIATELOAD | NO_ON_LOAD);
	if(!item || !item->obj) {
		return nullptr;
	}
	item->ioflags |= IO_NOSAVE | IO_NO_COLLISIONS;
	item->gameFlags &= ~GFLAG_INTERACTIVITY;
	item->coopPuppet = true; // never part of the shared world
	linkEntities(puppet, puppetVertex, *item, itemVertex, rotation);
	return item;
}

/*!
 * A lit torch (or lamp) hangs at the right hip, Elden Ring style: the human_base mesh has no
 * attach point there, so one is added on the origin vertex of the "right_hip" bone.
 */
constexpr const char * HipAttachName = "coop_hip_attach";

bool ensureHipAttach(EERIE_3DOBJ & obj) {
	if(getNamedVertex(&obj, HipAttachName)) {
		return true;
	}
	for(const VertexGroup & group : obj.grouplist) {
		if(group.name != "right_hip") {
			continue;
		}
		// The outer side of the thigh, level with the joint (the mesh's right is -x)
		const Vec3f & joint = obj.vertexlist[group.origin].v;
		VertexId best = group.origin;
		float bestX = joint.x;
		for(VertexId id : group.indexes) {
			const Vec3f & v = obj.vertexlist[id].v;
			if(glm::abs(v.y - joint.y) < 12.f && v.x < bestX) {
				bestX = v.x;
				best = id;
			}
		}
		EERIE_ACTIONLIST action;
		action.name = HipAttachName;
		action.idx = best;
		obj.actionlist.push_back(action);
		return true;
	}
	return false;
}

//! Orientation of the hanging torch in the thigh bone's frame (tuned by eye).
glm::quat hipTorchRotation() {
	return glm::angleAxis(glm::radians(TorchPitch), Vec3f(1.f, 0.f, 0.f))
	       * glm::angleAxis(glm::radians(TorchYaw), Vec3f(0.f, 1.f, 0.f));
}

/*!
 * Attach point on the torch itself: a third of the way up the handle, so the handle hangs
 * along the thigh and the flame ends up at the waist rather than at the shoulder.
 * Torch-like models extend along -z from their origin (the "fire" point is at the -z end).
 */
constexpr const char * TorchGripName = "coop_belt_grip";

void ensureTorchGrip(EERIE_3DOBJ & obj) {
	if(getNamedVertex(&obj, TorchGripName)) {
		return;
	}
	float minZ = 0.f;
	for(const EERIE_VERTEX & vertex : obj.vertexlist) {
		minZ = std::min(minZ, vertex.v.z);
	}
	float wantedZ = minZ * 0.85f;
	VertexId best;
	float bestDist = std::numeric_limits<float>::max();
	for(VertexId id : obj.vertexlist.handles()) {
		const Vec3f & v = obj.vertexlist[id].v;
		float dist = glm::abs(v.z - wantedZ) + glm::abs(v.x) * 0.25f + glm::abs(v.y) * 0.25f;
		if(dist < bestDist) {
			bestDist = dist;
			best = id;
		}
	}
	EERIE_ACTIONLIST action;
	action.name = TorchGripName;
	action.idx = best;
	obj.actionlist.push_back(action);
}

Entity * attachTorch(Entity & carrier, const std::string & classPath) {
	if(classPath.empty() || !carrier.obj || !ensureHipAttach(*carrier.obj)) {
		return nullptr;
	}
	Entity * item = AddItem(res::path::load(classPath), -1, IO_IMMEDIATELOAD | NO_ON_LOAD);
	if(!item || !item->obj) {
		return nullptr;
	}
	item->ioflags |= IO_NOSAVE | IO_NO_COLLISIONS;
	item->gameFlags &= ~GFLAG_INTERACTIVITY;
	item->coopPuppet = true;
	ensureTorchGrip(*item->obj);
	linkEntities(carrier, HipAttachName, *item, TorchGripName, hipTorchRotation());
	// Burning: flame particles, light and crackle come from the engine's ignition handling
	item->ignition = 25.f;
	item->durability = item->max_durability = 1e6f;
	return item;
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
	attachTorch(io, state.torch);
	applySkin(*io.obj, state.skin, puppetOwner(io));
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
	state.life = reader.f32_();
	state.hunger = reader.f32_();
	state.inDialogue = reader.remaining() ? reader.bool_() : false;
	state.ignition = reader.remaining() >= sizeof(float) ? reader.f32_() : 0.f;
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
	float pitch = reader.f32_();
	float yaw = reader.f32_();
	Entity * caster = findPuppet(from);
	if(!caster || !puppetsAllowed()) {
		return;
	}
	Entity * target = mapRemoteEntity(from, targetId);
	LogInfo << "[coop] player " << int(from) << " casts spell " << spell << " level " << level;
	// Aim exactly where the caster looked when casting, not where the smoothed puppet faces
	// now (the spells read the caster's yaw and, for puppets, the remote pitch)
	Anglef savedAngle = caster->angle;
	caster->angle.setYaw(MAKEANGLE(180.f - yaw));
	auto remote = g_remote.find(from);
	float savedPitch = 0.f;
	if(remote != g_remote.end()) {
		savedPitch = remote->second.angle.getPitch();
		remote->second.angle.setPitch(pitch);
	}
	g_applyingRemoteSpell++;
	ARX_SPELLS_Launch(SpellType(spell), *caster,
	                  SpellcastFlags::load(flags) | SPELLCAST_FLAG_NOCHECKCANCAST | SPELLCAST_FLAG_NOMANA
	                  | SPELLCAST_FLAG_NOANIM,
	                  long(level), target, GameDuration::ofRaw(duration));
	g_applyingRemoteSpell--;
	caster->angle = savedAngle;
	if(remote != g_remote.end()) {
		remote->second.angle.setPitch(savedPitch);
	}
}

void handlePlayerSpeech(PlayerId from, Reader & reader) {
	std::string sample = reader.string();
	Entity * puppet = findPuppet(from);
	if(!puppet || !puppetsAllowed() || sample.empty()) {
		return;
	}
	ARX_SOUND_PlaySpeech(res::path::load(sample), nullptr, puppet);
	LogInfo << "[coop] player " << int(from) << " says " << sample;
}

// NPC mirroring ------------------------------------------------------------------------

struct NpcSnapshot {
	Vec3f pos = Vec3f(0.f);
	float yaw = 0.f;
	float life = 0.f;
	u8 show = 0;
	bool dead = false;
	bool weaponInHand = false;
	u8 cuts = 0;
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
	writer.bool_(io._npcdata->weaponinhand == 1);
	writer.u8_(u8(DismembermentFlags::Type(io._npcdata->cuts)));
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		writeAnim(writer, io.animlayer[i]);
	}
	snap.pos = io.pos;
	snap.yaw = io.angle.getYaw();
	snap.life = io._npcdata->lifePool.current;
	snap.show = u8(io.show);
	snap.dead = io.mainevent == SM_DEAD;
	snap.weaponInHand = io._npcdata->weaponinhand == 1;
	snap.cuts = u8(DismembermentFlags::Type(io._npcdata->cuts));
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
	   || (io.mainevent == SM_DEAD) != snap.dead || (io._npcdata->weaponinhand == 1) != snap.weaponInHand
	   || u8(DismembermentFlags::Type(io._npcdata->cuts)) != snap.cuts) {
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
	bool weaponInHand;
	u8 cuts;
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
		target.weaponInHand = reader.bool_();
		target.cuts = reader.u8_();
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
				// The host ran the real death (script, loot...): here the corpse must just stop being
				// an enemy, so that looking at it shows the loot cursor and not the sword
				io->mainevent = SM_DEAD;
				resetNpcBehavior(*io);
				io->_npcdata->weaponinhand = 0;
				io->_npcdata->lifePool.current = 0.f;
				io->infracolor = Color3f::blue;
				for(size_t i = 1; i < MAX_ANIM_LAYERS; i++) {
					io->animlayer[i].cur_anim = nullptr;
				}
			}
			if(io->show != EntityShowState(target.show)
			   && (target.show == SHOW_FLAG_IN_SCENE || target.show == SHOW_FLAG_HIDDEN)) {
				io->show = EntityShowState(target.show);
			}
			// The host's AI draws / sheathes the weapon (relinks it to the hand or the back): mirror that
			if(!target.dead && io->_npcdata->weapon && (io->_npcdata->weaponinhand == 1) != target.weaponInHand) {
				io->_npcdata->weaponinhand = target.weaponInHand ? 1 : 0;
				if(target.weaponInHand) {
					SetWeapon_On(io);
				} else {
					SetWeapon_Back(io);
				}
			}
			for(size_t i = 0; i < SyncedAnimLayers; i++) {
				applyAnim(io->animlayer[i], target.layers[i]);
			}
			// Dismemberment happens on the host only (damage is forwarded there): mirror the fallen parts
			if(target.cuts != u8(DismembermentFlags::Type(io->_npcdata->cuts))) {
				ARX_NPC_ApplyRemoteCuts(*io, DismembermentFlags::load(DismembermentFlags::Type(target.cuts)));
			}
		}
	}
}

//! Blood of a hit landed on another machine: same effect, on our copy of the victim.
void handleBlood(PlayerId /* sender */, Reader & reader) {
	u32 area = reader.u32_();
	u8 kind = reader.u8_();
	Entity * target = nullptr;
	if(kind == 0) {
		target = entities.getById(reader.string());
	} else {
		PlayerId id = reader.u8_();
		target = (id == g_coop.localId()) ? entities.player() : findPuppet(id);
	}
	Vec3f pos = reader.vec3<Vec3f>();
	Vec3f sourcePos = reader.vec3<Vec3f>();
	float dmgs = reader.f32_();
	u8 r = reader.u8_(), g = reader.u8_(), b = reader.u8_();
	u8 effects = reader.u8_();
	if(!target || !target->obj || area != g_currentArea.handleData() || g_coop.state() != State::InGame) {
		return;
	}
	if(effects & 2) {
		ARX_EQUIPMENT_StrikeBlood(*target, pos, sourcePos, dmgs, Color(r, g, b), (effects & 1) != 0);
	}
	if(g_puppetsTestMode) {
		LogInfo << "[coop] test: blood on " << target->idString() << " effects " << int(effects);
	}
}

} // anonymous namespace

void bloodSpawned(const Entity & target, const Vec3f & pos, const Vec3f & sourcePos, float dmgs, Color color, u8 effects) {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || applyingRemote() || g_coop.players().size() < 2) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u32_(g_currentArea.handleData());
	if(&target == entities.player()) {
		writer.u8_(1);
		writer.u8_(g_coop.localId());
	} else if(PlayerId owner = puppetOwner(target); owner != InvalidPlayerId) {
		writer.u8_(1);
		writer.u8_(owner);
	} else {
		writer.u8_(0);
		writer.string(target.idString());
	}
	writer.f32_(pos.x);
	writer.f32_(pos.y);
	writer.f32_(pos.z);
	writer.f32_(sourcePos.x);
	writer.f32_(sourcePos.y);
	writer.f32_(sourcePos.z);
	writer.f32_(dmgs);
	writer.u8_(color.r);
	writer.u8_(color.g);
	writer.u8_(color.b);
	writer.u8_(effects);
	g_coop.sendToOthers(MessageType::Blood, writer);
}

void adminReviveLocal() {
	reviveLocalPlayer();
}

void puppetsInit() {
	g_coop.onBlood = handleBlood;
	g_coop.onPlayerState = handlePlayerState;
	g_coop.onPlayerEquipment = handlePlayerEquipment;
	g_coop.onPlayerFace = [](PlayerId id, Reader & reader) {
		handlePlayerFace(id, reader);
		if(auto it = g_remote.find(id); it != g_remote.end()) {
			it->second.equipmentApplied = false; // dress the puppet again with the new head
		}
	};
	g_coop.onSpellCast = handleSpellCast;
	g_coop.onPlayerSpeech = handlePlayerSpeech;
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
	return isLocalDowned() && !bledOut();
}

PlayerId teammateInDialogue() {
	for(const auto & entry : g_remote) {
		if(entry.second.inDialogue && entry.second.area == g_currentArea.handleData()) {
			return entry.first;
		}
	}
	return InvalidPlayerId;
}

bool dialogueHold() {
	return config.coop.dialogueHold && g_coop.isActive() && g_coop.state() == State::InGame
	       && !getCinematicSpeech() && teammateInDialogue() != InvalidPlayerId;
}

bool localPlayerDownedRaw() {
	return isLocalDowned();
}

PlayerId lookedAtDownedTeammate() {
	return lookedAtDownedPuppet();
}

void reviveTeammate(PlayerId target) {
	Writer writer;
	writer.u8_(target);
	if(g_coop.isHost()) {
		g_coop.sendTo(target, MessageType::Revive, Writer());
	} else {
		g_coop.sendToHost(MessageType::Revive, writer);
	}
}

std::vector<TeammateInfo> teammates() {
	std::vector<TeammateInfo> result;
	for(const Player & other : g_coop.players()) {
		if(other.id == g_coop.localId()) {
			continue;
		}
		auto it = g_remote.find(other.id);
		if(it == g_remote.end()) {
			continue;
		}
		const PlayerSnapshot & snap = it->second;
		TeammateInfo info;
		info.id = other.id;
		info.name = other.name;
		info.area = snap.area;
		info.pos = snap.pos;
		info.yaw = MAKEANGLE(180.f - snap.angle.getYaw());
		info.downed = snap.downed;
		info.here = snap.area == g_currentArea.handleData() && snap.visible;
		result.push_back(std::move(info));
	}
	return result;
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
	writer.f32_(player.angle.getPitch());
	writer.f32_(player.angle.getYaw());
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
	// A downed host is no target (like downed puppets below), unless nobody else stands
	float bestDist = isLocalDowned() ? std::numeric_limits<float>::max() : arx::distance2(from, best);
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

EntityHandle attackTarget(const Entity & npc, EntityHandle target) {
	if(!g_coop.isHost() || !entities.player() || target != entities.player()->index()) {
		return target;
	}
	EntityHandle best = target;
	float bestDist = isLocalDowned() ? std::numeric_limits<float>::max() : arx::distance2(npc.pos, entities.player()->pos);
	for(const auto & entry : g_remote) {
		const Entity * io = findPuppet(entry.first);
		if(!io || io->show != SHOW_FLAG_IN_SCENE || !entry.second.visible || entry.second.downed) {
			continue;
		}
		float dist = arx::distance2(npc.pos, io->pos);
		if(dist < bestDist) {
			bestDist = dist;
			best = io->index();
		}
	}
	return best;
}

std::string puppetIdString(PlayerId id) {
	return EntityId("coop_player", EntityInstance(id + 1)).string();
}

Entity * puppetOf(PlayerId id) {
	return findPuppet(id);
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

bool teammateWithin(const Vec3f & pos, float limit) {
	for(const auto & entry : g_remote) {
		if(entry.second.area == g_currentArea.handleData() && arx::distance2(entry.second.pos, pos) < limit * limit) {
			return true;
		}
	}
	return false;
}

bool puppetAimPitch(const Entity & caster, float & pitch) {
	PlayerId owner = puppetOwner(caster);
	if(owner == InvalidPlayerId) {
		return false;
	}
	auto it = g_remote.find(owner);
	if(it == g_remote.end()) {
		return false;
	}
	pitch = it->second.angle.getPitch();
	return true;
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
	// The host's world keeps running while it browses the menu: its puppets must too
	bool inGame = ARXmenu.mode() == Mode_InGame || (ARXmenu.mode() == Mode_MainMenu && g_coop.worldMustKeepRunning());
	return g_coop.isActive() && g_coop.state() == State::InGame && inGame
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
	writer.f32_(player.angle.getPitch()); // where the player looks; the entity angle itself has no pitch
	writer.f32_(io.angle.getYaw());
	writer.f32_(io.angle.getRoll());
	writer.bool_(io.show == SHOW_FLAG_IN_SCENE);
	writer.bool_(player.lifePool.current <= 0.f);
	for(size_t i = 0; i < SyncedAnimLayers; i++) {
		writeAnim(writer, io.animlayer[i]);
	}
	writer.f32_(player.lifePool.max > 0.f ? player.lifePool.current / player.lifePool.max : 0.f);
	writer.f32_(player.hunger * 0.01f);
	writer.bool_(getCinematicSpeech() != nullptr);
	writer.f32_(std::max(io.ignition, 0.f));

	g_coop.sendToOthers(MessageType::PlayerState, writer);
	sendEquipmentIfNeeded(false);

}

//! Diagnostic trail: logs the rare state changes that explain what a player sees during cutscenes.
void diagnosticsUpdate() {
	static bool blocked = false, cinema = false, dead = false;
	static std::string anim0, anim1;
	static Vec3f lastPos = Vec3f(0.f);
	static std::map<PlayerId, std::string> puppetAnim;
	static std::map<PlayerId, bool> puppetDowned;
	if(!entities.player()) {
		return;
	}
	const Entity & me = *entities.player();
	if(BLOCK_PLAYER_CONTROLS != blocked) {
		blocked = BLOCK_PLAYER_CONTROLS;
		LogInfo << "[coop] diag: player controls " << (blocked ? "BLOCKED" : "free");
	}
	if(cinematicBorder.isActive() != cinema) {
		cinema = cinematicBorder.isActive();
		LogInfo << "[coop] diag: cinemascope " << (cinema ? "on" : "off");
	}
	if((player.lifePool.current <= 0.f) != dead) {
		dead = player.lifePool.current <= 0.f;
		LogInfo << "[coop] diag: player life " << player.lifePool.current << (dead ? " (down)" : " (up)");
	}
	std::string a0 = me.animlayer[0].cur_anim ? std::string(me.animlayer[0].cur_anim->path.filename()) : "none";
	std::string a1 = me.animlayer[1].cur_anim ? std::string(me.animlayer[1].cur_anim->path.filename()) : "none";
	if(a0 != anim0 || a1 != anim1) {
		anim0 = a0;
		anim1 = a1;
		LogInfo << "[coop] diag: player anim0 " << a0 << " anim1 " << a1;
	}
	if(arx::distance2(me.pos, lastPos) > square(150.f)) {
		LogInfo << "[coop] diag: player jumped to " << int(me.pos.x) << "," << int(me.pos.y) << "," << int(me.pos.z);
	}
	lastPos = me.pos;
	for(const auto & entry : g_remote) {
		const Entity * puppet = findPuppet(entry.first);
		if(!puppet) {
			continue;
		}
		std::string pa = (puppet->animlayer[0].cur_anim ? std::string(puppet->animlayer[0].cur_anim->path.filename()) : "none")
		                 + " / " + (puppet->animlayer[1].cur_anim ? std::string(puppet->animlayer[1].cur_anim->path.filename()) : "none");
		if(puppetAnim[entry.first] != pa) {
			puppetAnim[entry.first] = pa;
			LogInfo << "[coop] diag: puppet " << int(entry.first) << " anim " << pa << " at "
			        << int(puppet->pos.x) << "," << int(puppet->pos.y) << "," << int(puppet->pos.z);
		}
		if(puppetDowned[entry.first] != entry.second.downed) {
			puppetDowned[entry.first] = entry.second.downed;
			LogInfo << "[coop] diag: puppet " << int(entry.first) << (entry.second.downed ? " DOWN" : " up");
		}
	}
	Logger::flush();
}

void puppetsUpdate() {

	if(puppetsAllowed()) {
		reviveUpdate();
		diagnosticsUpdate();
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
		if(created) {
			it->second.equipmentApplied = false; // a fresh mesh: dress it again
		}
		if(!state.equipmentApplied) {
			it->second.equipmentApplied = true;
			applyEquipment(*io, state.equipment);
		}

		++it;
	}

}

bool g_puppetsTestMode = false;
bool g_puppetsTestLean = false;
int g_puppetsTestLevel = -1;
std::string g_puppetsTestTarget;

//! Developer aid: runs one script line in an entity's context (the script stays alive for deferred parts).
//! The same container on every machine: the first (by id) non-NPC entity with an inventory.
Entity * testContainer() {
	Entity * best = nullptr;
	for(Entity & io : entities) {
		if(!io.inventory || (io.ioflags & IO_NPC) || &io == entities.player() || io.coopPuppet) {
			continue;
		}
		if(!best || io.idString() < best->idString()) {
			best = &io;
		}
	}
	return best;
}

void logContainer(const char * when, const Entity & container) {
	std::string contents;
	for(auto slot : container.inventory->slotsInOrder()) {
		if(slot.show && slot.entity) {
			contents += " " + slot.entity->idString() + " x" + std::to_string(slot.entity->_itemdata->count)
			            + (slot.entity->ioflags & IO_GOLD ? " (" + std::to_string(slot.entity->_itemdata->price) + " gold)" : "");
		}
	}
	LogInfo << "[coop] test: " << when << " " << container.idString() << " holds:" << (contents.empty() ? " nothing" : contents);
}

void runScriptLine(Entity & io, const std::string & line) {
	static std::vector<std::unique_ptr<EERIE_SCRIPT>> scripts;
	scripts.push_back(std::make_unique<EERIE_SCRIPT>());
	EERIE_SCRIPT & script = *scripts.back();
	script.valid = true;
	script.data = line + "\n";
	ScriptEvent::send(&script, nullptr, &io, SM_EXECUTELINE, ScriptParameters(), 0); // no sender: like a timer
}

void logCutsceneState(const char * when) {
	LogInfo << "[coop] test: cutscene " << when << " me at " << int(player.pos.x) << "," << int(player.pos.y) << "," << int(player.pos.z)
	        << " controls blocked " << BLOCK_PLAYER_CONTROLS << " cinemascope " << cinematicBorder.isActive()
	        << " anim0 " << (entities.player()->animlayer[0].cur_anim ? entities.player()->animlayer[0].cur_anim->path.filename() : "none")
	        << " anim1 " << (entities.player()->animlayer[1].cur_anim ? entities.player()->animlayer[1].cur_anim->path.filename() : "none")
	        << " anim2 " << (entities.player()->animlayer[2].cur_anim ? entities.player()->animlayer[2].cur_anim->path.filename() : "none")
	        << " anim3 " << (entities.player()->animlayer[3].cur_anim ? entities.player()->animlayer[3].cur_anim->path.filename() : "none");
	for(const auto & entry : g_remote) {
		if(const Entity * puppet = findPuppet(entry.first)) {
			LogInfo << "[coop] test: cutscene " << when << " puppet " << int(entry.first) << " at " << int(puppet->pos.x) << "," << int(puppet->pos.y) << "," << int(puppet->pos.z)
			        << " anim0 " << (puppet->animlayer[0].cur_anim ? puppet->animlayer[0].cur_anim->path.filename() : "none")
			        << " anim1 " << (puppet->animlayer[1].cur_anim ? puppet->animlayer[1].cur_anim->path.filename() : "none")
			        << " anim2 " << (puppet->animlayer[2].cur_anim ? puppet->animlayer[2].cur_anim->path.filename() : "none")
			        << " anim3 " << (puppet->animlayer[3].cur_anim ? puppet->animlayer[3].cur_anim->path.filename() : "none")
			        << " show " << int(puppet->show);
		}
	}
	Logger::flush();
}

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
	static bool chatDone = false;
	static bool doorFound = false;
	static bool doorActed = false;
	static bool doorChecked = false;
	static bool throwTaken = false;
	static bool throwDone = false;
	static bool throwChecked = false;
	static bool throwChecked2 = false;
	static bool hostThrowDone = false;
	static bool cutsceneArmed = false;
	static bool cutsceneChatDone = false;
	static bool cutsceneChatChecked = false;
	static int hostThrowChecks = 0;
	static bool tpDone = false;
	static bool killDone = false;
	static bool killChecked = false;
	static bool tpChecked = false;
	static bool tpViewDone = false;
	static bool cineStarted = false;
	static bool cineShot = false;
	static bool cineShot2 = false;
	static bool cineEnded = false;
	static bool cineClientStarted = false;
	static bool cineClientShot = false;
	static bool cineClientEnded = false;
	static bool adminDone = false;
	static bool adminShot = false;
	static bool adminClientDone = false;
	static bool pingDone = false;
	static bool giveDone = false;
	static bool voiceDone = false;
	static bool chestHostDone = false;
	static bool chestClientDone = false;
	static bool chestChecked = false;
	static std::string testDoor;
	static std::string testThrown;
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

	// Admin page opened by the scenario: capture it, then back to the game
	if(adminDone && !adminShot && ARXmenu.mode() == Mode_MainMenu && g_coop.state() == State::InGame) {
		static PlatformInstant opened;
		if(opened == PlatformInstant()) {
			opened = now;
		}
		if(now - opened > std::chrono::seconds(3)) {
			adminShot = true;
			if(!g_remote.empty()) {
				adminTeleportToMe(g_remote.begin()->first); // from the menu, like a real host would
				LogInfo << "[coop] test: admin from the menu: " << adminStatus();
			}
			GetSnapShot();
			LogInfo << "[coop] test: admin page snapshot taken";
			ARX_MENU_Clicked_QUIT();
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

	// --coop-fieldtest LEVEL[:marker]: the host jumps to that level, both sides then list the
	// magic fields they have (the persistent ones are cast by markers on game_ready, which the
	// clients never run: they must get them from the host) and quit
	if(g_puppetsTestLevel >= 0) {
		static bool jumped = false;
		static bool listed = false;
		static PlatformInstant arrived;
		if(g_coop.isHost() && !jumped && elapsed > std::chrono::seconds(10)) {
			jumped = true;
			std::string line = "teleport -ln " + std::to_string(g_puppetsTestLevel) + " " + g_puppetsTestTarget;
			LogInfo << "[coop] test: host runs '" << line << "'";
			Logger::flush();
			runScriptLine(*entities.player(), line);
		}
		if(g_currentArea == AreaId(u32(g_puppetsTestLevel)) && arrived == PlatformInstant()) {
			arrived = now;
			LogInfo << "[coop] test: arrived in level " << g_puppetsTestLevel;
		}
		if(arrived != PlatformInstant() && !listed && now - arrived > std::chrono::seconds(25)) {
			listed = true;
			for(const Spell & spell : spells.ofType(SPELL_CREATE_FIELD)) {
				const Entity * caster = entities.get(spell.m_caster);
				const Entity * field = entities.get(static_cast<const CreateFieldSpell &>(spell).m_entity);
				Vec3f pos = static_cast<const CreateFieldSpell &>(spell).getPosition();
				LogInfo << "[coop] test: field spell of " << (caster ? caster->idString() : std::string("?"))
				        << " level " << spell.m_level << " entity " << (field ? field->idString() : std::string("none"))
				        << " at " << int(pos.x) << "," << int(pos.y) << "," << int(pos.z);
			}
			size_t fields = 0;
			for(const Entity & entity : entities) {
				if(entity.ioflags & IO_FIELD) {
					fields++;
					LogInfo << "[coop] test: field entity " << entity.idString() << " at " << int(entity.pos.x) << ","
					        << int(entity.pos.y) << "," << int(entity.pos.z) << " show " << int(entity.show)
					        << " flags " << (entity.ioflags & IO_NOSAVE ? " nosave" : " saved");
				}
			}
			for(const Spell & spell : spells) {
				const Entity * caster = entities.get(spell.m_caster);
				LogInfo << "[coop] test: spell " << spell.m_type << " of " << (caster ? caster->idString() : std::string("?"));
			}
			LogInfo << "[coop] test: magic fields here: " << fields << ", me at " << int(entities.player()->pos.x)
			        << "," << int(entities.player()->pos.y) << "," << int(entities.player()->pos.z);
			Logger::flush();
			GetSnapShot();
		}
		// Skills and burning of a client, as seen by the host
		static bool skillSet = false;
		static bool skillChecked = false;
		static bool fireSet = false;
		static int fireChecks = 0;
		if(arrived != PlatformInstant() && g_coop.isClient() && !skillSet && now - arrived > std::chrono::seconds(5)) {
			skillSet = true;
			player.m_skill.mecanism = 77.f;
			entities.player()->ignition = 100.f;
			LogInfo << "[coop] test: client sets mecanism 77 and catches fire";
		}
		if(arrived != PlatformInstant() && g_coop.isHost() && now - arrived > std::chrono::seconds(fireChecks == 0 ? 10 : 22)
		   && fireChecks < 2) {
			fireChecks++;
			for(const auto & entry : g_remote) {
				if(const Entity * io = findPuppet(entry.first)) {
					LogInfo << "[coop] test: puppet " << io->idString() << " ignition " << io->ignition
					        << " (player state says " << entry.second.ignition << ")";
				}
			}
			Logger::flush();
		}
		if(arrived != PlatformInstant() && g_coop.isHost() && !skillChecked && now - arrived > std::chrono::seconds(12)) {
			skillChecked = true;
			Entity * marker = nullptr;
			for(Entity & entity : entities) {
				if(entity.classPath().string().find("system/marker") != std::string::npos) {
					marker = &entity;
					break;
				}
			}
			if(marker && !g_remote.empty()) {
				PlayerId client = g_remote.begin()->first;
				runScriptLine(*marker, "set @tskill ~^player_skill_mecanism~");
				float mine = GETVarValueFloat(marker->m_variables, "@tskill");
				{
					PlayerActorScope actor(client);
					runScriptLine(*marker, "set @tskill ~^player_skill_mecanism~");
				}
				float theirs = GETVarValueFloat(marker->m_variables, "@tskill");
				LogInfo << "[coop] test: ^player_skill_mecanism for me " << mine << " (" << player.m_skillFull.mecanism
				        << "), acting for player " << int(client) << " " << theirs;
				Logger::flush();
				{
					// A trap's spell aimed at "the player" who set it off (the client here)
					PlayerActorScope actor(client);
					runScriptLine(*marker, "spellcast -smf 3 magic_missile player");
					LogInfo << "[coop] test: " << marker->idString() << " shoots a magic missile at player " << int(client);
				}
			}
		}
		// A client picks a lock and brews a potion (its items are stand-ins on the host)
		static int craftStep = 0;
		if(arrived != PlatformInstant() && g_coop.isClient()) {
			Entity * chest = entities.getById("chest_metal_0097");
			Entity * apparatus = entities.getById("apparatus_0001");
			if(craftStep == 0 && now - arrived > std::chrono::seconds(14)) {
				craftStep = 1;
				player.m_skill.mecanism = 5.f;         // too clumsy for that chest (40): "impossible", the tools wear
				player.m_skill.objectKnowledge = 80.f; // good enough for a mana potion (59)
				if(Entity * tools = AddItem("graph/obj3d/interactive/items/provisions/lockpicks/lockpicks", -1, IO_IMMEDIATELOAD)) {
					SendInitScriptEvent(tools);
					giveToPlayer(tools);
				}
				if(Entity * ingredient = AddItem("graph/obj3d/interactive/items/magic/potion2beblue/potion2beblue", -1, IO_IMMEDIATELOAD)) {
					SendInitScriptEvent(ingredient);
					giveToPlayer(ingredient);
				}
				if(chest) {
					ARX_INTERACTIVE_Teleport(entities.player(), chest->pos + Vec3f(0.f, -50.f, -120.f));
					LogInfo << "[coop] test: client at the chest " << chest->idString() << " unlock " << GETVarValueLong(chest->m_variables, "§unlock")
					        << " pickability " << GETVarValueLong(chest->m_variables, "§lockpickability");
				} else {
					LogInfo << "[coop] test: no chest_metal_0097 in this level";
				}
			} else if(craftStep == 1 && now - arrived > std::chrono::seconds(16)) {
				craftStep = 2;
				for(Entity & entity : entities) {
					if(chest && entity.className() == "lockpicks" && IsInPlayerInventory(&entity)) {
						LogInfo << "[coop] test: client combines " << entity.idString() << " with the chest";
						SendIOScriptEvent(&entity, chest, SM_COMBINE, ScriptParameters(entity.idString()));
						break;
					}
				}
			} else if(craftStep == 2 && now - arrived > std::chrono::seconds(21)) {
				craftStep = 3;
				if(apparatus) {
					ARX_INTERACTIVE_Teleport(entities.player(), apparatus->pos + Vec3f(0.f, -50.f, -120.f));
					for(Entity & entity : entities) {
						if(entity.className() == "potion2beblue" && IsInPlayerInventory(&entity)) {
							LogInfo << "[coop] test: client combines " << entity.idString() << " with " << apparatus->idString();
							SendIOScriptEvent(&entity, apparatus, SM_COMBINE, ScriptParameters(entity.idString()));
							break;
						}
					}
				} else {
					LogInfo << "[coop] test: no apparatus_0001 in this level";
				}
			} else if(craftStep == 3 && now - arrived > std::chrono::seconds(28)) {
				craftStep = 4;
				for(const Entity & entity : entities) {
					if((entity.ioflags & IO_ITEM) && IsInPlayerInventory(const_cast<Entity *>(&entity))) {
						LogInfo << "[coop] test: client has " << entity.idString() << " x" << entity._itemdata->count
						        << " durability " << entity.durability << " interactive " << bool(entity.gameFlags & GFLAG_INTERACTIVITY)
						        << " objectlife " << GETVarValueLong(entity.m_variables, "§objectlife");
					}
				}
				if(chest) {
					LogInfo << "[coop] test: chest unlock " << GETVarValueLong(chest->m_variables, "§unlock");
				}
				Logger::flush();
			}
		}
		if(listed && now - arrived > std::chrono::seconds(g_coop.isHost() ? 40 : 32)) {
			mainApp->quit();
		}
		return;
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
		if(g_coop.isHost()) {
			// A persistent magic field like the level markers cast on game_ready (sync check)
			Entity * marker = nullptr;
			float best = 0.f;
			for(Entity & entity : entities) {
				float dist = arx::distance2(entity.pos, player.pos);
				if(entity.classPath().string().find("system/marker") != std::string::npos && dist > best && dist < square(1500.f)) {
					marker = &entity;
					best = dist;
				}
			}
			if(!marker) {
				size_t markers = 0;
				std::string sample;
				for(const Entity & entity : entities) {
					if(entity.idString().find("marker") != std::string::npos) {
						markers++;
						if(sample.empty()) {
							sample = entity.idString() + " class " + entity.classPath().string() + " dist "
							         + std::to_string(int(fdist(entity.pos, player.pos)));
						}
					}
				}
				LogInfo << "[coop] test: no marker in range, " << markers << " markers, e.g. " << sample;
				Logger::flush();
			}
			if(marker) {
				TryToCastSpell(marker, SPELL_CREATE_FIELD, 6, marker->index(),
				               SPELLCAST_FLAG_NOMANA | SPELLCAST_FLAG_NOCHECKCANCAST, std::chrono::milliseconds(99999999));
				LogInfo << "[coop] test: field cast on " << marker->idString() << " at " << int(std::sqrt(best)) << " units";
				Logger::flush();
			}
		}
		// Clients step aside from the shared spawn point and face the host's puppet, torch lit
		if(g_coop.isClient()) {
			if(Entity * torch = AddItem("graph/obj3d/interactive/items/provisions/torch/torch", -1, IO_IMMEDIATELOAD)) {
				ARX_PLAYER_ClickedOnTorch(torch);
				torch->show = SHOW_FLAG_ON_PLAYER; // as if taken from the inventory
				LogInfo << "[coop] test: client lit a torch";
			}
			Vec3f spawn = entities.player()->pos; // feet position, unlike player.pos (eyes)
			Vec3f side = angleToVectorXZ(player.angle.getYaw()) * 60.f;
			ARX_INTERACTIVE_Teleport(entities.player(), spawn + side);
			Vec3f dir = spawn - entities.player()->pos;
			float yaw = glm::degrees(std::atan2(-dir.x, dir.z)); // inverse of angleToVectorXZ()
			player.angle.setYaw(MAKEANGLE(yaw));
			player.angle.setPitch(45.f); // looks down for the host's snapshot (spine bending sync)
			player.desiredangle = player.angle;
			LogInfo << "[coop] test: stepped aside, facing " << yaw;
		}
		step = 1;
	} else if(step == 1 && elapsed > std::chrono::seconds(34)) {
		GetSnapShot();
		LogInfo << "[coop] test: snapshot taken, player at " << player.pos.x << "," << player.pos.y << "," << player.pos.z
		        << " yaw " << player.angle.getYaw();
		{
			size_t fields = 0;
			for(const Entity & entity : entities) {
				if(entity.ioflags & IO_FIELD) {
					fields++;
				}
			}
			LogInfo << "[coop] test: magic fields here: " << fields;
		}
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
	} else if(step == 2 && elapsed > std::chrono::seconds(34) && g_coop.isClient() && !pingDone) {
		pingDone = true;
		qolTestPing();
		LogInfo << "[coop] test: client pinged";
	} else if(step >= 3 && elapsed > std::chrono::seconds(80) && g_coop.isClient() && !giveDone) {
		giveDone = true;
		if(Entity * item = AddItem("graph/obj3d/interactive/items/provisions/mushroom/food_mushroom", -1, IO_IMMEDIATELOAD)) {
			giveToPlayer(item);
			for(const auto & entry : g_remote) {
				if(Entity * puppet = findPuppet(entry.first)) {
					LogInfo << "[coop] test: client gives a mushroom to player " << int(entry.first);
					giveItemToPuppet(*item, *puppet);
					break;
				}
			}
		}
	} else if(step == 2 && elapsed > std::chrono::seconds(35) && !tpViewDone) {
		// Third person views: over the right shoulder, orbit turned 70 degrees, left shoulder
		static int view = 0;
		if(view == 0 && elapsed > std::chrono::seconds(35)) {
			player.angle.setPitch(0.f);
			player.desiredangle = player.angle;
			thirdPersonTestSet(true, false, true, 0.f);
			view = 1;
		} else if(view == 1 && elapsed > std::chrono::seconds(36)) {
			GetSnapShot();
			thirdPersonTestSet(true, true, true, 70.f);
			view = 2;
		} else if(view == 2 && elapsed > std::chrono::seconds(37)) {
			GetSnapShot();
			thirdPersonTestSet(true, false, false, 0.f);
			view = 3;
		} else if(view == 3 && elapsed > std::chrono::seconds(38)) {
			GetSnapShot();
			thirdPersonTestSet(false, false, true, 0.f);
			LogInfo << "[coop] test: third person snapshots taken";
			tpViewDone = true;
		}
	} else if(step == 2 && elapsed > std::chrono::seconds(40)) {
		// World sync check: the client pulls the lever next to the cell...
		if(g_coop.isClient()) {
			if(Entity * lever = entities.getById("lever_0011")) {
				LogInfo << "[coop] test: client pulls " << lever->idString();
				SendIOScriptEvent(entities.player(), lever, SM_ACTION);
			}
		}
		step = 3;
	} else if(step >= 3 && elapsed > std::chrono::seconds(43) && g_coop.isHost() && !cineStarted) {
		cineStarted = true;
		// Scripted cutscene staging like Atok's (goblin_base_0017): what do the clients see?
		if(Entity * goblin = entities.getById("goblin_base_0006")) {
			std::string marker;
			float best = 1500.f;
			for(Entity & io : entities) {
				if(io.idString().compare(0, 7, "marker_") == 0) {
					float d = glm::distance(io.pos, entities.player()->pos);
					if(d > 200.f && d < best) {
						best = d;
						marker = io.idString();
					}
				}
			}
			LogInfo << "[coop] test: host stages a cutscene on " << goblin->idString() << " at " << marker;
			runScriptLine(*goblin, "set_player_controls off");
			if(!marker.empty()) {
				runScriptLine(*goblin, "teleport -p " + marker);
			}
			runScriptLine(*goblin, "cinemascope -s on");
			runScriptLine(*goblin, "playerinterface hide");
			runScriptLine(*goblin, "loadanim -p action1 \"human_normal_sit_cycle\"");
			runScriptLine(*goblin, "playanim -pl action1"); // Polsius' tavern scene: the host sits
			runScriptLine(*goblin, "speak -o [atok_greetings] nop");
			runScriptLine(*goblin, "speak -cp zoom 0 90 0 90 100 100 [player_atok_introduce] nop");
		}
	}
	g_puppetsTestLean = step >= 3 && g_coop.isClient() && elapsed > std::chrono::seconds(44) && elapsed < std::chrono::seconds(48);
	if(false) {
	} else if(step >= 3 && elapsed > std::chrono::seconds(46) && !cineShot) {
		cineShot = true;
		GetSnapShot();
		logCutsceneState("46s");
	} else if(step >= 3 && elapsed > std::chrono::seconds(49) && !cineShot2) {
		cineShot2 = true;
		GetSnapShot();
		logCutsceneState("49s");
	} else if(step >= 3 && elapsed > std::chrono::seconds(51) && g_coop.isHost() && !cineEnded) {
		cineEnded = true;
		if(Entity * goblin = entities.getById("goblin_base_0006")) {
			runScriptLine(*goblin, "speak killall");
			runScriptLine(*goblin, "playanim -p wait");
			runScriptLine(*goblin, "set_player_controls on");
			runScriptLine(*goblin, "cinemascope -s off");
			runScriptLine(*goblin, "playerinterface show");
			LogInfo << "[coop] test: host ends the cutscene";
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(53) && g_coop.isHost() && !cineClientStarted) {
		cineClientStarted = true;
		// The same scene triggered by the client: only the client must sit
		Entity * goblin = entities.getById("goblin_base_0006");
		Entity * puppet = g_remote.empty() ? nullptr : findPuppet(g_remote.begin()->first);
		if(goblin && puppet) {
			PuppetActorScope actor(puppet);
			LogInfo << "[coop] test: cutscene for the client (player " << int(g_remote.begin()->first) << ")";
			runScriptLine(*goblin, "loadanim -p action1 \"human_normal_sit_cycle\"");
			runScriptLine(*goblin, "playanim -pl action1");
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(56) && !cineClientShot) {
		cineClientShot = true;
		logCutsceneState("56s");
	} else if(step >= 3 && elapsed > std::chrono::seconds(58) && g_coop.isHost() && !cineClientEnded) {
		cineClientEnded = true;
		Entity * goblin = entities.getById("goblin_base_0006");
		Entity * puppet = g_remote.empty() ? nullptr : findPuppet(g_remote.begin()->first);
		if(goblin && puppet) {
			PuppetActorScope actor(puppet);
			runScriptLine(*goblin, "playanim -p wait");
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(52) && g_coop.isClient() && !lootTaken) {
		lootTaken = true;
		if(Entity * item = entities.getById("food_mushroom_0009")) {
			LogInfo << "[coop] test: client takes " << item->idString();
			giveToPlayer(item);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(64) && g_coop.isClient() && !voiceDone) {
		voiceDone = true;
		LogInfo << "[coop] test: client's hero speaks";
		ARX_SPEECH_AddSpeech(*entities.player(), "player_jump", ANIM_TALK_NEUTRAL, ARX_SPEECH_FLAG_NOTEXT);
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
	} else if(step >= 3 && elapsed > std::chrono::seconds(56) && g_coop.isClient() && !chatDone) {
		chatDone = true;
		if(Entity * goblin = entities.getById("goblin_base_0006")) {
			LogInfo << "[coop] test: client chats with " << goblin->idString();
			SendIOScriptEvent(entities.player(), goblin, SM_CHAT);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(57) && !doorFound) {
		doorFound = true;
		// Everyone stands in front of the same door (the first scripted door of the level)
		for(const Entity & io : entities(IO_FIX)) {
			if(io.idString().find("door") == std::string::npos) {
				continue;
			}
			LogInfo << "[coop] test: level door " << io.idString() << (io.script.valid ? " script" : "")
			        << " anims " << (io.anims[ANIM_ACTION] != nullptr) << (io.anims[ANIM_ACTION2] != nullptr)
			        << " dist " << int(glm::distance(io.pos, entities.player()->pos));
			if(io.script.valid && io.anims[ANIM_ACTION2] && (testDoor.empty() || io.idString() < testDoor)) {
				testDoor = io.idString();
			}
		}
		Logger::flush();
		if(Entity * door = entities.getById(testDoor)) {
			Vec3f spot = door->pos + angleToVectorXZ(door->angle.getYaw()) * 150.f;
			if(g_coop.isClient()) {
				spot += Vec3f(60.f, 0.f, 0.f);
			}
			ARX_INTERACTIVE_Teleport(entities.player(), spot);
			LogInfo << "[coop] test: standing at door " << testDoor << " pos " << int(door->pos.x) << "," << int(door->pos.y) << "," << int(door->pos.z)
			        << " open=" << GETVarValueLong(door->m_variables, "\xA7open") << " unlock=" << GETVarValueLong(door->m_variables, "\xA7unlock");
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(62) && g_coop.isClient() && !doorActed) {
		doorActed = true;
		if(Entity * door = entities.getById(testDoor)) {
			LogInfo << "[coop] test: client uses " << testDoor;
			SendIOScriptEvent(entities.player(), door, SM_ACTION);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(66) && !doorChecked) {
		doorChecked = true;
		if(Entity * door = entities.getById(testDoor)) {
			LogInfo << "[coop] test: door " << testDoor << " anim0="
			        << (door->animlayer[0].cur_anim ? door->animlayer[0].cur_anim->path.string() : "none")
			        << " open=" << GETVarValueLong(door->m_variables, "\xA7open")
			        << " collision=" << !(door->ioflags & IO_NO_COLLISIONS);
		}
		Logger::flush();
	} else if(step >= 3 && elapsed > std::chrono::seconds(70) && g_coop.isHost() && !killDone) {
		killDone = true;
		if(Entity * goblin = entities.getById("goblin_base_0006")) {
			LogInfo << "[coop] test: host kills " << goblin->idString();
			ARX_DAMAGES_ForceDeath(*goblin, entities.player());
			// ... and cuts its head off, aiming at the head selection itself (dismemberment sync check)
			for(VertexSelectionId selection : goblin->obj->selections.handles()) {
				const EERIE_SELECTIONS & sel = goblin->obj->selections[selection];
				if(sel.name == "cut_head" && !sel.selected.empty()) {
					Vec3f pos = goblin->obj->vertexWorldPositions[sel.selected[0]].v;
					ARX_NPC_TryToCutSomething(goblin, &pos);
				}
			}
			// ... which bleeds for everyone (blood sync check)
			ARX_EQUIPMENT_StrikeBlood(*goblin, goblin->pos, entities.player()->pos, 20.f, Color::red, true);
			bloodSpawned(*goblin, goblin->pos, entities.player()->pos, 20.f, Color::red, 3);
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(74) && !killChecked) {
		killChecked = true;
		if(Entity * goblin = entities.getById("goblin_base_0006")) {
			LogInfo << "[coop] test: goblin dead=" << (goblin->mainevent == SM_DEAD) << " life " << goblin->_npcdata->lifePool.current
			        << " enemy=" << isEnemy(goblin) << " behavior " << goblin->_npcdata->behavior
			        << " cuts " << int(DismembermentFlags::Type(goblin->_npcdata->cuts))
			        << " ragdoll " << physics::hasRagdoll(*goblin) << " pos " << int(goblin->pos.x) << "," << int(goblin->pos.y) << "," << int(goblin->pos.z)
			        << " head " << int(goblin->obj->vertexWorldPositions[goblin->obj->fastaccess.head_group_origin].v.y);
		}
		Logger::flush();
	} else if(step >= 3 && elapsed > std::chrono::seconds(78) && g_coop.isClient() && !throwTaken) {
		throwTaken = true;
		// A brand new inventory item of ours (client id range), like a bottle bought in town
		if(Entity * item = AddItem("graph/obj3d/interactive/items/provisions/mushroom/food_mushroom", -1, IO_IMMEDIATELOAD)) {
			SendInitScriptEvent(item);
			giveToPlayer(item);
			testThrown = item->idString();
			LogInfo << "[coop] test: client got " << testThrown << " to throw it";
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(80) && g_coop.isClient() && !throwDone) {
		throwDone = true;
		if(Entity * item = entities.getById(testThrown)) {
			removeFromInventories(item);
			item->pos = entities.player()->pos + Vec3f(0.f, -100.f, 0.f);
			item->show = SHOW_FLAG_ON_PLAYER;
			coop::itemDragged(*item); // like a real drag: carried (hidden) first
			item->show = SHOW_FLAG_IN_SCENE;
			Vec3f direction = glm::normalize(angleToVectorXZ(player.angle.getYaw()) + Vec3f(0.f, -0.3f, 0.f));
			EERIE_PHYSICS_BOX_Launch(item->obj, item->pos, item->angle, direction);
			coop::itemDropped(*item, true, direction);
			LogInfo << "[coop] test: client throws " << item->idString();
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(82) && (!throwChecked || (elapsed > std::chrono::seconds(88) && !throwChecked2))) {
		if(throwChecked) {
			throwChecked2 = true;
		}
		throwChecked = true;
		if(Entity * item = entities.getById(testThrown.empty() ? std::string("food_mushroom_10001") : testThrown)) {
			LogInfo << "[coop] test: thrown " << item->idString() << " pos " << int(item->pos.x) << "," << int(item->pos.y) << "," << int(item->pos.z)
			        << " show " << int(item->show) << " pbox " << (item->obj && item->obj->pbox ? int(item->obj->pbox->active) : -1)
			        << " treat " << bool(item->gameFlags & GFLAG_ISINTREATZONE) << " nocomp " << bool(item->gameFlags & GFLAG_NOCOMPUTATION);
		}
		Logger::flush();
	} else if(step >= 3 && elapsed > std::chrono::seconds(84) && g_coop.isHost() && !hostThrowDone) {
		hostThrowDone = true;
		// The host throws one of its own inventory items too
		if(Entity * item = AddItem("graph/obj3d/interactive/items/provisions/mushroom/food_mushroom", -1, IO_IMMEDIATELOAD)) {
			SendInitScriptEvent(item);
			giveToPlayer(item);
			removeFromInventories(item);
			item->pos = entities.player()->pos + Vec3f(0.f, -100.f, 0.f);
			item->show = SHOW_FLAG_ON_PLAYER;
			coop::itemDragged(*item);
			item->show = SHOW_FLAG_IN_SCENE;
			Vec3f direction = glm::normalize(angleToVectorXZ(player.angle.getYaw()) + Vec3f(0.f, -0.3f, 0.f));
			EERIE_PHYSICS_BOX_Launch(item->obj, item->pos, item->angle, direction);
			coop::itemDropped(*item, true, direction);
			LogInfo << "[coop] test: host throws " << item->idString();
		}
	} else if(step >= 3 && elapsed > std::chrono::seconds(86 + 3 * hostThrowChecks) && hostThrowChecks < 3) {
		hostThrowChecks++;
		for(const Entity & io : entities.inScene(IO_ITEM)) {
			if(io.idString().compare(0, 14, "food_mushroom_") == 0 && io.obj && io.obj->pbox && io.idString() != "food_mushroom_0009") {
				LogInfo << "[coop] test: mushroom " << io.idString() << " pos " << int(io.pos.x) << "," << int(io.pos.y) << "," << int(io.pos.z)
				        << " pbox " << int(io.obj->pbox->active) << " treat " << bool(io.gameFlags & GFLAG_ISINTREATZONE);
			}
		}
		Logger::flush();
	} else if(step >= 4 && elapsed > std::chrono::seconds(118) && g_coop.isHost() && !tpDone) {
		tpDone = true;
		LogInfo << "[coop] test: host types 'tp p2'";
		Logger::flush();
		coop::consoleCommand("tp p2");
		Logger::flush();
	} else if(step >= 4 && elapsed > std::chrono::seconds(121) && g_coop.isHost() && !cutsceneArmed) {
		cutsceneArmed = true;
		if(Entity * kultar = entities.getById("human_base_0028")) {
			SETVarValueLong(kultar->m_variables, "�" "chatpos", 1); // back to his first dialogue, the one with the camera
			LogInfo << "[coop] test: Kultar's dialogue rearmed";
		}
	} else if(step >= 4 && elapsed > std::chrono::seconds(123) && g_coop.isClient() && !cutsceneChatDone) {
		cutsceneChatDone = true;
		if(Entity * kultar = entities.getById("human_base_0028")) {
			LogInfo << "[coop] test: client talks to " << kultar->idString() << " (cutscene takeover)";
			SendIOScriptEvent(entities.player(), kultar, SM_CHAT);
		}
	} else if(step >= 4 && elapsed > std::chrono::seconds(128) && !cutsceneChatChecked) {
		cutsceneChatChecked = true;
		LogInfo << "[coop] test: after chat my pos " << int(entities.player()->pos.x) << "," << int(entities.player()->pos.y) << "," << int(entities.player()->pos.z)
		        << " controls blocked " << BLOCK_PLAYER_CONTROLS << " cinemascope " << cinematicBorder.isActive()
		        << " speech " << (getCinematicSpeech() != nullptr);
		Logger::flush();
	} else if(step >= 4 && elapsed > std::chrono::seconds(120) && !tpChecked) {
		tpChecked = true;
		LogInfo << "[coop] test: after tp my pos " << int(entities.player()->pos.x) << "," << int(entities.player()->pos.y) << "," << int(entities.player()->pos.z);
		for(const auto & entry : g_remote) {
			LogInfo << "[coop] test: after tp remote " << int(entry.first) << " pos " << int(entry.second.pos.x) << "," << int(entry.second.pos.y) << "," << int(entry.second.pos.z);
		}
		Logger::flush();
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
				Vec3f delta(io->pos.x - entities.player()->pos.x, 0.f, io->pos.z - entities.player()->pos.z);
				Vec3f dir = glm::length(delta) > 1.f ? glm::normalize(delta) : Vec3f(0.f, 0.f, 1.f);
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
	} else if(step == 6 && elapsed > std::chrono::seconds(132) && g_coop.isHost() && !adminDone) {
		adminDone = true;
		// Admin tools: grants to the client, then the page itself
		if(!g_remote.empty()) {
			PlayerId client = g_remote.begin()->first;
			adminGiveGold(client, 50);
			adminGiveXp(client, 200);
			adminGiveItem(client, "potion_life", 2);
			adminHeal(client);
			LogInfo << "[coop] test: admin grants sent to player " << int(client) << " (" << adminStatus() << ")";
		}
		adminGiveItem(g_coop.localId(), "short_sword", 1);
		adminGiveItem(g_coop.localId(), "sword", 1);
		LogInfo << "[coop] test: admin self item: " << adminStatus();
		adminOpenPage();
	} else if(step >= 4 && elapsed > std::chrono::seconds(140) && g_coop.isClient() && !adminClientDone) {
		adminClientDone = true;
		if(!g_remote.empty()) {
			adminTeleportMeTo(g_remote.begin()->first);
			LogInfo << "[coop] test: client " << adminStatus();
		}
	} else if(step >= 4 && elapsed > std::chrono::seconds(152) && g_coop.isHost() && !chestHostDone) {
		chestHostDone = true;
		// A script stocks a container (same ids everywhere?), then the client shops in it
		if(Entity * chest = testContainer()) {
			runScriptLine(*chest, "inventory addmulti provisions/mushroom/food_mushroom 3");
			LogInfo << "[coop] test: host stocked " << chest->idString() << " with " << (LASTSPAWNED ? LASTSPAWNED->idString() : "nothing");
			runScriptLine(*chest, "inventory addmulti jewelry/gold_coin/gold_coin 25");
			LogInfo << "[coop] test: host stocked " << chest->idString() << " with " << (LASTSPAWNED ? LASTSPAWNED->idString() : "nothing");
			logContainer("host before", *chest);
		} else {
			LogInfo << "[coop] test: no container in this level";
		}
	} else if(step >= 4 && elapsed > std::chrono::seconds(156) && g_coop.isClient() && !chestClientDone) {
		chestClientDone = true;
		if(Entity * chest = testContainer()) {
			logContainer("client before", *chest);
			// Takes one mushroom of the stack (like buying one), pockets the gold, leaves a potion
			for(auto slot : chest->inventory->slotsInOrder()) {
				if(!slot.show || !slot.entity) {
					continue;
				}
				if(slot.entity->className() == "food_mushroom" && slot.entity->_itemdata->count > 1) {
					slot.entity->_itemdata->count--;
					coop::itemCountChanged(*slot.entity);
					LogInfo << "[coop] test: client took one of " << slot.entity->idString();
				} else if(slot.entity->ioflags & IO_GOLD) {
					Entity * gold = slot.entity;
					LogInfo << "[coop] test: client pockets " << gold->idString();
					removeFromInventories(gold);
					entities.player()->inventory->insert(gold);
					break; // the slot view is stale now
				}
			}
			if(Entity * potion = AddItem("graph/obj3d/interactive/items/magic/potion_life/potion_life", -1, IO_IMMEDIATELOAD)) {
				SendInitScriptEvent(potion);
				giveToPlayer(potion);
				{
					coop::ContainerDropScope scope(*chest, potion);
					removeFromInventories(potion);
					chest->inventory->insert(potion);
				}
				LogInfo << "[coop] test: client stored " << potion->idString();
			}
			logContainer("client after", *chest);
		}
	} else if(step >= 4 && elapsed > std::chrono::seconds(160) && !chestChecked) {
		chestChecked = true;
		if(Entity * chest = testContainer()) {
			logContainer("after", *chest);
		}
		LogInfo << "[coop] test: my gold " << player.gold << " xp " << player.xp << " bags " << entities.player()->inventory->bags();
		Logger::flush();
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


// Party HUD ------------------------------------------------------------------------------

void fillRect(Vec2f origin, float width, float height, Color color) {
	EERIEDrawFill2DRectDegrad(origin, origin + Vec2f(width, height), 0.01f, color, color);
}

void partyHudDraw() {

	if(!g_coop.isActive() || g_coop.state() != State::InGame || ARXmenu.mode() != Mode_InGame
	   || !(player.Interface & INTER_LIFE_MANA) || (player.Interface & INTER_PLAYERBOOK) || cinematicBorder.isActive()) {
		return;
	}

	UseRenderState state(render2D());
	float s = std::max(1.f, float(g_size.height()) / 720.f);

	// Game clock, top center
	{
		s64 seconds = toMsi(g_gameTime.now()) / 1000;
		char clock[32];
		std::snprintf(clock, sizeof(clock), "%02lld:%02lld:%02lld", (long long)(seconds / 3600), (long long)((seconds / 60) % 60),
		              (long long)(seconds % 60));
		Font::TextSize size = hFontInGame->getTextSize(clock);
		Vec2i pos(g_size.center().x - size.width() / 2, int(6.f * s));
		fillRect(Vec2f(float(pos.x) - 6.f * s, float(pos.y) - 2.f * s), float(size.width()) + 12.f * s, float(size.height()) + 4.f * s,
		         Color(0, 0, 0, 110));
		hFontInGame->draw(pos, clock, Color(232, 204, 142));
	}

	// Teammates, left side: name, life bar, hunger bar
	float barWidth = 150.f * s;
	float x = 12.f * s;
	float y = float(g_size.height()) * 0.30f;
	for(const Player & other : g_coop.players()) {
		if(other.id == g_coop.localId()) {
			continue;
		}
		auto it = g_remote.find(other.id);
		const PlayerSnapshot * snap = (it != g_remote.end()) ? &it->second : nullptr;
		bool downed = snap && snap->downed;
		float life = snap ? glm::clamp(snap->life, 0.f, 1.f) : 0.f;
		float hunger = snap ? glm::clamp(snap->hunger, 0.f, 1.f) : 0.f;
		bool here = snap && snap->area == g_currentArea.handleData();

		std::string label = other.name;
		if(!snap) {
			label += " (?)";
		} else if(downed) {
			label += " - " + trs("coop_hud_downed", "\xC3\xA0 terre");
		} else if(!here) {
			label += " - " + trs("coop_hud_elsewhere", "ailleurs");
		}
		if(u16 ms = latencyOf(other.id)) {
			label += " " + std::to_string(ms) + " ms";
		}
		Font::TextSize size = hFontInGame->draw(Vec2i(int(x), int(y)), label, downed ? Color(255, 90, 90) : Color(232, 204, 142));
		y += float(size.height()) + 3.f * s;

		float lifeHeight = 9.f * s;
		fillRect(Vec2f(x, y), barWidth, lifeHeight, Color(25, 25, 25, 170));
		if(life > 0.f) {
			fillRect(Vec2f(x, y), barWidth * life, lifeHeight, downed ? Color(120, 30, 30) : Color(200, 30, 30));
		}
		y += lifeHeight + 2.f * s;

		float hungerHeight = 5.f * s;
		fillRect(Vec2f(x, y), barWidth, hungerHeight, Color(25, 25, 25, 170));
		if(hunger > 0.f) {
			fillRect(Vec2f(x, y), barWidth * hunger, hungerHeight, Color(215, 140, 40));
		}
		y += hungerHeight + 12.f * s;
	}

}

// Local player's torch at the hip (third person only) ---------------------------------------

Entity * g_localTorchDisplay = nullptr;
std::string g_localTorchClass;

void localTorchDisplayUpdate() {

	// The copy may have been destroyed with the level
	if(g_localTorchDisplay && !entities.get(g_localTorchDisplay->index())) {
		g_localTorchDisplay = nullptr;
	}

	bool wanted = thirdPersonActive() && player.torch && entities.player() && entities.player()->obj
	              && ARXmenu.mode() == Mode_InGame;
	std::string classPath = wanted ? player.torch->classPath().string() : std::string();

	if(g_localTorchDisplay && (!wanted || classPath != g_localTorchClass)) {
		g_localTorchDisplay->destroy();
		g_localTorchDisplay = nullptr;
	}
	if(wanted && !g_localTorchDisplay) {
		g_localTorchDisplay = attachTorch(*entities.player(), classPath);
		g_localTorchClass = classPath;
	}

}

bool localHudActive() {
	return g_coop.isActive() && g_coop.state() == State::InGame;
}

void localHudDraw(const Rectf & rect, float scale, Color lifeColor, float life) {

	UseRenderState state(render2D());

	float x = rect.left;
	float width = rect.width();
	float y = rect.bottom;

	// Bottom up, so the bars stay anchored where the orb was
	float hungerHeight = 5.f * scale;
	y -= hungerHeight;
	float hunger = glm::clamp(player.hunger * 0.01f, 0.f, 1.f);
	fillRect(Vec2f(x, y), width, hungerHeight, Color(25, 25, 25, 170));
	if(hunger > 0.f) {
		fillRect(Vec2f(x, y), width * hunger, hungerHeight, Color(215, 140, 40));
	}

	float lifeHeight = 10.f * scale;
	y -= lifeHeight + 2.f * scale;
	life = glm::clamp(life, 0.f, 1.f);
	fillRect(Vec2f(x, y), width, lifeHeight, Color(25, 25, 25, 170));
	if(life > 0.f) {
		fillRect(Vec2f(x, y), width * life, lifeHeight, lifeColor);
	}

	// Name line: "<name> <ms>" on the left like the teammates' bars, "<life> / <max>" on the right;
	// when both do not fit the ping moves to its own line above, and a long name is shortened
	const Player * me = g_coop.player(g_coop.localId());
	std::string name = me ? me->name : config.coop.nickname;
	std::string ping;
	if(u16 ms = latencyOf(g_coop.localId())) {
		ping = std::to_string(ms) + " ms";
	}
	char value[32];
	std::snprintf(value, sizeof(value), "%d / %d", int(std::ceil(player.lifePool.current)), int(player.lifePool.max));
	Font::TextSize valueSize = hFontInGame->getTextSize(value);
	float gap = 10.f * scale;
	float nameRoom = width - float(valueSize.width()) - gap;
	std::string label = ping.empty() ? name : name + " " + ping;
	bool pingAbove = false;
	if(!ping.empty() && float(hFontInGame->getTextSize(label).width()) > nameRoom) {
		label = name;
		pingAbove = true;
	}
	while(label.size() > 2 && float(hFontInGame->getTextSize(label + "\xE2\x80\xA6").width()) > nameRoom) {
		label.pop_back();
		while(!label.empty() && (u8(label.back()) & 0xC0) == 0x80) {
			label.pop_back(); // do not cut a UTF-8 sequence in half
		}
		if(float(hFontInGame->getTextSize(label + "\xE2\x80\xA6").width()) <= nameRoom) {
			label += "\xE2\x80\xA6";
			break;
		}
	}
	Font::TextSize nameSize = hFontInGame->getTextSize(label);
	y -= float(nameSize.height()) + 3.f * scale;
	hFontInGame->draw(Vec2i(int(x), int(y)), label, Color(232, 204, 142));
	hFontInGame->draw(Vec2i(int(x + width) - valueSize.width(), int(y)), value, Color(232, 204, 142));
	if(pingAbove) {
		Font::TextSize pingSize = hFontInGame->getTextSize(ping);
		y -= float(pingSize.height()) + 1.f * scale;
		hFontInGame->draw(Vec2i(int(x + width) - pingSize.width(), int(y)), ping, Color(200, 180, 130));
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
				label += " - " + trs("coop_hud_reviving", "r\xC3\xA9" "animation") + " " + std::to_string(int(g_reviveProgress * 100.f)) + " %";
			} else {
				label += " - " + trs("coop_hud_downed_hold", "\xC3\xA0 terre (maintenir clic gauche)");
			}
		}
		drawTextAt(hFontInGame, pos, label, entry.second.downed ? Color(255, 90, 90) : Color(232, 204, 142));
	}

}

} // namespace coop
