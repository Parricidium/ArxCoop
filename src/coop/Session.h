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

#ifndef ARX_COOP_SESSION_H
#define ARX_COOP_SESSION_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coop/Protocol.h"

namespace coop {

class Writer;

enum class Role {
	None,   //!< Single player, no networking
	Host,   //!< We own the world
	Client, //!< We follow a host
};

enum class State {
	Idle,
	Connecting,  //!< Client: resolving / connecting / waiting for Welcome
	Lobby,       //!< Waiting in the lobby for the host to start
	InGame,
	Failed,      //!< Connection attempt or session ended with an error, see \ref Session::error()
};

struct Player {
	PlayerId id;
	std::string name;
};

/*!
 * The co-op session: owns the network I/O and the list of connected players.
 *
 * Networking is polled from the game thread once per frame via \ref update(),
 * so nothing here needs locking.
 */
class Session {

public:

	Session();
	~Session();

	Session(const Session &) = delete;
	Session & operator=(const Session &) = delete;

	//! Starts listening for players. Returns false (and sets \ref error()) on failure.
	bool host(u16 port, std::string_view nickname);

	//! Starts connecting to a host asynchronously; watch \ref state() for the result.
	void join(std::string_view address, u16 port, std::string_view nickname);

	//! Ends the session (tells the peers first when possible).
	void leave();
	//! Host: disconnects a client.
	void kick(PlayerId id);

	//! Pumps the network and dispatches messages. Call once per frame.
	void update();

	//! Host only: tells everyone to start a new quest, and does so locally too.
	void startGame();

	//! Host only: the session starts from a save the host just loaded (clients load theirs).
	void resumeFromSave();

	//! Client: the host resumed a saved game while we were in the lobby.
	void resumeFromHost() { if(m_role == Role::Client && m_state == State::Lobby) { m_state = State::InGame; } }

	Role role() const { return m_role; }
	State state() const { return m_state; }
	bool isActive() const { return m_role != Role::None; }
	bool isHost() const { return m_role == Role::Host; }
	bool isClient() const { return m_role == Role::Client; }

	//! Host of a running co-op game: the shared world must never stop for a menu.
	bool worldMustKeepRunning() const { return m_role == Role::Host && m_state == State::InGame; }

	PlayerId localId() const { return m_localId; }
	const std::vector<Player> & players() const { return m_players; }
	const Player * player(PlayerId id) const;

	//! Human readable description of the last failure.
	const std::string & error() const { return m_error; }
	
	/*!
	 * Action requested on the command line, performed on the first 
ef update().
	 */
	struct StartupRequest {
		bool host = false;
		bool join = false;
		std::string address;
		u16 port = 0; //!< 0 = use the configured port
		std::string nickname; //!< empty = use the configured nickname
	};
	StartupRequest startup;
	
	//! Returns true once after a command-line session was started, so the menu can show the lobby.
	bool consumeLobbyRequest() {
		bool pending = m_lobbyRequested;
		m_lobbyRequested = false;
		return pending;
	}

	//! Where we are listening or connected, for display.
	const std::string & endpointDescription() const { return m_endpoint; }

	//! IPv4 addresses of this machine (LAN), for the menu.
	std::vector<std::string> localAddresses() const;
	//! Starts fetching our public address (api.ipify.org); \ref publicAddress() fills in later.
	void fetchPublicAddress();
	const std::string & publicAddress() const { return m_publicAddress; }

	//! Client: sends a message to the host. Host: no-op.
	void sendToHost(MessageType type, const Writer & payload);

	//! Host: sends a message to every client (optionally skipping one). Client: no-op.
	void broadcast(MessageType type, const Writer & payload, PlayerId except = InvalidPlayerId);

	/*!
	 * Sends a message about ourselves to everyone else: clients send it to the host,
	 * which relays it to the other clients; the host broadcasts directly.
	 * The first payload byte must be the player id (the host overwrites it for relayed messages).
	 */
	void sendToOthers(MessageType type, const Writer & payload);

	//! Called with the state of another player (both on the host and on clients).
	std::function<void(PlayerId id, Reader & payload)> onPlayerState;

	//! Same routing as PlayerState, for the visible equipment.
	std::function<void(PlayerId id, Reader & payload)> onPlayerEquipment;

	//! Same routing as PlayerState, for spells cast by players.
	std::function<void(PlayerId id, Reader & payload)> onSpellCast;

	//! Same routing as PlayerState, for custom faces.
	std::function<void(PlayerId id, Reader & payload)> onPlayerFace;

	//! Same routing as PlayerState, for "look here" markers.
	std::function<void(PlayerId id, Reader & payload)> onPlayerMarker;

	//! Same routing as PlayerState, for blood effects of hits landed elsewhere.
	std::function<void(PlayerId id, Reader & payload)> onBlood;
	std::function<void(PlayerId id, Reader & payload)> onPlayerSpeech;

	//! Client: round trip time to the host in ms (0 until measured).
	u16 ownLatency() const { return m_ownLatency; }
	//! Host: round trip time to a client in ms (0 until measured).
	u16 measuredLatency(PlayerId id) const;

	//! Called whenever a player (ourselves included) is added to the roster; on the host  sendTo(id) already works.
	std::function<void(PlayerId id)> onPlayerJoined;

	//! Called whenever a player is removed from the roster (everyone when the session ends).
	std::function<void(PlayerId id)> onPlayerLeft;

	//! Called for game messages (type >= 40): on the host the sender is the client, on clients it is 0.
	std::function<void(PlayerId from, MessageType type, Reader & payload)> onGameMessage;

	//! Client: called with the host's NPC states.
	std::function<void(Reader & payload)> onNpcState;
	std::function<void(Reader & payload)> onPhysicsState;

	//! Revive requests: on the host from a client (u8 target), on a client from the host (you are revived).
	std::function<void(PlayerId from, Reader & payload)> onRevive;

	//! Host: sends a message to one client.
	void sendTo(PlayerId id, MessageType type, const Writer & payload);

private:

	struct Impl;
	friend struct Impl;

	void reset();
	void fail(std::string reason);
	void addPlayer(PlayerId id, std::string_view name);
	void removePlayer(PlayerId id);

	std::unique_ptr<Impl> m_impl;

	Role m_role;
	State m_state;
	PlayerId m_localId;
	std::vector<Player> m_players;
	std::string m_error;
	std::string m_endpoint;
	bool m_startRequested;
	bool m_lobbyRequested = false;
	bool m_joinedRunningGame = false;
	u16 m_ownLatency = 0;
	std::string m_publicAddress;
	std::map<PlayerId, u16> m_latencies; //!< Host: per client
	u64 m_lastPingTime = 0;

	void pingPeers();

};

//! Sanitizes a user-entered nickname (length, whitespace); returns a default if empty.
std::string sanitizeNickname(std::string_view name);

} // namespace coop

extern coop::Session g_coop;

#endif // ARX_COOP_SESSION_H
