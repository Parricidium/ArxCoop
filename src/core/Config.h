/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
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

#ifndef ARX_CORE_CONFIG_H
#define ARX_CORE_CONFIG_H

#include <string>

#include "audio/AudioTypes.h"

#include "input/InputKey.h"

#include "io/fs/FilePath.h"

#include "math/Types.h"
#include "math/Vector.h"

#include "window/DisplayMode.h"

//! Enum for all the controlling actions
enum ControlAction {
	CONTROLS_CUST_JUMP = 0,
	CONTROLS_CUST_MAGICMODE,
	CONTROLS_CUST_STEALTHMODE,
	CONTROLS_CUST_WALKFORWARD,
	CONTROLS_CUST_WALKBACKWARD,
	CONTROLS_CUST_STRAFELEFT,
	CONTROLS_CUST_STRAFERIGHT,
	CONTROLS_CUST_LEANLEFT,
	CONTROLS_CUST_LEANRIGHT,
	CONTROLS_CUST_CROUCH,
	CONTROLS_CUST_USE,
	CONTROLS_CUST_ACTION,
	CONTROLS_CUST_INVENTORY,
	CONTROLS_CUST_BOOK,
	CONTROLS_CUST_BOOKCHARSHEET,
	CONTROLS_CUST_BOOKSPELL,
	CONTROLS_CUST_BOOKMAP,
	CONTROLS_CUST_BOOKQUEST,
	CONTROLS_CUST_DRINKPOTIONLIFE,
	CONTROLS_CUST_DRINKPOTIONMANA,
	CONTROLS_CUST_DRINKPOTIONCURE,
	CONTROLS_CUST_TORCH,
	CONTROLS_CUST_PRECAST1,
	CONTROLS_CUST_PRECAST2,
	CONTROLS_CUST_PRECAST3,
	CONTROLS_CUST_WEAPON,
	CONTROLS_CUST_QUICKLOAD,
	CONTROLS_CUST_QUICKSAVE,
	CONTROLS_CUST_TURNLEFT,
	CONTROLS_CUST_TURNRIGHT,
	CONTROLS_CUST_LOOKUP,
	CONTROLS_CUST_LOOKDOWN,
	CONTROLS_CUST_STRAFE,
	CONTROLS_CUST_CENTERVIEW,
	CONTROLS_CUST_FREELOOK,
	CONTROLS_CUST_PREVIOUS,
	CONTROLS_CUST_NEXT,
	CONTROLS_CUST_CROUCHTOGGLE,
	CONTROLS_CUST_UNEQUIPWEAPON,
	CONTROLS_CUST_CANCELCURSPELL,
	CONTROLS_CUST_MINIMAP,
	CONTROLS_CUST_TOGGLE_FULLSCREEN,
	CONTROLS_CUST_CONSOLE,
	CONTROLS_CUST_DEBUG,
	CONTROLS_CUST_THIRDPERSON,   //!< co-op mod: first / third person view
	CONTROLS_CUST_CAMERA_ORBIT,  //!< co-op mod: third person, orbit the camera without turning
	CONTROLS_CUST_SWAP_SHOULDER, //!< co-op mod: third person, camera on the other shoulder
	CONTROLS_CUST_CAMERA_ZOOM_IN,  //!< co-op mod: third person, camera closer
	CONTROLS_CUST_CAMERA_ZOOM_OUT, //!< co-op mod: third person, camera further
	CONTROLS_CUST_PING,            //!< co-op mod: "look here" marker for the teammates
	CONTROLS_CUST_ADMIN,           //!< co-op mod: in-game administration / tools page
	CONTROLS_CUST_ROLL,            //!< co-op mod: dodge roll (coop/Roll.cpp)
	CONTROLS_CUST_SPRAY,           //!< co-op mod: paint the spray tag (coop/Spray.cpp)
	CONTROLS_CUST_KICK,            //!< co-op mod: the kick (coop/Kick.cpp)
NUM_ACTION_KEY
};

enum CinematicWidescreenMode {
	CinematicLetterbox = 0,
	CinematicHardEdges = 1,
	CinematicFadeEdges = 2
};

enum UIScaleFilter {
	UIFilterNearest = 0,
	UIFilterBilinear = 1
};

enum QuickLevelTransition {
	NoQuickLevelTransition = 0,
	JumpToChangeLevel = 1,
	ChangeLevelImmediately = 2,
};

enum AutoReadyWeapon {
	NeverAutoReadyWeapon = 0,
	AutoReadyWeaponNearEnemies = 1,
	AlwaysAutoReadyWeapon = 2,
};

struct ActionKey {
	
	explicit constexpr ActionKey(InputKeyId key_0 = UNUSED,
	                             InputKeyId key_1 = UNUSED)
		: key{ key_0, (key_0 != UNUSED && key_0 == key_1) ? UNUSED : key_1 }
	{ }
	
	InputKeyId key[2];
	static const InputKeyId UNUSED = -1;
};

class Config {
	
public:
	
	// section 'video'
	struct {
		
		std::string renderer;
		
		bool fullscreen;
		DisplayMode mode;
		float gamma;
		
		int vsync;
		int fpsLimit;
		
		float fov;
		bool viewBobbing;
		bool screenShake;
		
		int levelOfDetail;
		float fogDistance;
		bool antialiasing;
		int maxAnisotropicFiltering;
		bool colorkeyAntialiasing;
		int alphaCutoutAntialiasing;
		
