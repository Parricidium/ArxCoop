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

#ifndef ARX_COOP_PROTOCOL_H
#define ARX_COOP_PROTOCOL_H

#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/Platform.h"

/*!
 * Wire protocol of the co-op mod.
 *
 * Every message is framed as [u16 type][u32 payload size][payload], little endian.
 * Payloads are written with \ref coop::Writer and parsed with \ref coop::Reader.
 */
namespace coop {

constexpr u32 ProtocolVersion = 2;
constexpr u16 DefaultPort = 27015;
constexpr size_t MaxPlayers = 4;
constexpr size_t MaxNicknameLength = 24;
constexpr u32 MaxPayloadSize = 64 * 1024 * 1024; // level states are large (but compressed)

//! Player slot index; the host is always slot 0.
typedef u8 PlayerId;
constexpr PlayerId InvalidPlayerId = 0xFF;

enum class MessageType : u16 {

	// Client -> host
	Hello         = 1, //!< u32 protocol version, string nickname

	// Host -> client
	Welcome       = 10, //!< u8 your id, u8 count, (u8 id, string name) * count, u8 inGame
	Reject        = 11, //!< string reason
	PlayerJoined  = 12, //!< u8 id, string name
	PlayerLeft    = 13, //!< u8 id
	StartGame     = 14, //!< (empty) everyone starts a new quest

	// Both directions
	Ping          = 20, //!< u32 nonce
	Pong          = 21, //!< u32 nonce
	Chat          = 22, //!< u8 sender id, string text

	// Client -> host (id ignored) and host -> clients (id = the player the state belongs to)
	PlayerState   = 30, //!< u8 id, then see coop/Puppets.cpp

	// World synchronization, see coop/Replication.cpp
	EventForward  = 40, //!< C->H: string entity, u16 event id, string event name, u8 n, string params[n], string sender, string senderClass, s32 senderInstance
	ScriptCommand = 41, //!< H->C: string entity, u8 n, string words[n]
	SetGlobal     = 42, //!< H->C: string name, u8 type, (string | s32 | f32)
	SharedQuest   = 43, //!< both: string quest
	SharedKey     = 44, //!< both: string key
	SharedRune    = 45, //!< both: u32 rune
	SharedXP      = 46, //!< both: s32 amount
	SharedGold    = 61, //!< both: s32 amount (script rewards)
	SpellCast     = 62, //!< like PlayerState: u8 caster, u32 spell, f32 level, u32 flags, string target, s64 duration
	SpawnEntity   = 47, //!< H->C: u8 kind, string classPath, s32 instance, f32 pos[3], f32 angle[3]
	LevelState    = 48, //!< H->C: u32 area, string levelBlob, string globalsBlob, f32 pos[3]
	RequestLevel  = 49, //!< C->H: (empty)
	WorldSync     = 50, //!< H->C: u16 n quests, strings, u16 n keys, strings, u32 rune flags
	NpcState      = 51, //!< H->C: u16 n, then per NPC see coop/Puppets.cpp
	DamagePlayer  = 52, //!< both: u8 target, f32 damage, u32 type (a client sends it to the host, which applies or relays)
	DamageNpc     = 53, //!< C->H: string id, f32 damage, u32 type, u8 hasPos, f32 pos[3]
	SaveRequest   = 54, //!< H->C: string name: save your character under this name too
	LoadRequest   = 55, //!< H->C: string name: the host loaded this save, load your character from it
	SpeechSkip    = 56, //!< both: (empty) someone skipped the current speech / cutscene
	Revive        = 57, //!< C->H: u8 target id / H->C: (empty) you are revived
	TakeItem      = 58, //!< both: string id: this world item is now in someone's inventory
	DropItem      = 59, //!< both: string id, string classPath, s32 instance, f32 pos[3], f32 angle[3], s16 count, u8 thrown, f32 dir[3]
	PlayerEquipment = 60, //!< like PlayerState: u8 id, u8 skin, u8 combat, 3 x (string tweak, string skinFrom, string skinTo), string weapon, string shield
	DragItem      = 63, //!< both: string id, f32 pos[3], f32 angle[3]: a world item is being carried around
	SharedBag     = 64, //!< both: (empty) someone used a backpack: everyone gets the extra inventory
	TeleportPlayer = 65, //!< both: u8 target, u32 area, f32 pos[3], f32 yaw (a client sends it to the host, which applies or relays)

};

struct FrameHeader {
	u16 type;
	u32 size;
};
constexpr size_t FrameHeaderSize = sizeof(u16) + sizeof(u32);

inline void encodeHeader(u8 * out, MessageType type, u32 size) {
	u16 t = u16(type);
	std::memcpy(out, &t, sizeof(t));
	std::memcpy(out + sizeof(t), &size, sizeof(size));
}

inline FrameHeader decodeHeader(const u8 * in) {
	FrameHeader header;
	std::memcpy(&header.type, in, sizeof(header.type));
	std::memcpy(&header.size, in + sizeof(header.type), sizeof(header.size));
	return header;
}

//! Appends little-endian values to a byte buffer.
class Writer {

