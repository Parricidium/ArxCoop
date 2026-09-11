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

#include "coop/Session.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

#include "coop/Connection.h"
#include "coop/Puppets.h"
#include "coop/Replication.h"
#include "core/Config.h"
#include "cinematic/CinematicController.h"
#include "gui/Menu.h"
#include "gui/MenuPublic.h"
#include "gui/MenuWidgets.h"
#include "io/log/Logger.h"
#include "util/String.h"

coop::Session g_coop;

namespace coop {

namespace {

// Network events are worth having on disk even if the game dies right after.
void flushLog() {
	Logger::flush();
}

//! Host side: a connection that has not sent its Hello yet.
struct PendingPeer {
	std::shared_ptr<Connection> connection;
};

} // anonymous namespace

struct Session::Impl {

	boost::asio::io_context io;

	// Host
	std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
	std::vector<std::shared_ptr<Connection>> pending;
	std::map<PlayerId, std::shared_ptr<Connection>> clients;

	// Client
	std::unique_ptr<boost::asio::ip::tcp::resolver> resolver;
	std::unique_ptr<boost::asio::steady_timer> retryTimer;
	std::shared_ptr<Connection> server;
	std::string nickname;
	std::string address;
	u16 port = 0;
	int connectAttempts = 0;

	void connect(Session & session);

	void accept(Session & session);
	void handleHello(Session & session, std::shared_ptr<Connection> connection, Reader & payload);
	void handleClientMessage(Session & session, PlayerId id, MessageType type, Reader & payload);
	void handleServerMessage(Session & session, MessageType type, Reader & payload);

