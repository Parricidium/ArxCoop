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

#include <algorithm>
#include <ctime>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <cmath>

#include "animation/AnimationRender.h"
#include "coop/Admin.h"
#include "coop/Faces.h"
#include "coop/Text.h"
#include "coop/Session.h"
#include "core/Config.h"
#include "core/Core.h"
#include "core/GameTime.h"
#include "game/Camera.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Player.h"
#include "graphics/effects/Halo.h"
#include "platform/Time.h"
#include "scene/Light.h"
#include "gui/widget/CheckboxWidget.h"
#include "core/SaveGame.h"
#include "core/ArxGame.h"
#include "core/Localisation.h"
#include "graphics/Draw.h"
#include "graphics/Renderer.h"
#include "graphics/data/TextureContainer.h"
#include "gui/MainMenu.h"
#include "gui/MenuWidgets.h"
#include "gui/Text.h"
#include "gui/menu/MenuPage.h"
#include "gui/widget/ButtonWidget.h"
#include "gui/widget/CycleTextWidget.h"
#include "gui/widget/Spacer.h"
#include "gui/widget/TextInputWidget.h"
#include "gui/widget/TextWidget.h"
#include "graphics/font/Font.h"
#include "input/Keyboard.h"
#include "io/fs/FilePath.h"
#include "platform/Process.h"
#include "util/Number.h"

extern long IN_BOOK_DRAW; // see gui/book/Book.cpp

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
	TextWidget * m_continueButton;

