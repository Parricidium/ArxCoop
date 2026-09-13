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

#ifndef ARX_COOP_ADMIN_H
#define ARX_COOP_ADMIN_H

/*!
 * Administration tools: an in-game menu page for the host (players, healing, gold, XP, items,
 * kicking, emergency helpers) and a "teleport me to a teammate" for everyone (unstuck).
 * The world keeps running while the page is open (the host's menu never pauses a co-op game).
 */
#include <string>
#include <string_view>

#include "coop/Protocol.h"

namespace coop {

//! Opens the in-game menu on the admin page when the admin key is pressed. Call once per in-game frame.
void adminHandleInput();
//! Menu startup hook: true once, right after the admin key opened the menu.
bool consumeAdminPageRequest();
//! Opens the in-game menu on the admin page (what the key does).
void adminOpenPage();

//! Teleports (host only): a teammate next to us / us next to a teammate (everyone).
bool adminTeleportToMe(PlayerId id);
bool adminTeleportMeTo(PlayerId id);
void adminGatherAll();

//! Host only. \a id may be our own id.
void adminHeal(PlayerId id);
void adminHealAll();
void adminGiveGold(PlayerId id, long amount);
void adminGiveXp(PlayerId id, long amount);
//! Item by class name ("potion_life", "sword_short"...): the class is searched under items/.
bool adminGiveItem(PlayerId id, std::string_view className, long count);
void adminSetInvulnerable(bool all, bool enabled);
bool adminInvulnerableAll();
//! Kills the hostile NPCs within \a radius of us.
size_t adminKillHostiles(float radius);
void adminKick(PlayerId id);
void adminSaveNow();

//! Result of the last admin action, for the page's status line.
const std::string & adminStatus();

//! One line per player: name, level, latency.
std::string adminRosterLine();

//! Network: a client applies what the host granted.
void applyAdminGrant(Reader & reader);

} // namespace coop

#endif // ARX_COOP_ADMIN_H