		int bufferSize;
		std::string bufferUpload;
		std::string extensionOverride;
		
		// ArxModern: "auto", "fixed" (legacy fixed-function pipeline) or "shader"
		std::string pipeline;
		// ArxModern: "vertex" (legacy CPU lighting) or "pixel" (dynamic lights in the fragment shader)
		std::string lighting;
		// ArxModern: number of dynamic lights casting shadows (0 = off) and cube map face size
		int shadows;
		int shadowResolution;
		int raytracing; // ArxModern RT: 0 = off, 1 = traced reflections, 2 = + traced shadows, 3 = + traced occlusion and indirect light, 4 = + shadows of the static lights (OpenGL 4.3)
		float raytracingBounce; // strength of the traced indirect light (mode 3+), 1 = default
		float raytracingStaticMix; // 0..1, share of the original static lighting kept under the traced static shadows (mode 4)
		// ArxModern: off-screen scene + full-screen passes (bloom intensity 0 = off, FXAA)
		bool postprocess;
		float bloom;
		bool fxaa;
		float ambientOcclusion;
		float darkness; // ArxModern: 0 = as lit, 1 = the dark places go really dark (torches matter)
		float volumetric; // ArxModern: density of the volumetric haze, 0 = off
		float volumetricLight; // ArxModern: strength of the light shafts (and their shadows) in the haze, 1 = default
		float normalMaps; // relief strength of the (generated or provided) normal maps, 0 = off
		bool physics; // Jolt physics: ragdoll corpses, loose objects
		float water; // water shader strength, 0 = the original overlay
		bool waterRipples; // ArxModern: rings and wakes on the water from what moves in it (GLRipples.cpp)
		bool bloodTrails; // ArxModern: bloody footprints after stepping in blood (Decal.cpp)
float lava; // lava shader strength, 0 = the original overlay
		float parallax; // depth of the parallax relief, 0 = off
		float specular; // strength of the specular highlights, 0 = off
		float reflections; // strength of the screen-space reflections on glossy floors, 0 = off
		bool softParticles; // particles fade against the geometry
		bool smaa; // SMAA anti-aliasing (takes precedence over fxaa)
		std::string postDebug; // "", "ao" or "bloom": show that buffer instead of the scene
		
	} video;
	
	// section 'interface'
	struct {
		
		std::string language;
		
		bool showCrosshair;
		
		bool limitSpeechWidth;
		CinematicWidescreenMode cinematicWidescreenMode;
		
		float hudScale;
		bool hudScaleInteger;
		float bookScale;
		bool bookScaleInteger;
		float cursorScale;
		bool cursorScaleInteger;
		UIScaleFilter scaleFilter;
		
		float fontSize;
		int fontWeight;
		
		Vec2i thumbnailSize;
		
	} interface;
	
	// section 'window'
	struct {
		
		Vec2i size;
		
		bool minimizeOnFocusLost;
		
	} window;
	
	// section 'audio'
	struct {
		
		std::string language;
		
		std::string backend;
		std::string device;
		
		float volume;
		float sfxVolume;
		float speechVolume;
		float ambianceVolume;
		
		bool eax;
		audio::HRTFAttribute hrtf;
		bool muteOnFocusLost;
		
	} audio;
	
	// section 'input'
	struct {
		
		bool invertMouse;
		AutoReadyWeapon autoReadyWeapon;
		bool mouseLookToggle;
		bool autoDescription;
		int mouseSensitivity;
		int mouseAcceleration;
		bool rawMouseInput;
		bool borderTurning;
		bool useAltRuneRecognition;
		bool improvedBowAim;
		QuickLevelTransition quickLevelTransition;
		bool allowConsole;
		
	} input;
	
	// section 'key'
	ActionKey actions[NUM_ACTION_KEY];
	
	enum MigrationStatus {
		OriginalAssets = 0,
		CaseSensitiveFilenames = 1
	};
	
	// section 'misc'
	struct {
		
		bool forceToggle; // should be in input?
		
		MigrationStatus migration;
		
		int quicksaveSlots;
		
		std::string debug; //!< Logger debug levels.
		
		std::string realtimeOverride;
		
		bool skipIntro; //!< co-op mod: go straight to the main menu at startup
		
	} misc;
	
	// section 'coop'
	struct {
		
		std::string nickname;
		
		std::string address; //!< Last host address used to join
		
		int port;
		
		std::string face; //!< Custom face image (file name in <user dir>/coop/faces/), empty = the character's skin
		std::string spray; //!< Spray tag image (file name in <user dir>/coop/sprays/), empty = none

		std::string favorites; //!< "name|address:port;name|address:port"

		bool dialogueHold; //!< freeze me while a teammate is in a cinematic dialogue

		bool thirdPerson;  //!< last camera mode

		bool rightShoulder;

	} coop;
	
	bool setActionKey(ControlAction actionId, size_t index, InputKeyId key);
	void setDefaultActionKeys();
	
	/*!
	 * Saves all config entries to a file.
	 * \return true if the config was saved successfully.
	 */
	bool save();
	
	bool init(const fs::path & file);
	
	void setOutputFile(const fs::path & file);
	
private:
	
	fs::path m_file;
	
};

extern Config config;

#endif // ARX_CORE_CONFIG_H
