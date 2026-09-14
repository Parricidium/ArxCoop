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

#include "coop/PhysicsSync.h"

#include <chrono>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "coop/Protocol.h"
#include "coop/Puppets.h"
#include "coop/Session.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "io/log/Logger.h"
#include "physics/LooseObjects.h"
#include "physics/Ragdoll.h"
#include "platform/Time.h"

namespace coop {

namespace {

constexpr PlatformDuration SendInterval = std::chrono::milliseconds(66); // 15 Hz while moving
constexpr PlatformDuration KeyframeInterval = std::chrono::seconds(5);   // everything, for late joiners

struct SentState {
	bool active = false;
	PlatformInstant sent;
};

PlatformInstant g_lastSend;
std::unordered_map<std::string, SentState> g_sentRagdolls;
std::unordered_map<std::string, SentState> g_sentObjects;
std::vector<physics::BonePose> g_bones;

void writeRagdoll(Writer & out, Entity & io, const Vec3f & pos, bool active) {
	out.string(io.idString());
	out.f32_(pos.x), out.f32_(pos.y), out.f32_(pos.z);
	out.bool_(active);
	out.u16_(u16(g_bones.size()));
	for(const physics::BonePose & bone : g_bones) {
		out.f32_(bone.pos.x), out.f32_(bone.pos.y), out.f32_(bone.pos.z);
		out.f32_(bone.rot.x), out.f32_(bone.rot.y), out.f32_(bone.rot.z), out.f32_(bone.rot.w);
	}
}

void applyState(Reader & reader) {

	u16 ragdolls = reader.u16_();
	for(u16 n = 0; n < ragdolls; n++) {
		std::string id = reader.string();
		Vec3f pos = reader.vec3<Vec3f>();
		bool active = reader.bool_();
		u16 bones = reader.u16_();
		std::vector<physics::BonePose> poses(bones);
		for(u16 i = 0; i < bones; i++) {
			poses[i].pos = reader.vec3<Vec3f>();
			float x = reader.f32_();
			float y = reader.f32_();
			float z = reader.f32_();
			float w = reader.f32_();
			poses[i].rot = glm::normalize(glm::quat(w, x, y, z));
		}
		if(Entity * io = entities.getById(id)) {
			physics::mirrorRagdoll(*io, pos, active, poses);
		}
	}

	u16 objects = reader.u16_();
	for(u16 n = 0; n < objects; n++) {
		std::string id = reader.string();
		Vec3f pos = reader.vec3<Vec3f>();
		Vec3f angle = reader.vec3<Vec3f>();
		bool active = reader.bool_();
		if(Entity * io = entities.getById(id)) {
			physics::mirrorLooseObject(*io, pos, Anglef(angle.x, angle.y, angle.z), active);
		}
	}

}

void hostSend() {

	if(!g_coop.isHost() || g_coop.state() != State::InGame || g_coop.players().size() < 2) {
		return;
	}
	PlatformInstant now = platform::getTime();
	if(now - g_lastSend < SendInterval) {
		return;
	}
	g_lastSend = now;

	// What moves, what just stopped, and everything now and then
	Writer ragdolls;
	u16 ragdollCount = 0;
	physics::forEachRagdoll([&](Entity & io, bool active) {
		SentState & sent = g_sentRagdolls[io.idString()];
		bool keyframe = now - sent.sent > KeyframeInterval;
		if(!active && !sent.active && !keyframe) {
			return;
		}
		Vec3f pos;
		bool nowActive;
		if(!physics::getRagdollPose(io, pos, nowActive, g_bones)) {
			return;
		}
		writeRagdoll(ragdolls, io, pos, nowActive);
		sent.active = nowActive;
		sent.sent = now;
		ragdollCount++;
	});

	// Every object the engine still has in flight (simulated by Jolt or, outside the world, by
	// the engine's own box), and one last state for those that just came to rest
	Writer objects;
	u16 objectCount = 0;
	auto writeObject = [&](const Entity & io, bool active) {
		objects.string(io.idString());
		objects.f32_(io.pos.x), objects.f32_(io.pos.y), objects.f32_(io.pos.z);
		objects.f32_(io.angle.getPitch()), objects.f32_(io.angle.getYaw()), objects.f32_(io.angle.getRoll());
		objects.bool_(active);
		objectCount++;
	};
	for(Entity & io : entities.inScene()) {
		if(!io.obj || !io.obj->pbox || io.coopPuppet) {
			continue;
		}
		bool active = io.obj->pbox->active == 1;
		auto sent = g_sentObjects.find(io.idString());
		bool wasActive = sent != g_sentObjects.end() && sent->second.active;
		if(!active && !wasActive) {
			continue;
		}
		writeObject(io, active);
		SentState & state = g_sentObjects[io.idString()];
		state.active = active;
		state.sent = now;
	}
	for(auto it = g_sentObjects.begin(); it != g_sentObjects.end(); ) {
		it = (!it->second.active) ? g_sentObjects.erase(it) : std::next(it);
	}

	if(ragdollCount == 0 && objectCount == 0) {
		return;
	}
	Writer message;
	message.u16_(ragdollCount);
	message.bytes(ragdolls.data().data(), ragdolls.size());
	message.u16_(objectCount);
	message.bytes(objects.data().data(), objects.size());
	g_coop.broadcast(MessageType::PhysicsState, message);

}

} // anonymous namespace

void physicsSyncInit() {
	g_coop.onPhysicsState = [](Reader & reader) {
		if(npcsAreMirrored()) {
			applyState(reader);
		}
	};
}

void physicsSyncUpdate() {
	// Clients show what the host simulates, and simulate nothing themselves
	physics::setMirrorMode(npcsAreMirrored());
	if(!g_coop.isHost()) {
		g_sentRagdolls.clear();
		g_sentObjects.clear();
		return;
	}
	hostSend();
}

} // namespace coop
