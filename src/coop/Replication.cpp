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

#include "coop/Replication.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <zlib.h>

#include "coop/Protocol.h"
#include "coop/Session.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Damage.h"
#include "game/Inventory.h"
#include "game/Item.h"
#include "game/Player.h"
#include "coop/Puppets.h"
#include "gui/Menu.h"
#include "gui/book/Book.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/SaveGame.h"
#include "graphics/Renderer.h"
#include "gui/MenuWidgets.h"
#include "platform/Time.h"
#include "scene/ChangeLevel.h"
#include "io/log/Logger.h"
#include "io/resource/ResourcePath.h"
#include "scene/Interactive.h"
#include "script/ScriptEvent.h"
#include "script/ScriptUtils.h"

extern Entity * LASTSPAWNED;
extern bool REQUEST_SPEECH_SKIP;

namespace coop {

namespace {

PlayerId g_actingPlayer = InvalidPlayerId; //!< Host: the player whose action is being processed, if any
bool g_playthroughStarted = false;         //!< Client: our own character is ready for the shared world
int g_levelLoading = 0;                    //!< > 0 while a level is being loaded
int g_applyingRemote = 0;      //!< > 0 while applying something received from the network
bool g_creatingProxy = false;  //!< Host: suppress replication while a proxy item initializes

enum class Category {
	World,  //!< Affects the shared world: executed on the host, replayed on every client
	Player, //!< Affects "the player": executed by the acting player only
};

const std::map<std::string, Category> & commandTable() {
	static const std::map<std::string, Category> table = {
		// World
		{ "setevent", Category::World }, { "objecthide", Category::World }, { "destroy", Category::World },
		{ "teleport", Category::World }, { "playanim", Category::World }, { "forceanim", Category::World },
		{ "rotate", Category::World }, { "move", Category::World }, { "setinteractivity", Category::World },
		{ "collision", Category::World }, { "setlight", Category::World }, { "ambiance", Category::World },
		{ "play", Category::World }, { "playspeech", Category::World }, { "setspeakpitch", Category::World },
		{ "speak", Category::World }, { "inventory", Category::World }, { "setweapon", Category::World },
		{ "usemesh", Category::World }, { "tweak", Category::World }, { "settransparency", Category::World },
		{ "setscale", Category::World }, { "halo", Category::World }, { "linkobjtome", Category::World },
		{ "setname", Category::World }, { "setmaterial", Category::World }, { "setstepmaterial", Category::World },
		{ "setarmormaterial", Category::World }, { "setweaponmaterial", Category::World },
		{ "behavior", Category::World }, { "settarget", Category::World }, { "setmovemode", Category::World },
		{ "setlife", Category::World }, { "forcedeath", Category::World }, { "revive", Category::World },
		{ "setnpcstat", Category::World }, { "setdetect", Category::World }, { "setgroup", Category::World },
		{ "setcontrolledzone", Category::World }, { "unsetcontrolledzone", Category::World },
		{ "zoneparam", Category::World }, { "usepath", Category::World }, { "setpath", Category::World },
		{ "attach", Category::World }, { "detach", Category::World }, { "spellcast", Category::World },
		{ "setsecret", Category::World }, { "settrap", Category::World }, { "replaceme", Category::World },
		{ "physical", Category::World }, { "activatephysics", Category::World }, { "magic", Category::World },
		{ "attractor", Category::World }, { "anchorblock", Category::World }, { "specialfx", Category::World },
		{ "setxpvalue", Category::World }, { "shopcategory", Category::World }, { "shopmultiply", Category::World },
		{ "setequip", Category::World }, { "setdurability", Category::World }, { "setprice", Category::World },
		{ "setcount", Category::World }, { "setmaxcount", Category::World }, { "setpoisonous", Category::World },
		{ "setsteal", Category::World }, { "setfood", Category::World }, { "setobjecttype", Category::World },
		{ "equip", Category::World }, { "weapon", Category::World }, { "repair", Category::World },
		{ "skin", Category::World }, { "dodamage", Category::World }, { "damager", Category::World },
		{ "setblood", Category::World }, { "setspeed", Category::World }, { "setstarefactor", Category::World },
		{ "setircolor", Category::World }, { "setweight", Category::World }, { "unset", Category::World },
		{ "spawn", Category::World },
		// Player
		{ "addgold", Category::Player }, { "book", Category::Player }, { "note", Category::Player },
		{ "popup", Category::Player }, { "herosay", Category::Player }, { "playerinterface", Category::Player },
		{ "setplayercontrols", Category::Player }, { "playerlookat", Category::Player },
		{ "poison", Category::Player }, { "playermanadrain", Category::Player },
		{ "invulnerability", Category::Player }, { "precast", Category::Player },
		{ "setplayertweak", Category::Player }, { "sethunger", Category::Player },
		{ "playerstacksize", Category::Player }, { "cine", Category::Player }, { "cinemascope", Category::Player },
		{ "cameraactivate", Category::Player }, { "camerasmoothing", Category::Player },
		{ "camerafocal", Category::Player }, { "cameratranslatetarget", Category::Player },
		{ "worldfade", Category::Player }, { "quake", Category::Player }, { "endintro", Category::Player },
		{ "endgame", Category::Player }, { "mapmarker", Category::Player }, { "drawsymbol", Category::Player },
		{ "conversation", Category::Player }, { "eatme", Category::Player }, { "closestealbag", Category::Player },
	};
	return table;
}

bool isPlayerSide(const Entity * io) {
	if(!io) {
		return false;
	}
	if(io == entities.player() || io->coopProxy) {
		return true;
	}
	for(const Entity * owner = io->owner(); owner; owner = owner->owner()) {
		if(owner == entities.player()) {
			return true;
		}
	}
	return false;
}

std::string quote(const std::string & word) {
	bool needed = word.empty();
	for(char c : word) {
		if(c == ' ' || c == '\t' || c == '"') {
			needed = true;
		}
	}
	return needed ? "\"" + word + "\"" : word;
}

std::string buildLine(std::string_view command, const std::vector<std::string> & words) {
	std::string line(command);
	for(const std::string & word : words) {
		line += ' ';
		line += quote(word);
	}
	return line;
}

//! Whether the command line uses a sub-command that is player-directed (checked before executing).
bool peekPlayerDirected(std::string_view command, const script::Context & context) {
	if(command != "inventory" && command != "teleport" && command != "speak" && command != "dodamage") {
		return false;
	}
	// Look at the raw first word without consuming it
	std::string_view data = context.getScript()->data;
	size_t pos = context.getPosition();
	while(pos < data.size() && (data[pos] == ' ' || data[pos] == '\t')) {
		pos++;
	}
	size_t end = pos;
	while(end < data.size() && data[end] != ' ' && data[end] != '\t' && data[end] != '\n' && data[end] != '\r') {
		end++;
	}
	std::string_view first = data.substr(pos, end - pos);
	if(command == "inventory") {
		return first == "playeradd" || first == "playeraddfromscene" || first == "playeraddmulti";
	}
	if(command == "dodamage") {
		return first == "player";
	}
	return first.size() > 1 && first[0] == '-' && first.find('p') != std::string_view::npos;
}

/*!
 * Removes the -c flag and its camera parameters from a recorded speak command:
 * speak -c keep [text] / -c zoom p y p y d d / -c ccctalker_x target d d / -c side d d target d d d
 */
std::vector<std::string> stripCinematicSpeech(const std::vector<std::string> & words) {
	std::vector<std::string> plain = words;
	if(plain.empty() || plain[0].empty() || plain[0][0] != '-') {
		return plain;
	}
	plain[0].erase(std::remove(plain[0].begin(), plain[0].end(), 'c'), plain[0].end());
	size_t params = 0;
	if(plain.size() > 1) {
		const std::string & mode = plain[1];
		if(mode == "keep") {
			params = 0;
		} else if(mode == "zoom") {
			params = 6;
		} else if(mode == "ccctalker_l" || mode == "ccctalker_r" || mode == "ccclistener_l" || mode == "ccclistener_r") {
			params = 3;
		} else if(mode == "side" || mode == "side_l" || mode == "side_r") {
			params = 6;
		}
		size_t end = std::min(plain.size(), size_t(2) + params);
		plain.erase(plain.begin() + 1, plain.begin() + end);
	}
	if(plain[0] == "-") {
		plain.erase(plain.begin());
	}
	return plain;
}

void sendScriptCommand(PlayerId to, const std::string & entityId, std::string_view command,
                       const std::vector<std::string> & words) {
	Writer writer;
	writer.string(entityId);
	writer.u8_(u8(words.size() + 1));
	writer.string(command);
	for(const std::string & word : words) {
		writer.string(word);
	}
	if(to == InvalidPlayerId) {
		g_coop.broadcast(MessageType::ScriptCommand, writer);
	} else {
		g_coop.sendTo(to, MessageType::ScriptCommand, writer);
	}
}

void applyScriptCommand(Reader & reader) {

	std::string entityId = reader.string();
	u8 count = reader.u8_();
	if(count == 0) {
		return;
	}
	std::string line = reader.string();
	for(u8 i = 1; i < count; i++) {
		line += ' ';
		line += quote(reader.string());
	}

	Entity * entity = entityId.empty() ? entities.player() : entities.getById(entityId);
	if(!entity) {
		// World commands may reference entities we do not have (e.g. another player's items)
		LogDebug("[coop] ignoring command for unknown entity " << entityId << ": " << line);
		return;
	}

	LogDebug("[coop] apply " << entity->idString() << ": " << line);

	EERIE_SCRIPT es;
	es.valid = true;
	es.data = line + "\naccept\n";

	g_applyingRemote++;
	ScriptEvent::send(&es, nullptr, entity, SM_EXECUTELINE, ScriptParameters(), 0);
	g_applyingRemote--;

}

// Spawned entities must get the same id everywhere, so they are created explicitly on the clients.
void sendSpawn(const Entity & io) {
	Writer writer;
	writer.u8_((io.ioflags & IO_NPC) ? 1 : (io.ioflags & IO_ITEM) ? 2 : 3);
	writer.string(io.classPath().string());
	writer.s32_(io.instance());
	writer.f32_(io.pos.x);
	writer.f32_(io.pos.y);
	writer.f32_(io.pos.z);
	writer.f32_(io.angle.getPitch());
	writer.f32_(io.angle.getYaw());
	writer.f32_(io.angle.getRoll());
	g_coop.broadcast(MessageType::SpawnEntity, writer);
}

void applySpawn(Reader & reader) {
	u8 kind = reader.u8_();
	res::path classPath = res::path::load(reader.string());
	EntityInstance instance = reader.s32_();
	Vec3f pos = reader.vec3<Vec3f>();
	float pitch = reader.f32_();
	float yaw = reader.f32_();
	float roll = reader.f32_();
	if(entities.getById(EntityId(classPath.filename(), instance).string())) {
		return;
	}
	g_applyingRemote++;
	Entity * io = nullptr;
	if(kind == 1) {
		io = AddNPC(classPath, instance, IO_IMMEDIATELOAD);
	} else if(kind == 2) {
		io = AddItem(classPath, instance, IO_IMMEDIATELOAD);
	} else {
		io = AddFix(classPath, instance, IO_IMMEDIATELOAD);
	}
	if(io) {
		io->scriptload = 1;
		io->pos = pos;
		io->angle = Anglef(pitch, yaw, roll);
		SendInitScriptEvent(io);
		LogInfo << "[coop] spawned " << io->idString();
	}
	g_applyingRemote--;
}

// Client -> host event forwarding ---------------------------------------------------------

const std::set<ScriptMessage> & forwardedEvents() {
	static const std::set<ScriptMessage> events = {
		SM_ACTION, SM_CHAT, SM_COMBINE, SM_STEAL, SM_TRAP_DISARMED, SM_CLICKED, SM_AGGRESSION
	};
	return events;
}

void forwardEvent(Entity * sender, Entity * entity, const ScriptEventName & event,
                  const ScriptParameters & parameters) {
	Writer writer;
	writer.string(entity->idString());
	writer.u16_(u16(event.getId()));
	writer.string(event.getName());
	writer.u8_(u8(parameters.size()));
	for(const std::string & parameter : parameters) {
		writer.string(parameter);
	}
	if(sender && sender != entities.player()) {
		writer.string(sender->idString());
		writer.string(sender->classPath().string());
		writer.s32_(sender->instance());
	} else {
		writer.string("player");
		writer.string("");
		writer.s32_(0);
	}
	g_coop.sendToHost(MessageType::EventForward, writer);
	LogDebug("[coop] forwarded " << event << " to " << entity->idString());
}

void applyForwardedEvent(PlayerId from, Reader & reader) {

	std::string entityId = reader.string();
	u16 eventId = reader.u16_();
	std::string eventName = reader.string();
	u8 count = reader.u8_();
	ScriptParameters parameters;
	for(u8 i = 0; i < count; i++) {
		parameters.push_back(reader.string());
	}
	std::string senderId = reader.string();
	std::string senderClass = reader.string();
	EntityInstance senderInstance = reader.s32_();

	Entity * entity = entities.getById(entityId);
	if(!entity) {
		LogWarning << "[coop] player " << int(from) << " sent an event to unknown entity " << entityId;
		return;
	}

	// The sender is usually the player, or one of its inventory items (which only exists on
	// its machine): give the scripts a temporary stand-in so ^sender, isgroup, destroy... work.
	Entity * sender = entities.player();
	Entity * proxy = nullptr;
	if(senderId != "player") {
		sender = entities.getById(senderId);
		if(!sender && !senderClass.empty()) {
			g_creatingProxy = true;
			proxy = AddItem(res::path::load(senderClass), senderInstance, IO_IMMEDIATELOAD);
			g_creatingProxy = false;
			if(proxy) {
				proxy->coopProxy = true;
				proxy->ioflags |= IO_NOSAVE | IO_NO_COLLISIONS;
				proxy->show = SHOW_FLAG_HIDDEN;
				sender = proxy;
			}
		}
	}

	ScriptEventName event = eventName.empty() ? ScriptEventName(ScriptMessage(eventId)) : ScriptEventName(eventName);
	LogDebug("[coop] player " << int(from) << " -> " << event << " on " << entity->idString());

	PlayerId previous = g_actingPlayer;
	g_actingPlayer = from;
	SendIOScriptEvent(sender, entity, event, parameters);
	g_actingPlayer = previous;

	if(proxy && ValidIOAddress(proxy)) {
		if(proxy->owner() || proxy->show != SHOW_FLAG_HIDDEN) {
			proxy->coopProxy = false; // the script took it: it is part of the world now
			proxy->ioflags &= ~(IO_NOSAVE | IO_NO_COLLISIONS);
		} else {
			proxy->destroy();
		}
	}

}

// Shared state --------------------------------------------------------------------------

void applyShared(PlayerId from, MessageType type, Reader & reader) {
	g_applyingRemote++;
	Writer relay;
	switch(type) {
		case MessageType::SharedQuest: {
			std::string quest = reader.string();
			ARX_PLAYER_Quest_Add(quest);
			relay.string(quest);
			break;
		}
		case MessageType::SharedKey: {
			std::string key = reader.string();
			ARX_KEYRING_Add(key);
			relay.string(key);
			break;
		}
		case MessageType::SharedRune: {
			u32 rune = reader.u32_();
			ARX_Player_Rune_Add(RuneFlag(rune));
			relay.u32_(rune);
			break;
		}
		case MessageType::SharedXP: {
			s32 amount = reader.s32_();
			ARX_PLAYER_Modify_XP(amount);
			relay.s32_(amount);
			break;
		}
		default: break;
	}
	g_applyingRemote--;
	if(g_coop.isHost()) {
		g_coop.broadcast(type, relay, from);
	}
}

void applySetGlobal(PlayerId from, Reader & reader) {
	std::string name = reader.string();
	u8 type = reader.u8_();
	g_applyingRemote++;
	Writer relay;
	relay.string(name);
	relay.u8_(type);
	if(type == 0) {
		std::string value = reader.string();
		relay.string(value);
		SETVarValueText(svar, name, std::move(value));
	} else if(type == 1) {
		s32 value = reader.s32_();
		relay.s32_(value);
		SETVarValueLong(svar, name, value);
	} else {
		float value = reader.f32_();
		relay.f32_(value);
		SETVarValueFloat(svar, name, value);
	}
	g_applyingRemote--;
	if(g_coop.isHost()) {
		g_coop.broadcast(MessageType::SetGlobal, relay, from);
	}
}

// Level state --------------------------------------------------------------------------

bool g_levelSynced = false;               //!< Client: we have loaded the host's level state
PlatformInstant g_lastLevelRequest;       //!< Client: last time we asked for it
std::set<PlayerId> g_pendingLevelRequests; //!< Host: clients waiting for the level
std::set<PlayerId> g_readyClients;         //!< Host: clients whose own playthrough has started

bool inLevel() {
	return ARXmenu.mode() == Mode_InGame && g_currentArea && entities.player();
}

void sendWorldSync(PlayerId to) {
	Writer writer;
	writer.u16_(u16(g_playerQuestLogEntries.size()));
	for(const std::string & quest : g_playerQuestLogEntries) {
		writer.string(quest);
	}
	writer.u16_(u16(g_playerKeyring.size()));
	for(const std::string & key : g_playerKeyring) {
		writer.string(key);
	}
	writer.u32_(u32(player.rune_flags));
	g_coop.sendTo(to, MessageType::WorldSync, writer);
}

void applyWorldSync(Reader & reader) {
	g_applyingRemote++;
	u16 quests = reader.u16_();
	g_playerQuestLogEntries.clear();
	for(u16 i = 0; i < quests; i++) {
		g_playerQuestLogEntries.push_back(reader.string());
	}
	g_playerBook.clearJournal();
	u16 keys = reader.u16_();
	g_playerKeyring.clear();
	for(u16 i = 0; i < keys; i++) {
		g_playerKeyring.push_back(reader.string());
	}
	player.rune_flags = RuneFlags::load(reader.u32_());
	g_applyingRemote--;
	LogInfo << "[coop] world sync: " << quests << " quests, " << keys << " keys";
}

// Saved entities are mostly zero-filled fixed-size structs: they compress extremely well.
void sendLevelState(PlayerId to) {
	std::vector<std::pair<std::string, std::string>> files;
	if(!ARX_CHANGELEVEL_ExportLevel(files)) {
		LogWarning << "[coop] cannot export the level state";
		return;
	}
	Writer raw;
	raw.u32_(u32(files.size()));
	for(const auto & file : files) {
		raw.string(file.first);
		raw.u32_(u32(file.second.size()));
		raw.bytes(reinterpret_cast<const u8 *>(file.second.data()), file.second.size());
	}
	uLongf compressedSize = compressBound(uLong(raw.size()));
	std::vector<u8> compressed(compressedSize);
	if(compress2(compressed.data(), &compressedSize, raw.data().data(), uLong(raw.size()), 6) != Z_OK) {
		LogWarning << "[coop] cannot compress the level state";
		return;
	}
	Writer writer;
	writer.u32_(g_currentArea.handleData());
	writer.f32_(player.pos.x);
	writer.f32_(player.pos.y);
	writer.f32_(player.pos.z);
	writer.u32_(u32(raw.size()));
	writer.u32_(u32(compressedSize));
	writer.bytes(compressed.data(), compressedSize);
	g_coop.sendTo(to, MessageType::LevelState, writer);
	sendWorldSync(to);
	LogInfo << "[coop] sent level " << g_currentArea << " state to player " << int(to) << ": "
	        << files.size() << " entries, " << raw.size() / 1024 << " KiB -> " << compressedSize / 1024 << " KiB";
}

void applyLevelState(Reader & reader) {
	AreaId area = AreaId(reader.u32_());
	Vec3f pos = reader.vec3<Vec3f>();
	u32 rawSize = reader.u32_();
	u32 compressedSize = reader.u32_();
	if(compressedSize > reader.remaining() || rawSize > MaxPayloadSize) {
		throw ReadError("truncated level state");
	}
	std::vector<u8> raw(rawSize);
	uLongf size = rawSize;
	if(uncompress(raw.data(), &size, reader.rest().first, compressedSize) != Z_OK || size != rawSize) {
		LogError << "[coop] cannot decompress the level state";
		return;
	}
	reader.skip(compressedSize);
	Reader inner(raw.data(), raw.size());
	u32 count = inner.u32_();
	std::vector<std::pair<std::string, std::string>> files;
	for(u32 i = 0; i < count; i++) {
		std::string name = inner.string();
		u32 fileSize = inner.u32_();
		if(fileSize > inner.remaining()) {
			throw ReadError("truncated level state");
		}
		files.emplace_back(std::move(name), std::string(reinterpret_cast<const char *>(inner.rest().first), fileSize));
		inner.skip(fileSize);
	}
	LogInfo << "[coop] loading level " << area << " state from the host (" << count << " entries)";
	g_applyingRemote++;
	bool ok = ARX_CHANGELEVEL_ImportLevel(area, files, pos);
	g_applyingRemote--;
	g_levelSynced = ok;
	if(!ok) {
		LogError << "[coop] failed to load the host's level state";
	}
}

void applyDamagePlayer(Reader & reader) {
	float damage = reader.f32_();
	u32 type = reader.u32_();
	g_applyingRemote++;
	damagePlayer(damage, DamageType::load(type), nullptr);
	g_applyingRemote--;
	LogInfo << "[coop] took " << damage << " damage from the host's world";
}

void applyDamageNpc(PlayerId from, Reader & reader) {
	std::string id = reader.string();
	float damage = reader.f32_();
	u32 type = reader.u32_();
	bool hasPos = reader.bool_();
	Vec3f pos = reader.vec3<Vec3f>();
	Entity * npc = entities.getById(id);
	if(!npc || !(npc->ioflags & IO_NPC) || npc->coopPuppet) {
		return;
	}
	PlayerId previous = g_actingPlayer;
	g_actingPlayer = from;
	// The players are one and the same for the NPCs: the source is "the player"
	damageNpc(*npc, damage, entities.player(), nullptr, DamageType::load(type), hasPos ? &pos : nullptr);
	g_actingPlayer = previous;
}

bool g_hostDrivenSaveLoad = false;

std::string coopSaveName(std::string_view hostName) {
	return "coop: " + std::string(hostName);
}

SavegameHandle findSaveByName(const std::string & name) {
	for(size_t i = 0; i < savegames.size(); i++) {
		if(savegames[SavegameHandle(long(i))].name == name) {
			return SavegameHandle(long(i));
		}
	}
	return SavegameHandle();
}

void applySaveRequest(Reader & reader) {
	std::string name = coopSaveName(reader.string());
	if(!inLevel()) {
		return;
	}
	g_hostDrivenSaveLoad = true;
	GRenderer->getSnapshot(savegame_thumbnail, config.interface.thumbnailSize.x, config.interface.thumbnailSize.y);
	bool ok = savegames.save(name, findSaveByName(name), savegame_thumbnail);
	g_hostDrivenSaveLoad = false;
	LogInfo << "[coop] saved my character as \"" << name << "\"" << (ok ? "" : " (failed)");
}

void applyLoadRequest(Reader & reader) {
	std::string name = coopSaveName(reader.string());
	SavegameHandle save = findSaveByName(name);
	if(save == SavegameHandle()) {
		LogInfo << "[coop] the host loaded \"" << name << "\" but I have no such save: keeping my character";
		g_levelSynced = false; // the host's world will be fetched again anyway
		return;
	}
	LogInfo << "[coop] the host loaded a save: loading my character from \"" << name << "\"";
	g_hostDrivenSaveLoad = true;
	ARX_LoadGame(savegames[save]);
	g_hostDrivenSaveLoad = false;
	playthroughStarted();
}


// Loot -------------------------------------------------------------------------------------

void hideTakenItem(Entity & item) {
	removeFromInventories(&item);
	if(item.show != SHOW_FLAG_MEGAHIDE) {
		item.show = SHOW_FLAG_MEGAHIDE;
		LogInfo << "[coop] " << item.idString() << " was taken by another player";
	}
}

void applyTakeItem(PlayerId from, Reader & reader) {
	std::string id = reader.string();
	Entity * item = entities.getById(id);
	if(item && (item->ioflags & IO_ITEM) && !isPlayerSide(item)) {
		g_applyingRemote++;
		hideTakenItem(*item);
		g_applyingRemote--;
	}
	if(g_coop.isHost()) {
		Writer writer;
		writer.string(id);
		g_coop.broadcast(MessageType::TakeItem, writer, from);
	}
}

void applyDropItem(PlayerId from, Reader & reader) {
	std::string id = reader.string();
	res::path classPath = res::path::load(reader.string());
	EntityInstance instance = reader.s32_();
	Vec3f pos = reader.vec3<Vec3f>();
	float yaw = reader.f32_();
	s16 count = reader.raw<s16>();
	g_applyingRemote++;
	Entity * item = entities.getById(id);
	if(!item) {
		item = AddItem(classPath, instance, IO_IMMEDIATELOAD);
		if(item) {
			item->scriptload = 1;
			SendInitScriptEvent(item);
		}
	}
	if(item && !isPlayerSide(item)) {
		removeFromInventories(item);
		item->pos = pos;
		item->angle.setYaw(yaw);
		item->show = SHOW_FLAG_IN_SCENE;
		item->requestRoomUpdate = true;
		if((item->ioflags & IO_ITEM) && count > 0) {
			item->_itemdata->count = count;
		}
		LogInfo << "[coop] " << item->idString() << " was dropped by another player";
	}
	g_applyingRemote--;
	if(g_coop.isHost()) {
		Writer writer;
		writer.string(id);
		writer.string(classPath.string());
		writer.s32_(instance);
		writer.f32_(pos.x);
		writer.f32_(pos.y);
		writer.f32_(pos.z);
		writer.f32_(yaw);
		writer.raw<s16>(count);
		g_coop.broadcast(MessageType::DropItem, writer, from);
	}
}

void handleGameMessage(PlayerId from, MessageType type, Reader & reader) {
	switch(type) {
		case MessageType::TakeItem: {
			applyTakeItem(from, reader);
			break;
		}
		case MessageType::DropItem: {
			applyDropItem(from, reader);
			break;
		}
		case MessageType::SpeechSkip: {
			g_applyingRemote++;
			REQUEST_SPEECH_SKIP = true;
			if(g_coop.isHost()) {
				SendMsgToAllIO(nullptr, SM_KEY_PRESSED);
				g_coop.broadcast(MessageType::SpeechSkip, Writer(), from);
			}
			g_applyingRemote--;
			break;
		}
		case MessageType::SaveRequest: {
			if(g_coop.isClient()) {
				applySaveRequest(reader);
			}
			break;
		}
		case MessageType::LoadRequest: {
			if(g_coop.isClient()) {
				applyLoadRequest(reader);
			}
			break;
		}
		case MessageType::DamagePlayer: {
			if(g_coop.isClient()) {
				applyDamagePlayer(reader);
			}
			break;
		}
		case MessageType::DamageNpc: {
			if(g_coop.isHost()) {
				applyDamageNpc(from, reader);
			}
			break;
		}
		case MessageType::RequestLevel: {
			if(g_coop.isHost()) {
				g_readyClients.insert(from);
				g_pendingLevelRequests.insert(from);
			}
			break;
		}
		case MessageType::LevelState: {
			if(g_coop.isClient() && g_playthroughStarted) {
				applyLevelState(reader);
			}
			break;
		}
		case MessageType::WorldSync: {
			if(g_coop.isClient()) {
				applyWorldSync(reader);
			}
			break;
		}
		case MessageType::EventForward: {
			if(g_coop.isHost()) {
				applyForwardedEvent(from, reader);
			}
			break;
		}
		case MessageType::ScriptCommand: {
			if(g_coop.isClient() && g_levelSynced) {
				applyScriptCommand(reader);
			}
			break;
		}
		case MessageType::SpawnEntity: {
			if(g_coop.isClient() && g_levelSynced) {
				applySpawn(reader);
			}
			break;
		}
		case MessageType::SetGlobal: {
			applySetGlobal(from, reader);
			break;
		}
		case MessageType::SharedQuest:
		case MessageType::SharedKey:
		case MessageType::SharedRune:
		case MessageType::SharedXP: {
			applyShared(from, type, reader);
			break;
		}
		default: {
			LogWarning << "[coop] unhandled game message " << int(type);
			break;
		}
	}
}

} // anonymous namespace

void replicationInit() {
	g_coop.onGameMessage = handleGameMessage;
}

void playthroughStarted() {
	g_playthroughStarted = true;
	g_levelSynced = false;
}

void levelLoadBegin() {
	g_levelLoading++;
}

void levelLoadEnd() {
	g_levelLoading--;
	if(g_coop.isHost() && g_levelLoading == 0) {
		// Every client already playing needs the new state of the world
		for(PlayerId id : g_readyClients) {
			g_pendingLevelRequests.insert(id);
		}
	}
}

void replicationUpdate() {

	if(!g_coop.isActive() || g_coop.state() != State::InGame) {
		g_levelSynced = false;
		g_playthroughStarted = false;
		g_pendingLevelRequests.clear();
		g_readyClients.clear();
		return;
	}

	if(g_coop.isHost()) {
		if(!g_pendingLevelRequests.empty() && inLevel()) {
			for(PlayerId id : g_pendingLevelRequests) {
				if(g_coop.player(id)) {
					sendLevelState(id);
				} else {
					g_readyClients.erase(id);
				}
			}
			g_pendingLevelRequests.clear();
		}
		return;
	}

	if(!g_levelSynced && g_playthroughStarted && inLevel()) {
		PlatformInstant now = platform::getTime();
		if(now - g_lastLevelRequest > std::chrono::seconds(5)) {
			g_lastLevelRequest = now;
			g_coop.sendToHost(MessageType::RequestLevel, Writer());
			LogInfo << "[coop] requesting the level state from the host";
		}
	}

}

ActorScope::ActorScope(const Entity * sender, const Entity * entity) {
	if(g_coop.isHost() && g_actingPlayer == InvalidPlayerId && g_applyingRemote == 0
	   && sender && isPlayerSide(sender) && entity && !isPlayerSide(entity)) {
		m_active = true;
		m_previous = g_actingPlayer;
		g_actingPlayer = g_coop.localId();
	}
}

ActorScope::~ActorScope() {
	if(m_active) {
		g_actingPlayer = PlayerId(m_previous);
	}
}

EntityInstance instanceBase() {
	if(g_coop.isClient()) {
		return EntityInstance(g_coop.localId()) * 10000 + 1;
	}
	return 1;
}

bool applyingRemote() {
	return g_applyingRemote > 0;
}

bool levelSynced() {
	return g_levelSynced;
}

bool isPlaythroughStarted() {
	return g_playthroughStarted;
}

void gameSaved(std::string_view name) {
	if(g_coop.isHost() && g_coop.state() == State::InGame) {
		Writer writer;
		writer.string(name);
		g_coop.broadcast(MessageType::SaveRequest, writer);
	}
}

void gameLoaded(std::string_view name) {
	if(g_coop.isHost() && g_coop.state() == State::InGame) {
		Writer writer;
		writer.string(name);
		g_coop.broadcast(MessageType::LoadRequest, writer);
	}
}

bool hostDrivenSaveLoad() {
	return g_hostDrivenSaveLoad;
}

void itemTaken(const Entity & item) {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || g_applyingRemote > 0 || !(item.ioflags & IO_ITEM)
	   || item.coopProxy || (item.ioflags & IO_NOSAVE)) {
		return;
	}
	Writer writer;
	writer.string(item.idString());
	g_coop.sendToOthers(MessageType::TakeItem, writer);
}

