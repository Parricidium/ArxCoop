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

#include "physics/Debris.h"

#include <algorithm>
#include <string>
#include <vector>

#include "animation/AnimationRender.h"
#include "animation/Skeleton.h"
#include "core/GameTime.h"
#include "game/Entity.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/data/Mesh.h"
#include "io/log/Logger.h"
#include "math/Random.h"
#include "math/RandomVector.h"
#include "math/Vector.h"
#include "physics/Ragdoll.h"
#include "scene/Object.h"
#include "util/String.h"

#ifdef ARX_HAVE_JOLT

#include "physics/PhysicsInternal.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

namespace physics {

namespace {

constexpr GameDuration RestBeforeRemoval = 40s;   // pieces lying still this long are removed
constexpr GameDuration MaxLifetime = 180s;        // ... or after this in any case
constexpr float MinPieceMass = 0.4f;              // kg
constexpr float ThrowSpeed = 1.6f;                // m/s away from the centre
constexpr float ThrowSpread = 1.4f;               // random part of it

struct DebrisInstance {
	Entity * io = nullptr;           //!< the entity while it exists
	EERIE_3DOBJ * obj = nullptr;     //!< the entity's object, or our own copy once detached
	bool owned = false;              //!< obj is our copy
	float scale = 1.f;
	std::vector<JPH::BodyID> bodies; //!< one per bone
	GameInstant created;
	GameInstant restSince;           //!< since when every piece has been asleep
	bool resting = false;
};

std::vector<DebrisInstance> g_debris;

//! Body shape of one vertex group, in the bone frame (see Ragdoll.cpp for the frame)
JPH::RefConst<JPH::Shape> pieceShape(const EERIE_3DOBJ & obj, VertexGroupId bone, float scale) {
	std::vector<JPH::Vec3> points;
	JPH::Vec3 low(1e9f, 1e9f, 1e9f), high(-1e9f, -1e9f, -1e9f);
	for(VertexId vertex : obj.m_boneVertices[bone]) {
		points.push_back(toJolt(obj.vertexlocal[vertex] * scale));
		low = JPH::Vec3::sMin(low, points.back());
		high = JPH::Vec3::sMax(high, points.back());
	}
	if(points.size() >= 4) {
		JPH::ConvexHullShapeSettings hull(points.data(), int(points.size()), 0.01f);
		JPH::Shape::ShapeResult result = hull.Create();
		if(result.IsValid()) {
			return result.Get();
		}
	}
	if(points.empty()) {
		return nullptr;
	}
	// Flat or degenerate groups: a thin box around the points
	JPH::Vec3 half = JPH::Vec3::sMax((high - low) * 0.5f, JPH::Vec3::sReplicate(0.01f));
	JPH::Vec3 centre = (high + low) * 0.5f;
	JPH::RotatedTranslatedShapeSettings box(centre, JPH::Quat::sIdentity(), new JPH::BoxShape(half, 0.005f));
	JPH::Shape::ShapeResult result = box.Create();
	return result.IsValid() ? result.Get() : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(half.Length()));
}

void destroyBodies(DebrisInstance & instance) {
	if(JPH::PhysicsSystem * world = system()) {
		JPH::BodyInterface & bodies = world->GetBodyInterface();
		for(JPH::BodyID id : instance.bodies) {
			if(!id.IsInvalid()) {
				bodies.RemoveBody(id);
				bodies.DestroyBody(id);
			}
		}
	}
	instance.bodies.clear();
	if(instance.owned && instance.obj) {
		delete instance.obj;
	}
	instance.obj = nullptr;
}

//! Write the pieces' transforms into the bones of the object
void poseFromBodies(const DebrisInstance & instance, Skeleton & skeleton, Vec3f & centre) {
	const JPH::BodyInterface & bodies = system()->GetBodyInterface();
	centre = Vec3f(0.f);
	size_t count = 0;
	for(VertexGroupId bone : skeleton.bones.handles()) {
		size_t index = size_t(bone);
		if(index >= instance.bodies.size() || instance.bodies[index].IsInvalid()) {
			continue;
		}
		JPH::RVec3 position;
		JPH::Quat rotation;
		bodies.GetPositionAndRotation(instance.bodies[index], position, rotation);
		Bone & data = skeleton.bones[bone];
		data.anim.trans = fromJolt(JPH::Vec3(position));
		data.anim.quat = fromJolt(rotation);
		data.anim.scale = (data.init.scale + Vec3f(1.f)) * instance.scale;
		centre += data.anim.trans;
		count++;
	}
	if(count) {
		centre /= float(count);
	}
}

} // anonymous namespace

bool isShattered(const Entity & io) {
	for(const DebrisInstance & instance : g_debris) {
		if(instance.io == &io) {
			return true;
		}
	}
	return false;
}

