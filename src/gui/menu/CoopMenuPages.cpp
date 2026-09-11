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

#include "gui/menu/CoopMenuPages.h"

#include <string>
#include <utility>
#include <vector>

#include "coop/Session.h"
#include "core/Config.h"
#include "core/Localisation.h"
#include "gui/MainMenu.h"
#include "gui/MenuWidgets.h"
#include "gui/Text.h"
#include "gui/menu/MenuPage.h"
#include "gui/widget/ButtonWidget.h"
#include "gui/widget/Spacer.h"
#include "gui/widget/TextInputWidget.h"
#include "gui/widget/TextWidget.h"
#include "graphics/font/Font.h"
#include "input/Keyboard.h"
#include "util/Number.h"

namespace {

// Strings are looked up in the localisation files first so translations can override the French defaults.
std::string_view coopText(std::string_view key, std::string_view fallback) {
	return getLocalised(key, fallback);
}

//! Shows the session status and the roster; refreshed every frame from g_coop.
class CoopLobbyWidget final : public Widget {

	Font * m_font;
	std::vector<std::string> m_lines;
	TextWidget * m_startButton;

public:

	CoopLobbyWidget(Font * font, float width, TextWidget * startButton)
		: m_font(font)
		, m_startButton(startButton)
	{
		m_rect = Rectf(Vec2f(0.f), width, float(m_font->getLineHeight() * int(coop::MaxPlayers + 2)));
	}

	void update() override {

		m_lines.clear();

		std::string status;
		switch(g_coop.state()) {
			case coop::State::Idle: {
				status = coopText("system_menus_coop_status_idle", "Aucune session");
				break;
			}
			case coop::State::Connecting: {
				status = std::string(coopText("system_menus_coop_status_connecting", "Connexion à "));
				status += g_coop.endpointDescription() + "...";
				break;
			}
			case coop::State::Lobby:
			case coop::State::InGame: {
				if(g_coop.isHost()) {
					status = std::string(coopText("system_menus_coop_status_hosting", "Hébergement sur "));
				} else {
					status = std::string(coopText("system_menus_coop_status_connected", "Connecté à "));
				}
				status += g_coop.endpointDescription();
				break;
			}
			case coop::State::Failed: {
				status = std::string(coopText("system_menus_coop_status_error", "Erreur : ")) + g_coop.error();
				break;
			}
		}
		m_lines.push_back(std::move(status));
		m_lines.emplace_back();

		for(size_t slot = 0; slot < coop::MaxPlayers; slot++) {
			std::string line = std::to_string(slot + 1) + ". ";
			const coop::Player * player = g_coop.player(coop::PlayerId(slot));
			if(player) {
				line += player->name;
				if(player->id == 0) {
					line += " (";
					line += coopText("system_menus_coop_host_tag", "hôte");
					line += ")";
				}
				if(player->id == g_coop.localId()) {
					line += " *";
				}
			} else {
				line += coopText("system_menus_coop_slot_free", "- libre -");
			}
			m_lines.push_back(std::move(line));
		}

		if(m_startButton) {
			m_startButton->setEnabled(g_coop.isHost() && g_coop.state() == coop::State::Lobby);
		}

	}

	void render(bool /* mouseOver */) override {
		Vec2f pos = m_rect.topLeft();
		for(const std::string & line : m_lines) {
			if(!line.empty()) {
				ARX_UNICODE_DrawTextInRect(m_font, pos, m_rect.right, line, Color(232, 204, 142), nullptr);
			}
			pos.y += float(m_font->getLineHeight());
		}
	}

	WidgetType type() const override {
		return WidgetType_Text;
	}

};

class CoopMenuPage final : public MenuPage {

	TextInputWidget * m_nickname;
	TextInputWidget * m_address;
	TextInputWidget * m_port;
	TextWidget * m_error;

	void commitFields() {
		m_nickname->unfocus();
		m_address->unfocus();
		m_port->unfocus();
		config.coop.nickname = coop::sanitizeNickname(m_nickname->text());
		config.coop.address = m_address->text();
		config.coop.port = util::toInt(m_port->text()).value_or(coop::DefaultPort);
		if(config.coop.port <= 0 || config.coop.port > 65535) {
			config.coop.port = coop::DefaultPort;
		}
		config.save();
	}

