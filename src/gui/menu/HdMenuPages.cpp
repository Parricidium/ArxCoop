/*
 * Copyright 2026 Arx Libertatis Team (see the AUTHORS file)
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

#include "gui/menu/HdMenuPages.h"

#include <algorithm>
#include <cmath>

#include "core/Config.h"
#include "core/Localisation.h"
#include "graphics/Renderer.h"
#include "graphics/font/Font.h"
#include "physics/PhysicsWorld.h"
#include "gui/MenuWidgets.h"
#include "gui/Text.h"
#include "gui/menu/MenuPage.h"
#include "gui/widget/CheckboxWidget.h"
#include "gui/widget/CycleTextWidget.h"
#include "gui/widget/SliderWidget.h"
#include "gui/widget/Spacer.h"
#include "gui/widget/TextWidget.h"

// French is the built-in fallback, English lives in localisation/xtext_english_003_arxmodern.ini
static std::string_view hdText(std::string_view key, std::string_view fr) {
	return getLocalised(key, fr);
}

enum HdPreset {
	HdOff = 0,
	HdLow,
	HdMedium,
	HdHigh,
	HdUltra,
	HdCustom
};

void applyHdSettings() {
	GRenderer->applyGraphicsConfig();
	// The physics world follows the setting at once (the level mesh is rebuilt when turned on)
	if(config.video.physics && !physics::isActive()) {
		physics::levelLoaded();
	} else if(!config.video.physics && physics::isActive()) {
		physics::levelCleared();
	}
}

void applyHdPreset(int preset) {

	switch(preset) {
		case HdOff: {
			config.video.pipeline = "fixed";
			config.video.lighting = "vertex";
			config.video.shadows = 0;
			config.video.postprocess = false;
			config.video.bloom = 0.f;
			config.video.fxaa = false;
			config.video.smaa = false;
			config.video.ambientOcclusion = 0.f;
			config.video.normalMaps = 0.f;
			config.video.parallax = 0.f;
			config.video.specular = 0.f;
			config.video.reflections = 0.f;
			config.video.water = 0.f;
			config.video.lava = 0.f;
			config.video.softParticles = false;
			break;
		}
		case HdLow: {
			config.video.pipeline = "auto";
			config.video.lighting = "pixel";
			config.video.shadows = 0;
			config.video.postprocess = true;
			config.video.bloom = 0.35f;
			config.video.fxaa = false;
			config.video.smaa = false;
			config.video.ambientOcclusion = 0.f;
			config.video.normalMaps = 1.f;
			config.video.parallax = 0.f;
			config.video.specular = 1.f;
			config.video.reflections = 0.f;
			config.video.water = 1.f;
			config.video.lava = 1.f;
			config.video.softParticles = true;
			break;
		}
		case HdMedium: {
			config.video.pipeline = "auto";
			config.video.lighting = "pixel";
			config.video.shadows = 2;
			config.video.shadowResolution = 512;
			config.video.postprocess = true;
			config.video.bloom = 0.35f;
			config.video.fxaa = false;
			config.video.smaa = false;
			config.video.ambientOcclusion = 0.f;
			config.video.normalMaps = 1.f;
			config.video.parallax = 1.f;
			config.video.specular = 1.f;
			config.video.reflections = 1.f;
			config.video.water = 1.f;
			config.video.lava = 1.f;
			config.video.softParticles = true;
			break;
		}
		case HdHigh: {
			config.video.pipeline = "auto";
			config.video.lighting = "pixel";
			config.video.shadows = 4;
			config.video.shadowResolution = 1024;
			config.video.postprocess = true;
			config.video.bloom = 0.35f;
			config.video.fxaa = false;
			config.video.smaa = true;
			config.video.ambientOcclusion = 0.f;
			config.video.normalMaps = 1.f;
			config.video.parallax = 1.f;
			config.video.specular = 1.f;
			config.video.reflections = 1.f;
			config.video.water = 1.f;
			config.video.lava = 1.f;
			config.video.softParticles = true;
			break;
		}
		case HdUltra: {
			config.video.pipeline = "auto";
			config.video.lighting = "pixel";
			config.video.shadows = 4;
			config.video.shadowResolution = 2048;
			config.video.postprocess = true;
			config.video.bloom = 0.4f;
			config.video.fxaa = false;
			config.video.smaa = true;
			config.video.ambientOcclusion = 0.5f;
			config.video.normalMaps = 1.f;
			config.video.parallax = 1.f;
			config.video.specular = 1.f;
			config.video.reflections = 1.f;
			config.video.water = 1.f;
			config.video.lava = 1.f;
			config.video.softParticles = true;
			break;
		}
		default: return;
	}

	applyHdSettings();
}

//! Which preset matches the current settings exactly, or HdCustom
static int detectHdPreset() {

	if(config.video.pipeline == "fixed") {
		return HdOff;
	}
	if(config.video.lighting != "pixel" || !config.video.postprocess || config.video.fxaa) {
		return HdCustom;
	}
	if(std::abs(config.video.water - 1.f) > 0.01f || std::abs(config.video.lava - 1.f) > 0.01f) {
		return HdCustom;
	}
	if(std::abs(config.video.normalMaps - 1.f) > 0.01f || std::abs(config.video.specular - 1.f) > 0.01f
	   || !config.video.softParticles) {
		return HdCustom;
	}
	bool bloom = std::abs(config.video.bloom - 0.35f) < 0.01f;
	bool noAo = config.video.ambientOcclusion <= 0.f;
	bool materials = std::abs(config.video.parallax - 1.f) < 0.01f && std::abs(config.video.reflections - 1.f) < 0.01f;
	bool flat = config.video.parallax <= 0.f && config.video.reflections <= 0.f;
	if(config.video.shadows == 0 && bloom && noAo && flat && !config.video.smaa) {
		return HdLow;
	}
	if(config.video.shadows == 2 && config.video.shadowResolution == 512 && bloom && noAo && materials && !config.video.smaa) {
		return HdMedium;
	}
	if(config.video.shadows == 4 && config.video.shadowResolution == 1024 && bloom && noAo && materials && config.video.smaa) {
		return HdHigh;
	}
	if(config.video.shadows == 4 && config.video.shadowResolution == 2048
	   && std::abs(config.video.bloom - 0.4f) < 0.01f && std::abs(config.video.ambientOcclusion - 0.5f) < 0.01f) {
		return HdUltra;
	}

	return HdCustom;
}

class HdOptionsMenuPage final : public MenuPage {

public:

	HdOptionsMenuPage()
		: MenuPage(Page_OptionsHd)
		, m_preset(nullptr)
		, m_lighting(nullptr)
		, m_shadows(nullptr)
		, m_shadowResolution(nullptr)
		, m_bloom(nullptr)
		, m_physics(nullptr)
		, m_ao(nullptr)
		, m_normalMaps(nullptr)
		, m_water(nullptr)
		, m_parallax(nullptr)
		, m_reflections(nullptr)
		, m_softParticles(nullptr)
		, m_raytracing(nullptr)
		, m_darkness(nullptr)
		, m_volumetric(nullptr)
		, m_antialiasing(nullptr)
	{ }

	void init() override {

		reserveBottom();

		// Quality preset
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_quality", "Qualité HD"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_off", "Désactivée"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_low", "Faible"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_medium", "Moyenne"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_high", "Élevée"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_ultra", "Ultra"));
			cycle->addEntry(hdText("system_menus_options_hd_quality_custom", "Personnalisée"));
			cycle->valueChanged = [this](int pos, std::string_view /* string */) {
				if(pos == HdCustom) {
					// "Custom" is only a display state: nothing to apply
					return;
				}
				applyHdPreset(pos);
				refresh();
			};
			m_preset = cycle.get();
			addCenter(std::move(cycle));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		// Per-pixel lighting
		{
			auto cb = std::make_unique<CheckboxWidget>(checkboxSize(), hFontMenu,
			                                           hdText("system_menus_options_hd_lighting", "Éclairage par pixel"));
			cb->stateChanged = [this](bool checked) {
				config.video.lighting = checked ? "pixel" : "vertex";
				customChanged();
			};
			m_lighting = cb.get();
			addCenter(std::move(cb));
		}

		// Shadowed lights
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_shadows", "Ombres dynamiques"));
			cycle->addEntry(hdText("system_menus_options_hd_shadows_off", "Désactivées"));
			cycle->addEntry(hdText("system_menus_options_hd_shadows_1", "1 lumière"));
			cycle->addEntry(hdText("system_menus_options_hd_shadows_2", "2 lumières"));
			cycle->addEntry(hdText("system_menus_options_hd_shadows_4", "4 lumières"));
			cycle->valueChanged = [this](int pos, std::string_view /* string */) {
				static const int counts[] = { 0, 1, 2, 4 };
				config.video.shadows = counts[std::clamp(pos, 0, 3)];
				customChanged();
			};
			m_shadows = cycle.get();
			addCenter(std::move(cycle));
		}

		// Shadow resolution
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_shadow_resolution", "Finesse des ombres"));
			cycle->addEntry("512");
			cycle->addEntry("1024");
			cycle->addEntry("2048");
			cycle->valueChanged = [this](int pos, std::string_view /* string */) {
				static const int sizes[] = { 512, 1024, 2048 };
				config.video.shadowResolution = sizes[std::clamp(pos, 0, 2)];
				customChanged();
			};
			m_shadowResolution = cycle.get();
			addCenter(std::move(cycle));
		}

		// Normal maps (relief), slider 0..10 = strength 0..2
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_normal_maps", "Relief des textures"));
			slider->valueChanged = [this](int value) {
				config.video.normalMaps = float(value) * 0.2f;
				customChanged();
			};
			m_normalMaps = slider.get();
			addCenter(std::move(slider));
		}

		// Parallax relief, slider 0..10 = depth 0..2
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_parallax", "Relief en profondeur"));
			slider->valueChanged = [this](int value) {
				config.video.parallax = float(value) * 0.2f;
				customChanged();
			};
			m_parallax = slider.get();
			addCenter(std::move(slider));
		}

		// Specular highlights and screen-space reflections, slider 0..10
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_reflections", "Reflets des matériaux"));
			slider->valueChanged = [this](int value) {
				config.video.specular = float(value) * 0.2f;
				config.video.reflections = std::min(float(value) * 0.2f, 1.f);
				config.video.postprocess = true;
				customChanged();
			};
			m_reflections = slider.get();
			addCenter(std::move(slider));
		}

		// Water and lava shaders, slider 0..10 = strength 0..1 (0 = the original overlays)
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_water", "Eau et lave"));
			slider->valueChanged = [this](int value) {
				config.video.water = float(value) * 0.1f;
				config.video.lava = float(value) * 0.1f;
				config.video.postprocess = true;
				customChanged();
			};
			m_water = slider.get();
			addCenter(std::move(slider));
		}

		// Bloom
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_bloom", "Halo des lumières (bloom)"));
			slider->valueChanged = [this](int value) {
				config.video.bloom = float(value) * 0.1f;
				config.video.postprocess = true;
				customChanged();
			};
			m_bloom = slider.get();
			addCenter(std::move(slider));
		}

		// Ambient occlusion
		{
			auto slider = std::make_unique<SliderWidget>(sliderSize(), hFontMenu,
			                                             hdText("system_menus_options_hd_ao", "Occlusion ambiante"));
			slider->valueChanged = [this](int value) {
				config.video.ambientOcclusion = float(value) * 0.1f;
				config.video.postprocess = true;
				customChanged();
			};
			m_ao = slider.get();
			addCenter(std::move(slider));
		}

		// Anti-aliasing filter
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_antialiasing", "Anticrénelage"));
			cycle->addEntry(hdText("system_menus_options_hd_antialiasing_off", "Désactivé"));
			cycle->addEntry("FXAA");
			cycle->addEntry("SMAA");
			cycle->valueChanged = [this](int pos, std::string_view /* string */) {
				config.video.fxaa = (pos == 1);
				config.video.smaa = (pos == 2);
				config.video.postprocess = true;
				customChanged();
			};
			m_antialiasing = cycle.get();
			addCenter(std::move(cycle));
		}

		// Ambiance: how dark the unlit places are (independent of the quality presets)
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_darkness", "Ambiance"));
			cycle->addEntry(hdText("system_menus_options_hd_darkness_normal", "Normale"));
			cycle->addEntry(hdText("system_menus_options_hd_darkness_dark", "Sombre"));
			cycle->addEntry(hdText("system_menus_options_hd_darkness_darker", "Obscure"));
			cycle->valueChanged = [](int pos, std::string_view /* string */) {
				config.video.darkness = float(pos) * 0.5f;
				if(pos > 0) {
					config.video.postprocess = true;
				}
				applyHdSettings();
			};
			m_darkness = cycle.get();
			addCenter(std::move(cycle));
		}

		// Volumetric haze (independent of the quality presets)
		{
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_volumetric", "Brume volumétrique"));
			cycle->addEntry(hdText("system_menus_options_hd_volumetric_off", "Désactivée"));
			cycle->addEntry(hdText("system_menus_options_hd_volumetric_light", "Légère"));
			cycle->addEntry(hdText("system_menus_options_hd_volumetric_dense", "Dense"));
			cycle->valueChanged = [](int pos, std::string_view /* string */) {
				config.video.volumetric = float(pos) * 0.5f;
				if(pos > 0) {
					config.video.postprocess = true;
				}
				applyHdSettings();
			};
			m_volumetric = cycle.get();
			addCenter(std::move(cycle));
		}

		// Ray tracing (independent of the quality presets, needs OpenGL 4.3)
		if(GRenderer->hasRayTracing()) {
			auto cycle = std::make_unique<CycleTextWidget>(sliderSize(), hFontMenu,
			                                               hdText("system_menus_options_hd_raytracing", "Ray tracing"));
			cycle->addEntry(hdText("system_menus_options_hd_raytracing_off", "Désactivé"));
			cycle->addEntry(hdText("system_menus_options_hd_raytracing_reflections", "Reflets"));
			cycle->addEntry(hdText("system_menus_options_hd_raytracing_shadows", "Reflets + ombres"));
			cycle->valueChanged = [](int pos, std::string_view /* string */) {
				config.video.raytracing = pos;
				if(pos > 0) {
					config.video.postprocess = true;
					if(config.video.reflections <= 0.f) {
						config.video.reflections = 1.f;
					}
				}
				applyHdSettings();
			};
			m_raytracing = cycle.get();
			addCenter(std::move(cycle));
		}

		// Soft particles
		{
			auto cb = std::make_unique<CheckboxWidget>(checkboxSize(), hFontMenu,
			                                           hdText("system_menus_options_hd_soft_particles", "Particules douces"));
			cb->stateChanged = [this](bool checked) {
				config.video.softParticles = checked;
				config.video.postprocess = true;
				customChanged();
			};
			m_softParticles = cb.get();
			addCenter(std::move(cb));
		}

		// Physics (independent of the quality presets)
		if(physics::isAvailable()) {
			auto cb = std::make_unique<CheckboxWidget>(checkboxSize(), hFontMenu,
			                                           hdText("system_menus_options_hd_physics", "Physique : cadavres, objets, tissus"));
			cb->stateChanged = [](bool checked) {
				config.video.physics = checked;
				applyHdSettings();
			};
			m_physics = cb.get();
			addCenter(std::move(cb));
		}

		addCenter(std::make_unique<Spacer>(hFontMenu->getLineHeight() / 2));

		{
			auto txt = std::make_unique<TextWidget>(hFontControls,
			                                        hdText("system_menus_options_hd_note", "F7 recharge les shaders (Graph/shaders)"));
			txt->setEnabled(false);
			addCenter(std::move(txt));
		}

		addBackButton(Page_Options);

		refresh();
	}

	void focus() override {
		MenuPage::focus();
		refresh();
	}