public:

	CoopLobbyWidget(Font * font, float width, TextWidget * startButton, TextWidget * continueButton = nullptr)
		: m_font(font)
		, m_startButton(startButton)
		, m_continueButton(continueButton)
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

		bool canStart = g_coop.isHost() && g_coop.state() == coop::State::Lobby;
		if(m_startButton) {
			m_startButton->setEnabled(canStart);
		}
		if(m_continueButton) {
			m_continueButton->setEnabled(canStart);
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

/*!
 * The player's head in 3D with the face currently selected, exactly as the character creation
 * page of the book draws it (same lights, camera and pose, see StatsPage::RenderBookPlayerCharacter),
 * slowly turning so the hair and the sides show too.
 */
class HeadPreviewWidget final : public Widget {

	// The book's head viewport is 137 x 214 units, its projection centre sits 15.5 units left and
	// 8.5 units below the viewport's centre, at (128, 120.5) of the 513 x 313 book
	static constexpr float ViewWidth = 137.f;
	static constexpr float ViewHeight = 214.f;

public:

	explicit HeadPreviewWidget(float height) {
		m_rect = Rectf(Vec2f(0.f), height * ViewWidth / ViewHeight, height);
	}

	void render(bool /* mouseOver */) override {

		Entity * io = entities.player();
		if(!io || !io->obj || !player.bookAnimation[0].cur_anim) {
			return;
		}

		float k = m_rect.height() / ViewHeight;
		Rect area(Vec2i(m_rect.topLeft()), Vec2i(m_rect.bottomRight()));
		GRenderer->Clear(Renderer::DepthBuffer, Color(), 1.f, 1, &area);
		GRenderer->SetScissor(area);

		EERIE_LIGHT light1;
		light1.pos = Vec3f(50.f, 50.f, -90.f);
		light1.m_exists = true;
		light1.rgb = Color3f(0.15f, 0.06f, 0.003f);
		light1.intensity = 8.8f;
		light1.fallstart = 2020;
		light1.fallend = light1.fallstart + 60;
		RecalcLight(&light1);

		EERIE_LIGHT light2;
		light2.m_exists = true;
		light2.pos = Vec3f(-50.f, -50.f, -200.f);
		light2.rgb = Color3f::gray(0.6f);
		light2.intensity = 3.8f;
		light2.fallstart = 0;
		light2.fallend = light2.fallstart + 3460.f;
		RecalcLight(&light2);

		EERIE_LIGHT * savedLights[2] = { g_culledDynamicLights[0], g_culledDynamicLights[1] };
		size_t savedLightCount = g_culledDynamicLightsCount;
		g_culledDynamicLights[0] = &light1;
		g_culledDynamicLights[1] = &light2;
		g_culledDynamicLightsCount = 2;

		Camera camera;
		camera.angle = Anglef();
		camera.m_pos = Vec3f(0.f);
		camera.focal = 520.f;
		camera.cdepth = 2200.f;
		Camera * oldCamera = g_camera;
		Vec2i center(m_rect.center() + Vec2f(-15.5f, 8.5f) * k);
		Rect viewport(center - Vec2i(Vec2f(128.f, 120.5f) * k), s32(513.f * k), s32(313.f * k));
		PrepareCamera(&camera, viewport, center);
		GRenderer->SetAntialiasing(true);

		float seconds = float(toMsi(platform::getTime() - PlatformInstant())) * 0.001f;
		Anglef angle;
		angle.setYaw(-10.f + 30.f * std::sin(seconds * 0.6f));
		Vec3f pos(8.f, 162.f, 75.f);

		bool improve = player.m_improve;
		player.m_improve = false;
		auto vertices = io->obj->vertexWorldPositions;

		IN_BOOK_DRAW = 1;
		AnimationDuration time = toAnimationDuration(g_platformTime.lastFrameDuration());
		EERIEDrawAnimQuatUpdate(io->obj, player.bookAnimation, angle, pos, time, nullptr, true);
		EERIEDrawAnimQuatRender(io->obj, pos, nullptr, 0.f);
		IN_BOOK_DRAW = 0;

		Halo_Render();
		PopAllTriangleListOpaque(render3D().fog(false));
		PopAllTriangleListTransparency();

		g_culledDynamicLights[0] = savedLights[0];
		g_culledDynamicLights[1] = savedLights[1];
		g_culledDynamicLightsCount = savedLightCount;
		io->obj->vertexWorldPositions = vertices;
		player.m_improve = improve;

		GRenderer->SetScissor(Rect());
		GRenderer->SetAntialiasing(false);
		if(oldCamera) {
			PrepareCamera(oldCamera, Rect(g_size));
		} else {
			GRenderer->SetViewport(Rect(g_size));
		}

	}

	WidgetType type() const override {
		return WidgetType_Text;
	}

};

//! Favorites are stored as "name|address:port;name|address:port".
struct Favorite {
	std::string name;
	std::string address;
};

std::vector<Favorite> loadFavorites() {
	std::vector<Favorite> result;
	std::string_view rest = config.coop.favorites;
	while(!rest.empty()) {
		size_t end = rest.find(';');
		std::string_view entry = rest.substr(0, end);
		rest = (end == std::string_view::npos) ? std::string_view() : rest.substr(end + 1);
		size_t sep = entry.find('|');
		if(sep == std::string_view::npos || sep + 1 >= entry.size()) {
			continue;
		}
		result.push_back(Favorite{ std::string(entry.substr(0, sep)), std::string(entry.substr(sep + 1)) });
	}
	return result;
}

void saveFavorites(const std::vector<Favorite> & favorites) {
	std::string joined;
	for(const Favorite & favorite : favorites) {
		if(!joined.empty()) {
			joined += ';';
		}
		joined += favorite.name + "|" + favorite.address;
	}
	config.coop.favorites = joined;
	config.save();
}

//! The newest regular save (the host's own game), or an invalid handle.
SavegameHandle newestHostSave() {
	SavegameHandle best;
	std::time_t newest = 0;
	for(size_t i = 0; i < savegames.size(); i++) {
		const SaveGame & save = savegames[SavegameHandle(long(i))];
		if(save.name.compare(0, 6, "coop: ") == 0) {
			continue; // a client's character
		}
		if(save.stime >= newest) {
			newest = save.stime;
			best = SavegameHandle(long(i));
		}
	}
	return best;
}

std::string saveLabel(SavegameHandle handle) {
	if(handle == SavegameHandle()) {
		return std::string();
	}
	const SaveGame & save = savegames[handle];
	char when[32] = "";
	if(std::tm * local = std::localtime(&save.stime)) {
		std::strftime(when, sizeof(when), "%d/%m %H:%M", local);
	}
	std::string name = save.name;
	if(name.compare(0, 13, "ARX_QUICK_ARX") == 0) {
		name = coop::trs("coop_quicksave", "sauvegarde rapide");
	}
	return name + " (" + when + ")";
}

//! A text line refreshed every frame from a function (public address arrives asynchronously).
class LiveTextWidget final : public Widget {

	Font * m_font;
	std::function<std::string()> m_text;
	std::string m_current;

public:

	LiveTextWidget(Font * font, float width, std::function<std::string()> text)
		: m_font(font)
		, m_text(std::move(text))
	{
		m_rect = Rectf(Vec2f(0.f), width, float(m_font->getLineHeight()));
	}

	void update() override {
		m_current = m_text();
		// Keep it inside the page
		while(m_current.size() > 3 && float(m_font->getTextSize(m_current).width()) > m_rect.width()) {
			m_current.pop_back();
			while(!m_current.empty() && (u8(m_current.back()) & 0xC0) == 0x80) {
				m_current.pop_back();
			}
			if(float(m_font->getTextSize(m_current + "â¦").width()) <= m_rect.width()) {
				m_current += "â¦";
				break;
			}
		}
	}

	void render(bool /* mouseOver */) override {
		Font::TextSize size = m_font->getTextSize(m_current);
		Vec2f pos(m_rect.center().x - float(size.width()) * 0.5f, m_rect.top);
		m_font->draw(Vec2i(pos), m_current, Color(232, 204, 142));
	}

	WidgetType type() const override {
		return WidgetType_Text;
	}

};

//! Fields shared by the host and join pages (nickname, address, port), saved to the config.
class CoopConnectPage : public MenuPage {

protected:

	TextInputWidget * m_nickname;
	TextInputWidget * m_address;
	TextInputWidget * m_port;
	TextWidget * m_error;

	explicit CoopConnectPage(MENUSTATE id)
		: MenuPage(id)
		, m_nickname(nullptr)
		, m_address(nullptr)
		, m_port(nullptr)
		, m_error(nullptr)
	{ }

	void commitFields() {
		if(m_nickname) {
			m_nickname->unfocus();
			config.coop.nickname = coop::sanitizeNickname(m_nickname->text());
		}
		if(m_address) {
			m_address->unfocus();
			config.coop.address = m_address->text();
		}
		if(m_port) {
			m_port->unfocus();
			config.coop.port = util::toInt(m_port->text()).value_or(coop::DefaultPort);
			if(config.coop.port <= 0 || config.coop.port > 65535) {
				config.coop.port = coop::DefaultPort;
			}
		}
		config.save();
	}

	void addTitle(std::string_view key, std::string_view fallback) {
		auto title = std::make_unique<TextWidget>(hFontMenu, coopText(key, fallback));
		title->setEnabled(false);
		addCenter(std::move(title));
	}

	//! A dimmed explanation line under an entry.
	void addHint(std::string_view key, std::string_view fallback) {
		auto txt = std::make_unique<TextWidget>(hFontControls, coopText(key, fallback));
		txt->setEnabled(false);
		txt->forceDisplay(TextWidget::Enabled);
		addCenter(std::move(txt));
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

	void addErrorLine() {
		auto txt = std::make_unique<TextWidget>(hFontControls, "");
		txt->setEnabled(false);
		txt->forceDisplay(TextWidget::Enabled);
		m_error = txt.get();
		addCenter(std::move(txt));
	}

};

//! Cooperation: host or join, the details come on the next page.
class CoopMenuPage final : public MenuPage {

public:

	CoopMenuPage()
		: MenuPage(Page_Coop)
	{ }

	void init() override {

		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop", "Coopération"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight()));

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_host_page", "Héberger une partie"));
			txt->setTargetPage(Page_CoopHost);
			addCenter(std::move(txt));
		}
		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_join_page", "Rejoindre une partie"));
			txt->setTargetPage(Page_CoopJoin);
			addCenter(std::move(txt));
		}
		addBackButton(Page_None);

	}

};

