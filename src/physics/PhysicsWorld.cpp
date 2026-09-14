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

#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <thread>

#include <mutex>

#include "core/Config.h"
#include "core/GameTime.h"
#include "game/GameTypes.h"
#include "io/log/Logger.h"
#include "physics/LooseObjects.h"
#include "physics/Ragdoll.h"
#include "core/TimeTypes.h"
#include "platform/Time.h"
#include "scene/Tiles.h"

#ifdef ARX_HAVE_JOLT

#include "physics/PhysicsInternal.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/Body.h>

namespace physics {

namespace {

//! Fixed simulation step and the most steps done in one frame (after which time is dropped)
constexpr float StepSeconds = 1.f / 60.f;
constexpr int MaxStepsPerFrame = 4;

struct Runtime {
	std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
	std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
	std::unique_ptr<JPH::BroadPhaseLayerInterfaceTable> broadPhaseLayers;
	std::unique_ptr<JPH::ObjectLayerPairFilterTable> layerPairs;
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
};

//! Records the contacts of the moving bodies (called from the job threads during a step)
class ContactRecorder final : public JPH::ContactListener {
	
	std::mutex m_mutex;
	std::vector<ContactEvent> m_events;
	
public:
	
	void OnContactAdded(const JPH::Body & body1, const JPH::Body & body2, const JPH::ContactManifold & manifold,
	                    JPH::ContactSettings & settings) override {
		ARX_UNUSED(settings);
		ContactEvent event;
		event.userData1 = body1.GetUserData();
		event.userData2 = body2.GetUserData();
		event.material1 = staticMaterial(body1, manifold.mSubShapeID1);
		event.material2 = staticMaterial(body2, manifold.mSubShapeID2);
		JPH::Vec3 relative = body2.GetLinearVelocity() - body1.GetLinearVelocity();
		event.speed = std::abs(relative.Dot(manifold.mWorldSpaceNormal));
		event.position = fromJolt(JPH::Vec3(manifold.GetWorldSpaceContactPointOn1(0)));
		std::lock_guard<std::mutex> lock(m_mutex);
		if(m_events.size() < 256) {
			m_events.push_back(event);
		}
	}
	
	void take(std::vector<ContactEvent> & out) {
		std::lock_guard<std::mutex> lock(m_mutex);
		out.swap(m_events);
		m_events.clear();
	}
	
};

struct World {
	JPH::PhysicsSystem system;
	JPH::BodyID levelBody;
	ContactRecorder contacts;
	float accumulator = 0.f;
};

std::unique_ptr<Runtime> g_runtime;
std::unique_ptr<World> g_world;

int polygonMaterial(const EERIEPOLY & poly) {
	if(poly.type & POLY_METAL) {
		return MATERIAL_METAL;
	}
	if(poly.type & POLY_WOOD) {
		return MATERIAL_WOOD;
	}
	if(poly.type & POLY_STONE) {
		return MATERIAL_STONE;
	}
	if(poly.type & POLY_GRAVEL) {
		return MATERIAL_GRAVEL;
	}
	if(poly.type & POLY_EARTH) {
		return MATERIAL_EARTH;
	}
	return MATERIAL_STONE;
}

void traceToLog(const char * format, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	LogInfo << "Jolt: " << buffer;
}

//! The level geometry as one static triangle mesh
JPH::BodyID createLevelBody(JPH::PhysicsSystem & system) {

	JPH::TriangleList triangles;
	if(g_tiles) {
		for(auto tile : g_tiles->tiles()) {
			for(const EERIEPOLY & poly : tile.polygons()) {
				// Water is not walked on; no-collision polygons are decoration
				if(poly.type & (POLY_NOCOL | POLY_WATER)) {
					continue;
				}
				JPH::Float3 v[4];
				size_t count = (poly.type & POLY_QUAD) ? 4 : 3;
				for(size_t i = 0; i < count; i++) {
					v[i] = JPH::Float3(poly.v[i].p.x * MetresPerUnit, poly.v[i].p.y * MetresPerUnit,
					                   poly.v[i].p.z * MetresPerUnit);
				}
				JPH::uint32 material = JPH::uint32(polygonMaterial(poly));
				triangles.push_back(JPH::Triangle(v[0], v[1], v[2], 0, material));
				if(count == 4) {
					triangles.push_back(JPH::Triangle(v[3], v[2], v[1], 0, material));
				}
			}
		}
	}
	if(triangles.empty()) {
		return JPH::BodyID();
	}

	JPH::MeshShapeSettings settings(triangles);
	settings.mPerTriangleUserData = true;
	JPH::Shape::ShapeResult result = settings.Create();
	if(result.HasError()) {
		LogWarning << "Jolt: level mesh: " << result.GetError().c_str();
		return JPH::BodyID();
	}

	JPH::BodyCreationSettings body(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
	                               JPH::EMotionType::Static, LayerStatic);
	body.mFriction = 0.6f;
	body.mUserData = makeUserData(KindLevel, 0);
	JPH::BodyID id = system.GetBodyInterface().CreateAndAddBody(body, JPH::EActivation::DontActivate);
	LogInfo << "Jolt: level mesh with " << triangles.size() << " triangles";
	return id;
}

} // anonymous namespace

JPH::PhysicsSystem * system() {
	return g_world ? &g_world->system : nullptr;
}

int staticMaterial(const JPH::Body & body, const JPH::SubShapeID & subShape) {
	JPH::uint64 data = body.GetUserData();
	switch(userDataKind(data)) {
		case KindLevel: {
			const JPH::Shape * shape = body.GetShape();
			if(shape && shape->GetSubType() == JPH::EShapeSubType::Mesh) {
				return int(static_cast<const JPH::MeshShape *>(shape)->GetTriangleUserData(subShape));
			}
			return MATERIAL_STONE;
		}
		case KindFixed: return fixedMaterial(data);
		default: return MATERIAL_NONE;
	}
}

void takeContactEvents(std::vector<ContactEvent> & out) {
	out.clear();
	if(g_world) {
		g_world->contacts.take(out);
	}
}

bool isAvailable() {
	return true;
}

bool isActive() {
	return g_world != nullptr;
}

void init() {

	if(g_runtime) {
		return;
	}

	JPH::RegisterDefaultAllocator();
	JPH::Trace = traceToLog;
	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	g_runtime = std::make_unique<Runtime>();
	g_runtime->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
	int threads = int(std::thread::hardware_concurrency());
	threads = std::clamp(threads - 1, 1, 4);
	g_runtime->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
	                                                                  JPH::cMaxPhysicsBarriers, threads);

