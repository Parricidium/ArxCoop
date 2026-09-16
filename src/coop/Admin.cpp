/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "coop/Admin.h"

#include <algorithm>
#include <string>
#include <vector>

#include "cinematic/CinematicController.h"
#include "coop/Puppets.h"
#include "coop/Qol.h"
#include "coop/Replication.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "core/Config.h"
#include "core/GameTime.h"
#include "game/Damage.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Inventory.h"
#include "game/Item.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "graphics/Math.h"
#include "gui/CinematicBorder.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/MenuWidgets.h"
#include "gui/Notification.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"
#include "scene/Interactive.h"
#include "util/String.h"

namespace coop {

namespace {

std::string g_status;
bool g_pageRequested = false;
bool g_invulnerableAll = false;

enum class Grant : u8 {
	Heal = 0,
	Gold = 1,
	Xp = 2,
	Invulnerability = 3,
};

std::string nameOf(PlayerId id) {
	const Player * who = g_coop.player(id);
	return who ? who->name : std::string("?");
}

void setStatus(std::string text) {
	LogInfo << "[coop] admin: " << text;
	g_status = std::move(text);
}

bool hostInGame() {
	return g_coop.isHost() && g_coop.state() == State::InGame && entities.player();
}

void sendGrant(PlayerId id, Grant kind, long amount) {
	Writer writer;
	writer.u8_(u8(kind));
	writer.s32_(s32(amount));
	g_coop.sendTo(id, MessageType::AdminGrant, writer);
}

//! What a grant does on the machine of the player it is for.
void applyGrantLocally(Grant kind, long amount) {
	switch(kind) {
		case Grant::Heal: {
			if(localPlayerDownedRaw()) {
				adminReviveLocal();
			}
			player.lifePool.current = player.lifePool.max;
			player.manaPool.current = player.manaPool.max;
			player.poison = 0.f;
			notification_add(trs("coop_admin_healed", "Soins complets (admin)"));
			break;
		}
		case Grant::Gold: {
			ARX_PLAYER_AddGold(amount);
			notification_add("+" + std::to_string(amount) + trs("coop_admin_gold_received", " pi\xC3\xA8" "ces d'or (admin)"));
			break;
		}
		case Grant::Xp: {
			ARX_PLAYER_Modify_XP(amount);
			notification_add("+" + std::to_string(amount) + trs("coop_admin_xp_received", " XP (admin)"));
			break;
		}
		case Grant::Invulnerability: {
			if(amount) {
				player.playerflags |= PLAYERFLAGS_INVULNERABILITY;
			} else {
				player.playerflags &= ~PLAYERFLAGS_INVULNERABILITY;
			}
			notification_add(amount ? trs("coop_admin_invulnerable_on", "Invuln\xC3\xA9rabilit\xC3\xA9 activ\xC3\xA9" "e (admin)")
			                        : trs("coop_admin_invulnerable_off", "Invuln\xC3\xA9rabilit\xC3\xA9 d\xC3\xA9sactiv\xC3\xA9" "e (admin)"));
			break;
		}
	}
}

void grant(PlayerId id, Grant kind, long amount) {
	if(id == g_coop.localId()) {
		applyGrantLocally(kind, amount);
	} else if(g_coop.player(id)) {
		sendGrant(id, kind, amount);
	}
}

//! Finds "graph/obj3d/interactive/items/.../<name>/<name>" for an item class name (the class
//! directory holds <name>.asl);  similar receives other class names containing the query.
res::path findItemClass(std::string_view name, std::vector<std::string> & similar) {
	std::string wanted = util::toLowercase(std::string(name));
	if(wanted.empty()) {
		return res::path();
	}
	res::path root = "graph/obj3d/interactive/items";
	PakDirectory * items = g_resources->getDirectory(root);
	if(!items) {
		return res::path();
	}
	res::path found;
	std::vector<std::pair<res::path, PakDirectory *>> stack;
	stack.emplace_back(root, items);
	while(!stack.empty()) {
		auto [path, dir] = stack.back();
		stack.pop_back();
		for(auto entry : dir->dirs()) {
			const std::string & sub = entry;
			PakDirectory & subdir = entry;
			res::path subpath = path / sub;
			if(subdir.hasFile(sub + ".asl")) {
				if(sub == wanted) {
					found = subpath / sub;
				} else if(sub.find(wanted) != std::string::npos && similar.size() < 8) {
					similar.push_back(sub);
				}
				continue; // class directories only hold instances below
			}
			stack.emplace_back(subpath, &subdir);
		}
	}
	return found;
}

} // anonymous namespace

void adminHandleInput() {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || !entities.player()) {
		return;
	}
	if(ARXmenu.mode() != Mode_InGame || isInCinematic() || cinematicBorder.isActive() || BLOCK_PLAYER_CONTROLS) {
		return;
	}
	if(!GInput->actionNowPressed(CONTROLS_CUST_ADMIN)) {
		return;
	}
	adminOpenPage();
}

