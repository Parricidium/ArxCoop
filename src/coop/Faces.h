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

#ifndef ARX_COOP_FACES_H
#define ARX_COOP_FACES_H

/*!
 * Custom faces: a player can replace the face of its character with an image from
 * <user dir>/coop/faces/. The image is sent once to the other players (PlayerFace) and
 * composited by everyone onto the hero head textures, so the puppet of that player (and
 * its own book portrait) show the photo. Without a custom face the character's regular
 * skin (player.skin) is used as before.
 *
 * Two kinds of files are accepted:
 *  - a photo of a face (framed from the forehead to the chin): it is pasted over the face
 *    area of the head texture, keeping the skin's hair and ears;
 *  - a complete head texture (same layout as npc_human_base_hero_head), recognized by a
 *    "_skin" suffix in the file name (e.g. "bob_skin.png"): used as is for the bare head.
 */
#include <string>
#include <vector>

#include "coop/Protocol.h"

class TextureContainer;

namespace fs { class path; }

namespace coop {

class Reader;

enum class FaceMode : u8 {
	None    = 0, //!< no custom face
	Photo   = 1, //!< a face photo to composite over the skin's head texture
	Texture = 2, //!< a complete head texture
};

//! Registers the network callbacks. Call once at startup.
void facesInit();

//! Forgets every remote face (session end).
void facesReset();

//! <user dir>/coop/faces (created if missing).
fs::path facesDirectory();

//! A player's own menu pictures (<user dir>/coop/menu_background.*, menu_panel.*, menu_border.*), or nullptr.
TextureContainer * customMenuBackground();
TextureContainer * customMenuPanel();
TextureContainer * customMenuBorder();
//! Writes the game's own menu pictures to <user dir>/coop/menu_modeles/ (done with the face templates).
void exportMenuTemplates();

//! Image files in \ref facesDirectory(), sorted by name.
std::vector<std::string> availableFaces();

/*!
 * Writes editing templates into <faces>/modeles/: the four hero head textures (ready to be
 * edited and saved back as <name>_skin.png), the same with the face area outlined, and a
 * framing guide for photos. Returns that directory (empty on failure).
 */
fs::path exportFaceTemplates();

/*!
 * Loads and prepares our own face from config.coop.face (empty = none) and, when a session
 * is active, sends it to the other players. Returns false (see \ref localFaceError()) on failure.
 */
bool loadLocalFace();

//! What went wrong in the last \ref loadLocalFace() (empty if nothing).
const std::string & localFaceError();

//! Our own prepared head texture for the menu preview, or nullptr without a custom face.
TextureContainer * localFacePreview();

//! Received a PlayerFace message about player \a id.
void handlePlayerFace(PlayerId id, Reader & reader);

/*!
 * The head texture to use for the puppet of player \a id instead of the one of its skin,
 * or nullptr to keep the skin's. \a variant indexes the four textures returned by
 * ARX_PLAYER_SkinTextures (bare, chainmail, mithril, leather).
 */
TextureContainer * playerFaceTexture(PlayerId id, size_t variant, u8 skin);

//! Re-applies our own face on the shared hero head textures (book portrait). Called after ARX_PLAYER_Restore_Skin().
void applyLocalFaceToHero();

} // namespace coop

#endif // ARX_COOP_FACES_H