//! Host page: nickname, port, new game or continue, and the addresses to give to the others.
class CoopHostMenuPage final : public CoopConnectPage {

	std::string addressesLine() const {
		std::string line = coop::trs("coop_my_ip", "Mon IP : ");
		static std::vector<std::string> local = g_coop.localAddresses();
		if(local.empty()) {
			line += "?";
		}
		for(size_t i = 0; i < local.size(); i++) {
			line += (i ? ", " : "") + local[i];
		}
		line += coop::trs("coop_ip_local", " (locale)");
		if(!g_coop.publicAddress().empty()) {
			line += "  -  " + g_coop.publicAddress() + coop::trs("coop_ip_internet", " (internet)");
		}
		return line;
	}

public:

	CoopHostMenuPage()
		: CoopConnectPage(Page_CoopHost)
	{ }

	void focus() override {
		MenuPage::focus();
		g_coop.fetchPublicAddress();
	}

	void init() override {

		reserveTop();
		reserveBottom();

		addTitle("system_menus_coop_host_page", "Héberger une partie");

		{
			auto input = addField("system_menus_coop_nickname", "Pseudo", config.coop.nickname, coop::MaxNicknameLength);
			m_nickname = input.get();
			addCenter(std::move(input));
		}

		{
			auto input = addField("system_menus_coop_port", "Port", std::to_string(config.coop.port), 5);
			m_port = input.get();
			addCenter(std::move(input));
		}
		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_host", "Héberger une nouvelle partie"));
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

		addErrorLine();

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		addCenter(std::make_unique<LiveTextWidget>(hFontControls, m_rect.width(), [this]() { return addressesLine(); }), false);

		addBackButton(Page_Coop);

	}

};