void itemDropped(const Entity & item) {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || g_applyingRemote > 0 || !(item.ioflags & IO_ITEM)
	   || (item.ioflags & IO_NOSAVE)) {
		return;
	}
	Writer writer;
	writer.string(item.idString());
	writer.string(item.classPath().string());
	writer.s32_(item.instance());
	writer.f32_(item.pos.x);
	writer.f32_(item.pos.y);
	writer.f32_(item.pos.z);
	writer.f32_(item.angle.getYaw());
	writer.raw<s16>(s16(item._itemdata->count));
	g_coop.sendToOthers(MessageType::DropItem, writer);
}

bool cinematicSpeechIsSomeoneElses() {
	return g_coop.isHost() && g_actingPlayer != InvalidPlayerId && g_actingPlayer != g_coop.localId();
}

void speechSkipped() {
	if(g_coop.isActive() && g_applyingRemote == 0) {
		g_coop.sendToOthers(MessageType::SpeechSkip, Writer());
	}
}

float damagePuppet(const Entity & puppet, float damage, unsigned type) {
	PlayerId owner = puppetOwner(puppet);
	if(owner == InvalidPlayerId || damage <= 0.f) {
		return 0.f;
	}
	Writer writer;
	writer.f32_(damage);
	writer.u32_(type);
	g_coop.sendTo(owner, MessageType::DamagePlayer, writer);
	return damage;
}

