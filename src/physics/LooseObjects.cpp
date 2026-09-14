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

#include "physics/LooseObjects.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/GameTime.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "graphics/data/Mesh.h"
#include "io/log/Logger.h"
#include "math/GtxFunctions.h"
#include "math/Vector.h"
#include "physics/Physics.h"
#include "physics/Ragdoll.h"
#include "scene/GameSound.h"
#include "script/Script.h"

#ifdef ARX_HAVE_JOLT

#include "physics/PhysicsInternal.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

namespace physics {

namespace {

//! Launch vectors of the engine are directions (throw) or small pushes (drop): metres per second
constexpr float LaunchSpeed = 5.f;
constexpr float MaxLaunchSpeed = 6.f;
constexpr float MinObjectMass = 0.3f; // kg
constexpr size_t MaxHullPoints = 200;
//! NPCs farther than this from the player are not obstacles (nothing moves there anyway)
constexpr float NpcObstacleRange = 2500.f;
//! A body that falls this far below where it started has left the level: give it back to the engine
constexpr float FallOutDistance = 5000.f;

struct LooseBody {
	JPH::BodyID id;
	float startY = 0.f;
	GameInstant launched;
	bool spawning = true; //!< still in LayerSpawning
};

//! How long a launched object ignores NPCs and corpses: a weapon dropped by a dying NPC starts
//! inside its hand (and its collision cylinder) and would be shot out of it otherwise
constexpr GameDuration SpawnGrace = 400ms;

struct Obstacle {
	JPH::BodyID id;
	Vec3f pos = Vec3f(0.f);
	Anglef angle;
	float radius = 0.f;
	float height = 0.f;
	bool npc = false;
	bool seen = false;
};

std::unordered_map<Entity *, LooseBody> g_loose;
std::unordered_map<Entity *, Obstacle> g_obstacles;
std::vector<ContactEvent> g_contacts;

//! Inverse of toQuaternion(): the engine's pitch/yaw/roll of a rotation (M = Rz(-roll) Rx(pitch) Ry(yaw))
Anglef rotationToAngle(const glm::quat & q) {
	glm::mat3 m = glm::mat3_cast(q);
	float t1 = std::atan2(-m[1][0], m[1][1]);
	float c2 = std::sqrt(m[0][2] * m[0][2] + m[2][2] * m[2][2]);
	float t2 = std::atan2(m[1][2], c2);
	float s1 = std::sin(t1), c1 = std::cos(t1);
	float t3 = std::atan2(c1 * m[2][0] + s1 * m[2][1], c1 * m[0][0] + s1 * m[0][1]);
	return Anglef(glm::degrees(t2), glm::degrees(t3), -glm::degrees(t1));
}

JPH::RefConst<JPH::Shape> hullShape(const EERIE_3DOBJ & obj, float scale) {

	std::vector<JPH::Vec3> points;
	size_t count = obj.vertexlist.size();
	size_t step = std::max<size_t>(1, count / MaxHullPoints);
	for(size_t i = 0; i < count; i += step) {
		points.push_back(toJolt(obj.vertexlist[VertexId(i)].v * scale));
	}

	if(points.size() >= 4) {
		JPH::ConvexHullShapeSettings settings(points.data(), int(points.size()), 0.01f);
		JPH::Shape::ShapeResult result = settings.Create();
		if(result.IsValid()) {
			return result.Get();
		}
	}

	// Flat things (keys, coins, papers) have no volume for a hull: a thin box around them,
	// kept in the object's frame so that the body origin stays the object origin
	JPH::AABox bounds;
	for(const JPH::Vec3 & p : points) {
		bounds.Encapsulate(p);
	}
	if(!bounds.IsValid()) {
		return new JPH::SphereShape(0.03f);
	}
	JPH::Vec3 halfExtent = JPH::Vec3::sMax(bounds.GetExtent(), JPH::Vec3::sReplicate(0.004f));
	float convexRadius = std::min(0.005f, halfExtent.ReduceMin() * 0.5f);
	JPH::RotatedTranslatedShapeSettings settings(bounds.GetCenter(), JPH::Quat::sIdentity(),
	                                             new JPH::BoxShape(halfExtent, convexRadius));
	JPH::Shape::ShapeResult result = settings.Create();
	if(result.IsValid()) {
		return result.Get();
	}
	return new JPH::SphereShape(std::max(0.03f, halfExtent.Length()));
}

JPH::RefConst<JPH::Shape> meshShape(const EERIE_3DOBJ & obj, float scale) {

	JPH::TriangleList triangles;
	for(const EERIE_FACE & face : obj.facelist) {
		if(face.facetype & POLY_HIDE) {
			continue;
		}
		JPH::Float3 v[3];
		for(size_t i = 0; i < 3; i++) {
			Vec3f p = obj.vertexlist[face.vid[i]].v * scale * MetresPerUnit;
			v[i] = JPH::Float3(p.x, p.y, p.z);
		}
		triangles.push_back(JPH::Triangle(v[0], v[1], v[2]));
	}
	if(triangles.empty()) {
		return nullptr;
	}

	JPH::MeshShapeSettings settings(triangles);
	JPH::Shape::ShapeResult result = settings.Create();
	if(!result.IsValid()) {
		return nullptr;
	}
	return result.Get();
}

bool isFixedObstacle(const Entity & io) {
	return (io.ioflags & IO_FIX) && !(io.ioflags & IO_NO_COLLISIONS) && io.obj && !io.obj->facelist.empty()
	       && io.show == SHOW_FLAG_IN_SCENE;
}

bool isNpcObstacle(const Entity & io) {
	return (io.ioflags & IO_NPC) && &io != entities.player() && io.show == SHOW_FLAG_IN_SCENE
	       && !IsDeadNPC(io) && !(io.ioflags & IO_NO_COLLISIONS) && io.physics.cyl.radius > 0.f
	       && closerThan(io.pos, player.pos, NpcObstacleRange);
}

void removeObstacle(std::unordered_map<Entity *, Obstacle>::iterator it) {
	if(JPH::PhysicsSystem * world = system()) {
		JPH::BodyInterface & bodies = world->GetBodyInterface();
		bodies.RemoveBody(it->second.id);
		bodies.DestroyBody(it->second.id);
	}
	g_obstacles.erase(it);
}

void addFixedObstacle(Entity & io) {

	JPH::PhysicsSystem * world = system();
	JPH::RefConst<JPH::Shape> shape = meshShape(*io.obj, io.scale);
	if(!world || !shape) {
		return;
	}

	JPH::BodyCreationSettings settings(shape, JPH::RVec3(toJolt(io.pos)), toJolt(toQuaternion(io.angle)).Normalized(),
	                                   JPH::EMotionType::Static, LayerStatic);
	settings.mFriction = 0.6f;
	settings.mUserData = fixedUserData(io.index().handleData(), int(io.material));
	JPH::BodyID id = world->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
	if(id.IsInvalid()) {
		return;
	}

	Obstacle & obstacle = g_obstacles[&io];
	obstacle.id = id;
	obstacle.pos = io.pos;
	obstacle.angle = io.angle;
	obstacle.npc = false;
	obstacle.seen = true;
}

//! The engine's cylinder: origin at the feet, height upwards (negative y)
void npcCylinder(const Entity & io, JPH::RVec3 & position, float & halfHeight, float & radius) {
	const Cylinder & cyl = io.physics.cyl;
	halfHeight = std::max(std::abs(cyl.height) * 0.5f, 10.f) * MetresPerUnit;
	radius = std::max(cyl.radius, 10.f) * MetresPerUnit;
	position = JPH::RVec3(toJolt(cyl.origin + Vec3f(0.f, cyl.height * 0.5f, 0.f)));
}

void addNpcObstacle(Entity & io) {

	JPH::PhysicsSystem * world = system();
	if(!world) {
		return;
	}

	JPH::RVec3 position;
	float halfHeight, radius;
	npcCylinder(io, position, halfHeight, radius);

	JPH::BodyCreationSettings settings(new JPH::CylinderShape(halfHeight, radius), position, JPH::Quat::sIdentity(),
	                                   JPH::EMotionType::Kinematic, LayerMoving);
	settings.mFriction = 0.4f;
	settings.mUserData = makeUserData(KindNpc, io.index().handleData());
	JPH::BodyID id = world->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
	if(id.IsInvalid()) {
		return;
	}

	Obstacle & obstacle = g_obstacles[&io];
	obstacle.id = id;
	obstacle.pos = io.pos;
	obstacle.radius = io.physics.cyl.radius;
	obstacle.height = io.physics.cyl.height;
	obstacle.npc = true;
	obstacle.seen = true;
}

void removeLooseBody(std::unordered_map<Entity *, LooseBody>::iterator it) {
	if(JPH::PhysicsSystem * world = system()) {
		JPH::BodyInterface & bodies = world->GetBodyInterface();
		bodies.RemoveBody(it->second.id);
		bodies.DestroyBody(it->second.id);
	}
	g_loose.erase(it);
}

//! Put the engine's box to rest where the entity is, as ARX_PHYSICS_BOX_ApplyModel does
void settleEngineBox(Entity & io) {

	if(!io.obj || !io.obj->pbox) {
		return;
	}
	PHYSICS_BOX_DATA & pbox = *io.obj->pbox;
	glm::quat rotation = toQuaternion(io.angle);
	for(PhysicsParticle & particle : pbox.vert) {
		particle.pos = io.pos + rotation * particle.initpos;
		particle.velocity = Vec3f(0.f);
		particle.force = Vec3f(0.f);
	}
	pbox.active = 2;
	pbox.stopcount = 0;
	pbox.storedtiming = 0;

	io.soundcount = 0;
	io.soundtime = g_gameTime.now() + 2s;
}

void playContactSound(Entity & io, int material, float speed, const Vec3f & position) {

	// As ARX_TEMPORARY_TrySound(): a few sounds per object, none faster than every 100 ms
	GameInstant now = g_gameTime.now();
	if(now <= io.soundtime || speed < 0.3f) {
		return;
	}
	io.soundcount++;
	if(io.soundcount >= 5) {
		return;
	}
	Material own = EEIsUnderWater(io.pos) ? MATERIAL_WATER : io.material;
	Material other = (material > MATERIAL_NONE && material < MAX_MATERIALS) ? Material(material) : MATERIAL_STONE;
	float volume = glm::clamp(0.3f + speed * 0.2f, 0.f, 1.f);
	ARX_SOUND_PlayCollision(own, other, volume, 1.f, position, &io);
	io.soundtime = now + 100ms;
}

} // anonymous namespace

void launchObject(EERIE_3DOBJ * obj, const Vec3f & pos, const Anglef & angle, const Vec3f & vect, Entity * io) {

	JPH::PhysicsSystem * world = system();
	if(!world || !obj) {
		return;
	}

	if(!io) {
		for(Entity & entity : entities) {
			if(entity.obj == obj) {
				io = &entity;
				break;
			}
		}
	}
	if(!io || io->obj != obj || io->show != SHOW_FLAG_IN_SCENE) {
		return;
	}
	LogDebug("launch " << io->idString() << " at " << pos.x << " " << pos.y << " " << pos.z);

	auto existing = g_loose.find(io);
	if(existing != g_loose.end()) {
		removeLooseBody(existing);
	}

	// A dying NPC's hand may never have been placed in the world (never in view): the engine
	// would launch from the origin, then move the object in front of the player at the next
	// level change. Let it.
	Vec3f start = pos;
	if(!EERIE_PHYSICS_BOX_IsValidPosition(start)) {
		if(!EERIE_PHYSICS_BOX_IsValidPosition(io->pos)) {
			LogWarning << "Jolt: " << io->idString() << " launched outside the world, left to the engine";
			return;
		}
		start = io->pos;
	}

	// The engine moves the entity to the launch position on its first step: do it now
	io->pos = io->lastpos = start;
	io->angle = angle;
	io->requestRoomUpdate = true;

	JPH::BodyCreationSettings settings(hullShape(*obj, io->scale), JPH::RVec3(toJolt(start)),
	                                   toJolt(toQuaternion(angle)).Normalized(),
	                                   JPH::EMotionType::Dynamic, LayerSpawning);
	settings.mFriction = 0.5f;
	settings.mRestitution = 0.1f;
	settings.mLinearDamping = 0.1f;
	settings.mAngularDamping = 0.4f;
	// Small, thin things (keys, rings) must not tunnel through the floor when thrown
	settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
	settings.mUserData = makeUserData(KindLoose, io->index().handleData());
	if(settings.GetShape()->GetMassProperties().mMass < MinObjectMass) {
		settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
		settings.mMassPropertiesOverride.mMass = MinObjectMass;
	}
	JPH::Vec3 velocity = toJoltDirection(vect) * LaunchSpeed;
	if(velocity.Length() > MaxLaunchSpeed) {
		velocity = velocity.Normalized() * MaxLaunchSpeed;
	}
	settings.mLinearVelocity = velocity;

	JPH::BodyID id = world->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
	if(id.IsInvalid()) {
		LogWarning << "Jolt: no body for " << io->idString();
		return;
	}

	LooseBody & body = g_loose[io];
	body.id = id;
	body.startY = start.y;
	body.launched = g_gameTime.now();
	body.spawning = true;

	io->soundcount = 0;
	io->soundtime = 0;
}

bool updateLooseObject(Entity & io) {

	auto it = g_loose.find(&io);
	if(it == g_loose.end()) {
		// Gold coins share one object (and so one box) between all their entities: while one of
		// them is simulated here the others must not be moved by the engine's box either
		for(const auto & entry : g_loose) {
			if(entry.first->obj == io.obj) {
				return true;
			}
		}
		return false;
	}
	JPH::PhysicsSystem * world = system();
	if(!world) {
		g_loose.erase(it);
		return false;
	}

	JPH::BodyInterface & bodies = world->GetBodyInterface();
	JPH::RVec3 position;
	JPH::Quat rotation;
	bodies.GetPositionAndRotation(it->second.id, position, rotation);
	Vec3f pos = fromJolt(JPH::Vec3(position));

	if(pos.y - it->second.startY > FallOutDistance || !isallfinite(pos)) {
		// Left the level: freeze it where it was rather than lose it
		removeLooseBody(it);
		settleEngineBox(io);
		return true;
	}

	io.pos = pos;
	io.angle = rotationToAngle(fromJolt(rotation));
	io.requestRoomUpdate = true;

	if(!bodies.IsActive(it->second.id)) {
		removeLooseBody(it);
		settleEngineBox(io);
	}

	return true;
}

void createObstacles() {

	if(!system()) {
		return;
	}

	for(Entity & io : entities) {
		if(isFixedObstacle(io) && !g_obstacles.count(&io)) {
			addFixedObstacle(io);
		}
	}

	LogInfo << "Jolt: " << g_obstacles.size() << " fixed entities as obstacles";
}

void syncObstacles() {

	JPH::PhysicsSystem * world = system();
	if(!world) {
		return;
	}
	JPH::BodyInterface & bodies = world->GetBodyInterface();

	// NPCs only matter while something is moving around them
	bool wantNpcs = !g_loose.empty() || ragdollCount() > 0;

	for(auto & entry : g_obstacles) {
		entry.second.seen = false;
	}

	for(Entity & io : entities) {

		if(isFixedObstacle(io)) {
			auto it = g_obstacles.find(&io);
			if(it == g_obstacles.end()) {
				addFixedObstacle(io);
				continue;
			}
			Obstacle & obstacle = it->second;
			obstacle.seen = true;
			if(obstacle.pos != io.pos || obstacle.angle != io.angle) {
				obstacle.pos = io.pos;
				obstacle.angle = io.angle;
				bodies.SetPositionAndRotation(obstacle.id, JPH::RVec3(toJolt(io.pos)),
				                              toJolt(toQuaternion(io.angle)).Normalized(), JPH::EActivation::DontActivate);
			}
			continue;
		}

		if(wantNpcs && isNpcObstacle(io)) {
			auto it = g_obstacles.find(&io);
			if(it == g_obstacles.end()) {
				addNpcObstacle(io);
				continue;
			}
			Obstacle & obstacle = it->second;
			obstacle.seen = true;
			if(obstacle.radius != io.physics.cyl.radius || obstacle.height != io.physics.cyl.height) {
				removeObstacle(it);
				addNpcObstacle(io);
				continue;
			}
			JPH::RVec3 position;
			float halfHeight, radius;
			npcCylinder(io, position, halfHeight, radius);
			bodies.MoveKinematic(obstacle.id, position, JPH::Quat::sIdentity(), 1.f / 60.f);
			continue;
		}

	}

	std::vector<Entity *> gone;
	for(auto & entry : g_obstacles) {
		if(!entry.second.seen) {
			gone.push_back(entry.first);
		}
	}
	for(Entity * io : gone) {
		removeObstacle(g_obstacles.find(io));
	}
}

void updateLooseObjects() {

	if(!system()) {
		return;
	}

	takeContactEvents(g_contacts);
	for(const ContactEvent & contact : g_contacts) {
		for(int side = 0; side < 2; side++) {
			JPH::uint64 data = side ? contact.userData2 : contact.userData1;
			JPH::uint64 otherData = side ? contact.userData1 : contact.userData2;
			int otherMaterial = side ? contact.material1 : contact.material2;
			if(userDataKind(data) != KindLoose) {
				continue;
			}
			Entity * io = entities.get(EntityHandle(userDataValue(data)));
			if(!io || !g_loose.count(io)) {
				continue;
			}
			playContactSound(*io, otherMaterial, contact.speed, contact.position);
			// A thrown object hitting an entity of the "door" group (spider webs, the glass
			// crypt...) tells both scripts, as the engine's box does - a web tears, the glass breaks
			if(userDataKind(otherData) == KindFixed) {
				Entity * fixed = entities.get(EntityHandle(fixedEntityIndex(otherData)));
				GameInstant now = g_gameTime.now();
				if(fixed && (fixed->ioflags & IO_FIELD) && now - fixed->collide_door_time > 500ms) {
					// A magic field (Create Field) is a fixed entity like any other for Jolt; the
					// engine also tells the object it hit one
					fixed->collide_door_time = now;
					EntityHandle source = io->index();
					SendIOScriptEvent(nullptr, io, SM_COLLIDE_FIELD);
					io = entities.get(source);
					if(!io) {
						continue;
					}
				}
				if(fixed && (fixed->gameFlags & GFLAG_DOOR) && now - fixed->collide_door_time > 500ms) {
					fixed->collide_door_time = now;
					EntityHandle source = io->index(), target = fixed->index();
					SendIOScriptEvent(io, fixed, SM_COLLIDE_DOOR);
					// Either script may have destroyed or replaced its entity
					io = entities.get(source);
					fixed = entities.get(target);
					if(io && fixed) {
						fixed->collide_door_time = now;
						SendIOScriptEvent(fixed, io, SM_COLLIDE_DOOR);
					}
				}
			}
		}
	}

	// Entities that stopped being loose without going through updateLooseObject() (picked up,
	// teleported, put away): the engine reset their box
	std::vector<Entity *> stopped;
	JPH::BodyInterface & bodies = system()->GetBodyInterface();
	for(auto & entry : g_loose) {
		Entity & io = *entry.first;
		if(!io.obj || !io.obj->pbox || io.obj->pbox->active != 1 || io.show != SHOW_FLAG_IN_SCENE) {
			stopped.push_back(&io);
			continue;
		}
		LooseBody & body = entry.second;
		if(body.spawning && g_gameTime.now() - body.launched > SpawnGrace) {
			bodies.SetObjectLayer(body.id, LayerMoving);
			body.spawning = false;
		}
	}
	for(Entity * io : stopped) {
		removeLooseBody(g_loose.find(io));
	}
}

void removeLooseObject(Entity & io) {
	auto loose = g_loose.find(&io);
	if(loose != g_loose.end()) {
		removeLooseBody(loose);
	}
	auto obstacle = g_obstacles.find(&io);
	if(obstacle != g_obstacles.end()) {
		removeObstacle(obstacle);
	}
}

void clearLooseObjects() {
	while(!g_loose.empty()) {
		removeLooseBody(g_loose.begin());
	}
	while(!g_obstacles.empty()) {
		removeObstacle(g_obstacles.begin());
	}
}

size_t looseObjectCount() {
	return g_loose.size();
}

void dumpLooseObjects() {
	for(const auto & entry : g_loose) {
		const Entity & io = *entry.first;
		LogInfo << "  loose " << io.idString() << " at " << io.pos.x << " " << io.pos.y << " " << io.pos.z
		        << " box " << (io.obj && io.obj->pbox ? io.obj->pbox->active : -1);
	}
}

} // namespace physics

#else // ARX_HAVE_JOLT

namespace physics {

void launchObject(EERIE_3DOBJ * obj, const Vec3f & pos, const Anglef & angle, const Vec3f & vect, Entity * io) {
	ARX_UNUSED(obj), ARX_UNUSED(pos), ARX_UNUSED(angle), ARX_UNUSED(vect), ARX_UNUSED(io);
}
bool updateLooseObject(Entity & io) { ARX_UNUSED(io); return false; }
void createObstacles() { }
void syncObstacles() { }
void updateLooseObjects() { }
void removeLooseObject(Entity & io) { ARX_UNUSED(io); }
void clearLooseObjects() { }
size_t looseObjectCount() { return 0; }
void dumpLooseObjects() { }

} // namespace physics

#endif // ARX_HAVE_JOLT