//! Join page: nickname, host address and port, favorites.
class CoopJoinMenuPage final : public CoopConnectPage {

	std::vector<Favorite> m_favorites;

public:

	CoopJoinMenuPage()
		: CoopConnectPage(Page_CoopJoin)
	{ }

	void init() override {

		reserveTop();
		reserveBottom();

		addTitle("system_menus_coop_join_page", "Rejoindre une partie");

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

		// Favorites: pick one to fill the address, remember the current one
		{
			m_favorites = loadFavorites();
			auto slider = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu, coopText("system_menus_coop_favorites", "Favoris"), hFontControls);
			slider->valueChanged = [this](int pos, std::string_view /* string */) {
				if(pos <= 0 || size_t(pos - 1) >= m_favorites.size()) {
					return;
				}
				const Favorite & favorite = m_favorites[size_t(pos - 1)];
				std::string address = favorite.address;
				std::string port = std::to_string(coop::DefaultPort);
				size_t colon = address.rfind(':');
				if(colon != std::string::npos && address.find(':') == colon) {
					port = address.substr(colon + 1);
					address = address.substr(0, colon);
				}
				m_address->setText(address);
				m_port->setText(port);
			};
			slider->addEntry(m_favorites.empty() ? coopText("coop_favorites_none", "- aucun -") : coopText("coop_favorites_pick", "- choisir -"));
			for(const Favorite & favorite : m_favorites) {
				slider->addEntry(favorite.name + "  " + favorite.address);
			}
			slider->setEnabled(!m_favorites.empty());
			addCenter(std::move(slider));
		}

		{
			auto txt = std::make_unique<TextWidget>(hFontControls, coopText("system_menus_coop_favorite_add", "Mémoriser cette adresse dans les favoris"));
			txt->clicked = [this](Widget * /* widget */) {
				commitFields();
				std::string address = config.coop.address + ":" + std::to_string(config.coop.port);
				for(const Favorite & favorite : m_favorites) {
					if(favorite.address == address) {
						return;
					}
				}
				m_favorites.push_back(Favorite{ coop::trs("coop_favorite_name", "Serveur ") + std::to_string(m_favorites.size() + 1), address });
				saveFavorites(m_favorites);
				g_mainMenu->bReInitAll = true;
			};
			addCenter(std::move(txt));
		}

		{
			auto txt = std::make_unique<TextWidget>(hFontControls, coopText("system_menus_coop_favorite_forget", "Oublier le favori de cette adresse"));
			txt->clicked = [this](Widget * /* widget */) {
				commitFields();
				std::string address = config.coop.address + ":" + std::to_string(config.coop.port);
				size_t before = m_favorites.size();
				m_favorites.erase(std::remove_if(m_favorites.begin(), m_favorites.end(), [&address](const Favorite & favorite) {
					return favorite.address == address;
				}), m_favorites.end());
				if(m_favorites.size() != before) {
					saveFavorites(m_favorites);
					g_mainMenu->bReInitAll = true;
				}
			};
			addCenter(std::move(txt));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto txt = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_join", "Rejoindre la partie"));
			txt->clicked = [this](Widget * /* widget */) {
				commitFields();
				m_error->setText("");
				g_coop.join(config.coop.address, u16(config.coop.port), config.coop.nickname);
			};
			txt->setTargetPage(Page_CoopLobby);
			addCenter(std::move(txt));
		}

		addErrorLine();

		addBackButton(Page_Coop);

	}

};

class CoopOptionsMenuPage final : public MenuPage {

	TextWidget * m_faceError;
	std::vector<std::string> m_faces;