void forwardNpcDamage(const Entity & npc, float damage, unsigned type, const Vec3f * pos) {
	if(damage <= 0.f) {
		return;
	}
	Writer writer;
	writer.string(npc.idString());
	writer.f32_(damage);
	writer.u32_(type);
	writer.bool_(pos != nullptr);
	writer.f32_(pos ? pos->x : 0.f);
	writer.f32_(pos ? pos->y : 0.f);
	writer.f32_(pos ? pos->z : 0.f);
	g_coop.sendToHost(MessageType::DamageNpc, writer);
}

bool interceptScriptEvent(Entity * sender, Entity * entity, const ScriptEventName & event,
                          const ScriptParameters & parameters, ScriptResult & result) {

	if(!g_coop.isClient() || g_coop.state() != State::InGame || g_applyingRemote > 0 || !entity
	   || !g_playthroughStarted) {
		return false; // before the co-op game starts (menu, intro) everything runs locally
	}

	if(isPlayerSide(entity)) {
		return false;
	}

	ScriptMessage id = event.getId();

	// Level load / appearance setup and local cinematic ends are fine to run locally
	// ("reload" is not: the host runs it and replicates the effects, see Kultar's DESTROY SELF)
	if(id == SM_INIT || id == SM_INITEND || id == SM_LOAD || id == SM_CINE_END) {
		return false;
	}

	// Everything else about world entities is the host's business
	if(forwardedEvents().count(id) && isPlayerSide(sender)) {
		forwardEvent(sender, entity, event, parameters);
	}

	result = ACCEPT;
	return true;
}