void onBreakAnimation(Entity & io, const res::path & animation) {

	JPH::PhysicsSystem * world = system();
	if(!world || isMirrorMode() || !(io.ioflags & IO_FIX) || !io.obj || !io.obj->m_skeleton) {
		return;
	}
	std::string name = util::toLowercase(std::string(animation.basename()));
	if(name.find("break") == std::string::npos || isShattered(io)) {
		return;
	}
	Skeleton & skeleton = *io.obj->m_skeleton;
	if(skeleton.bones.size() < 2) {
		return;
	}

	// The pose the object is in now
	animateSkeleton(&io, io.animlayer.data(), skeleton);

	DebrisInstance instance;
	instance.io = &io;
	instance.obj = io.obj;
	instance.scale = io.scale;
	instance.created = g_gameTime.now();
	instance.bodies.resize(skeleton.bones.size());

	// Pieces fly away from the middle of the object
	Vec3f centre(0.f);
	for(VertexGroupId bone : skeleton.bones.handles()) {
		centre += skeleton.bones[bone].anim.trans;
	}
	centre /= float(skeleton.bones.size());

	JPH::BodyInterface & bodies = world->GetBodyInterface();
	size_t created = 0;
	for(VertexGroupId bone : skeleton.bones.handles()) {
		JPH::RefConst<JPH::Shape> shape = pieceShape(*io.obj, bone, io.scale);
		if(!shape) {
			continue;
		}
		const Bone & data = skeleton.bones[bone];
		JPH::BodyCreationSettings settings(shape, JPH::RVec3(toJolt(data.anim.trans)),
		                                   toJolt(data.anim.quat).Normalized(), JPH::EMotionType::Dynamic, LayerMoving);
		settings.mFriction = 0.6f;
		settings.mRestitution = 0.1f;
		settings.mLinearDamping = 0.2f;
		settings.mAngularDamping = 0.6f;
		settings.mUserData = makeUserData(KindDebris, io.index().handleData());
		float mass = shape->GetMassProperties().mMass;
		if(mass < MinPieceMass) {
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = MinPieceMass;
		}
		Vec3f away = data.anim.trans - centre;
		if(arx::length2(away) < 1.f) {
			away = arx::randomVec(-1.f, 1.f);
		}
		away = glm::normalize(away);
		away.y -= 0.6f; // a little upwards (y is down)
		JPH::Vec3 velocity = toJoltDirection(away) * (ThrowSpeed + Random::getf(0.f, ThrowSpread));
		settings.mLinearVelocity = velocity;
		settings.mAngularVelocity = toJoltDirection(arx::randomVec(-1.f, 1.f)) * Random::getf(2.f, 6.f);
		JPH::BodyID id = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
		if(id.IsInvalid()) {
			continue;
		}
		instance.bodies[size_t(bone)] = id;
		created++;
	}

	if(!created) {
		return;
	}
	g_debris.push_back(std::move(instance));
	LogInfo << "physics: " << io.idString() << " breaks into " << created << " pieces (" << name << ")";
}

bool applyDebrisPose(Entity & io, Skeleton & skeleton) {
	if(!system()) {
		return false;
	}
	for(const DebrisInstance & instance : g_debris) {
		if(instance.io == &io) {
			Vec3f centre;
			poseFromBodies(instance, skeleton, centre);
			return true;
		}
	}
	return false;
}

void detachDebris(Entity & io) {
	for(DebrisInstance & instance : g_debris) {
		if(instance.io != &io) {
			continue;
		}
		instance.io = nullptr;
		if(io.obj && system()) {
			instance.obj = Eerie_Copy(io.obj);
			instance.owned = true;
		} else {
			instance.obj = nullptr;
		}
	}
	// Instances without an object cannot be drawn: drop their bodies
	for(DebrisInstance & instance : g_debris) {
		if(!instance.io && !instance.obj) {
			destroyBodies(instance);
		}
	}
	g_debris.erase(std::remove_if(g_debris.begin(), g_debris.end(), [](const DebrisInstance & instance) {
		return !instance.io && !instance.obj;
	}), g_debris.end());
}

void updateDebris() {

	JPH::PhysicsSystem * world = system();
	if(!world || g_debris.empty()) {
		return;
	}
	const JPH::BodyInterface & bodies = world->GetBodyInterface();
	GameInstant now = g_gameTime.now();

	for(DebrisInstance & instance : g_debris) {
		bool active = false;
		for(JPH::BodyID id : instance.bodies) {
			if(!id.IsInvalid() && bodies.IsActive(id)) {
				active = true;
				break;
			}
		}
		if(active) {
			instance.resting = false;
		} else if(!instance.resting) {
			instance.resting = true;
			instance.restSince = now;
		}
		bool old = (now - instance.created) > MaxLifetime
		           || (instance.resting && !instance.io && (now - instance.restSince) > RestBeforeRemoval);
		if(old && !instance.io) {
			destroyBodies(instance);
		}
	}
	g_debris.erase(std::remove_if(g_debris.begin(), g_debris.end(), [](const DebrisInstance & instance) {
		return !instance.io && !instance.obj;
	}), g_debris.end());

}

void renderDebris() {
	if(!system()) {
		return;
	}
	for(DebrisInstance & instance : g_debris) {
		if(instance.io || !instance.obj || !instance.obj->m_skeleton) {
			continue;
		}
		Vec3f centre;
		poseFromBodies(instance, *instance.obj->m_skeleton, centre);
		DrawDetachedObject(instance.obj, centre);
	}
}

void clearDebris() {
	for(DebrisInstance & instance : g_debris) {
		destroyBodies(instance);
	}
	g_debris.clear();
}

size_t debrisCount() {
	return g_debris.size();
}

} // namespace physics

#else // ARX_HAVE_JOLT

namespace physics {

bool isShattered(const Entity & io) { ARX_UNUSED(io); return false; }
void onBreakAnimation(Entity & io, const res::path & animation) { ARX_UNUSED(io), ARX_UNUSED(animation); }
bool applyDebrisPose(Entity & io, Skeleton & skeleton) { ARX_UNUSED(io), ARX_UNUSED(skeleton); return false; }
void detachDebris(Entity & io) { ARX_UNUSED(io); }
void updateDebris() { }
void renderDebris() { }
void clearDebris() { }
size_t debrisCount() { return 0; }

} // namespace physics

#endif // ARX_HAVE_JOLT
