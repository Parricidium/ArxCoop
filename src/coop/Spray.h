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

#ifndef ARX_COOP_SPRAY_H
#define ARX_COOP_SPRAY_H

/*!
 * Spray tags, like the sprays of Counter-Strike: a player picks an image in
 * <user dir>/coop/sprays/ (menu "Personnalisation"), the image travels once to the other
 * players (PlayerSpray, like the custom faces), and the spray key paints it on the wall, floor
 * or ceiling the player looks at (SprayPlaced): everybody sees it there, lit like the surface,
 * with the transparency of the PNG. The sprays of a level are saved with it (a file of their
 * own in the save block, like the corpses' physics) and sent to a joining client with the
 * level state.
 */
#include <string>
#include <string_view>
#include <vector>

#include "coop/Protocol.h"
#include "math/Types.h"

class TextureContainer;

namespace fs { class path; }

namespace coop {

class Reader;

//! Registers the network callbacks. Call once at startup, after facesInit().
void sprayInit();

//! Forgets every remote spray (session end).
void sprayReset();

//! <user dir>/coop/sprays (created if missing).
fs::path spraysDirectory();

//! Image files in \ref spraysDirectory(), sorted by name.
std::vector<std::string> availableSprays();

/*!
 * Loads and prepares our own spray from config.coop.spray (empty = none) and, when a session
 * is active, sends it to the other players. Returns false (see \ref localSprayError()) on failure.
 */
bool loadLocalSpray();

//! What went wrong in the last \ref loadLocalSpray() (empty if nothing).
const std::string & localSprayError();

//! Our own prepared spray for the menu preview, or nullptr without one.
TextureContainer * localSprayPreview();

//! Received a PlayerSpray message about player \a id.
void handlePlayerSpray(PlayerId id, Reader & reader);

//! Received a SprayPlaced message from player \a id.
void handleSprayPlaced(PlayerId id, Reader & reader);

//! Once per frame in game: the spray key.
void sprayUpdate();

//! Draws the sprays of the level (after the decals).
void spraysDraw();

//! The level is unloaded: forget its sprays (the polygons they sit on are gone).
void spraysClear();

//! The sprays of the current level, for the save block (empty when there are none).
std::string serializeSprays();

//! Recreates the sprays saved by \ref serializeSprays(), once the level is loaded.
void restoreSprays(std::string_view buffer);

//! Test helpers: paint our spray where we look (no key, no cooldown); the sprays and pieces of the level
bool sprayTestPlace();
size_t sprayCount();
size_t sprayPieceCount();

} // namespace coop

#endif // ARX_COOP_SPRAY_H