CommandSync commandSync(std::string_view command, const script::Context & context) {

	if(!g_coop.isHost() || g_applyingRemote > 0 || g_creatingProxy || g_levelLoading > 0
	   || context.getParameters().isPeekOnly()) {
		return CommandSync::Local;
	}

	const Entity * entity = context.getEntity();
	if(!entity) {
		return CommandSync::Local;
	}

	// Commands in the context of a client's item stand-in belong to that client
	if(entity->coopProxy) {
		return (g_actingPlayer != InvalidPlayerId && g_actingPlayer != g_coop.localId())
		       ? CommandSync::Redirect : CommandSync::Local;
	}

	if(isPlayerSide(entity)) {
		return CommandSync::Local; // the host's own character
	}

	auto it = commandTable().find(std::string(command));
	if(it == commandTable().end()) {
		return CommandSync::Local;
	}

	Category category = it->second;
	if(category == Category::World && peekPlayerDirected(command, context)) {
		category = Category::Player;
	}

	if(category == Category::Player) {
		if(g_actingPlayer == InvalidPlayerId) {
			// Scripted for "the player" with nobody in particular behind it (zone the host walked
			// into, timer...): that is the host. Only teleports move everyone.
			return command == "teleport" ? CommandSync::Replicate : CommandSync::Local;
		}
		return g_actingPlayer != g_coop.localId() ? CommandSync::Redirect : CommandSync::Local;
	}

	return CommandSync::Replicate;
}