	PlayerId freeSlot(const Session & session) const {
		for(PlayerId id = 0; id < PlayerId(MaxPlayers); id++) {
			if(!session.player(id)) {
				return id;
			}
		}
		return InvalidPlayerId;
	}

};

Session::Session()
	: m_impl(new Impl)
	, m_role(Role::None)
	, m_state(State::Idle)
	, m_localId(InvalidPlayerId)
	, m_startRequested(false)
{ }

Session::~Session() = default;

const Player * Session::player(PlayerId id) const {
	for(const Player & player : m_players) {
		if(player.id == id) {
			return &player;
		}
	}
	return nullptr;
}

void Session::addPlayer(PlayerId id, std::string_view name) {
	removePlayer(id);
	m_players.push_back(Player{id, std::string(name)});
	std::sort(m_players.begin(), m_players.end(), [](const Player & a, const Player & b) {
		return a.id < b.id;
	});
	LogInfo << "[coop] player " << int(id) << " joined: " << name;
	flushLog();
}

void Session::removePlayer(PlayerId id) {
	auto it = std::remove_if(m_players.begin(), m_players.end(), [id](const Player & player) {
		return player.id == id;
	});
	if(it != m_players.end()) {
		LogInfo << "[coop] player " << int(id) << " left";
		m_players.erase(it, m_players.end());
		flushLog();
	}
}

void Session::reset() {

	if(m_impl->acceptor) {
		boost::system::error_code ec;
		m_impl->acceptor->close(ec);
		m_impl->acceptor.reset();
	}
	for(auto & connection : m_impl->pending) {
		connection->close();
	}
	m_impl->pending.clear();
	for(auto & entry : m_impl->clients) {
		entry.second->close();
	}
	m_impl->clients.clear();

	if(m_impl->resolver) {
		m_impl->resolver->cancel();
		m_impl->resolver.reset();
	}
	if(m_impl->retryTimer) {
		m_impl->retryTimer->cancel();
		m_impl->retryTimer.reset();
	}
	m_impl->connectAttempts = 0;
	if(m_impl->server) {
		m_impl->server->close();
		m_impl->server.reset();
	}

	// Run remaining completion handlers (they all bail out on closed connections).
	m_impl->io.poll();
	m_impl->io.restart();

	m_role = Role::None;
	m_state = State::Idle;
	m_localId = InvalidPlayerId;
	m_players.clear();
	m_endpoint.clear();
	m_startRequested = false;
	m_joinedRunningGame = false;

}

void Session::fail(std::string reason) {
	LogWarning << "[coop] " << reason;
	flushLog();
	reset();
	m_error = std::move(reason);
	m_state = State::Failed;
}

// Host --------------------------------------------------------------------------------------

bool Session::host(u16 port, std::string_view nickname) {

	reset();
	m_error.clear();

	using boost::asio::ip::tcp;

	boost::system::error_code ec;
	auto acceptor = std::make_unique<tcp::acceptor>(m_impl->io);
	tcp::endpoint endpoint(tcp::v4(), port);
	acceptor->open(endpoint.protocol(), ec);
	if(!ec) {
		acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
		acceptor->bind(endpoint, ec);
	}
	if(!ec) {
		acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
	}
	if(ec) {
		fail("cannot listen on port " + std::to_string(port) + ": " + ec.message());
		return false;
	}

	m_impl->acceptor = std::move(acceptor);
	m_impl->nickname = sanitizeNickname(nickname);
	m_role = Role::Host;
	m_state = State::Lobby;
	m_localId = 0;
	m_endpoint = "port " + std::to_string(port);
	addPlayer(0, m_impl->nickname);

	LogInfo << "[coop] hosting on " << m_endpoint << " as " << m_impl->nickname;
	flushLog();

	m_impl->accept(*this);

	return true;
}

void Session::Impl::accept(Session & session) {

	using boost::asio::ip::tcp;

	acceptor->async_accept([this, &session](const boost::system::error_code & ec, tcp::socket socket) {
		if(!acceptor) {
			return; // Session was reset
		}
		if(ec) {
			LogWarning << "[coop] accept failed: " << ec.message();
		} else {
			auto connection = std::make_shared<Connection>(std::move(socket));
			LogInfo << "[coop] incoming connection from " << connection->remoteAddress();
			flushLog();
			pending.push_back(connection);
			std::weak_ptr<Connection> weak = connection;
			connection->onMessage = [this, &session, weak](MessageType type, Reader & payload) {
				auto self = weak.lock();
				if(!self) {
					return;
				}
				if(type == MessageType::Hello) {
					handleHello(session, self, payload);
				} else {
					LogWarning << "[coop] unexpected message " << int(type) << " before Hello";
					self->close();
					pending.erase(std::remove(pending.begin(), pending.end(), self), pending.end());
				}
			};
			connection->onClose = [this, weak](const std::string & /* reason */) {
				auto self = weak.lock();
				if(self) {
					pending.erase(std::remove(pending.begin(), pending.end(), self), pending.end());
				}
			};
			connection->start();
		}
		accept(session);
	});

}

void Session::Impl::handleHello(Session & session, std::shared_ptr<Connection> connection, Reader & payload) {

	pending.erase(std::remove(pending.begin(), pending.end(), connection), pending.end());

	u32 version = payload.u32_();
	std::string name = sanitizeNickname(payload.string());

	auto reject = [&](std::string_view reason) {
		LogWarning << "[coop] rejecting " << connection->remoteAddress() << ": " << reason;
		Writer writer;
		writer.string(reason);
		connection->send(MessageType::Reject, writer);
		// Let the reject go out before closing: keep the connection around until it closes itself.
		std::weak_ptr<Connection> weak = connection;
		connection->onMessage = nullptr;
		connection->onClose = nullptr;
		pending.push_back(connection);
		boost::asio::post(io, [this, weak]() {
			if(auto self = weak.lock()) {
				self->close();
				pending.erase(std::remove(pending.begin(), pending.end(), self), pending.end());
			}
		});
	};

	if(version != ProtocolVersion) {
		reject("incompatible version");
		return;
	}
	if(session.m_state != State::Lobby && session.m_state != State::InGame) {
		reject("no game in progress");
		return;
	}
	PlayerId id = freeSlot(session);
	if(id == InvalidPlayerId) {
		reject("server is full");
		return;
	}

	// Welcome the new player with the current roster
	{
		Writer writer;
		writer.u8_(id);
		writer.u8_(u8(session.m_players.size()));
		for(const Player & player : session.m_players) {
			writer.u8_(player.id);
			writer.string(player.name);
		}
		writer.bool_(session.m_state == State::InGame);
		connection->send(MessageType::Welcome, writer);
	}

	// Tell the others
	{
		Writer writer;
		writer.u8_(id);
		writer.string(name);
		session.broadcast(MessageType::PlayerJoined, writer);
	}

	clients[id] = connection;
	session.addPlayer(id, name);

	connection->onMessage = [this, &session, id](MessageType type, Reader & reader) {
		handleClientMessage(session, id, type, reader);
	};
	connection->onClose = [this, &session, id](const std::string & reason) {
		LogInfo << "[coop] player " << int(id) << " disconnected: " << reason;
		clients.erase(id);
		session.removePlayer(id);
		Writer writer;
		writer.u8_(id);
		session.broadcast(MessageType::PlayerLeft, writer);
	};

}

void Session::Impl::handleClientMessage(Session & session, PlayerId id, MessageType type, Reader & payload) {

	switch(type) {

		case MessageType::Ping: {
			Writer writer;
			writer.u32_(payload.u32_());
			auto it = clients.find(id);
			if(it != clients.end()) {
				it->second->send(MessageType::Pong, writer);
			}
			break;
		}

		case MessageType::Chat: {
			payload.u8_(); // sender id is authoritative from the host side
			Writer writer;
			writer.u8_(id);
			writer.string(payload.string());
			session.broadcast(MessageType::Chat, writer);
			break;
		}

		case MessageType::Revive: {
			if(session.onRevive) {
				session.onRevive(id, payload);
			}
			break;
		}

		case MessageType::PlayerState:
		case MessageType::PlayerEquipment:
		case MessageType::SpellCast: {
			payload.u8_(); // sender id is authoritative from the host side
			// Relay to the other clients with the real id, then handle locally
			Writer writer;
			writer.u8_(id);
			writer.bytes(payload.rest());
			session.broadcast(type, writer, id);
			auto & handler = (type == MessageType::PlayerState) ? session.onPlayerState
			                 : (type == MessageType::PlayerEquipment) ? session.onPlayerEquipment : session.onSpellCast;
			if(handler) {
				handler(id, payload);
			}
			break;
		}

		default: {
			if(u16(type) >= 40 && session.onGameMessage) {
				session.onGameMessage(id, type, payload);
			} else {
				LogWarning << "[coop] unhandled message " << int(type) << " from player " << int(id);
			}
			break;
		}

	}

}

// Client ------------------------------------------------------------------------------------

void Session::join(std::string_view address, u16 port, std::string_view nickname) {

	reset();
	m_error.clear();

	m_impl->nickname = sanitizeNickname(nickname);
	m_impl->address = std::string(address);
	m_impl->port = port;
	m_role = Role::Client;
	m_state = State::Connecting;
	m_endpoint = std::string(address) + ":" + std::to_string(port);

	LogInfo << "[coop] connecting to " << m_endpoint << " as " << m_impl->nickname;
	flushLog();

	m_impl->connect(*this);

}

// The host may still be loading when the player clicks "join": keep trying for a while.
static constexpr int MaxConnectAttempts = 15;
static constexpr std::chrono::seconds ConnectRetryDelay(2);

void Session::Impl::connect(Session & session) {

	using boost::asio::ip::tcp;

	connectAttempts++;

	resolver = std::make_unique<tcp::resolver>(io);
	resolver->async_resolve(address, std::to_string(port),
		[this, &session](const boost::system::error_code & ec, tcp::resolver::results_type results) {
			if(!resolver) {
				return; // Session was reset
			}
			if(ec) {
				session.fail("cannot resolve host: " + ec.message());
				return;
			}
			auto socket = std::make_shared<tcp::socket>(io);
			boost::asio::async_connect(*socket, results,
				[this, &session, socket](const boost::system::error_code & ec2, const tcp::endpoint & /* endpoint */) {
					if(!resolver) {
						return; // Session was reset
					}
					resolver.reset();
					if(ec2) {
						if(ec2 == boost::asio::error::connection_refused && connectAttempts < MaxConnectAttempts) {
							LogInfo << "[coop] host not ready yet, retrying (" << connectAttempts << "/" << MaxConnectAttempts << ")";
							retryTimer = std::make_unique<boost::asio::steady_timer>(io, ConnectRetryDelay);
							retryTimer->async_wait([this, &session](const boost::system::error_code & ec3) {
								if(!retryTimer || ec3) {
									return; // Session was reset
								}
								retryTimer.reset();
								connect(session);
							});
							return;
						}
						session.fail("connection failed: " + ec2.message());
						return;
					}
					LogInfo << "[coop] connected to " << session.m_endpoint;
					flushLog();
					server = std::make_shared<Connection>(std::move(*socket));
					server->onMessage = [this, &session](MessageType type, Reader & payload) {
						handleServerMessage(session, type, payload);
					};
					server->onClose = [&session](const std::string & reason) {
						session.fail("disconnected from host: " + reason);
					};
					server->start();
					Writer writer;
					writer.u32_(ProtocolVersion);
					writer.string(nickname);
					server->send(MessageType::Hello, writer);
				}
			);
		}
	);

}

void Session::Impl::handleServerMessage(Session & session, MessageType type, Reader & payload) {

	switch(type) {

		case MessageType::Welcome: {
			session.m_localId = payload.u8_();
			u8 count = payload.u8_();
			session.m_players.clear();
			for(u8 i = 0; i < count; i++) {
				PlayerId id = payload.u8_();
				std::string name = payload.string();
				session.addPlayer(id, name);
			}
			session.addPlayer(session.m_localId, nickname);
			session.m_state = State::Lobby;
			bool inGame = payload.remaining() ? payload.bool_() : false;
			LogInfo << "[coop] joined as player " << int(session.m_localId) << (inGame ? " (game in progress)" : "");
			flushLog();
			if(inGame) {
				// Joining a running game: start our own character right away
				session.m_startRequested = true;
				session.m_joinedRunningGame = true;
				session.m_state = State::InGame;
			}
			break;
		}

		case MessageType::Reject: {
			session.fail("rejected by host: " + payload.string());
			break;
		}

		case MessageType::PlayerJoined: {
			PlayerId id = payload.u8_();
			std::string name = payload.string();
			session.addPlayer(id, name);
			break;
		}

		case MessageType::PlayerLeft: {
			session.removePlayer(payload.u8_());
			break;
		}

		case MessageType::StartGame: {
			session.m_startRequested = true;
			session.m_state = State::InGame; // no more joining, and the world sync may begin
			break;
		}

		case MessageType::Ping: {
			Writer writer;
			writer.u32_(payload.u32_());
			session.sendToHost(MessageType::Pong, writer);
			break;
		}

		case MessageType::Pong:
		case MessageType::Chat: {
			// Nothing to do with these yet
			break;
		}

		case MessageType::PlayerState:
		case MessageType::PlayerEquipment:
		case MessageType::SpellCast: {
			PlayerId id = payload.u8_();
			auto & handler = (type == MessageType::PlayerState) ? session.onPlayerState
			                 : (type == MessageType::PlayerEquipment) ? session.onPlayerEquipment : session.onSpellCast;
			if(id != session.m_localId && handler) {
				handler(id, payload);
			}
			break;
		}

		case MessageType::NpcState: {
			if(session.onNpcState) {
				session.onNpcState(payload);
			}
			break;
		}

		case MessageType::Revive: {
			if(session.onRevive) {
				session.onRevive(0, payload);
			}
			break;
		}

		default: {
			if(u16(type) >= 40 && session.onGameMessage) {
				session.onGameMessage(0, type, payload);
			} else {
				LogWarning << "[coop] unhandled message " << int(type) << " from host";
			}
			break;
		}

	}

}

// Common ------------------------------------------------------------------------------------

void Session::leave() {
	if(m_role != Role::None) {
		LogInfo << "[coop] leaving session";
	}
	reset();
	m_error.clear();
}

void Session::update() {

	if(startup.host || startup.join) {
		StartupRequest request = startup;
		startup = StartupRequest();
		if(!request.nickname.empty()) {
			config.coop.nickname = sanitizeNickname(request.nickname);
		}
		u16 port = request.port ? request.port : u16(config.coop.port);
		if(request.host) {
			host(port, config.coop.nickname);
		} else {
			if(!request.address.empty()) {
				config.coop.address = request.address;
			}
			join(config.coop.address, port, config.coop.nickname);
		}
		m_lobbyRequested = true;
	}

	if(m_role == Role::None) {
		return;
	}

	m_impl->io.poll();
	m_impl->io.restart();

	if(m_startRequested) {
		if(m_state != State::Lobby && m_state != State::InGame) {
			m_startRequested = false;
		} else {
			// Start the new quest right away, wherever we are (menu, intro fly-through, cinematic)
			if(!cinematicIsStopped()) {
				cinematicEnd();
			} else if(ARXmenu.mode() == Mode_MainMenu || ARXmenu.mode() == Mode_InGame) {
				m_startRequested = false;
				m_state = State::InGame;
				LogInfo << "[coop] starting the game";
				flushLog();
				puppetsReset();
				bool joined = m_joinedRunningGame;
				m_joinedRunningGame = false;
				if(!joined || !loadSavedCoopCharacter()) {
					ARX_MENU_Clicked_NEWQUEST();
				}
			}
		}
	}

}

void Session::resumeFromSave() {
	if(!isHost() || m_state != State::Lobby) {
		return;
	}
	m_state = State::InGame;
	LogInfo << "[coop] resuming a saved game";
	flushLog();
}

void Session::startGame() {
	if(!isHost() || m_state != State::Lobby) {
		return;
	}
	broadcast(MessageType::StartGame, Writer());
	m_startRequested = true;
	m_state = State::InGame;
}

void Session::sendToHost(MessageType type, const Writer & payload) {
	if(isClient() && m_impl->server) {
		m_impl->server->send(type, payload);
	}
}

void Session::sendToOthers(MessageType type, const Writer & payload) {
	if(isHost()) {
		broadcast(type, payload);
	} else {
		sendToHost(type, payload);
	}
}

void Session::sendTo(PlayerId id, MessageType type, const Writer & payload) {
	if(!isHost()) {
		return;
	}
	auto it = m_impl->clients.find(id);
	if(it != m_impl->clients.end()) {
		it->second->send(type, payload);
	}
}

void Session::broadcast(MessageType type, const Writer & payload, PlayerId except) {
	if(!isHost()) {
		return;
	}
	for(auto & entry : m_impl->clients) {
		if(entry.first != except) {
			entry.second->send(type, payload);
		}
	}
}

std::string sanitizeNickname(std::string_view name) {
	std::string result(util::trimRight(util::trimLeft(name)));
	for(char & c : result) {
		if(c == '\n' || c == '\r' || c == '\t') {
			c = ' ';
		}
	}
	if(result.size() > MaxNicknameLength) {
		result.resize(MaxNicknameLength);
	}
	if(result.empty()) {
		result = "Joueur";
	}
	return result;
}

} // namespace coop
