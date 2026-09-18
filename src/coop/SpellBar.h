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

#ifndef ARX_COOP_SPELLBAR_H
#define ARX_COOP_SPELLBAR_H

/*!
 * The spell bar, after the MMORPGs: a row of cases at the bottom of the screen. The first
 * three are the game's own precast slots (a scroll read, or runes drawn while sneaking),
 * launched with the keys 1-3 as always; the six others hold spells bound from the book,
 * keys 5 to 0 (4 stays "cancel the current spell"). A bound spell is not drawn: pressing
 * its key incants it - the runes appear one after the other in front of the caster, 0.4 s
 * each, then the spell leaves at the character's current level for a tenth more mana than
 * a drawn one. Taking a hit, rolling, kicking or opening the book interrupts the incantation.
 *
 * The bindings are the player's (config, like the face), not the save game.
 */
#include <cstddef>

#include "coop/Protocol.h"
#include "core/TimeTypes.h"
#include "game/magic/SpellData.h"
#include "math/Rectangle.h"

namespace coop {

class Reader;

//! The number of bound-spell cases (keys 5 to 0)
constexpr size_t SpellBarSlots = 6;

//! Reads the bindings from the config. Call once at startup, after the config is loaded.
void spellBarInit();

//! Once per frame in game: the keys (bind in the book, cast otherwise), the incantation.
void spellBarUpdate();

//! The bar is enabled and there is a game to show it in (the vanilla precast icons then hide).
bool spellBarActive();

//! Draws the bar at the bottom of \a parent (the screen), \a scale = the HUD scale.
void spellBarDraw(const Rectf & parent, float scale);

//! An incantation is under way (the character is busy).
bool spellBarCasting();

//! Book, spells page: the spell under the mouse this frame / the spell just clicked.
void spellBarBookHover(SpellType spell);
void spellBarBookClick(SpellType spell);

/*!
 * A rune we drew with the mouse (SpellRecognition.cpp) or incanted from the bar: the other players
 * see it traced in front of our puppet, \a duration long, and hear it (PlayerRune).
 */
void runeShown(Rune rune, GameDuration duration);

//! Received a PlayerRune message from player \a id.
void handlePlayerRune(PlayerId id, Reader & reader);

//! Test: bind \a spell to \a slot (0-based) without the book
void spellBarTestBind(size_t slot, SpellType spell);
//! Test: press the key of \a slot now
void spellBarTestPress(size_t slot);

} // namespace coop

#endif // ARX_COOP_SPELLBAR_H
