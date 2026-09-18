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

#include "coop/SpellBar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

#include "animation/Animation.h"
#include "coop/Kick.h"
#include "coop/Puppets.h"
#include "coop/Roll.h"
#include "coop/Session.h"
#include "coop/Text.h"
#include "coop/ThirdPerson.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "core/Localisation.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "game/Spells.h"
#include "game/magic/Precast.h"
#include "game/magic/RuneDraw.h"
#include "game/magic/SpellRecognition.h"
#include "graphics/Draw.h"
#include "graphics/DrawLine.h"
#include "graphics/Renderer.h"
#include "graphics/font/Font.h"
#include "graphics/data/TextureContainer.h"
#include "gui/CinematicBorder.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/Notification.h"
#include "gui/Text.h"
#include "gui/book/Necklace.h"
#include "gui/hud/HudCommon.h"
#include "gui/hud/PlayerInventory.h"
#include "input/Input.h"
#include "io/log/Logger.h"
#include "platform/Time.h"
#include "scene/GameSound.h"

extern bool GLOBAL_MAGIC_MODE;

namespace coop {

namespace {

constexpr size_t ChargeSlots = 3; //!< the game's precast slots (MAX_PRECAST in Precast.cpp)

constexpr GameDuration RuneTime = std::chrono::milliseconds(400); //!< per rune of the spell
constexpr GameDuration CastCooldown = std::chrono::milliseconds(1000); //!< after a launch, like the precast keys
constexpr GameDuration FlashTime = std::chrono::milliseconds(350);
constexpr GameDuration SelectionTime = std::chrono::seconds(30); //!< a spell clicked in the book stays "chosen" this long
constexpr float ManaSurcharge = 0.1f; //!< a tenth more than a drawn spell, paid when the incantation starts
constexpr float HitInterrupt = 0.75f; //!< life lost in one frame that breaks the incantation

// Layout, in HUD units (times the HUD scale)
constexpr float CaseSize = 30.f;
constexpr float CaseGap = 3.f;
constexpr float GroupGap = 12.f; //!< between the charges and the bound spells (where the 4 would be)
constexpr float BottomMargin = 6.f;
constexpr float RuneSize = 18.f;

const std::array<ControlAction, SpellBarSlots> SlotActions = {
	CONTROLS_CUST_SPELL1, CONTROLS_CUST_SPELL2, CONTROLS_CUST_SPELL3,
	CONTROLS_CUST_SPELL4, CONTROLS_CUST_SPELL5, CONTROLS_CUST_SPELL6
};
const std::array<ControlAction, ChargeSlots> ChargeActions = {
	CONTROLS_CUST_PRECAST1, CONTROLS_CUST_PRECAST2, CONTROLS_CUST_PRECAST3
};

std::array<SpellType, SpellBarSlots> g_slots;

//! The incantation of a bound spell
struct Incantation {
	SpellType spell = SPELL_NONE;
	size_t slot = 0;
	GameInstant start = 0;
	size_t runes = 0; //!< runes of the spell
	size_t drawn = 0; //!< runes started so far
	float life = 0.f; //!< the player's life last frame (a hit interrupts)
	bool launching = false; //!< the runes are done, the cast animation plays
	audio::SourcedSample drawLoop;
};
Incantation g_cast;

GameInstant g_lastLaunch = 0;
std::array<GameInstant, SpellBarSlots> g_flash = { };

// The book
SpellType g_hovered = SPELL_NONE;
PlatformInstant g_hoverTime = 0;
SpellType g_selected = SPELL_NONE;
GameInstant g_selectTime = 0;

float g_lift = 0.f; //!< the bar's current rise above the inventory (smoothed)
bool g_testPress[SpellBarSlots] = { };

std::string spellLabel(SpellType spell) {
	return std::string(getLocalised(spellicons[spell].name));
}

size_t runeCount(SpellType spell) {
	const std::array<Rune, 6> & symbols = spellicons[spell].symbols;
	return symbols.size() - size_t(std::count(symbols.begin(), symbols.end(), RUNE_NONE));
}

Rune runeAt(SpellType spell, size_t index) {
	size_t i = 0;
	for(Rune rune : spellicons[spell].symbols) {
		if(rune == RUNE_NONE) {
			continue;
		}
		if(i++ == index) {
			return rune;
		}
	}
	return RUNE_NONE;
}

bool knowsSpell(SpellType spell) {
	return spell != SPELL_NONE && !spellicons[spell].bSecret && player.hasAllRunes(spellicons[spell].symbols);
}

float manaCost(SpellType spell) {
	return ARX_SPELLS_GetManaCost(spell, player.spellLevel());
}

void saveSlots() {
	std::string value;
	for(size_t i = 0; i < SpellBarSlots; i++) {
		if(i) {
			value += ',';
		}
		if(g_slots[i] != SPELL_NONE) {
			if(const char * name = getSpellName(g_slots[i])) {
				value += name;
			}
		}
	}
	config.coop.spellBarSlots = value;
	config.save();
}

void stopDrawLoop() {
	ARX_SOUND_Stop(g_cast.drawLoop);
	g_cast.drawLoop = audio::SourcedSample();
}

//! The incantation stops short: the hands drop, nothing leaves
void interruptCast(const char * why) {
	if(g_cast.spell == SPELL_NONE) {
		return;
	}
	LogInfo << "[coop] spell bar: incantation interrupted (" << why << ")";
	stopDrawLoop();
	ARX_SOUND_PlaySFX(g_snd.MAGIC_FIZZLE);
	Entity * io = entities.player();
	if(io && !g_cast.launching && io->anims[ANIM_CAST_END]) {
		changeAnimation(io, 1, io->anims[ANIM_CAST_END]);
	}
	g_cast = Incantation();
}

void startCast(size_t slot) {

	SpellType spell = g_slots[slot];
	Entity * io = entities.player();
	if(spell == SPELL_NONE || !io) {
		return;
	}

	if(!knowsSpell(spell) || !GLOBAL_MAGIC_MODE) {
		LogInfo << "[coop] spell bar: cannot cast " << getSpellName(spell) << " (runes " << knowsSpell(spell) << ", magic " << GLOBAL_MAGIC_MODE << ")";
		ARX_SOUND_PlaySpeech("player_cantcast");
		return;
	}

	float cost = manaCost(spell);
	float surcharge = cost * ManaSurcharge;
	if(player.manaPool.current < cost + surcharge) {
		LogInfo << "[coop] spell bar: not enough mana for " << getSpellName(spell) << " (" << player.manaPool.current << " < " << cost + surcharge << ")";
		ARX_SOUND_PlaySFX(g_snd.MAGIC_FIZZLE);
		notification_add("player_cantcast");
		return;
	}
	player.manaPool.current -= surcharge;

	// Like the game's own magic mode: the weapon goes away for the cast, it comes back after
	AnimLayer & layer1 = io->animlayer[1];
	if(player.Interface & INTER_COMBATMODE) {
		WILLRETURNTOCOMBATMODE = true;
		ARX_INTERFACE_setCombatMode(COMBAT_MODE_OFF);
		ResetAnim(layer1);
		layer1.flags &= ~EA_LOOP;
	}
	if(io->anims[ANIM_CAST_START]) {
		changeAnimation(io, 1, io->anims[ANIM_CAST_START]);
	}

	g_cast = Incantation();
	g_cast.spell = spell;
	g_cast.slot = slot;
	g_cast.start = g_gameTime.now();
	g_cast.runes = runeCount(spell);
	g_cast.life = player.lifePool.current;
	g_cast.drawLoop = ARX_SOUND_PlaySFX_loop(g_snd.MAGIC_DRAW_LOOP, nullptr, 1.f);
	LogInfo << "[coop] spell bar: incanting " << getSpellName(spell) << " (" << g_cast.runes << " runes, mana " << player.manaPool.current << ")";

}

//! A rune of the incantation: its name spoken, its shape traced in front of the caster
void drawRune(Rune rune) {
	ARX_SOUND_PlaySFX(g_snd.SYMB[rune]);
	// In first person the runes are traced on the screen like the mouse would (RuneDraw.cpp takes
	// the player's symboldraw as "in front of the camera"); in third person, in front of the body
	ARX_SPELLS_RequestSymbolDraw2(entities.player(), rune, RuneTime, true);
	runeShown(rune, RuneTime);
}

void launchCast() {

	SpellType spell = g_cast.spell;
	size_t slot = g_cast.slot;
	stopDrawLoop();
	g_cast = Incantation();

	ARX_SPELLS_ResetRecognition();
	bool launched = ARX_SPELLS_Launch(spell, *entities.player(), 0, -1, nullptr, GameDuration::ofRaw(-1));
	g_lastLaunch = g_gameTime.now();
	g_flash[slot] = g_gameTime.now();
	LogInfo << "[coop] spell bar: launched " << getSpellName(spell) << " -> " << launched << " (mana " << player.manaPool.current << ")";

}

void updateCast() {

	Entity * io = entities.player();
	if(!io) {
		g_cast = Incantation();
		return;
	}

	// What breaks the incantation
	if(player.lifePool.current <= 0.f) {
		stopDrawLoop();
		g_cast = Incantation();
		return;
	}
	if(player.lifePool.current < g_cast.life - HitInterrupt) {
		interruptCast("hit");
		return;
	}
	g_cast.life = player.lifePool.current;
	if(rollActive() || kickPhase() > 0.f || player.doingmagic || BLOCK_PLAYER_CONTROLS
	   || (player.Interface & INTER_PLAYERBOOK) || ARXmenu.mode() != Mode_InGame) {
		interruptCast("busy");
		return;
	}

	AnimLayer & layer1 = io->animlayer[1];

	if(!g_cast.launching) {
		GameDuration elapsed = g_gameTime.now() - g_cast.start;
		size_t due = std::min(g_cast.runes, size_t(elapsed / RuneTime) + 1);
		while(g_cast.drawn < due) {
			drawRune(runeAt(g_cast.spell, g_cast.drawn));
			g_cast.drawn++;
		}
		if(elapsed < RuneTime * float(g_cast.runes)) {
			return;
		}
		// The runes are done: the throw (ARX_SPELLS_Precast_Check does the same for a precast)
		g_cast.launching = true;
		stopDrawLoop();
		if(io->anims[ANIM_CAST]) {
			changeAnimation(io, 1, io->anims[ANIM_CAST]);
		} else {
			launchCast();
		}
		return;
	}

	if(!layer1.cur_anim || layer1.cur_anim != io->anims[ANIM_CAST]) {
		// Something else took the arms: throw now rather than never
		launchCast();
		return;
	}
	if(layer1.ctime + std::chrono::milliseconds(550) > layer1.currentAltAnim()->anim_time) {
		launchCast();
	}

}

//! The spell the book offers for a binding right now, if any
SpellType bookCandidate() {
	if(g_hovered != SPELL_NONE && g_platformTime.frameStart() - g_hoverTime <= std::chrono::milliseconds(250)) {
		return g_hovered;
	}
	if(g_selected != SPELL_NONE && g_gameTime.now() - g_selectTime <= SelectionTime) {
		return g_selected;
	}
	return SPELL_NONE;
}

void bindSlot(size_t slot, SpellType spell) {

	if(spell == SPELL_NONE) {
		notification_add(trs("coop_spellbar_pick", "Survolez ou cliquez un sort du livre, puis la touche de la case"));
		return;
	}

	std::string key = GInput->getKeyDisplayName(config.actions[SlotActions[slot]].key[0]);
	if(g_slots[slot] == spell) {
		g_slots[slot] = SPELL_NONE;
		notification_add(trs("coop_spellbar_cleared", "Case") + " " + key + " " + trs("coop_spellbar_cleared_2", "vidée"));
	} else {
		// One case per spell
		for(SpellType & other : g_slots) {
			if(other == spell) {
				other = SPELL_NONE;
			}
		}
		g_slots[slot] = spell;
		notification_add(trs("coop_spellbar_bound", "Case") + " " + key + " : " + spellLabel(spell));
	}
	ARX_SOUND_PlaySFX(g_snd.MENU_CLICK);
	saveSlots();

}

void fillRect(const Rectf & rect, Color color) {
	EERIEDrawFill2DRectDegrad(rect.topLeft(), rect.bottomRight(), 0.01f, color, color);
}

Rectf shrink(const Rectf & rect, float by) {
	return Rectf(rect.left + by, rect.top + by, rect.right - by, rect.bottom - by);
}

} // anonymous namespace

void runeShown(Rune rune, GameDuration duration) {
	if(!g_coop.isActive() || rune == RUNE_NONE) {
		return;
	}
	Writer writer;
	writer.u8_(g_coop.localId());
	writer.u8_(u8(rune));
	writer.u16_(u16(glm::clamp(long(toMsf(duration)), 50l, 5000l)));
	g_coop.sendToOthers(MessageType::PlayerRune, writer);
}

void handlePlayerRune(PlayerId id, Reader & reader) {
	u8 rune = reader.u8_();
	u16 ms = reader.u16_();
	Entity * puppet = puppetOf(id);
	if(!puppet || rune >= RUNE_COUNT) {
		return;
	}
	LogInfo << "[coop] rune " << int(rune) << " from player " << int(id) << " traced by its puppet (" << ms << " ms)";
	ARX_SOUND_PlaySFX(g_snd.SYMB[rune], &puppet->pos);
	// The puppet is an NPC for RuneDraw.cpp: the rune is traced in front of it with the flares
	// tied to it (MagicFlare.cpp hangs them at chest height, the puppet's pos is at the eyes)
	ARX_SPELLS_RequestSymbolDraw2(puppet, Rune(rune), std::chrono::milliseconds(ms));
}

void spellBarInit() {

	g_coop.onPlayerRune = handlePlayerRune;
	g_slots.fill(SPELL_NONE);

	std::string_view value = config.coop.spellBarSlots;
	size_t slot = 0;
	while(slot < SpellBarSlots) {
		size_t comma = value.find(',');
		std::string_view name = value.substr(0, comma);
		if(!name.empty()) {
			SpellType spell = GetSpellId(name);
			if(spell != SPELL_NONE) {
				g_slots[slot] = spell;
			} else {
				LogWarning << "Spell bar: unknown spell \"" << name << "\" in the config";
			}
		}
		slot++;
		if(comma == std::string_view::npos) {
			break;
		}
		value.remove_prefix(comma + 1);
	}

}

bool spellBarActive() {
	return config.coop.spellBar && entities.player() && ARXmenu.mode() == Mode_InGame;
}

bool spellBarCasting() {
	return g_cast.spell != SPELL_NONE;
}

void spellBarBookHover(SpellType spell) {
	g_hovered = spell;
	g_hoverTime = g_platformTime.frameStart();
}

void spellBarBookClick(SpellType spell) {
	g_selected = spell;
	g_selectTime = g_gameTime.now();
}

void spellBarTestBind(size_t slot, SpellType spell) {
	if(slot < SpellBarSlots) {
		g_slots[slot] = spell;
	}
}

void spellBarTestPress(size_t slot) {
	if(slot < SpellBarSlots) {
		g_testPress[slot] = true;
	}
}

void spellBarUpdate() {

	if(!spellBarActive()) {
		interruptCast("menu"); // the arms come down when the game resumes
		for(bool & press : g_testPress) {
			if(press) {
				LogInfo << "[coop] spell bar: test press while inactive (enabled " << config.coop.spellBar << ", menu " << int(ARXmenu.mode()) << ")";
				press = false;
			}
		}
		return;
	}

	if(g_cast.spell != SPELL_NONE) {
		updateCast();
	}

	size_t pressed = SpellBarSlots;
	for(size_t i = 0; i < SpellBarSlots; i++) {
		if(GInput->actionNowPressed(SlotActions[i]) || g_testPress[i]) {
			g_testPress[i] = false;
			pressed = i;
		}
	}
	if(pressed == SpellBarSlots) {
		return;
	}

	if(player.Interface & INTER_PLAYERBOOK) {
		LogInfo << "[coop] spell bar: key " << pressed << " in the book";
		bindSlot(pressed, bookCandidate());
		return;
	}

	if(BLOCK_PLAYER_CONTROLS || player.lifePool.current <= 0.f || player.m_paralysed
	   || cinematicBorder.isActive() || !(player.Interface & INTER_LIFE_MANA)) {
		LogInfo << "[coop] spell bar: key ignored (controls blocked " << BLOCK_PLAYER_CONTROLS << ", paralysed "
		        << player.m_paralysed << ", cinematic " << cinematicBorder.isActive() << ", hud "
		        << ((player.Interface & INTER_LIFE_MANA) != 0) << ")";
		return;
	}
	if(g_cast.spell != SPELL_NONE) {
		if(g_cast.slot == pressed && !g_cast.launching) {
			interruptCast("key"); // the same key again gives up
		}
		return;
	}
	if(g_slots[pressed] == SPELL_NONE) {
		LogInfo << "[coop] spell bar: key " << pressed << " on an empty case";
		notification_add(trs("coop_spellbar_empty", "Case vide : ouvrez le livre de sorts pour y ranger un sort"));
		return;
	}
	// Same guards as the precast keys (Interface.cpp)
	if(!(((player.Interface & INTER_COMBATMODE) && !player.isAiming()) || !player.doingmagic)) {
		LogInfo << "[coop] spell bar: key ignored (magic mode)";
		return;
	}
	if(rollActive() || kickPhase() > 0.f || player.jumpphase != NotJumping
	   || (g_lastLaunch != 0 && g_gameTime.now() - g_lastLaunch < CastCooldown)) {
		LogInfo << "[coop] spell bar: key ignored (roll " << rollActive() << ", kick " << kickPhase() << ", jump "
		        << (player.jumpphase != NotJumping) << ", cooldown " << (g_lastLaunch != 0) << ")";
		return;
	}
	startCast(pressed);

}

void spellBarDraw(const Rectf & parent, float scale) {

	if(!spellBarActive() || !(player.Interface & INTER_LIFE_MANA) || cinematicBorder.isActive()) {
		return;
	}

	const float caseSize = CaseSize * scale;
	const float gap = CaseGap * scale;
	float width = float(ChargeSlots) * caseSize + float(ChargeSlots - 1) * gap + GroupGap * scale
	              + float(SpellBarSlots) * caseSize + float(SpellBarSlots - 1) * gap;
	Rectf bar = createChild(parent, Anchor_BottomCenter, Vec2f(width, caseSize), Anchor_BottomCenter);
	bar.move(0.f, -BottomMargin * scale);

	// Above the inventory when it is open (the precast icons do the same), smoothly
	float lift = 0.f;
	const Rectf & inventory = g_playerInventoryHud.rect();
	if(inventory.overlaps(bar) || inventory.overlaps(bar + Vec2f(0.f, -g_lift))) {
		lift = bar.bottom - inventory.top + 4.f * scale;
	}
	float dt = toMsf(g_platformTime.lastFrameDuration()) * 0.012f;
	g_lift += (lift - g_lift) * std::min(1.f, dt);
	if(std::abs(lift - g_lift) < 0.5f) {
		g_lift = lift;
	}
	bar.move(0.f, -g_lift);

	static TextureContainer * parchment = TextureContainer::LoadUI("graph/interface/icons/spell_blank_precasted");

	UseRenderState state(render2D());

	const Color frame(150, 125, 85, 150);
	const Color back(12, 12, 16, 180);
	const Color label(232, 204, 142);
	GameInstant now = g_gameTime.now();
	float pulse = glm::clamp(1.f - PULSATE * 0.5f, 0.f, 1.f);

	float x = bar.left;

	// The charges: the game's precast slots, keys 1-3
	for(size_t i = 0; i < ChargeSlots; i++) {
		Rectf box(x, bar.top, x + caseSize, bar.bottom);
		x += caseSize + gap;
		fillRect(box, frame);
		fillRect(shrink(box, scale), back);
		if(parchment) {
			EERIEDrawBitmap(shrink(box, 2.f * scale), 0.01f, parchment, Color(255, 255, 255, 70));
		}
		bool filled = i < g_precast.size();
		if(filled) {
			const PRECAST_STRUCT & precast = g_precast[PrecastHandle(i)];
			float val = pulse;
			if(precast.launch_time > 0 && now >= precast.launch_time) {
				val *= 1.f - std::min((now - precast.launch_time) / std::chrono::seconds(1), 1.f);
			}
			if(TextureContainer * tc = spellicons[precast.typ].tc) {
				UseRenderState additive(render2D().blendAdditive());
				EERIEDrawBitmap(shrink(box, 3.f * scale), 0.01f, tc, Color::rgb(0.2f, 0.5f * val + 0.3f, val));
			}
		}
		std::string key = GInput->getKeyDisplayName(config.actions[ChargeActions[i]].key[0]);
		hFontInGame->draw(Vec2i(int(box.left + 2.f * scale), int(box.top)), key, filled ? label : Color(150, 135, 100));
	}

	x += GroupGap * scale;

	// The bound spells, keys 5-0
	for(size_t i = 0; i < SpellBarSlots; i++) {
		Rectf box(x, bar.top, x + caseSize, bar.bottom);
		x += caseSize + gap;
		SpellType spell = g_slots[i];
		bool casting = g_cast.spell != SPELL_NONE && g_cast.slot == i;
		fillRect(box, casting ? Color(120, 170, 255, 200) : frame);
		fillRect(shrink(box, scale), back);

		if(spell != SPELL_NONE) {
			bool known = knowsSpell(spell);
			float cost = manaCost(spell) * (1.f + ManaSurcharge);
			bool affordable = player.manaPool.current >= cost;
			bool cooling = g_lastLaunch != 0 && now - g_lastLaunch < CastCooldown;
			Color color = Color::gray(0.28f);
			if(!known) {
				color = Color(110, 60, 60);
			} else if(affordable && !cooling) {
				color = Color(225, 215, 190);
			}
			if(casting) {
				// The case fills up with the incantation
				float progress = g_cast.launching ? 1.f
				                 : glm::clamp((now - g_cast.start) / (RuneTime * float(std::max<size_t>(g_cast.runes, 1))), 0.f, 1.f);
				Rectf fill = shrink(box, scale);
				fill.top = fill.bottom - fill.height() * progress;
				fillRect(fill, Color(70, 120, 220, 110));
				color = Color(255, 255, 255);
			}
			if(now - g_flash[i] < FlashTime) {
				float f = 1.f - (now - g_flash[i]) / FlashTime;
				fillRect(shrink(box, scale), Color(255, 240, 200, u8(140.f * f)));
			}
			if(TextureContainer * tc = spellicons[spell].tc) {
				UseRenderState additive(render2D().blendAdditive());
				EERIEDrawBitmap(shrink(box, 3.f * scale), 0.01f, tc, color);
			}
		}

		std::string key = GInput->getKeyDisplayName(config.actions[SlotActions[i]].key[0]);
		hFontInGame->draw(Vec2i(int(box.left + 2.f * scale), int(box.top)), key, spell != SPELL_NONE ? label : Color(120, 110, 90));

		if(casting) {
			// The runes of the spell above the case, lit as they are traced
			float runeSize = RuneSize * scale;
			float total = float(g_cast.runes) * runeSize + float(g_cast.runes - 1) * gap;
			float rx = box.left + (box.width() - total) * 0.5f;
			float ry = box.top - runeSize - 4.f * scale;
			UseRenderState additive(render2D().blendAdditive());
			for(size_t r = 0; r < g_cast.runes; r++) {
				Rune rune = runeAt(g_cast.spell, r);
				if(TextureContainer * tc = gui::necklace.pTexTab[rune]) {
					Color c = r < g_cast.drawn ? Color(255, 245, 210) : Color::gray(0.25f);
					EERIEDrawBitmap(Rectf(rx, ry, rx + runeSize, ry + runeSize), 0.01f, tc, c);
				}
				rx += runeSize + gap;
			}
		}
	}

}

} // namespace coop
