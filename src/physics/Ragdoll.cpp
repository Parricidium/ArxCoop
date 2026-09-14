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

#include "physics/Ragdoll.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "animation/AnimationRender.h"
#include "core/GameTime.h"
#include "animation/Skeleton.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "graphics/GraphicsTypes.h"
#include "io/log/Logger.h"
#include "math/GtxFunctions.h"
#include "math/Vector.h"
#include "platform/Time.h"

namespace physics {

namespace {

/*!
 * A mirrored ragdoll glides from the pose it showed to the pose last received over the time the
 * sender takes between two states (measured), instead of jumping: at 10-15 states a second the
 * jumps would show as stepping.
 */
struct MirroredRagdoll {
	std::vector<BonePose> from;
	std::vector<BonePose> to;
	bool active = false;
	PlatformInstant start;
	PlatformInstant received;
	PlatformDuration duration = std::chrono::milliseconds(100);
};

constexpr PlatformDuration MinMirrorStep = std::chrono::milliseconds(30);
constexpr PlatformDuration MaxMirrorStep = std::chrono::milliseconds(300);

bool g_mirrorMode = false;
std::unordered_map<Entity *, MirroredRagdoll> g_mirrored;

float mirrorFactor(PlatformInstant start, PlatformDuration duration, bool active) {
	if(!active || duration <= PlatformDuration(0)) {
		return 1.f;
	}
	return glm::clamp(toMsf(platform::getTime() - start) / toMsf(duration), 0.f, 1.f);
}

BonePose mixPose(const BonePose & a, const BonePose & b, float t) {
	BonePose pose;
	pose.pos = glm::mix(a.pos, b.pos, t);
	pose.rot = glm::slerp(a.rot, b.rot, t);
	return pose;
}

bool applyMirroredPose(Entity & io, Skeleton & skeleton) {
	auto it = g_mirrored.find(&io);
	if(it == g_mirrored.end() || it->second.to.size() != skeleton.bones.size()) {
		return false;
	}
	const MirroredRagdoll & mirrored = it->second;
	float t = mirrored.from.size() == mirrored.to.size() ? mirrorFactor(mirrored.start, mirrored.duration, mirrored.active) : 1.f;
	size_t i = 0;
	for(VertexGroupId bone : skeleton.bones.handles()) {
		BonePose pose = (t >= 1.f) ? mirrored.to[i] : mixPose(mirrored.from[i], mirrored.to[i], t);
		i++;
		Bone & data = skeleton.bones[bone];
		data.anim.trans = pose.pos;
		data.anim.quat = pose.rot;
	}
	return true;
}

} // anonymous namespace

void setMirrorMode(bool mirrored) {
	if(g_mirrorMode != mirrored) {
		g_mirrorMode = mirrored;
		if(!mirrored) {
			g_mirrored.clear();
		}
	}
}

bool isMirrorMode() {
	return g_mirrorMode;
}

bool ragdollIsSimulated(const Entity & io); // below, per build

bool hasRagdoll(const Entity & io) {
	return g_mirrored.count(const_cast<Entity *>(&io)) != 0 || ragdollIsSimulated(io);
}

void mirrorRagdoll(Entity & io, const Vec3f & pos, bool active, const std::vector<BonePose> & bones) {
	if(!io.obj || !io.obj->m_skeleton || io.obj->m_skeleton->bones.size() != bones.size()) {
		return;
	}
	PlatformInstant now = platform::getTime();
	auto existing = g_mirrored.find(&io);
	if(existing == g_mirrored.end()) {
		MirroredRagdoll & mirrored = g_mirrored[&io];
		mirrored.to = bones;
		mirrored.active = active;
		mirrored.start = mirrored.received = now;
	} else {
		MirroredRagdoll & mirrored = existing->second;
		// Start from where the body is shown right now, reach the new pose in the time the last
		// state took to arrive
		if(mirrored.from.size() == mirrored.to.size()) {
			float t = mirrorFactor(mirrored.start, mirrored.duration, mirrored.active);
			for(size_t i = 0; i < mirrored.to.size(); i++) {
				mirrored.from[i] = (t >= 1.f) ? mirrored.to[i] : mixPose(mirrored.from[i], mirrored.to[i], t);
			}
		} else {
			mirrored.from = mirrored.to;
		}
		mirrored.duration = std::clamp(now - mirrored.received, MinMirrorStep, MaxMirrorStep);
		mirrored.received = mirrored.start = now;
		mirrored.to = bones;
		mirrored.active = active;
	}
	if(io.pos != pos) {
		io.pos = io.lastpos = pos;
		io.requestRoomUpdate = true;
	}
}

} // namespace physics