	std::unique_ptr<TextInputWidget> addField(std::string_view labelKey, std::string_view labelFallback,
	                                          std::string_view value, size_t maxLength) {
		auto label = std::make_unique<TextWidget>(hFontMenu, coopText(labelKey, labelFallback));
		label->setEnabled(false);
		label->forceDisplay(TextWidget::Enabled);
		addCenter(std::move(label));
		auto input = std::make_unique<TextInputWidget>(hFontMenu, value, m_rect);
		input->setMaxLength(maxLength);
		return input;
	}

public:

	CoopMenuPage()
		: MenuPage(Page_Coop)
		, m_nickname(nullptr)
		, m_address(nullptr)
		, m_port(nullptr)
		, m_error(nullptr)
	{ }

	void init() override {

		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop", "Coopération"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto input = addField("system_menus_coop_nickname", "Pseudo", config.coop.nickname, coop::MaxNicknameLength);
			m_nickname = input.get();
			addCenter(std::move(input));
		}

		{
			auto input = addField("system_menus_coop_address", "Adresse de l'hôte", config.coop.address, 255);
			m_address = input.get();
			addCenter(std::move(input));
		}

		{
			auto input = addField("system_menus_coop_port", "Port", std::to_string(config.coop.port), 5);
			m_port = input.get();
			addCenter(std::move(input));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_host", "Héberger une partie"));
			txt->clicked = [this](Widget * /* widget */) {
				commitFields();
				if(g_coop.host(u16(config.coop.port), config.coop.nickname)) {
					m_error->setText("");
					g_mainMenu->requestPage(Page_CoopLobby);
				} else {
					m_error->setText(g_coop.error());
				}
			};
			addCenter(std::move(txt));
		}

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_join", "Rejoindre une partie"));
			txt->clicked = [this](Widget * /* widget */) {
				commitFields();
				m_error->setText("");
				g_coop.join(config.coop.address, u16(config.coop.port), config.coop.nickname);
			};
			txt->setTargetPage(Page_CoopLobby);
			addCenter(std::move(txt));
		}

		{
			auto txt = std::make_unique<TextWidget>(hFontControls, "");
			txt->setEnabled(false);
			txt->forceDisplay(TextWidget::Enabled);
			m_error = txt.get();
			addCenter(std::move(txt));
		}

		addBackButton(Page_None);

	}

};

class CoopLobbyMenuPage final : public MenuPage {

public:

	CoopLobbyMenuPage()
		: MenuPage(Page_CoopLobby)
	{ }

	void init() override {

		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_lobby", "Salon"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		// The start button is created first so the roster widget can enable/disable it.
		auto start = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_start", "Démarrer la partie"));
		start->clicked = [](Widget * /* widget */) {
			g_coop.startGame();
		};
		start->setEnabled(false);
		TextWidget * startButton = start.get();

		addCenter(std::make_unique<CoopLobbyWidget>(hFontMenu, m_rect.width(), startButton), false);

		addCorner(std::move(start), BottomRight);

		{
			// Before the game starts: leave the session. While playing: just close the page.
			auto leave = std::make_unique<ButtonWidget>(buttonSize(16, 16), "graph/interface/menus/back");
			leave->clicked = [](Widget * /* widget */) {
				if(g_coop.state() != coop::State::InGame) {
					g_coop.leave();
				}
			};
			leave->setTargetPage(g_coop.state() == coop::State::InGame ? Page_None : Page_Coop);
			leave->setShortcut(Keyboard::Key_Escape);
			addCorner(std::move(leave), BottomLeft);
		}

	}

};

} // anonymous namespace

std::unique_ptr<MenuPage> createCoopMenuPage() {
	return std::make_unique<CoopMenuPage>();
}

std::unique_ptr<MenuPage> createCoopLobbyMenuPage() {
	return std::make_unique<CoopLobbyMenuPage>();
}

void coopMenuHandleStartup() {
	if(g_coop.consumeLobbyRequest() && g_coop.state() != coop::State::InGame) {
		g_mainMenu->requestPage(Page_CoopLobby);
	}
	// In-game Escape must open the plain menu, never the lobby page left behind
	if(g_coop.state() == coop::State::InGame && g_mainMenu->m_window
	   && g_mainMenu->m_window->currentPageId() == Page_CoopLobby) {
		g_mainMenu->requestPage(Page_None);
	}
}