	void selectFace(size_t index) {
		std::string file = index < m_faces.size() ? m_faces[index] : std::string();
		if(file == config.coop.face) {
			return;
		}
		config.coop.face = file;
		config.save();
		coop::loadLocalFace();
		ARX_PLAYER_Restore_Skin(); // back to the skin's own textures when "Personnage" is picked
		m_faceError->setText(coop::localFaceError());
	}

	void addToggle(std::string_view key, std::string_view fallback, bool value, std::function<void(bool)> changed) {
		auto cb = std::make_unique<CheckboxWidget>(checkboxSize(), hFontMenu, coopText(key, fallback));
		cb->setChecked(value);
		cb->stateChanged = [changed](bool checked) {
			changed(checked);
			config.save();
		};
		addCenter(std::move(cb));
	}

public:

	CoopOptionsMenuPage()
		: MenuPage(Page_CoopOptions)
		, m_faceError(nullptr)
	{ }

	//! Images added to the faces folder while the page was hidden must show up in the selector.
	void focus() override {
		bool initialized = m_faceError != nullptr;
		MenuPage::focus();
		if(initialized && coop::availableFaces() != m_faces) {
			g_mainMenu->bReInitAll = true;
		}
	}

	void init() override {

		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_options", "Options coop"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		{
			// Custom face: the character's own skin, or an image dropped in <user dir>/coop/faces/
			m_faces = coop::availableFaces();
			auto slider = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu, coopText("system_menus_coop_face", "Visage"), hFontControls);
			slider->valueChanged = [this](int pos, std::string_view /* string */) {
				selectFace(pos <= 0 ? m_faces.size() : size_t(pos - 1));
			};
			slider->addEntry(coopText("system_menus_coop_face_default", "Personnage"));
			for(const std::string & file : m_faces) {
				slider->addEntry(file);
				if(file == config.coop.face) {
					slider->selectLast();
				}
			}
			addCenter(std::move(slider));
		}

		addCenter(std::make_unique<HeadPreviewWidget>(float(hFontMenu->getLineHeight() * 5)));

		{
			auto txt = std::make_unique<TextWidget>(hFontControls, coopText("system_menus_coop_face_folder", "Ouvrir le dossier des visages"));
			txt->clicked = [](Widget * /* widget */) {
				coop::exportFaceTemplates();
				platform::launchDefaultProgram(coop::facesDirectory().string());
			};
			addCenter(std::move(txt));
		}

		{
			auto txt = std::make_unique<TextWidget>(hFontControls, coop::localFaceError());
			txt->setEnabled(false);
			txt->forceDisplay(TextWidget::Enabled);
			m_faceError = txt.get();
			addCenter(std::move(txt));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 3));

		addToggle("system_menus_coop_third_person", "Vue à la 3e personne au lancement", config.coop.thirdPerson,
		          [](bool checked) { config.coop.thirdPerson = checked; });
		addToggle("system_menus_coop_right_shoulder", "Caméra sur l'épaule droite", config.coop.rightShoulder,
		          [](bool checked) { config.coop.rightShoulder = checked; });
		addToggle("system_menus_coop_dialogue_hold", "Me figer quand un coéquipier dialogue", config.coop.dialogueHold,
		          [](bool checked) { config.coop.dialogueHold = checked; });
		addToggle("system_menus_coop_skip_intro", "Sauter la cinématique d'intro", config.misc.skipIntro,
		          [](bool checked) { config.misc.skipIntro = checked; });

		addBackButton(Page_Options);

	}

};

class CoopLobbyMenuPage final : public MenuPage {

	bool m_built;
	bool m_builtAsHost;

public:

	CoopLobbyMenuPage()
		: MenuPage(Page_CoopLobby)
		, m_built(false)
		, m_builtAsHost(false)
	{ }

	//! The host's buttons only exist on the host: rebuild the page if our role changed since.
	void focus() override {
		bool initialized = m_built;
		MenuPage::focus();
		if(initialized && m_builtAsHost != g_coop.isHost()) {
			g_mainMenu->bReInitAll = true;
		}
	}