void adminOpenPage() {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || ARXmenu.mode() != Mode_InGame) {
		return;
	}
	// Same as the Escape key, then straight to our page
	if(!g_coop.worldMustKeepRunning()) {
		g_gameTime.pause(GameTime::PauseMenu);
	}
	ARX_MENU_Launch(true);
	TRUE_PLAYER_MOUSELOOK_ON = false;
	ARX_PLAYER_PutPlayerInNormalStance();
	g_pageRequested = true;
}

bool consumeAdminPageRequest() {
	bool requested = g_pageRequested;
	g_pageRequested = false;
	return requested;
}

bool adminTeleportToMe(PlayerId id) {
	if(!hostInGame() || id == g_coop.localId()) {
		return false;
	}
	for(const TeammateInfo & mate : teammates()) {
		if(mate.id != id) {
			continue;
		}
		if(!mate.here) {
			setStatus(mate.name + trs("coop_admin_other_level", " n'est pas dans ce niveau"));
			return false;
		}
		if(!teleportPlayerToMe(id, 0)) {
			setStatus(trs("coop_admin_not_now", "impossible pour le moment"));
			return false;
		}
		setStatus(mate.name + trs("coop_admin_teleported_to_you", " t\xC3\xA9l\xC3\xA9port\xC3\xA9 \xC3\xA0 vous"));
		return true;
	}
	setStatus(trs("coop_admin_unknown_player", "joueur inconnu"));
	return false;
}

bool adminTeleportMeTo(PlayerId id) {
	if(!g_coop.isActive() || g_coop.state() != State::InGame || !entities.player() || id == g_coop.localId()) {
		return false;
	}
	for(const TeammateInfo & mate : teammates()) {
		if(mate.id != id) {
			continue;
		}
		if(!mate.here) {
			setStatus(mate.name + trs("coop_admin_other_level", " n'est pas dans ce niveau"));
			return false;
		}
		// A step behind the teammate, facing the same way
		Vec3f back = -angleToVectorXZ(mate.yaw + 180.f); // NPC yaw convention: facing is yaw + 180
		Vec3f pos = mate.pos + back * 70.f;
		ARX_INTERACTIVE_Teleport(entities.player(), pos);
		player.desiredangle.setYaw(player.angle.getYaw());
		setStatus(trs("coop_teleported_to", "t\xC3\xA9l\xC3\xA9port\xC3\xA9 vers ") + mate.name);
		if(ARXmenu.mode() == Mode_MainMenu && g_canResumeGame) {
			ARX_MENU_Clicked_QUIT(); // back to the game right away: a client's world is paused in the menu
		}
		return true;
	}
	setStatus(trs("coop_admin_unknown_player", "joueur inconnu"));
	return false;
}

void adminGatherAll() {
	if(!hostInGame()) {
		return;
	}
	size_t slot = 0;
	for(const TeammateInfo & mate : teammates()) {
		if(mate.here) {
			teleportPlayerToMe(mate.id, slot++);
		}
	}
	setStatus(std::to_string(slot) + trs("coop_admin_gathered", " joueur(s) t\xC3\xA9l\xC3\xA9port\xC3\xA9(s) \xC3\xA0 vous"));
}

void adminHeal(PlayerId id) {
	if(!hostInGame()) {
		return;
	}
	grant(id, Grant::Heal, 0);
	setStatus(nameOf(id) + " : " + trs("coop_admin_healed_short", "soins complets"));
}

void adminHealAll() {
	if(!hostInGame()) {
		return;
	}
	for(const Player & who : g_coop.players()) {
		grant(who.id, Grant::Heal, 0);
	}
	setStatus(trs("coop_admin_all_healed", "tout le monde soign\xC3\xA9"));
}

void adminGiveGold(PlayerId id, long amount) {
	if(!hostInGame() || amount <= 0) {
		return;
	}
	grant(id, Grant::Gold, amount);
	setStatus(nameOf(id) + " : +" + std::to_string(amount) + " " + trs("coop_admin_gold", "or"));
}

void adminGiveXp(PlayerId id, long amount) {
	if(!hostInGame() || amount <= 0) {
		return;
	}
	grant(id, Grant::Xp, amount);
	setStatus(nameOf(id) + " : +" + std::to_string(amount) + " XP");
}