	std::vector<u8> m_data;

public:

	template <typename T>
	Writer & raw(const T & value) {
		static_assert(std::is_trivially_copyable<T>::value, "raw() needs a POD type");
		size_t pos = m_data.size();
		m_data.resize(pos + sizeof(T));
		std::memcpy(m_data.data() + pos, &value, sizeof(T));
		return *this;
	}

	Writer & u8_(u8 value) { return raw(value); }
	Writer & u16_(u16 value) { return raw(value); }
	Writer & u32_(u32 value) { return raw(value); }
	Writer & s32_(s32 value) { return raw(value); }
	Writer & s64_(s64 value) { return raw(value); }
	Writer & f32_(float value) { return raw(value); }
	Writer & bool_(bool value) { return raw(u8(value ? 1 : 0)); }

	Writer & string(std::string_view value) {
		if(value.size() > 0xFFFF) {
			value = value.substr(0, 0xFFFF);
		}
		u16_(u16(value.size()));
		size_t pos = m_data.size();
		m_data.resize(pos + value.size());
		std::memcpy(m_data.data() + pos, value.data(), value.size());
		return *this;
	}

	Writer & bytes(const u8 * data, size_t size) {
		m_data.insert(m_data.end(), data, data + size);
		return *this;
	}
	Writer & bytes(std::pair<const u8 *, size_t> span) { return bytes(span.first, span.second); }

	const std::vector<u8> & data() const { return m_data; }
	size_t size() const { return m_data.size(); }

};

struct ReadError : std::runtime_error {
	explicit ReadError(const char * what) : std::runtime_error(what) { }
};

//! Reads little-endian values from a byte buffer, throwing \ref ReadError on truncation.
class Reader {

	const u8 * m_data;
	size_t m_size;
	size_t m_pos;

public:

	Reader(const u8 * data, size_t size) : m_data(data), m_size(size), m_pos(0) { }

	template <typename T>
	T raw() {
		static_assert(std::is_trivially_copyable<T>::value, "raw() needs a POD type");
		if(m_pos + sizeof(T) > m_size) {
			throw ReadError("truncated message");
		}
		T value;
		std::memcpy(&value, m_data + m_pos, sizeof(T));
		m_pos += sizeof(T);
		return value;
	}

	u8 u8_() { return raw<u8>(); }
	u16 u16_() { return raw<u16>(); }
	u32 u32_() { return raw<u32>(); }
	s32 s32_() { return raw<s32>(); }
	s64 s64_() { return raw<s64>(); }
	float f32_() { return raw<float>(); }
	bool bool_() { return raw<u8>() != 0; }

	//! Three floats in order (never build a vector from three calls in one expression: evaluation order is unspecified).
	template <typename Vec>
	Vec vec3() {
		Vec v;
		v.x = f32_();
		v.y = f32_();
		v.z = f32_();
		return v;
	}

	std::string string() {
		u16 length = u16_();
		if(m_pos + length > m_size) {
			throw ReadError("truncated string");
		}
		std::string value(reinterpret_cast<const char *>(m_data + m_pos), length);
		m_pos += length;
		return value;
	}

	size_t remaining() const { return m_size - m_pos; }

	void skip(size_t count) {
		if(m_pos + count > m_size) {
			throw ReadError("truncated message");
		}
		m_pos += count;
	}

	//! The unread part of the buffer (does not advance the read position).
	std::pair<const u8 *, size_t> rest() const { return { m_data + m_pos, m_size - m_pos }; }

};

} // namespace coop

#endif // ARX_COOP_PROTOCOL_H