	void init() override {

		m_built = true;
		m_builtAsHost = g_coop.isHost();
		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_lobby", "Salon"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		// The host's buttons are created first so the roster widget can enable/disable them.
		std::unique_ptr<TextWidget> start;
		std::unique_ptr<TextWidget> cont;
		if(g_coop.isHost()) {
			start = std::make_unique<TextWidget>(hFontMenu, coopText("system_menus_coop_start", "Commencer du début"));
			start->clicked = [](Widget * /* widget */) {
				g_coop.startGame();
			};
			start->setEnabled(false);
			// Continue the latest save with whoever is in the lobby (the others load their character)
			if(SavegameHandle save = newestHostSave(); save != SavegameHandle()) {
				std::string label = std::string(coopText("system_menus_coop_continue_short", "Continuer")) + " : " + saveLabel(save);
				cont = std::make_unique<TextWidget>(hFontMenu, label);
				cont->clicked = [save](Widget * /* widget */) {
					if(g_coop.isHost() && g_coop.state() == coop::State::Lobby) {
						LOADQUEST_SLOT = save;
					}
				};
				cont->setEnabled(false);
			}
		}

		addCenter(std::make_unique<CoopLobbyWidget>(hFontMenu, m_rect.width(), start.get(), cont.get()), false);

		if(start) {
			addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));
			if(cont) {
				addCenter(std::move(cont));
			}
			addCenter(std::move(start));
		}

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

//! In-game tools: the host administers the session, everyone can teleport to a teammate (unstuck).
class CoopAdminMenuPage final : public MenuPage {

	std::vector<coop::PlayerId> m_targets;
	size_t m_target;
	TextInputWidget * m_amount;
	TextInputWidget * m_item;
	std::string m_roster; //!< players the page was built for
	bool m_builtAsHost;

	std::string rosterSignature() const {
		std::string signature = g_coop.isHost() ? "H" : "C";
		for(const coop::Player & who : g_coop.players()) {
			signature += ";" + std::to_string(int(who.id)) + ":" + who.name;
		}
		return signature;
	}

	coop::PlayerId target() const {
		return m_target < m_targets.size() ? m_targets[m_target] : coop::InvalidPlayerId;
	}

	long amount() const {
		if(!m_amount) {
			return 0;
		}
		m_amount->unfocus();
		return std::max(0l, long(util::toInt(m_amount->text()).value_or(0)));
	}

	void addAction(std::string_view key, std::string_view fallback, std::function<void()> action) {
		auto txt = std::make_unique<TextWidget>(hFontControls, coopText(key, fallback));
		txt->clicked = [action](Widget * /* widget */) { action(); };
		addCenter(std::move(txt));
	}

public:

	CoopAdminMenuPage()
		: MenuPage(Page_CoopAdmin)
		, m_target(0)
		, m_amount(nullptr)
		, m_item(nullptr)
		, m_builtAsHost(false)
	{ }

	//! Players come and go: the page follows.
	void focus() override {
		bool built = !m_roster.empty();
		MenuPage::focus();
		if(built && m_roster != rosterSignature()) {
			g_mainMenu->bReInitAll = true;
		}
	}

	void init() override {

		m_roster = rosterSignature();
		m_builtAsHost = g_coop.isHost();
		bool host = m_builtAsHost;

		reserveTop();
		reserveBottom();

		{
			auto title = std::make_unique<TextWidget>(hFontMenu, coopText(host ? "system_menus_coop_admin" : "system_menus_coop_tools",
			                                                              host ? "Administration" : "Outils coop"));
			title->setEnabled(false);
			addCenter(std::move(title));
		}

		// Who the per-player actions apply to
		{
			m_targets.clear();
			auto slider = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu, coopText("system_menus_coop_admin_player", "Joueur"), hFontControls);
			for(const coop::Player & who : g_coop.players()) {
				if(who.id == g_coop.localId() && !host) {
					continue; // a client only ever acts towards the others
				}
				m_targets.push_back(who.id);
				slider->addEntry(who.id == g_coop.localId() ? coop::trs("coop_admin_me", "moi") + " (" + who.name + ")" : who.name);
			}
			if(m_targets.empty()) {
				slider->addEntry(coopText("coop_admin_nobody", "- personne -"));
				slider->setEnabled(false);
			} else if(host && m_targets.size() > 1) {
				m_target = 1; // the first teammate rather than ourselves
				slider->setValue(int(m_target));
			}
			slider->valueChanged = [this](int pos, std::string_view /* string */) {
				m_target = size_t(std::max(0, pos));
			};
			addCenter(std::move(slider));
		}

