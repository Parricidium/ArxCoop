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
#include <ctime>
#include <map>
#include <set>
#include <utility>

#include <zlib.h>

#include "coop/Protocol.h"
#include "coop/Admin.h"
#include "coop/Qol.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Damage.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "physics/Physics.h"
#include "scene/GameSound.h"
#include "game/Item.h"
#include "game/Player.h"
#include "game/magic/Spell.h"
#include "game/Spells.h"
#include "coop/Puppets.h"
#include "gui/Dragging.h"
#include "gui/Menu.h"
#include "gui/book/Book.h"
#include "cinematic/CinematicController.h"
#include "core/Config.h"
#include "gui/Menu.h"
#include "core/Core.h"
#include "core/SaveGame.h"
#include "graphics/Renderer.h"
#include "gui/MenuWidgets.h"
#include "platform/Time.h"
#include "scene/ChangeLevel.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"
#include "script/Script.h"
#include "scene/Interactive.h"
#include "audio/AudioTypes.h"
#include "script/ScriptEvent.h"
#include "script/ScriptUtils.h"
#include "gui/Speech.h"
#include "gui/CinematicBorder.h"
#include "util/Number.h"
#include "util/String.h"

#include <boost/algorithm/string/trim.hpp>

extern Entity * LASTSPAWNED;
extern bool REQUEST_SPEECH_SKIP;