void commandReplicated(std::string_view command, const std::vector<std::string> & words,
                       const script::Context & context) {

	const Entity * entity = context.getEntity();

	if(command == "spawn") {
		if(LASTSPAWNED && ValidIOAddress(LASTSPAWNED)) {
			sendSpawn(*LASTSPAWNED);
		}
		return;
	}

	if(command == "unset" && (words.empty() || isLocalVariable(words[0]))) {
		return; // local variables of world entities are not needed on the clients
	}

	std::vector<std::string> sent = words;
	if((command == "playanim" || command == "forceanim") && !sent.empty() && sent[0].size() > 1 && sent[0][0] == '-') {
		// The -e "execute when done" part of the line stays on the host
		sent[0].erase(std::remove(sent[0].begin(), sent[0].end(), 'e'), sent[0].end());
		if(sent[0] == "-") {
			sent.erase(sent.begin());
		}
	}

	if(command == "speak" && !sent.empty() && sent[0].size() > 1 && sent[0][0] == '-'
	   && sent[0].find('c') != std::string::npos) {
		// Cinematic camera only for the acting player (or the host when nobody in particular
		// triggered it); the others just hear the line.
		std::vector<std::string> plain = stripCinematicSpeech(sent);
		for(const Player & player : g_coop.players()) {
			if(player.id == g_coop.localId()) {
				continue;
			}
			sendScriptCommand(player.id, entity->idString(), command, player.id == g_actingPlayer ? sent : plain);
		}
		return;
	}

	sendScriptCommand(InvalidPlayerId, entity->idString(), command, sent);
}