		if(host) {
			addAction("system_menus_coop_admin_tp_to_me", "Téléporter ce joueur à moi", [this]() { coop::adminTeleportToMe(target()); });
		}
		addAction("system_menus_coop_admin_tp_me", "Me téléporter vers ce joueur", [this]() { coop::adminTeleportMeTo(target()); });

		if(host) {
			addAction("system_menus_coop_admin_heal", "Relever / soigner ce joueur", [this]() { coop::adminHeal(target()); });

			// Amount for gold / XP, class name for items
			{
				auto label = std::make_unique<TextWidget>(hFontControls, coopText("system_menus_coop_admin_amount", "Quantité (or, XP, objets)"));
				label->setEnabled(false);
				label->forceDisplay(TextWidget::Enabled);
				addCenter(std::move(label));
				auto input = std::make_unique<TextInputWidget>(hFontControls, "100", m_rect);
				input->setMaxLength(6);
				m_amount = input.get();
				addCenter(std::move(input));
			}
			addAction("system_menus_coop_admin_gold", "Donner de l'or", [this]() { coop::adminGiveGold(target(), amount()); });
			addAction("system_menus_coop_admin_xp", "Donner de l'XP", [this]() { coop::adminGiveXp(target(), amount()); });
			{
				auto label = std::make_unique<TextWidget>(hFontControls, coopText("system_menus_coop_admin_item", "Objet (nom de classe)"));
				label->setEnabled(false);
				label->forceDisplay(TextWidget::Enabled);
				addCenter(std::move(label));
				auto input = std::make_unique<TextInputWidget>(hFontControls, "potion_life", m_rect);
				input->setMaxLength(40);
				m_item = input.get();
				addCenter(std::move(input));
			}
			addAction("system_menus_coop_admin_give", "Donner cet objet (x quantité)", [this]() {
				m_item->unfocus();
				coop::adminGiveItem(target(), m_item->text(), std::max(1l, amount()));
			});
			addAction("system_menus_coop_admin_kick", "Expulser ce joueur", [this]() { coop::adminKick(target()); });

			addCenter(std::make_unique<Spacer>(hFontControls->getLineHeight() / 2));

			addAction("system_menus_coop_admin_gather", "Tout le monde à moi", []() { coop::adminGatherAll(); });
			addAction("system_menus_coop_admin_heal_all", "Soigner tout le monde", []() { coop::adminHealAll(); });
			{
				auto cb = std::make_unique<CheckboxWidget>(checkboxSize(), hFontControls, coopText("system_menus_coop_admin_invulnerable", "Invulnérabilité pour tous"));
				cb->setChecked(coop::adminInvulnerableAll());
				cb->stateChanged = [](bool checked) { coop::adminSetInvulnerable(true, checked); };
				addCenter(std::move(cb));
			}
			addAction("system_menus_coop_admin_kill", "Tuer les PNJ hostiles autour de moi (20 m)", []() { coop::adminKillHostiles(2000.f); });
			addAction("system_menus_coop_admin_save", "Sauvegarder maintenant", []() { coop::adminSaveNow(); });
		}

		addCenter(std::make_unique<Spacer>(hFontControls->getLineHeight() / 2));

		addCenter(std::make_unique<LiveTextWidget>(hFontControls, m_rect.width(), []() { return coop::adminRosterLine(); }), false);
		addCenter(std::make_unique<LiveTextWidget>(hFontControls, m_rect.width(), []() { return coop::adminStatus(); }), false);

		addBackButton(Page_None);

	}

};

} // anonymous namespace

std::unique_ptr<MenuPage> createCoopMenuPage() {
	return std::make_unique<CoopMenuPage>();
}

std::unique_ptr<MenuPage> createCoopHostMenuPage() {
	return std::make_unique<CoopHostMenuPage>();
}

std::unique_ptr<MenuPage> createCoopJoinMenuPage() {
	return std::make_unique<CoopJoinMenuPage>();
}

std::unique_ptr<MenuPage> createCoopLobbyMenuPage() {
	return std::make_unique<CoopLobbyMenuPage>();
}

std::unique_ptr<MenuPage> createCoopAdminMenuPage() {
	return std::make_unique<CoopAdminMenuPage>();
}

std::unique_ptr<MenuPage> createCoopOptionsMenuPage() {
	return std::make_unique<CoopOptionsMenuPage>();
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
	if(coop::consumeAdminPageRequest() && g_coop.state() == coop::State::InGame) {
		g_mainMenu->requestPage(Page_CoopAdmin);
	}
}