	g_runtime->broadPhaseLayers = std::make_unique<JPH::BroadPhaseLayerInterfaceTable>(LayerCount, BroadPhaseCount);
	g_runtime->broadPhaseLayers->MapObjectToBroadPhaseLayer(LayerStatic, JPH::BroadPhaseLayer(BroadPhaseStatic));
	g_runtime->broadPhaseLayers->MapObjectToBroadPhaseLayer(LayerMoving, JPH::BroadPhaseLayer(BroadPhaseMoving));
	g_runtime->broadPhaseLayers->MapObjectToBroadPhaseLayer(LayerSpawning, JPH::BroadPhaseLayer(BroadPhaseMoving));
	g_runtime->layerPairs = std::make_unique<JPH::ObjectLayerPairFilterTable>(LayerCount);
	g_runtime->layerPairs->EnableCollision(LayerMoving, LayerStatic);
	g_runtime->layerPairs->EnableCollision(LayerMoving, LayerMoving);
	g_runtime->layerPairs->EnableCollision(LayerSpawning, LayerStatic);
	g_runtime->objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
		*g_runtime->broadPhaseLayers, BroadPhaseCount, *g_runtime->layerPairs, LayerCount);

	LogInfo << "Jolt physics ready (" << threads << " worker threads)";
}

void shutdown() {

	levelCleared();

	if(!g_runtime) {
		return;
	}
	g_runtime.reset();

	JPH::UnregisterTypes();
	delete JPH::Factory::sInstance;
	JPH::Factory::sInstance = nullptr;
}

void levelLoaded() {

	levelCleared();

	if(!g_runtime || !config.video.physics) {
		return;
	}

	PlatformInstant start = platform::getTime();

	g_world = std::make_unique<World>();
	g_world->system.Init(4096, 0, 8192, 4096, *g_runtime->broadPhaseLayers,
	                     *g_runtime->objectVsBroadPhase, *g_runtime->layerPairs);
	// The engine's y axis points down
	g_world->system.SetGravity(JPH::Vec3(0.f, 9.81f, 0.f));
	JPH::PhysicsSettings settings = g_world->system.GetPhysicsSettings();
	settings.mTimeBeforeSleep = 0.4f;
	// Jolt's defaults let bodies sink 2 cm into each other, which for a key (1 cm thick) lying on
	// the floor is the whole key: keep the allowed overlap at a millimetre
	settings.mPenetrationSlop = 0.001f;
	settings.mSpeculativeContactDistance = 0.01f;
	g_world->system.SetPhysicsSettings(settings);

	g_world->system.SetContactListener(&g_world->contacts);

	g_world->levelBody = createLevelBody(g_world->system);
	createObstacles();
	g_world->system.OptimizeBroadPhase();

	LogInfo << "Jolt: world built in " << toMsi(platform::getTime() - start) << " ms";
}

void levelCleared() {

	if(!g_world) {
		return;
	}

	clearRagdolls();
	clearLooseObjects();
	g_world.reset();
}

void update() {

	updateMirroredObjects(); // objects following another machine (co-op client)

	if(!g_world) {
		return;
	}

	float dt = toMsf(g_gameTime.lastFrameDuration()) * 0.001f;
	if(dt <= 0.f) {
		return;
	}

	g_world->accumulator = std::min(g_world->accumulator + dt, StepSeconds * MaxStepsPerFrame);
	if(g_world->accumulator < StepSeconds) {
		return;
	}

	syncObstacles(); // doors, platforms and NPCs moved by the engine since the last step

	int steps = 0;
	while(g_world->accumulator >= StepSeconds) {
		g_world->system.Update(StepSeconds, 1, g_runtime->tempAllocator.get(), g_runtime->jobSystem.get());
		g_world->accumulator -= StepSeconds;
		steps++;
	}

	updateRagdolls();
	updateLooseObjects();
}

void onEntityDestroyed(Entity & io) {
	removeRagdoll(io);
	removeLooseObject(io);
}

void dumpState() {
	LogInfo << "physics: " << (g_world ? g_world->system.GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0)
	        << " active bodies, " << ragdollCount() << " ragdolls, " << looseObjectCount() << " loose objects";
	dumpRagdolls();
	dumpLooseObjects();
}

} // namespace physics

#else // ARX_HAVE_JOLT

namespace physics {

bool isAvailable() { return false; }
bool isActive() { return false; }
void init() { }
void shutdown() { }
void levelLoaded() { }
void levelCleared() { }
void update() { updateMirroredObjects(); }
void onEntityDestroyed(Entity & io) { ARX_UNUSED(io); }
void dumpState() { }

} // namespace physics

#endif // ARX_HAVE_JOLT
