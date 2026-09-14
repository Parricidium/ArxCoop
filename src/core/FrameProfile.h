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

#ifndef ARX_CORE_FRAMEPROFILE_H
#define ARX_CORE_FRAMEPROFILE_H

#include "platform/Platform.h"

//! Where the last frame's time went, in milliseconds (hitch diagnostics, see coop/PhysicsSync.cpp)
struct FrameProfile {
	s64 network = 0; //!< co-op session update (socket polling, message handling)
	s64 update = 0;  //!< updateLevel(): game logic, physics, entities
	s64 render = 0;  //!< renderLevel() without the shadow pass and the post-processing
	s64 shadows = 0; //!< shadow map pass
	s64 post = 0;    //!< post-processing (endScene)
	s64 swap = 0;    //!< showFrame(): waiting for the GPU / vsync
};

extern FrameProfile g_frameProfile;     //!< being measured
extern FrameProfile g_lastFrameProfile; //!< the previous frame, complete

//! Named sections of updateLevel(), milliseconds, for the previous frame ("name=ms name=ms ...")
struct FrameSections {
	static constexpr size_t Max = 32;
	const char * name[Max] = { };
	s64 ms[Max] = { };
	size_t count = 0;
	void add(const char * sectionName, s64 sectionMs) {
		if(count < Max) {
			name[count] = sectionName;
			ms[count] = sectionMs;
			count++;
		}
	}
};
extern FrameSections g_frameSections;
extern FrameSections g_lastFrameSections;

#include "platform/Time.h"
#include "core/TimeTypes.h"

//! RAII timer of one section of the frame
class FrameSectionTimer {
	const char * m_name;
	PlatformInstant m_start;
public:
	explicit FrameSectionTimer(const char * name) : m_name(name), m_start(platform::getTime()) { }
	~FrameSectionTimer() { g_frameSections.add(m_name, toMsi(platform::getTime() - m_start)); }
};
#define FRAME_SECTION(name) FrameSectionTimer frameSection_##__LINE__(name)

#endif // ARX_CORE_FRAMEPROFILE_H