private:

	CycleTextWidget * m_preset;
	CheckboxWidget * m_lighting;
	CycleTextWidget * m_shadows;
	CycleTextWidget * m_shadowResolution;
	SliderWidget * m_bloom;
	CheckboxWidget * m_physics;
	SliderWidget * m_ao;
	SliderWidget * m_normalMaps;
	SliderWidget * m_water;
	SliderWidget * m_parallax;
	SliderWidget * m_reflections;
	CheckboxWidget * m_softParticles;
	CycleTextWidget * m_antialiasing;
	CycleTextWidget * m_raytracing;
	CycleTextWidget * m_darkness;
	CycleTextWidget * m_volumetric;

	//! An individual setting changed: the modern pipeline is needed, apply and show "custom"
	void customChanged() {
		if(config.video.pipeline == "fixed") {
			config.video.pipeline = "auto";
		}
		applyHdSettings();
		refresh();
	}

	//! Update the widgets from the configuration
	void refresh() {

		if(m_preset) {
			m_preset->setValue(detectHdPreset());
		}
		if(m_lighting) {
			m_lighting->setChecked(config.video.pipeline != "fixed" && config.video.lighting == "pixel");
		}
		if(m_shadows) {
			int pos = (config.video.shadows >= 4) ? 3 : (config.video.shadows >= 2) ? 2 : (config.video.shadows >= 1) ? 1 : 0;
			m_shadows->setValue(pos);
		}
		if(m_shadowResolution) {
			int pos = (config.video.shadowResolution >= 2048) ? 2 : (config.video.shadowResolution >= 1024) ? 1 : 0;
			m_shadowResolution->setValue(pos);
		}
		if(m_bloom) {
			m_bloom->setValue(config.video.postprocess ? int(std::lround(config.video.bloom * 10.f)) : 0);
		}
		if(m_ao) {
			m_ao->setValue(config.video.postprocess ? int(std::lround(config.video.ambientOcclusion * 10.f)) : 0);
		}
		if(m_antialiasing) {
			m_antialiasing->setValue(!config.video.postprocess ? 0 : config.video.smaa ? 2 : config.video.fxaa ? 1 : 0);
		}
		if(m_raytracing) {
			m_raytracing->setValue(std::clamp(config.video.raytracing, 0, 2));
		}
		if(m_darkness) {
			m_darkness->setValue(std::clamp(int(config.video.darkness * 2.f + 0.5f), 0, 2));
		}
		if(m_volumetric) {
			m_volumetric->setValue(std::clamp(int(config.video.volumetric * 2.f + 0.5f), 0, 2));
		}
		if(m_softParticles) {
			m_softParticles->setChecked(config.video.postprocess && config.video.softParticles);
		}
		if(m_parallax) {
			m_parallax->setValue(config.video.pipeline != "fixed" ? int(std::lround(config.video.parallax * 5.f)) : 0);
		}
		if(m_reflections) {
			m_reflections->setValue(config.video.pipeline != "fixed" ? int(std::lround(config.video.specular * 5.f)) : 0);
		}
		if(m_physics) {
			m_physics->setChecked(config.video.physics);
		}
		if(m_normalMaps) {
			m_normalMaps->setValue(config.video.pipeline != "fixed" ? int(std::lround(config.video.normalMaps * 5.f)) : 0);
		}
		if(m_water) {
			m_water->setValue(config.video.pipeline != "fixed" && config.video.postprocess ? int(std::lround(config.video.water * 10.f)) : 0);
		}

	}

};

std::unique_ptr<MenuPage> createHdOptionsMenuPage() {
	return std::make_unique<HdOptionsMenuPage>();
}