void commandRedirected(std::string_view command, script::Context & context) {

	// Read the parameters the way the command would, resolving variables, without executing it
	std::vector<std::string> words;
	std::string_view data = context.getScript()->data;
	for(;;) {
		size_t pos = context.getPosition();
		while(pos < data.size() && (data[pos] == ' ' || data[pos] == '\t')) {
			pos++;
		}
		if(pos >= data.size() || data[pos] == '\n' || data[pos] == '\r') {
			break;
		}
		std::string word = context.getWord();
		if(!word.empty() && (word[0] == '^' || word[0] == '#' || word[0] == '&' || word[0] == '$'
		                     || word[0] == '\xA7' || word[0] == '@' || word[0] == '\xA3')) {
			word = context.getStringVar(word);
		}
		words.push_back(std::move(word));
		if(context.getPosition() == pos) {
			break; // no progress, avoid looping forever
		}
	}
	context.skipCommand();

	// For a client's item stand-in, the client has the real item under the same id
	const Entity * entity = context.getEntity();
	std::string entityId = entity ? entity->idString() : std::string();
	sendScriptCommand(g_actingPlayer, entityId, command, words);
	LogDebug("[coop] redirected to player " << int(g_actingPlayer) << ": " << buildLine(command, words));
}

void globalVariableChanged(std::string_view name, const SCRIPT_VAR & var) {
	if(!g_coop.isActive() || g_applyingRemote > 0) {
		return;
	}
	Writer writer;
	writer.string(name);
	if(!name.empty() && name[0] == '$') {
		writer.u8_(0);
		writer.string(var.text);
	} else if(!name.empty() && name[0] == '#') {
		writer.u8_(1);
		writer.s32_(s32(var.ival));
	} else {
		writer.u8_(2);
		writer.f32_(var.fval);
	}
	g_coop.sendToOthers(MessageType::SetGlobal, writer);
}

void sharedQuestAdded(std::string_view quest) {
	if(g_coop.isActive() && g_applyingRemote == 0) {
		Writer writer;
		writer.string(quest);
		g_coop.sendToOthers(MessageType::SharedQuest, writer);
	}
}

void sharedKeyAdded(std::string_view key) {
	if(g_coop.isActive() && g_applyingRemote == 0) {
		Writer writer;
		writer.string(key);
		g_coop.sendToOthers(MessageType::SharedKey, writer);
	}
}

void sharedRuneAdded(unsigned rune) {
	if(g_coop.isActive() && g_applyingRemote == 0) {
		Writer writer;
		writer.u32_(rune);
		g_coop.sendToOthers(MessageType::SharedRune, writer);
	}
}

void sharedExperience(long amount) {
	if(g_coop.isActive() && g_applyingRemote == 0 && amount != 0) {
		Writer writer;
		writer.s32_(s32(amount));
		g_coop.sendToOthers(MessageType::SharedXP, writer);
	}
}

} // namespace coop
