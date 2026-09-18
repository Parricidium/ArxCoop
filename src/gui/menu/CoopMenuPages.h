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

#ifndef ARX_GUI_MENU_COOPMENUPAGES_H
#define ARX_GUI_MENU_COOPMENUPAGES_H

#include <memory>

class MenuPage;

//! "Coopération" page: nickname, address, port, host / join buttons.
std::unique_ptr<MenuPage> createCoopMenuPage();

//! Lobby page: connection status, player list, start / leave buttons.
std::unique_ptr<MenuPage> createCoopHostMenuPage();
std::unique_ptr<MenuPage> createCoopJoinMenuPage();
std::unique_ptr<MenuPage> createCoopLobbyMenuPage();
std::unique_ptr<MenuPage> createCoopAdminMenuPage();

//! "Options coop" page: face, camera, dialogue and intro settings.
std::unique_ptr<MenuPage> createCoopOptionsMenuPage();
//! Co-op mod: the player's face and spray tag
std::unique_ptr<MenuPage> createCustomizeMenuPage();

//! Performs a pending --coop-host / --coop-join request; call from the main menu update.
void coopMenuHandleStartup();

#endif // ARX_GUI_MENU_COOPMENUPAGES_H
