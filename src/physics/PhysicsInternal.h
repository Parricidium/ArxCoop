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

#ifndef ARX_PHYSICS_PHYSICSINTERNAL_H
#define ARX_PHYSICS_PHYSICSINTERNAL_H

// ArxModern: shared between the Jolt-using translation units of src/physics/ only

#ifdef ARX_HAVE_JOLT

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <vector>

#include "math/Types.h"

namespace physics {

//! Object layers
enum Layer : JPH::ObjectLayer {
	LayerStatic = 0, //!< level geometry, fixed entities
	LayerMoving = 1, //!< ragdoll parts, loose objects, NPC cylinders
	LayerSpawning = 2, //!< a just-launched object: collides with the static world only, until it has left the hand that held it
	LayerCount = 3
};

//! Broad phase layers
enum BroadPhase : JPH::uint8 {
	BroadPhaseStatic = 0,
	BroadPhaseMoving = 1,
	BroadPhaseCount = 2
};

constexpr float UnitsPerMetre = 100.f;
constexpr float MetresPerUnit = 1.f / UnitsPerMetre;

inline JPH::Vec3 toJolt(const Vec3f & v) {
	return JPH::Vec3(v.x, v.y, v.z) * MetresPerUnit;
}

inline JPH::Vec3 toJoltDirection(const Vec3f & v) {
	return JPH::Vec3(v.x, v.y, v.z);
}

inline Vec3f fromJolt(JPH::Vec3Arg v) {
	return Vec3f(v.GetX(), v.GetY(), v.GetZ()) * UnitsPerMetre;
}

inline JPH::Quat toJolt(const glm::quat & q) {
	return JPH::Quat(q.x, q.y, q.z, q.w);
}

inline glm::quat fromJolt(JPH::QuatArg q) {
	return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

//! What a body stands for, kept in the body user data with a value (see makeUserData)
enum BodyKind : JPH::uint64 {
	KindLevel = 0, //!< the level mesh, value unused (materials are per triangle)
	KindFixed = 1, //!< a fixed entity (IO_FIX), value = entity index | material << 20
	KindNpc = 2, //!< a living NPC's cylinder, value = entity index
	KindLoose = 3, //!< a dropped or thrown entity, value = entity index
	KindRagdoll = 4 //!< a ragdoll part, value = entity index
};

inline JPH::uint64 makeUserData(BodyKind kind, JPH::uint64 value) {
	return (kind << 32) | (value & 0xffffffffu);
}

inline BodyKind userDataKind(JPH::uint64 data) {
	return BodyKind(data >> 32);
}

inline JPH::uint32 userDataValue(JPH::uint64 data) {
	return JPH::uint32(data & 0xffffffffu);
}

inline JPH::uint64 fixedUserData(JPH::uint64 entityIndex, int material) {
	return makeUserData(KindFixed, (entityIndex & 0xfffffu) | (JPH::uint64(material) << 20));
}

inline JPH::uint32 fixedEntityIndex(JPH::uint64 data) {
	return userDataValue(data) & 0xfffffu;
}

inline int fixedMaterial(JPH::uint64 data) {
	return int(userDataValue(data) >> 20);
}

//! The physics system of the current level, null if none
JPH::PhysicsSystem * system();

//! Material of the level triangle or fixed entity the given body/sub shape belongs to (MATERIAL_NONE if not static)
int staticMaterial(const JPH::Body & body, const JPH::SubShapeID & subShape);

//! Contact recorded by the listener during a step, consumed on the main thread after it
struct ContactEvent {
	JPH::uint64 userData1;
	JPH::uint64 userData2;
	int material1; //!< engine Material of the static side (or MATERIAL_NONE)
	int material2;
	float speed; //!< relative speed along the normal, m/s
	Vec3f position; //!< engine units
};

//! Take the contacts recorded since the last call
void takeContactEvents(std::vector<ContactEvent> & out);

} // namespace physics

#endif // ARX_HAVE_JOLT

#endif // ARX_PHYSICS_PHYSICSINTERNAL_H
