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

#ifndef ARX_COOP_TEXT_H
#define ARX_COOP_TEXT_H

#include <string>
#include <string_view>

#include "core/Localisation.h"

namespace coop {

/*!
 * User-facing text of the co-op mod: French by default (the fallback written in the code),
 * translated through the game's localisation files (localisation/xtext_<language>_*_arxcoop.ini).
 */
inline std::string_view tr(std::string_view key, std::string_view french) {
	return getLocalised(key, french);
}

inline std::string trs(std::string_view key, std::string_view french) {
	return std::string(tr(key, french));
}

} // namespace coop

#endif // ARX_COOP_TEXT_H