#ifdef ARX_HAVE_JOLT

#include "physics/PhysicsInternal.h"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>

namespace physics {

namespace {

struct RagdollInstance {
	JPH::Ref<JPH::Ragdoll> ragdoll;
	size_t bones = 0;
	GameInstant created;
	GameInstant lastCheck;
	Vec3f lastRoot = Vec3f(0.f);
};

//! A ragdoll whose pelvis moved less than this in a second is only twitching: put it to sleep
constexpr float CreepDistance = 2.f; // units
constexpr GameDuration CreepCheckInterval = 1s;
constexpr GameDuration CreepMinAge = 2s;

std::unordered_map<Entity *, RagdollInstance> g_ragdolls;
JPH::CollisionGroup::GroupID g_nextGroup = 1;

//! Generic joint limits, the same for every bone: enough for a body to fold, not to knot
constexpr float SwingHalfAngle = glm::radians(35.f);
constexpr float TwistHalfAngle = glm::radians(25.f);
constexpr float JointFriction = 4.f; // N m, damps the flailing and helps the body settle
constexpr float MinPartMass = 1.f; // kg
constexpr float MinPartRadius = 0.03f; // m, for bones without geometry

//! Bind pose position of a bone in object space
Vec3f bindPosition(const EERIE_3DOBJ & obj, VertexGroupId bone) {
	if(obj.grouplist.empty()) {
		return Vec3f(0.f);
	}
	return obj.vertexlist[obj.grouplist[bone].origin].v;
}

JPH::Vec3 anyPerpendicular(JPH::Vec3Arg axis) {
	JPH::Vec3 other = (std::abs(axis.GetY()) < 0.9f) ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisX();
	return axis.Cross(other).Normalized();
}

/*!
 * Build the ragdoll settings of an object in its bind pose (object space as world space).
 * Each bone becomes a body whose frame is the bone frame: origin at the bone origin, axes
 * of the object, so that bone.anim maps directly to the body transform.
 */
JPH::Ref<JPH::RagdollSettings> buildSettings(const EERIE_3DOBJ & obj, float scale) {

	const Skeleton & skeleton = *obj.m_skeleton;
	size_t count = skeleton.bones.size();

	JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
	settings->mSkeleton = new JPH::Skeleton();
	settings->mParts.resize(count);

	std::vector<JPH::Vec3> bind(count);
	for(VertexGroupId bone : skeleton.bones.handles()) {
		bind[size_t(bone)] = toJolt(bindPosition(obj, bone) * scale);
	}

	for(VertexGroupId bone : skeleton.bones.handles()) {

		size_t index = size_t(bone);
		const Bone & data = skeleton.bones[bone];
		// Bones without a parent hang from the first bone (the engine treats them as roots)
		int parent = data.father ? int(size_t(data.father)) : (index == 0 ? -1 : 0);
		std::string name = obj.grouplist.empty() ? std::string("root") : obj.grouplist[bone].name;
		settings->mSkeleton->AddJoint(name, parent);

		// Shape: convex hull of the bone's vertices in the bone frame
		std::vector<JPH::Vec3> points;
		JPH::Vec3 centroid = JPH::Vec3::sZero();
		for(VertexId vertex : obj.m_boneVertices[bone]) {
			points.push_back(toJolt(obj.vertexlocal[vertex] * scale));
			centroid += points.back();
		}
		if(!points.empty()) {
			centroid /= float(points.size());
		}
		JPH::RefConst<JPH::Shape> shape;
		if(points.size() >= 4) {
			JPH::ConvexHullShapeSettings hull(points.data(), int(points.size()), 0.02f);
			JPH::Shape::ShapeResult result = hull.Create();
			if(result.IsValid()) {
				shape = result.Get();
			}
		}
		if(!shape) {
			float radius = MinPartRadius;
			for(const JPH::Vec3 & p : points) {
				radius = std::max(radius, p.Length());
			}
			shape = new JPH::SphereShape(radius);
		}

		JPH::RagdollSettings::Part & part = settings->mParts[index];
		part.SetShape(shape);
		part.mPosition = JPH::RVec3(bind[index]);
		part.mRotation = JPH::Quat::sIdentity();
		part.mMotionType = JPH::EMotionType::Dynamic;
		part.mObjectLayer = LayerMoving;
		part.mFriction = 0.7f;
		part.mLinearDamping = 0.3f;
		part.mAngularDamping = 1.f;
		part.mMaxAngularVelocity = 4.f * glm::pi<float>();
		float mass = shape->GetMassProperties().mMass;
		if(mass < MinPartMass) {
			part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			part.mMassPropertiesOverride.mMass = MinPartMass;
		}

		if(parent >= 0) {
			// Twist axis = direction of the bone, from its origin towards its geometry
			JPH::Vec3 twist = centroid;
			if(twist.LengthSq() < 1e-4f) {
				twist = bind[index] - bind[size_t(parent)];
			}
			if(twist.LengthSq() < 1e-6f) {
				twist = JPH::Vec3::sAxisY();
			}
			twist = twist.Normalized();
			JPH::SwingTwistConstraintSettings * joint = new JPH::SwingTwistConstraintSettings();
			joint->mSpace = JPH::EConstraintSpace::WorldSpace;
			joint->mPosition1 = joint->mPosition2 = JPH::RVec3(bind[index]);
			joint->mTwistAxis1 = joint->mTwistAxis2 = twist;
			joint->mPlaneAxis1 = joint->mPlaneAxis2 = anyPerpendicular(twist);
			joint->mNormalHalfConeAngle = SwingHalfAngle;
			joint->mPlaneHalfConeAngle = SwingHalfAngle;
			joint->mTwistMinAngle = -TwistHalfAngle;
			joint->mTwistMaxAngle = TwistHalfAngle;
			joint->mMaxFrictionTorque = JointFriction;
			part.mToParent = joint;
		}
	}

	settings->Stabilize();
	settings->DisableParentChildCollisions();
	settings->CalculateBodyIndexToConstraintIndex();
	settings->CalculateConstraintIndexToBodyIdxPair();

	return settings;
}

} // anonymous namespace

namespace {

bool canRagdoll(const Entity & io) {
	if(!system() || !(io.ioflags & IO_NPC) || &io == entities.player() || !io.obj || !io.obj->m_skeleton) {
		return false;
	}
	if(io.obj->m_skeleton->bones.size() < 2 || (io.sfx_flag & SFX_TYPE_YLSIDE_DEATH)) {
		return false;
	}
	return !g_ragdolls.count(const_cast<Entity *>(&io));
}

//! Create the bodies of an entity in its bind pose (to be posed by the caller) and register them
RagdollInstance * createRagdoll(Entity & io) {

	JPH::Ref<JPH::RagdollSettings> settings = buildSettings(*io.obj, io.scale);
	JPH::Ref<JPH::Ragdoll> ragdoll = settings->CreateRagdoll(g_nextGroup++, 0, system());
	if(!ragdoll) {
		LogWarning << "Jolt: could not create ragdoll for " << io.idString();
		return nullptr;
	}
	ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

	RagdollInstance & instance = g_ragdolls[&io];
	instance.ragdoll = ragdoll;
	instance.bones = io.obj->m_skeleton->bones.size();
	instance.created = instance.lastCheck = g_gameTime.now();
	instance.lastRoot = io.pos;

	LogDebug("ragdoll for " << io.idString() << " with " << instance.bones << " bones");
	return &instance;
}

//! A save block file: little-endian binary, entity by entity
struct SaveWriter {
	std::string data;
	template <typename T>
	void write(const T & value) {
		data.append(reinterpret_cast<const char *>(&value), sizeof(value));
	}
	void write(std::string_view text) {
		write(JPH::uint32(text.size()));
		data.append(text.data(), text.size());
	}
};

struct SaveReader {
	std::string_view data;
	size_t pos = 0;
	bool failed = false;
	template <typename T>
	T read() {
		T value = T();
		if(failed || pos + sizeof(T) > data.size()) {
			failed = true;
			return value;
		}
		std::memcpy(&value, data.data() + pos, sizeof(T));
		pos += sizeof(T);
		return value;
	}
	std::string readString() {
		JPH::uint32 size = read<JPH::uint32>();
		if(failed || pos + size > data.size()) {
			failed = true;
			return std::string();
		}
		std::string text(data.substr(pos, size));
		pos += size;
		return text;
	}
};

constexpr char SaveMagic[8] = { 'A', 'R', 'X', 'R', 'A', 'G', 'D', '1' };

} // anonymous namespace

void onEntityDied(Entity & io, Entity * killer) {

	if(g_mirrorMode || !canRagdoll(io)) {
		return;
	}

	PlatformInstant start = platform::getTime();

	// Current pose of the skeleton in world space
	Skeleton & skeleton = *io.obj->m_skeleton;
	animateSkeleton(&io, io.animlayer.data(), skeleton);

	RagdollInstance * instance = createRagdoll(io);
	if(!instance) {
		return;
	}
	LogInfo << "physics: ragdoll for " << io.idString() << " (" << instance->bones << " bones) built in "
	        << toMsi(platform::getTime() - start) << " ms";
	JPH::Ragdoll & ragdoll = *instance->ragdoll;

	// Move the bodies to the pose the entity died in
	JPH::BodyInterface & bodies = system()->GetBodyInterface();
	for(VertexGroupId bone : skeleton.bones.handles()) {
		const Bone & data = skeleton.bones[bone];
		bodies.SetPositionAndRotation(ragdoll.GetBodyID(int(size_t(bone))), JPH::RVec3(toJolt(data.anim.trans)),
		                              toJolt(data.anim.quat).Normalized(), JPH::EActivation::Activate);
	}

	// A push away from whoever dealt the blow
	if(killer && killer != &io) {
		Vec3f dir = io.pos - killer->pos;
		dir.y = 0.f;
		if(arx::length2(dir) > 1.f) {
			dir = glm::normalize(dir);
			ragdoll.SetLinearVelocity(toJoltDirection(dir) * 1.5f + JPH::Vec3(0.f, -0.5f, 0.f));
		}
	}
}

std::string serializeRagdolls() {

	JPH::PhysicsSystem * world = system();
	if(!world || g_ragdolls.empty()) {
		return std::string();
	}

	SaveWriter out;
	out.data.append(SaveMagic, sizeof(SaveMagic));
	out.write(JPH::uint32(g_ragdolls.size()));

	const JPH::BodyInterface & bodies = world->GetBodyInterface();
	for(const auto & entry : g_ragdolls) {
		const Entity & io = *entry.first;
		const JPH::Ragdoll & ragdoll = *entry.second.ragdoll;
		out.write(std::string_view(io.idString()));
		out.write(JPH::uint8(ragdoll.IsActive() ? 1 : 0));
		out.write(JPH::uint32(ragdoll.GetBodyCount()));
		for(size_t i = 0; i < ragdoll.GetBodyCount(); i++) {
			JPH::RVec3 position;
			JPH::Quat rotation;
			bodies.GetPositionAndRotation(ragdoll.GetBodyID(int(i)), position, rotation);
			Vec3f pos = fromJolt(JPH::Vec3(position));
			out.write(pos.x), out.write(pos.y), out.write(pos.z);
			out.write(rotation.GetX()), out.write(rotation.GetY()), out.write(rotation.GetZ()), out.write(rotation.GetW());
		}
	}

	return out.data;
}

void restoreRagdolls(std::string_view buffer) {

	JPH::PhysicsSystem * world = system();
	if((!world && !g_mirrorMode) || buffer.size() < sizeof(SaveMagic)
	   || buffer.compare(0, sizeof(SaveMagic), SaveMagic, sizeof(SaveMagic)) != 0) {
		return;
	}

	SaveReader in { buffer, sizeof(SaveMagic) };
	JPH::uint32 count = in.read<JPH::uint32>();
	size_t restored = 0;
	for(JPH::uint32 n = 0; n < count && !in.failed; n++) {

		std::string id = in.readString();
		bool active = in.read<JPH::uint8>() != 0;
		JPH::uint32 bones = in.read<JPH::uint32>();
		std::vector<float> pose(size_t(bones) * 7);
		for(float & value : pose) {
			value = in.read<float>();
		}
		if(in.failed) {
			LogWarning << "Jolt: truncated ragdoll save data";
			break;
		}

		Entity * io = entities.getById(id);
		if(!io || !IsDeadNPC(*io) || !canRagdoll(*io) || io->obj->m_skeleton->bones.size() != bones) {
			continue; // the entity is gone, alive again or has another skeleton: keep its animation
		}

		if(g_mirrorMode) {
			// Not simulating here: the saved pose is simply shown
			std::vector<BonePose> poses(bones);
			for(size_t i = 0; i < bones; i++) {
				const float * v = &pose[i * 7];
				poses[i].pos = Vec3f(v[0], v[1], v[2]);
				poses[i].rot = glm::normalize(glm::quat(v[6], v[3], v[4], v[5]));
			}
			mirrorRagdoll(*io, io->pos, false, poses);
			restored++;
			continue;
		}

		RagdollInstance * instance = createRagdoll(*io);
		if(!instance) {
			continue;
		}
		JPH::Ragdoll & ragdoll = *instance->ragdoll;
		JPH::BodyInterface & bodies = world->GetBodyInterface();
		for(size_t i = 0; i < bones; i++) {
			const float * v = &pose[i * 7];
			JPH::Quat rotation(v[3], v[4], v[5], v[6]);
			bodies.SetPositionAndRotation(ragdoll.GetBodyID(int(i)), JPH::RVec3(toJolt(Vec3f(v[0], v[1], v[2]))),
			                              rotation.Normalized(), active ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		}
		if(!active) {
			// It lay still when it was saved: no need to let it settle again
			for(size_t i = 0; i < bones; i++) {
				bodies.DeactivateBody(ragdoll.GetBodyID(int(i)));
			}
		}
		restored++;
	}

	if(restored > 0) {
		LogInfo << "Jolt: restored " << restored << " ragdolls";
	}
}

void removeRagdoll(Entity & io) {

	g_mirrored.erase(&io);

	auto it = g_ragdolls.find(&io);
	if(it == g_ragdolls.end()) {
		return;
	}
	if(system()) {
		it->second.ragdoll->RemoveFromPhysicsSystem();
	}
	g_ragdolls.erase(it);
}

bool applyRagdollPose(Entity & io, Skeleton & skeleton) {

	if(applyMirroredPose(io, skeleton)) {
		return true;
	}

	auto it = g_ragdolls.find(&io);
	if(it == g_ragdolls.end()) {
		return false;
	}
	JPH::PhysicsSystem * world = system();
	if(!world || it->second.bones != skeleton.bones.size()) {
		return false;
	}

	const JPH::Ragdoll & ragdoll = *it->second.ragdoll;
	const JPH::BodyInterface & bodies = world->GetBodyInterface();
	for(VertexGroupId bone : skeleton.bones.handles()) {
		JPH::RVec3 position;
		JPH::Quat rotation;
		bodies.GetPositionAndRotation(ragdoll.GetBodyID(int(size_t(bone))), position, rotation);
		Bone & data = skeleton.bones[bone];
		data.anim.trans = fromJolt(JPH::Vec3(position));
		data.anim.quat = fromJolt(rotation);
	}

	return true;
}

void updateRagdolls() {

	JPH::PhysicsSystem * world = system();
	if(!world) {
		return;
	}

	JPH::BodyInterface & bodies = world->GetBodyInterface();
	GameInstant now = g_gameTime.now();
	for(auto & entry : g_ragdolls) {
		Entity & io = *entry.first;
		RagdollInstance & instance = entry.second;
		const JPH::Ragdoll & ragdoll = *instance.ragdoll;
		if(!ragdoll.IsActive()) {
			continue;
		}
		// The entity stands where its pelvis lies, on the floor below it
		JPH::RVec3 pelvis = bodies.GetPosition(ragdoll.GetBodyID(0));
		Vec3f pos = fromJolt(JPH::Vec3(pelvis));
		
		// Joint limits fighting each other keep a lying body twitching (and creeping) for ever
		if(now - instance.lastCheck > CreepCheckInterval) {
			bool creeping = now - instance.created > CreepMinAge && glm::distance(pos, instance.lastRoot) < CreepDistance;
			instance.lastCheck = now;
			instance.lastRoot = pos;
			if(creeping) {
				for(size_t i = 0; i < ragdoll.GetBodyCount(); i++) {
					bodies.DeactivateBody(ragdoll.GetBodyID(int(i)));
				}
				continue;
			}
		}
		JPH::RRayCast ray(pelvis, JPH::Vec3(0.f, 3.f, 0.f));
		JPH::RayCastResult hit;
		if(world->GetNarrowPhaseQuery().CastRay(ray, hit, JPH::SpecifiedBroadPhaseLayerFilter(JPH::BroadPhaseLayer(BroadPhaseStatic)),
		                                        JPH::SpecifiedObjectLayerFilter(LayerStatic))) {
			pos.y += hit.mFraction * 3.f * UnitsPerMetre;
		} else {
			pos.y += 20.f;
		}
		if(arx::distance2(pos, io.pos) > 1.f) {
			io.pos = pos;
			io.lastpos = pos;
			io.requestRoomUpdate = true;
		}
	}
}

void clearRagdolls() {

	g_mirrored.clear();
	if(system()) {
		for(auto & entry : g_ragdolls) {
			entry.second.ragdoll->RemoveFromPhysicsSystem();
		}
	}
	g_ragdolls.clear();
}

bool getRagdollPose(const Entity & io, Vec3f & pos, bool & active, std::vector<BonePose> & bones) {

	auto it = g_ragdolls.find(const_cast<Entity *>(&io));
	JPH::PhysicsSystem * world = system();
	if(it == g_ragdolls.end() || !world) {
		return false;
	}
	const JPH::Ragdoll & ragdoll = *it->second.ragdoll;
	const JPH::BodyInterface & bodies = world->GetBodyInterface();
	pos = io.pos;
	active = ragdoll.IsActive();
	bones.resize(ragdoll.GetBodyCount());
	for(size_t i = 0; i < bones.size(); i++) {
		JPH::RVec3 position;
		JPH::Quat rotation;
		bodies.GetPositionAndRotation(ragdoll.GetBodyID(int(i)), position, rotation);
		bones[i].pos = fromJolt(JPH::Vec3(position));
		bones[i].rot = fromJolt(rotation);
	}
	return true;
}

void forEachRagdoll(const std::function<void(Entity & io, bool active)> & visit) {
	for(auto & entry : g_ragdolls) {
		visit(*entry.first, entry.second.ragdoll->IsActive());
	}
}

bool ragdollIsSimulated(const Entity & io) {
	return g_ragdolls.count(const_cast<Entity *>(&io)) != 0;
}

size_t ragdollCount() {
	return g_ragdolls.size();
}

void dumpRagdolls() {
	for(const auto & entry : g_ragdolls) {
		const Entity & io = *entry.first;
		LogInfo << "  ragdoll " << io.idString() << " at " << io.pos.x << " " << io.pos.y << " " << io.pos.z
		        << (entry.second.ragdoll->IsActive() ? " (active)" : " (asleep)");
	}
}

} // namespace physics

#else // ARX_HAVE_JOLT

namespace physics {

void onEntityDied(Entity & io, Entity * killer) { ARX_UNUSED(io), ARX_UNUSED(killer); }
void removeRagdoll(Entity & io) { g_mirrored.erase(&io); }
bool applyRagdollPose(Entity & io, Skeleton & skeleton) { return applyMirroredPose(io, skeleton); }
void updateRagdolls() { }
void clearRagdolls() { g_mirrored.clear(); }
bool getRagdollPose(const Entity & io, Vec3f & pos, bool & active, std::vector<BonePose> & bones) {
	ARX_UNUSED(io), ARX_UNUSED(pos), ARX_UNUSED(active), ARX_UNUSED(bones);
	return false;
}
void forEachRagdoll(const std::function<void(Entity & io, bool active)> & visit) { ARX_UNUSED(visit); }
bool ragdollIsSimulated(const Entity & io) { ARX_UNUSED(io); return false; }
size_t ragdollCount() { return 0; }
void dumpRagdolls() { }
std::string serializeRagdolls() { return std::string(); }
void restoreRagdolls(std::string_view buffer) { ARX_UNUSED(buffer); }

} // namespace physics

#endif // ARX_HAVE_JOLT