namespace coop {

// Container transfers (defined with the other item transfers below)
void applyStoreItem(PlayerId from, Reader & reader);
void applySetCount(PlayerId from, Reader & reader);
void applyInventoryAdd(Reader & reader);
void sendInventoryAdd(const Entity & container, const Entity & item);

namespace {

PlayerId g_actingPlayer = InvalidPlayerId; //!< Host: the player whose action is being processed, if any

/*!
 * Host: a script moved us around without a player behind it (intro, cutscene staging): once the
 * scene is over the teammates left behind are brought to us. Also used after a level change, once
 * we have landed, for the clients placed from a position we had not settled at yet.
 */
bool g_gatherPending = false;
std::map<PlayerId, PlatformInstant> g_settleTeleports;
constexpr PlatformDuration SettleDelay = std::chrono::milliseconds(3000);
constexpr float GatherDistance = 300.f;
bool g_playthroughStarted = false;         //!< Client: our own character is ready for the shared world
int g_levelLoading = 0;                    //!< > 0 while a level is being loaded
int g_applyingRemote = 0;      //!< > 0 while applying something received from the network
bool g_creatingProxy = false;  //!< Host: suppress replication while a proxy item initializes
std::set<std::string> g_inFlight;    //!< Items we threw and that are still flying
std::string g_lastDragId;            //!< Carried item last streamed to the others
bool g_lastDragInScene = false;
PlatformInstant g_lastDragSend;
constexpr PlatformDuration DragSendInterval = std::chrono::milliseconds(50);

enum class Category {
	World,  //!< Affects the shared world: executed on the host, replayed on every client
	Player, //!< Affects "the player": executed by the acting player only
};

const std::map<std::string, Category> & commandTable() {
	static const std::map<std::string, Category> table = {
		// World
		{ "setevent", Category::World }, { "objecthide", Category::World }, { "destroy", Category::World },
		{ "teleport", Category::World }, { "playanim", Category::World }, { "forceanim", Category::World },
		{ "loadanim", Category::World },
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
		{ "equip", Category::Player }, { "weapon", Category::Player }, { "repair", Category::Player },
		{ "skin", Category::World }, { "dodamage", Category::World }, { "damager", Category::World },
		{ "setblood", Category::World }, { "setspeed", Category::World }, { "setstarefactor", Category::World },
		{ "setircolor", Category::World }, { "setweight", Category::World }, { "unset", Category::World },
		{ "spawn", Category::World },
		{ "set", Category::World }, { "inc", Category::World }, { "dec", Category::World },
		{ "mul", Category::World }, { "div", Category::World },
		// Player
		{ "book", Category::Player }, { "note", Category::Player },
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
	if(io == entities.player() || io->coopProxy || isEquippedByPlayer(io)) {
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
	if(command != "inventory" && command != "teleport" && command != "speak" && command != "dodamage"
	   && command != "playanim" && command != "forceanim" && command != "loadanim") {
		return false;
	}
	// Read the first word the way the command will, without consuming it (a copy of the
	// context does the variable expansion: "speak -~^$param2~" is not "speak -p")
	script::Context probe(context);
	probe.setTranscript(nullptr);
	std::string first = probe.getWord();
	if(command == "inventory") {
		return first == "playeradd" || first == "playeraddfromscene" || first == "playeraddmulti";
	}
	if(command == "dodamage") {
		return first == "player";
	}
	return first.size() > 1 && first[0] == '-' && first.find('p') != std::string_view::npos;
}

//! The flag word of a command line ("-pe"), or empty, without consuming it.
std::string peekFlags(const script::Context & context) {
	script::Context probe(context);
	probe.setTranscript(nullptr);
	std::string first = probe.getWord();
	return (first.size() > 1 && first[0] == '-') ? first : std::string();
}

//! Whether a teleport command line changes level (-l flag), as opposed to a same-level placement.
bool peekTeleportChangesLevel(const script::Context & context) {
	script::Context probe(context);
	probe.setTranscript(nullptr);
	std::string first = probe.getWord();
	return first.size() > 1 && first[0] == '-' && first.find('l') != std::string_view::npos;
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

//! Inserts "-v N" (voice line variant) right after the flags of a recorded speak command.
void addSpeechVariant(std::vector<std::string> & words, long variant) {
	if(!words.empty() && words[0].size() > 1 && words[0][0] == '-') {
		words[0] += 'v';
	} else {
		words.insert(words.begin(), "-v");
	}
	words.insert(words.begin() + 1, std::to_string(variant));
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
	if(line.rfind("spellcast", 0) == 0) {
		LogInfo << "[coop] spell replayed on " << entity->idString() << ": " << line;
	} else if(line.rfind("playanim", 0) != 0 && line.rfind("forceanim", 0) != 0 && line.rfind("settarget", 0) != 0
	          && line.rfind("behavior", 0) != 0 && line.rfind("setmovemode", 0) != 0 && line.rfind("set ", 0) != 0) {
		LogInfo << "[coop] replayed on " << entity->idString() << ": " << line; // diagnostic trail
	}

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

/*!
 * Items placed in a level carry an instance script (note_0003/note.asl: the text of that note,
 * the spell of that scroll...). AddItem() only loads the class script: give a copy created from
 * another player's item the same instance script its original has.
 */
void loadInstanceScript(Entity & io) {
	if(PakDirectory * dir = g_resources->getDirectory(io.instancePath())) {
		loadScript(io.over_script, dir->getFile(io.className() + ".asl"));
	}
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
		writer.bool_(sender->over_script.valid);
	} else {
		writer.string("player");
		writer.string("");
		writer.s32_(0);
		writer.bool_(false);
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
	bool senderHasInstanceScript = reader.bool_();

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
			if(proxy && senderHasInstanceScript) {
				loadInstanceScript(*proxy);
				SendInitScriptEvent(proxy);
			}
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
	LogInfo << "[coop] player " << int(from) << " -> " << event << " on " << entity->idString();

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
			if(std::find(g_playerQuestLogEntries.begin(), g_playerQuestLogEntries.end(), quest) == g_playerQuestLogEntries.end()) {
				ARX_PLAYER_Quest_Add(quest);
			}
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
		case MessageType::SharedGold: {
			s32 amount = reader.s32_();
			ARX_PLAYER_AddGold(long(amount));
			relay.s32_(amount);
			break;
		}
		case MessageType::SharedBag: {
			ARX_PLAYER_AddBag();
			LogInfo << "[coop] another player found a backpack: extra inventory for everyone";
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
	writer.s32_(s32(player.xp));
	writer.u8_(u8(entities.player() ? entities.player()->inventory->bags() : 1));
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
	// Progress is shared as it happens; someone who joins later starts from the party's level
	s32 xp = reader.s32_();
	u8 bags = reader.u8_();
	if(player.xp < long(xp)) {
		LogInfo << "[coop] world sync: catching up " << (long(xp) - player.xp) << " xp";
		ARX_PLAYER_Modify_XP(long(xp) - player.xp);
	}
	if(entities.player()) {
		while(entities.player()->inventory->bags() < size_t(bags) && entities.player()->inventory->bags() < 3) {
			ARX_PLAYER_AddBag();
			LogInfo << "[coop] world sync: extra inventory bag";
		}
	}
	g_applyingRemote--;
	LogInfo << "[coop] world sync: " << quests << " quests, " << keys << " keys, " << xp << " xp, " << int(bags) << " bags";
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
	// Persistent magic fields (the "blue walls") are spells cast by markers on game_ready, an
	// event clients never run, and spells are not part of the level state: recast them there.
	for(const Spell & spell : spells.ofType(SPELL_CREATE_FIELD)) {
		const Entity * caster = entities.get(spell.m_caster);
		if(!caster || caster == entities.player() || caster->coopPuppet) {
			continue;
		}
		sendScriptCommand(to, caster->idString(), "spellcast",
		                  { "-msfdz", "-1", std::to_string(std::max(1, int(spell.m_level))), "create_field", "self" });
		LogInfo << "[coop] recast the field of " << caster->idString() << " for player " << int(to);
	}
	if(!restoreRejoiningPlayer(to)) { // back where it left, if it left from this level
		g_settleTeleports[to] = platform::getTime() + SettleDelay; // else next to us once we have landed
	}
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
	// Safety net: the transition protection (invulnerability -p on) may have been saved with our
	// character before its lift reached us; a level arrival always ends it
	if(player.playerflags & PLAYERFLAGS_INVULNERABILITY) {
		player.playerflags &= ~PLAYERFLAGS_INVULNERABILITY;
		LogInfo << "[coop] invulnerability left over from a transition: cleared";
	}
	g_levelSynced = ok;
	if(!ok) {
		LogError << "[coop] failed to load the host's level state";
	}
}

void applyDamagePlayer(Reader & reader) {
	PlayerId target = reader.u8_();
	float damage = reader.f32_();
	u32 type = reader.u32_();
	if(target != g_coop.localId()) {
		if(g_coop.isHost()) {
			Writer writer;
			writer.u8_(target);
			writer.f32_(damage);
			writer.u32_(type);
			g_coop.sendTo(target, MessageType::DamagePlayer, writer);
		}
		return;
	}
	g_applyingRemote++;
	float done = damagePlayer(damage, DamageType::load(type), nullptr);
	g_applyingRemote--;
	LogInfo << "[coop] took " << damage << " damage from the host's world" << (done > 0.f ? "" : " (ignored: invulnerable or dead)");
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
	saveMyCharacter(reader.string());
}

void applyLoadRequest(Reader & reader) {
	std::string name = coopSaveName(reader.string());
	SavegameHandle save = findSaveByName(name);
	bool wasInLobby = g_coop.state() == State::Lobby;
	if(wasInLobby) {
		g_coop.resumeFromHost();
	}
	if(save == SavegameHandle()) {
		if(wasInLobby) {
			LogInfo << "[coop] the host resumed \"" << name << "\" and I have no such save: new character";
			puppetsReset();
			if(!cinematicIsStopped()) {
				cinematicEnd();
			}
			ARX_MENU_Clicked_NEWQUEST();
			return;
		}
		LogInfo << "[coop] the host loaded \"" << name << "\" but I have no such save: keeping my character";
		g_levelSynced = false; // the host's world will be fetched again anyway
		return;
	}
	if(wasInLobby) {
		puppetsReset();
		if(!cinematicIsStopped()) {
			cinematicEnd();
		}
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
	if(&item == g_draggedEntity) {
		setDraggedEntity(nullptr);
	}
	// Its owner's machine has the real one now: keep this copy out of saves and level states,
	// otherwise the owner would get a duplicate back with the next level sync
	item.ioflags |= IO_NOSAVE;
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

struct ItemPlacement {
	std::string id;
	res::path classPath;
	EntityInstance instance = 0;
	Vec3f pos = Vec3f(0.f);
	Anglef angle;
	bool hasInstanceScript = false;
};

ItemPlacement readItemPlacement(Reader & reader) {
	ItemPlacement placement;
	placement.id = reader.string();
	placement.classPath = res::path::load(reader.string());
	placement.instance = reader.s32_();
	placement.pos = reader.vec3<Vec3f>();
	float pitch = reader.f32_();
	float yaw = reader.f32_();
	float roll = reader.f32_();
	placement.angle = Anglef(pitch, yaw, roll);
	placement.hasInstanceScript = reader.bool_();
	return placement;
}

void writeItemPlacement(Writer & writer, const ItemPlacement & placement) {
	writer.string(placement.id);
	writer.string(placement.classPath.string());
	writer.s32_(placement.instance);
	writer.f32_(placement.pos.x);
	writer.f32_(placement.pos.y);
	writer.f32_(placement.pos.z);
	writer.f32_(placement.angle.getPitch());
	writer.f32_(placement.angle.getYaw());
	writer.f32_(placement.angle.getRoll());
	writer.bool_(placement.hasInstanceScript);
}

//! The world item another player is handling, created on the spot if we do not have it yet.
Entity * placedItem(const ItemPlacement & placement, bool create) {
	Entity * item = entities.getById(placement.id);
	if(!item && create) {
		item = AddItem(placement.classPath, placement.instance, IO_IMMEDIATELOAD);
		if(item) {
			item->scriptload = 1;
			if(placement.hasInstanceScript) {
				loadInstanceScript(*item);
			}
			SendInitScriptEvent(item);
		}
	}
	if(!item || isPlayerSide(item)) {
		return nullptr;
	}
	removeFromInventories(item);
	if(create) {
		item->ioflags &= ~IO_NOSAVE; // back in the shared world
	}
	item->pos = placement.pos;
	item->angle = placement.angle;
	item->requestRoomUpdate = true;
	item->gameFlags &= ~GFLAG_NOCOMPUTATION;
	return item;
}

void writeItemPlacement(Writer & writer, const Entity & item) {
	writer.string(item.idString());
	writer.string(item.classPath().string());
	writer.s32_(item.instance());
	writer.f32_(item.pos.x);
	writer.f32_(item.pos.y);
	writer.f32_(item.pos.z);
	writer.f32_(item.angle.getPitch());
	writer.f32_(item.angle.getYaw());
	writer.f32_(item.angle.getRoll());
	writer.bool_(item.over_script.valid);
}

bool sharedWorldItem(const Entity & item) {
	return g_coop.isActive() && g_coop.state() == State::InGame && g_applyingRemote == 0 && (item.ioflags & IO_ITEM)
	       && !(item.ioflags & IO_NOSAVE) && !item.coopPuppet;
}

//! Thrown items: once ours comes to rest, tell the others where it ended up.
void settleThrownItems() {
	for(auto it = g_inFlight.begin(); it != g_inFlight.end(); ) {
		Entity * item = entities.getById(*it);
		if(!item || item->show != SHOW_FLAG_IN_SCENE || !item->obj || !item->obj->pbox) {
			it = g_inFlight.erase(it);
			continue;
		}
		if(item->obj->pbox->active == 1) {
			++it;
			continue;
		}
		Writer writer;
		writeItemPlacement(writer, *item);
		writer.raw<s16>(s16(item->_itemdata->count));
		writer.bool_(false);
		writer.f32_(0.f);
		writer.f32_(0.f);
		writer.f32_(0.f);
		g_coop.sendToOthers(MessageType::DropItem, writer);
		LogInfo << "[coop] my thrown " << item->idString() << " came to rest at " << int(item->pos.x) << "," << int(item->pos.y) << "," << int(item->pos.z)
		        << " (pbox " << int(item->obj->pbox->active) << ")";
		it = g_inFlight.erase(it);
	}
}

void applyTeleportPlayer(PlayerId from, Reader & reader) {
	PlayerId target = reader.u8_();
	u32 area = reader.u32_();
	Vec3f pos = reader.vec3<Vec3f>();
	float yaw = reader.f32_();
	if(target != g_coop.localId()) {
		if(g_coop.isHost() && g_coop.player(target)) {
			Writer writer;
			writer.u8_(target);
			writer.u32_(area);
			writer.f32_(pos.x);
			writer.f32_(pos.y);
			writer.f32_(pos.z);
			writer.f32_(yaw);
			g_coop.sendTo(target, MessageType::TeleportPlayer, writer);
		}
		return;
	}
	const Player * who = g_coop.player(from);
	std::string name = who ? who->name : std::string("?");
	if(!inLevel() || area != g_currentArea.handleData()) {
		ARX_LOG(Logger::Console) << "[coop] " << name << tr("coop_teleport_other_level", " veut me t\xC3\xA9l\xC3\xA9porter mais n'est pas dans le m\xC3\xAAme niveau");
		return;
	}
	LogInfo << "[coop] teleport request from player " << int(from);
	Logger::flush();
	ARX_INTERACTIVE_Teleport(entities.player(), pos);
	player.desiredangle.setYaw(yaw);
	player.angle.setYaw(yaw);
	ARX_LOG(Logger::Console) << "[coop] " << tr("coop_teleported_to", "t\xC3\xA9l\xC3\xA9port\xC3\xA9 vers ") << name;
	LogInfo << "[coop] teleported to player " << int(from);
}

void applyDragItem(PlayerId from, Reader & reader) {
	ItemPlacement placement = readItemPlacement(reader);
	bool inScene = reader.bool_();
	g_applyingRemote++;
	if(Entity * item = placedItem(placement, false)) {
		item->show = inScene ? SHOW_FLAG_IN_SCENE : SHOW_FLAG_HIDDEN;
		if(item->obj && item->obj->pbox) {
			item->obj->pbox->active = 0; // in someone's hand: no physics
		}
	}
	g_applyingRemote--;
	if(g_coop.isHost()) {
		Writer writer;
		writeItemPlacement(writer, placement);
		writer.bool_(inScene);
		g_coop.broadcast(MessageType::DragItem, writer, from);
	}
}

void applyDropItem(PlayerId from, Reader & reader) {
	ItemPlacement placement = readItemPlacement(reader);
	s16 count = reader.raw<s16>();
	bool thrown = reader.bool_();
	Vec3f direction = reader.vec3<Vec3f>();
	g_applyingRemote++;
	if(Entity * item = placedItem(placement, true)) {
		item->show = SHOW_FLAG_IN_SCENE;
		if((item->ioflags & IO_ITEM) && count > 0) {
			item->_itemdata->count = count;
		}
		if(item->obj && item->obj->pbox) {
			if(thrown) {
				EERIE_PHYSICS_BOX_Launch(item->obj, item->pos, item->angle, direction, item);
				ARX_SOUND_PlaySFX(g_snd.WHOOSH, &item->pos);
			} else {
				item->obj->pbox->active = 0; // at rest where its owner says it is
			}
		}
		g_inFlight.erase(item->idString());
		LogInfo << "[coop] " << item->idString() << " was " << (thrown ? "thrown" : "placed") << " by another player at "
		        << int(item->pos.x) << "," << int(item->pos.y) << "," << int(item->pos.z) << " pbox " << (item->obj && item->obj->pbox ? int(item->obj->pbox->active) : -1);
	}
	g_applyingRemote--;
	if(g_coop.isHost()) {
		Writer writer;
		writeItemPlacement(writer, placement);
		writer.raw<s16>(count);
		writer.bool_(thrown);
		writer.f32_(direction.x);
		writer.f32_(direction.y);
		writer.f32_(direction.z);
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
		case MessageType::StoreItem: {
			applyStoreItem(from, reader);
			break;
		}
		case MessageType::SetCount: {
			applySetCount(from, reader);
			break;
		}
		case MessageType::InventoryAdd: {
			if(g_coop.isClient() && g_levelSynced) {
				applyInventoryAdd(reader);
			}
			break;
		}
		case MessageType::DragItem: {
			applyDragItem(from, reader);
			break;
		}
		case MessageType::TeleportPlayer: {
			applyTeleportPlayer(from, reader);
			break;
		}
		case MessageType::GiveGold: {
			handleGiveGold(from, reader);
			break;
		}
		case MessageType::GiveItem: {
			handleGiveItem(from, reader);
			break;
		}
		case MessageType::Latency: {
			if(g_coop.isClient()) {
				handleLatencies(reader);
			}
			break;
		}
		case MessageType::Sound: {
			if(g_coop.isClient()) {
				applySound(reader);
			}
			break;
		}
		case MessageType::AdminGrant: {
			if(g_coop.isClient()) {
				applyAdminGrant(reader);
			}
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
			applyDamagePlayer(reader);
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
		case MessageType::SharedXP:
		case MessageType::SharedGold:
		case MessageType::SharedBag: {
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

//! Host: sends a teammate to our side (a little behind us so nobody stands inside anybody).
void gatherTeammate(PlayerId id, size_t slot, const char * why) {
	Vec3f side = angleToVectorXZ(player.angle.getYaw() + 90.f);
	Vec3f back = -angleToVectorXZ(player.angle.getYaw());
	Vec3f pos = entities.player()->pos + back * 60.f + side * (float(slot) - 0.5f) * 70.f;
	Writer writer;
	writer.u8_(id);
	writer.u32_(g_currentArea.handleData());
	writer.f32_(pos.x);
	writer.f32_(pos.y);
	writer.f32_(pos.z);
	writer.f32_(player.angle.getYaw());
	g_coop.sendTo(id, MessageType::TeleportPlayer, writer);
	LogInfo << "[coop] gathering player " << int(id) << " to us (" << why << ")";
}

void gatherUpdate() {
	if(!inLevel() || !entities.player()) {
		return;
	}
	bool sceneOver = !BLOCK_PLAYER_CONTROLS && !cinematicBorder.isActive() && !getCinematicSpeech();
	if(!sceneOver) {
		return; // wait until the scripted scene is over (and we have been put at our final spot)
	}
	PlatformInstant now = platform::getTime();
	size_t slot = 0;
	if(g_gatherPending) {
		g_gatherPending = false;
		for(const TeammateInfo & mate : teammates()) {
			if(mate.here && glm::distance(mate.pos, entities.player()->pos) > GatherDistance) {
				gatherTeammate(mate.id, slot++, "scripted scene");
			}
		}
	}
	for(auto it = g_settleTeleports.begin(); it != g_settleTeleports.end(); ) {
		if(now < it->second) {
			++it;
			continue;
		}
		for(const TeammateInfo & mate : teammates()) {
			if(mate.id == it->first && mate.here && glm::distance(mate.pos, entities.player()->pos) > GatherDistance) {
				gatherTeammate(mate.id, slot++, "level change");
			}
		}
		it = g_settleTeleports.erase(it);
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

	if(inLevel()) {
		settleThrownItems();
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
		gatherUpdate();
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

unsigned currentActor() {
	return g_coop.isHost() ? g_actingPlayer : InvalidPlayerId;
}

PlayerActorScope::PlayerActorScope(unsigned actor) {
	if(g_coop.isHost() && g_applyingRemote == 0 && actor != InvalidPlayerId && g_coop.player(PlayerId(actor))) {
		m_active = true;
		m_previous = g_actingPlayer;
		g_actingPlayer = PlayerId(actor);
	}
}

PlayerActorScope::~PlayerActorScope() {
	if(m_active) {
		g_actingPlayer = PlayerId(m_previous);
	}
}

PuppetActorScope::PuppetActorScope(const Entity * io) {
	if(g_coop.isHost() && g_applyingRemote == 0 && io && io->coopPuppet) {
		PlayerId owner = puppetOwner(*io);
		if(owner != InvalidPlayerId) {
			m_active = true;
			m_previous = g_actingPlayer;
			g_actingPlayer = owner;
		}
	}
}

PuppetActorScope::~PuppetActorScope() {
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
	if(!g_coop.isHost()) {
		return;
	}
	if(g_coop.state() == State::Lobby) {
		g_coop.resumeFromSave(); // the host resumed a saved game: this is the start of the session
	}
	if(g_coop.state() == State::InGame) {
		Writer writer;
		writer.string(name);
		g_coop.broadcast(MessageType::LoadRequest, writer);
	}
}

// Sounds ------------------------------------------------------------------------------------

int g_replicatingCommand = 0;

void replicatedCommandBegin() {
	g_replicatingCommand++;
}

void replicatedCommandEnd() {
	g_replicatingCommand--;
}

bool teleportPlayerToMe(PlayerId id, size_t slot) {
	// Not inLevel(): the host uses this from its menu, where its world keeps running
	if(!g_coop.isHost() || !g_currentArea || !entities.player() || !g_coop.player(id)) {
		return false;
	}
	gatherTeammate(id, slot, "admin");
	return true;
}

bool broadcastSounds() {
	return g_coop.isHost() && g_coop.state() == State::InGame && g_playthroughStarted && g_replicatingCommand == 0
	       && g_applyingRemote == 0 && g_levelLoading == 0 && !g_coop.players().empty() && g_coop.players().size() > 1;
}

void soundPlayed(std::string_view sample, const Vec3f & pos, float pitch) {
	if(!broadcastSounds() || sample.empty()) {
		return;
	}
	Writer writer;
	writer.u8_(0);
	writer.string(sample);
	writer.f32_(pos.x);
	writer.f32_(pos.y);
	writer.f32_(pos.z);
	writer.f32_(pitch);
	writer.f32_(1.f);
	g_coop.broadcast(MessageType::Sound, writer);
}

void collisionSoundPlayed(int mat1, int mat2, float volume, const Vec3f & pos) {
	if(!broadcastSounds()) {
		return;
	}
	Writer writer;
	writer.u8_(1);
	writer.u8_(u8(mat1));
	writer.u8_(u8(mat2));
	writer.f32_(pos.x);
	writer.f32_(pos.y);
	writer.f32_(pos.z);
	writer.f32_(1.f);
	writer.f32_(volume);
	g_coop.broadcast(MessageType::Sound, writer);
}

std::map<std::string, audio::SampleHandle> g_sampleCache;

void applySound(Reader & reader) {
	u8 kind = reader.u8_();
	if(kind == 0) {
		std::string name = reader.string();
		Vec3f pos = reader.vec3<Vec3f>();
		float pitch = reader.f32_();
		reader.f32_();
		auto it = g_sampleCache.find(name);
		if(it == g_sampleCache.end()) {
			it = g_sampleCache.emplace(name, ARX_SOUND_Load(res::path::load(name))).first;
		}
		if(it->second != audio::SampleHandle()) {
			g_applyingRemote++;
			ARX_SOUND_PlaySFX(it->second, &pos, pitch);
			g_applyingRemote--;
		}
	} else {
		int mat1 = reader.u8_();
		int mat2 = reader.u8_();
		Vec3f pos = reader.vec3<Vec3f>();
		reader.f32_();
		float volume = reader.f32_();
		if(mat1 < MAX_MATERIALS && mat2 < MAX_MATERIALS) {
			g_applyingRemote++;
			ARX_SOUND_PlayCollision(Material(mat1), Material(mat2), volume, 1.f, pos, nullptr);
			g_applyingRemote--;
		}
	}
}

void saveMyCharacter(std::string_view hostName) {
	std::string name = coopSaveName(hostName);
	if(!inLevel()) {
		return;
	}
	g_hostDrivenSaveLoad = true;
	GRenderer->getSnapshot(savegame_thumbnail, config.interface.thumbnailSize.x, config.interface.thumbnailSize.y);
	bool ok = savegames.save(name, findSaveByName(name), savegame_thumbnail);
	g_hostDrivenSaveLoad = false;
	LogInfo << "[coop] saved my character as \"" << name << "\"" << (ok ? "" : " (failed)");
}

bool hostDrivenSaveLoad() {
	return g_hostDrivenSaveLoad;
}

bool loadSavedCoopCharacter() {
	SavegameHandle best;
	std::time_t newest = 0;
	for(size_t i = 0; i < savegames.size(); i++) {
		const SaveGame & save = savegames[SavegameHandle(long(i))];
		if(save.name.compare(0, 6, "coop: ") == 0 && save.stime >= newest) {
			newest = save.stime;
			best = SavegameHandle(long(i));
		}
	}
	if(best == SavegameHandle()) {
		return false;
	}
	LogInfo << "[coop] joining with my saved character \"" << savegames[best].name << "\"";
	g_hostDrivenSaveLoad = true;
	ARX_LoadGame(savegames[best]);
	g_hostDrivenSaveLoad = false;
	playthroughStarted();
	return true;
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

void itemDropped(const Entity & item, bool thrown, const Vec3f & direction) {
	if(!sharedWorldItem(item)) {
		return;
	}
	Writer writer;
	writeItemPlacement(writer, item);
	writer.raw<s16>(s16(item._itemdata->count));
	writer.bool_(thrown);
	writer.f32_(direction.x);
	writer.f32_(direction.y);
	writer.f32_(direction.z);
	g_coop.sendToOthers(MessageType::DropItem, writer);
	if(thrown) {
		g_inFlight.insert(item.idString()); // everyone simulates the flight, we say where it lands
	} else {
		g_inFlight.erase(item.idString());
	}
	g_lastDragSend = PlatformInstant();
}

// Containers (chests, merchants, corpses) -----------------------------------------------

struct ContainerDrop {
	std::string container;
	std::string item;
	std::map<std::string, s16> counts; //!< stacks in the container before the drop
	bool active = false;
};
ContainerDrop g_containerDrop;

std::map<std::string, s16> containerCounts(const Entity & container) {
	std::map<std::string, s16> counts;
	if(!container.inventory) {
		return counts;
	}
	for(auto slot : container.inventory->slotsInOrder()) {
		if(slot.show && slot.entity && (slot.entity->ioflags & IO_ITEM)) {
			counts[slot.entity->idString()] = slot.entity->_itemdata->count;
		}
	}
	return counts;
}

void sendItemCount(const Entity & item) {
	Writer writer;
	writer.string(item.idString());
	writer.raw<s16>(item._itemdata->count);
	g_coop.sendToOthers(MessageType::SetCount, writer);
	LogInfo << "[coop] " << item.idString() << " now x" << item._itemdata->count << " for everyone";
}

ContainerDropScope::ContainerDropScope(const Entity & container, const Entity * item) {
	g_containerDrop.active = false;
	if(!g_coop.isActive() || g_coop.state() != State::InGame || g_applyingRemote > 0 || !item
	   || !(item->ioflags & IO_ITEM) || !container.inventory || isPlayerSide(&container) || container.coopPuppet) {
		return;
	}
	g_containerDrop.container = container.idString();
	g_containerDrop.item = item->idString();
	g_containerDrop.counts = containerCounts(container);
	g_containerDrop.active = true;
}

ContainerDropScope::~ContainerDropScope() {
	if(!g_containerDrop.active) {
		return;
	}
	g_containerDrop.active = false;
	Entity * container = entities.getById(g_containerDrop.container);
	if(!container || !container->inventory) {
		return;
	}
	Entity * item = entities.getById(g_containerDrop.item);
	if(item && (item->ioflags & IO_ITEM) && item->owner() == container) {
		InventoryPos pos = locateInInventories(item);
		Writer writer;
		writeItemPlacement(writer, *item);
		writer.raw<s16>(item->_itemdata->count);
		writer.string(container->idString());
		writer.raw<s16>(pos.bag);
		writer.raw<s16>(pos.x);
		writer.raw<s16>(pos.y);
		g_coop.sendToOthers(MessageType::StoreItem, writer);
		LogInfo << "[coop] stored " << item->idString() << " x" << item->_itemdata->count << " in " << container->idString()
		        << " at " << pos.bag << "/" << pos.x << "," << pos.y;
	}
	// Merged into a stack that was already there (sold potions on top of the merchant's...)
	for(const auto & entry : containerCounts(*container)) {
		auto before = g_containerDrop.counts.find(entry.first);
		if(before != g_containerDrop.counts.end() && before->second != entry.second && entry.first != g_containerDrop.item) {
			if(const Entity * stack = entities.getById(entry.first)) {
				sendItemCount(*stack);
			}
		}
	}
}

void itemCountChanged(const Entity & item) {
	if(!sharedWorldItem(item) || !item.owner() || isPlayerSide(item.owner()) || item.owner()->coopPuppet) {
		return;
	}
	sendItemCount(item);
}

void applyStoreItem(PlayerId from, Reader & reader) {
	ItemPlacement placement = readItemPlacement(reader);
	s16 count = reader.raw<s16>();
	std::string containerId = reader.string();
	s16 bag = reader.raw<s16>();
	s16 x = reader.raw<s16>();
	s16 y = reader.raw<s16>();
	Entity * container = entities.getById(containerId);
	if(container && container->inventory && !isPlayerSide(container)) {
		g_applyingRemote++;
		if(Entity * item = placedItem(placement, true)) {
			if((item->ioflags & IO_ITEM) && count > 0) {
				item->_itemdata->count = count;
			}
			if(item->obj && item->obj->pbox) {
				item->obj->pbox->active = 0;
			}
			bool ok = bag >= 0 && size_t(bag) < container->inventory->bags() && x >= 0 && y >= 0
			          && container->inventory->insertAtNoEvent(item, InventoryPos(container, Vec3s(x, y, bag)));
			if(!ok) {
				ok = container->inventory->insert(item);
			}
			// The insertion may have merged it into a stack (and deleted it): look it up again
			if(Entity * stored = entities.getById(placement.id)) {
				if(ok) {
					stored->show = SHOW_FLAG_IN_INVENTORY;
				} else {
					hideTakenItem(*stored); // no room: stays out of our copy of the world
				}
			}
			LogInfo << "[coop] " << placement.id << " x" << count << " was put in " << containerId << " by player " << int(from)
			        << (ok ? "" : " (no room here!)");
		}
		g_applyingRemote--;
	}
	if(g_coop.isHost()) {
		Writer writer;
		writeItemPlacement(writer, placement);
		writer.raw<s16>(count);
		writer.string(containerId);
		writer.raw<s16>(bag);
		writer.raw<s16>(x);
		writer.raw<s16>(y);
		g_coop.broadcast(MessageType::StoreItem, writer, from);
	}
}

void applySetCount(PlayerId from, Reader & reader) {
	std::string id = reader.string();
	s16 count = reader.raw<s16>();
	Entity * item = entities.getById(id);
	if(item && (item->ioflags & IO_ITEM) && !isPlayerSide(item)) {
		g_applyingRemote++;
		if(count <= 0) {
			hideTakenItem(*item);
		} else {
			item->_itemdata->count = count;
			LogInfo << "[coop] " << id << " now x" << count << " (player " << int(from) << ")";
		}
		g_applyingRemote--;
	}
	if(g_coop.isHost()) {
		Writer writer;
		writer.string(id);
		writer.raw<s16>(count);
		g_coop.broadcast(MessageType::SetCount, writer, from);
	}
}

//! Host: a script created an item straight into a container; the clients get the same id.
void sendInventoryAdd(const Entity & container, const Entity & item) {
	Writer writer;
	writer.string(container.idString());
	writer.string(item.classPath().string());
	writer.s32_(item.instance());
	writer.raw<s16>(item._itemdata->count);
	writer.s32_(s32(item._itemdata->price));
	g_coop.broadcast(MessageType::InventoryAdd, writer);
}

void applyInventoryAdd(Reader & reader) {
	std::string containerId = reader.string();
	res::path classPath = res::path::load(reader.string());
	EntityInstance instance = reader.s32_();
	s16 count = reader.raw<s16>();
	s32 price = reader.s32_();
	if(entities.getById(EntityId(classPath.filename(), instance).string())) {
		return;
	}
	Entity * container = entities.getById(containerId);
	if(!container || !container->inventory) {
		return;
	}
	g_applyingRemote++;
	if(Entity * item = AddItem(classPath, instance, IO_IMMEDIATELOAD)) {
		item->scriptload = 1;
		SendInitScriptEvent(item);
		if(item->ioflags & IO_GOLD) {
			item->_itemdata->price = price;
		} else if(count > 1) {
			item->_itemdata->maxcount = 9999;
			item->_itemdata->count = count;
		}
		if(!container->inventory->insert(item)) {
			item->destroy();
		} else {
			LogInfo << "[coop] script put " << EntityId(classPath.filename(), instance).string() << " x" << count << " in " << containerId;
		}
	}
	g_applyingRemote--;
}

void playerSpoke(const std::string & sample) {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || g_applyingRemote > 0 || sample.empty()) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.string(sample);
	g_coop.sendToOthers(MessageType::PlayerSpeech, writer);
}

bool nearTeammate(const Entity & entity, float limit) {
	if(!g_coop.isHost() || g_coop.state() != State::InGame) {
		return false;
	}
	return teammateWithin(entity.pos, limit);
}

bool keptOverLevelState(const Entity & io) {
	return g_coop.isClient() && g_coop.state() == State::InGame && isPlayerSide(&io);
}

//! "p2" / "j2" (lobby order), a nickname, or "all"/"tous"/nothing for everybody else
static std::vector<PlayerId> consoleTargets(const std::string & arg) {
	std::vector<PlayerId> targets;
	if(arg.empty() || arg == "all" || arg == "tous") {
		for(const Player & other : g_coop.players()) {
			if(other.id != g_coop.localId()) {
				targets.push_back(other.id);
			}
		}
		return targets;
	}
	std::string number = (arg[0] == 'p' || arg[0] == 'j') ? arg.substr(1) : arg;
	bool numeric = !number.empty() && std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
	for(const Player & other : g_coop.players()) {
		if((numeric && other.id == PlayerId(util::toInt(number).value_or(0) - 1))
		   || util::toLowercase(other.name) == arg) {
			targets.push_back(other.id);
		}
	}
	return targets;
}

bool consoleCommand(std::string_view line) {
	std::string text = util::toLowercase(std::string(boost::trim_copy(std::string(line))));
	bool teleport = (text == "tp" || text.compare(0, 3, "tp ") == 0);
	bool gold = (text.compare(0, 3, "or ") == 0 || text.compare(0, 5, "gold ") == 0);
	if(!teleport && !gold) {
		return false;
	}
	if(!g_coop.isActive() || g_coop.state() != State::InGame || !inLevel()) {
		ARX_LOG(Logger::Console) << "[coop] pas de partie coop en cours";
		return true;
	}
	if(gold) {
		// or <joueur> <montant>
		std::string rest = boost::trim_copy(text.substr(text.find(' ') + 1));
		size_t space = rest.find(' ');
		std::string who = boost::trim_copy(rest.substr(0, space));
		std::string amountText = (space == std::string::npos) ? std::string() : boost::trim_copy(rest.substr(space + 1));
		long amount = util::toInt(amountText).value_or(0);
		std::vector<PlayerId> targets = consoleTargets(who);
		if(who.empty() || who == "all" || who == "tous" || targets.size() != 1) {
			ARX_LOG(Logger::Console) << "[coop] usage: or <p2|pseudo> <montant>";
			return true;
		}
		if(amount <= 0) {
			ARX_LOG(Logger::Console) << "[coop] montant invalide: " << amountText;
			return true;
		}
		if(giveGoldToPlayer(targets[0], amount)) {
			const Player * who2 = g_coop.player(targets[0]);
			ARX_LOG(Logger::Console) << "[coop] " << amount << " pieces d'or donnees a " << (who2 ? who2->name : std::string("?"));
		} else {
			ARX_LOG(Logger::Console) << "[coop] impossible (pas assez d'or ?), il vous reste " << player.gold;
		}
		return true;
	}
	std::string arg = text.size() > 3 ? boost::trim_copy(text.substr(3)) : std::string();
	std::vector<PlayerId> targets = consoleTargets(arg);
	if(targets.empty() && !(arg.empty() || arg == "all" || arg == "tous")) {
		ARX_LOG(Logger::Console) << "[coop] joueur inconnu: " << arg << " (tp p2, tp <pseudo>, tp all)";
		return true;
	}
	for(PlayerId target : targets) {
		if(target == g_coop.localId()) {
			continue;
		}
		Writer writer;
		writer.u8_(target);
		writer.u32_(g_currentArea.handleData());
		writer.f32_(entities.player()->pos.x);
		writer.f32_(entities.player()->pos.y);
		writer.f32_(entities.player()->pos.z);
		writer.f32_(player.angle.getYaw());
		if(g_coop.isHost()) {
			g_coop.sendTo(target, MessageType::TeleportPlayer, writer);
		} else {
			g_coop.sendToHost(MessageType::TeleportPlayer, writer);
		}
		const Player * who = g_coop.player(target);
		ARX_LOG(Logger::Console) << "[coop] " << (who ? who->name : std::string("?")) << " arrive";
	}
	return true;
}

void itemDragged(const Entity & item) {
	if(!sharedWorldItem(item)) {
		return;
	}
	bool inScene = item.show == SHOW_FLAG_IN_SCENE;
	PlatformInstant now = platform::getTime();
	if(item.idString() == g_lastDragId && inScene == g_lastDragInScene && now - g_lastDragSend < DragSendInterval) {
		return;
	}
	g_lastDragId = item.idString();
	g_lastDragInScene = inScene;
	g_lastDragSend = now;
	Writer writer;
	writeItemPlacement(writer, item);
	writer.bool_(inScene);
	g_coop.sendToOthers(MessageType::DragItem, writer);
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
	writer.u8_(owner);
	writer.f32_(damage);
	writer.u32_(type);
	if(g_coop.isHost()) {
		g_coop.sendTo(owner, MessageType::DamagePlayer, writer);
	} else {
		g_coop.sendToHost(MessageType::DamagePlayer, writer);
	}
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
                          const ScriptParameters & parameters, bool baseScript, ScriptResult & result) {

	if(!g_coop.isClient() || g_coop.state() != State::InGame || g_applyingRemote > 0 || !entity
	   || !g_playthroughStarted) {
		return false; // before the co-op game starts (menu, intro) everything runs locally
	}

	if(isPlayerSide(entity)) {
		return false;
	}

	if(parameters.isPeekOnly()) {
		return false; // "would this combine do something?" for the cursor: answered locally, never forwarded
	}

	if(event.getId() == SM_INVENTORYUSE && isPlayerSide(sender) && (entity->ioflags & IO_ITEM)) {
		itemTaken(*entity); // F on an item on the ground: it is ours now, equip/eat it locally
		return false;
	}

	ScriptMessage id = event.getId();

	// Level load / appearance setup and local cinematic ends are fine to run locally
	// ("reload" is not: the host runs it and replicates the effects, see Kultar's DESTROY SELF)
	if(id == SM_INIT || id == SM_INITEND || id == SM_LOAD || id == SM_CINE_END
	   || id == SM_INVENTORY2_OPEN || id == SM_INVENTORY2_CLOSE) {
		return false; // query events: answered locally from the replicated entity variables
	}

	// Everything else about world entities is the host's business
	if(baseScript && forwardedEvents().count(id) && isPlayerSide(sender)) {
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
			// into, timer...): that is the host. Only level changes move everyone: a same-level
			// "teleport -p marker" is cutscene staging (Polsius, Atok...) and must not pile the
			// clients onto the host.
			if(command == "teleport") {
				if(peekTeleportChangesLevel(context)) {
					return CommandSync::Replicate;
				}
				g_gatherPending = true; // the others join us once the scene is over
			}
			if(command == "invulnerability") {
				// Level transitions protect "the player" (zone entered by anyone, so possibly a client)
				// and lift it from a timer with nobody behind it: the lift must reach everyone
				return CommandSync::Replicate;
			}
			return CommandSync::Local;
		}
		if(command == "teleport" && g_actingPlayer != g_coop.localId() && peekTeleportChangesLevel(context)) {
			return CommandSync::Replicate; // a client walked into the exit: the host leads the level change
		}
		if((command == "playanim" || command == "forceanim") && g_actingPlayer != g_coop.localId()
		   && peekFlags(context).find('e') != std::string::npos) {
			// "-e <command when done>": the host must run the animation too so that the script goes
			// on when it ends; the acting client gets the animation alone (see commandReplicated)
			return CommandSync::Replicate;
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

	if(command == "inventory" && !words.empty() && (words[0] == "add" || words[0] == "addmulti")
	   && LASTSPAWNED && ValidIOAddress(LASTSPAWNED) && (LASTSPAWNED->ioflags & IO_ITEM)
	   && LASTSPAWNED->owner() == entity) {
		// Replayed as-is, the clients would number the new item themselves (their own id range)
		// and it could never be matched with ours again: send the item with its id instead
		sendInventoryAdd(*entity, *LASTSPAWNED);
		return;
	}

	if((command == "set" || command == "inc" || command == "dec" || command == "mul" || command == "div")
	   && (words.empty() || !isLocalVariable(words[0]))) {
		return; // globals go through SetGlobal; local variables of world entities are replayed so
		        // that query events (chest locked?) can be answered on the clients
	}

	std::vector<std::string> sent = words;
	if((command == "playanim" || command == "forceanim") && !sent.empty() && sent[0].size() > 1 && sent[0][0] == '-') {
		bool playerDirected = sent[0].find('p') != std::string::npos;
		// The -e "execute when done" part of the line stays on the host
		sent[0].erase(std::remove(sent[0].begin(), sent[0].end(), 'e'), sent[0].end());
		if(sent[0] == "-") {
			sent.erase(sent.begin());
		}
		if(playerDirected) {
			// The player's own animation in a client's cutscene: that client only
			if(g_actingPlayer != InvalidPlayerId && g_actingPlayer != g_coop.localId()) {
				sendScriptCommand(g_actingPlayer, entity->idString(), command, sent);
			}
			return;
		}
	}

	if(command == "speak") {
		bool cine = !sent.empty() && sent[0].size() > 1 && sent[0][0] == '-' && sent[0].find('c') != std::string::npos;
		// Cinematic camera only for the acting player (or the host when nobody in particular
		// triggered it); the others just hear the line.
		std::vector<std::string> plain = cine ? stripCinematicSpeech(sent) : sent;
		if(g_lastSpeechVariant > 0) {
			// Random voice line: everyone plays the variant the host picked
			addSpeechVariant(sent, g_lastSpeechVariant);
			addSpeechVariant(plain, g_lastSpeechVariant);
		}
		if(!cine) {
			sendScriptCommand(InvalidPlayerId, entity->idString(), command, plain);
			return;
		}
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

/*!
 * Progress (globals, quests, keys, runes, xp, gold, bags) is shared as it changes, except while a
 * level is loading or while a client is not yet in the host's world: a newcomer creating its
 * character runs the player's init script, which resets about a hundred quest globals.
 */
bool sharingAllowed() {
	return g_coop.isActive() && g_coop.state() == State::InGame && g_applyingRemote == 0 && g_levelLoading == 0
	       && (!g_coop.isClient() || g_levelSynced);
}

void globalVariableChanged(std::string_view name, const SCRIPT_VAR & var) {
	if(!sharingAllowed()) {
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
	if(sharingAllowed()) {
		Writer writer;
		writer.string(quest);
		g_coop.sendToOthers(MessageType::SharedQuest, writer);
	}
}

void sharedKeyAdded(std::string_view key) {
	if(sharingAllowed()) {
		Writer writer;
		writer.string(key);
		g_coop.sendToOthers(MessageType::SharedKey, writer);
	}
}

void sharedRuneAdded(unsigned rune) {
	if(sharingAllowed()) {
		Writer writer;
		writer.u32_(rune);
		g_coop.sendToOthers(MessageType::SharedRune, writer);
	}
}

void sharedGold(long amount) {
	if(sharingAllowed() && amount != 0) {
		Writer writer;
		writer.s32_(s32(amount));
		g_coop.sendToOthers(MessageType::SharedGold, writer);
	}
}

void sharedBag() {
	if(sharingAllowed()) {
		g_coop.sendToOthers(MessageType::SharedBag, Writer());
	}
}

void sharedExperience(long amount) {
	if(sharingAllowed() && amount != 0) {
		Writer writer;
		writer.s32_(s32(amount));
		g_coop.sendToOthers(MessageType::SharedXP, writer);
	}
}

} // namespace coop