bool adminGiveItem(PlayerId id, std::string_view className, long count) {
	if(!hostInGame()) {
		return false;
	}
	std::vector<std::string> similar;
	res::path classPath = findItemClass(className, similar);
	if(classPath.empty()) {
		std::string hint;
		for(const std::string & other : similar) {
			hint += (hint.empty() ? "  (" + trs("coop_admin_similar", "proches") + " : " : std::string(", ")) + other;
		}
		setStatus(trs("coop_admin_unknown_item", "objet inconnu : ") + std::string(className) + (hint.empty() ? "" : hint + ")"));
		return false;
	}
	count = std::clamp(count, 1l, 999l);
	if(id == g_coop.localId()) {
		Entity * item = AddItem(classPath, -1, IO_IMMEDIATELOAD);
		if(!item || !item->_itemdata) {
			setStatus(trs("coop_admin_cannot_create", "impossible de cr\xC3\xA9" "er ") + classPath.string());
			return false;
		}
		initItemCopy(*item, ItemState()); // a fresh class item: INIT / INITEND like every other AddItem() of the engine
		if(!ValidIOAddress(item)) {
			return false;
		}
		long maxCount = item->_itemdata->maxcount > 0 ? long(item->_itemdata->maxcount) : count;
		item->_itemdata->count = s16(std::min(count, maxCount));
		giveToPlayer(item);
	} else if(g_coop.player(id)) {
		Writer writer;
		writer.u8_(g_coop.localId());
		writer.u8_(id);
		writer.string(classPath.string());
		writer.raw<s16>(s16(count));
		writer.f32_(100.f);
		writer.f32_(100.f);
		writer.raw<s16>(0);
		writer.raw<s16>(0);
		g_coop.sendTo(id, MessageType::GiveItem, writer);
	} else {
		return false;
	}
	setStatus(nameOf(id) + " : " + std::string(className) + " x" + std::to_string(count));
	return true;
}

void adminSetInvulnerable(bool all, bool enabled) {
	if(!hostInGame()) {
		return;
	}
	if(all) {
		g_invulnerableAll = enabled;
		for(const Player & who : g_coop.players()) {
			grant(who.id, Grant::Invulnerability, enabled ? 1 : 0);
		}
		setStatus(enabled ? trs("coop_admin_all_invulnerable", "invuln\xC3\xA9rabilit\xC3\xA9 pour tous") : trs("coop_admin_all_vulnerable", "invuln\xC3\xA9rabilit\xC3\xA9 retir\xC3\xA9" "e \xC3\xA0 tous"));
	} else {
		applyGrantLocally(Grant::Invulnerability, enabled ? 1 : 0);
		setStatus(enabled ? trs("coop_admin_me_invulnerable", "vous \xC3\xAAtes invuln\xC3\xA9rable") : trs("coop_admin_me_vulnerable", "vous \xC3\xAAtes de nouveau vuln\xC3\xA9rable"));
	}
}

bool adminInvulnerableAll() {
	return g_invulnerableAll;
}

size_t adminKillHostiles(float radius) {
	if(!hostInGame()) {
		return 0;
	}
	size_t killed = 0;
	std::vector<Entity *> victims;
	for(Entity & npc : entities(IO_NPC)) {
		if(&npc == entities.player() || npc.coopPuppet || npc.show != SHOW_FLAG_IN_SCENE) {
			continue;
		}
		if(npc._npcdata->lifePool.current <= 0.f || !isEnemy(&npc)) {
			continue;
		}
		if(glm::distance(npc.pos, entities.player()->pos) > radius) {
			continue;
		}
		victims.push_back(&npc);
	}
	for(Entity * npc : victims) {
		ARX_DAMAGES_ForceDeath(*npc, entities.player());
		killed++;
	}
	setStatus(std::to_string(killed) + trs("coop_admin_killed", " PNJ hostile(s) tu\xC3\xA9(s)"));
	return killed;
}

void adminKick(PlayerId id) {
	if(!g_coop.isHost() || id == g_coop.localId()) {
		return;
	}
	std::string name = nameOf(id);
	g_coop.kick(id);
	setStatus(name + trs("coop_admin_kicked", " expuls\xC3\xA9"));
}

void adminSaveNow() {
	if(!hostInGame()) {
		return;
	}
	ARX_QuickSave();
	setStatus(trs("coop_admin_saved", "sauvegarde rapide faite (les autres sauvegardent leur personnage)"));
}

const std::string & adminStatus() {
	return g_status;
}

std::string adminRosterLine() {
	std::string line;
	for(const Player & who : g_coop.players()) {
		if(!line.empty()) {
			line += "   |   ";
		}
		line += who.name;
		if(who.id == g_coop.localId()) {
			line += " (" + trs("coop_admin_me", "moi") + ")";
		}
		for(const TeammateInfo & mate : teammates()) {
			if(mate.id == who.id) {
				line += " " + trs("coop_admin_level", "niv.") + std::to_string(mate.area) + (mate.here ? "" : " (" + trs("coop_hud_elsewhere", "ailleurs") + ")")
				        + (mate.downed ? " " + trs("coop_hud_downed", "\xC3\xA0 terre") : "");
			}
		}
		if(u16 ms = latencyOf(who.id)) {
			line += " " + std::to_string(ms) + " ms";
		}
	}
	return line;
}

void applyAdminGrant(Reader & reader) {
	u8 kind = reader.u8_();
	long amount = reader.s32_();
	if(kind > u8(Grant::Invulnerability)) {
		return;
	}
	applyGrantLocally(Grant(kind), amount);
	LogInfo << "[coop] admin grant " << int(kind) << " " << amount << " from the host";
}

} // namespace coop
