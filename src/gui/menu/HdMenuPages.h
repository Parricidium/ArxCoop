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

#ifndef ARX_GUI_MENU_HDMENUPAGES_H
#define ARX_GUI_MENU_HDMENUPAGES_H

#include <memory>

class MenuPage;

/*!
 * ArxModern: the "Options HD" menu page - quality presets and the individual settings of the
 * modern renderer (per-pixel lighting, shadows, bloom, FXAA, ambient occlusion).
 * Changes are applied to the renderer immediately and saved with the configuration.
 */
std::unique_ptr<MenuPage> createHdOptionsMenuPage();

//! Apply a quality preset (0 = off ... 4 = ultra) to the configuration and the renderer
void applyHdPreset(int preset);

//! Push the current [video] HD settings to the renderer
void applyHdSettings();

#endif // ARX_GUI_MENU_HDMENUPAGES_H
