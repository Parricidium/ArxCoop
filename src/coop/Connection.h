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

#ifndef ARX_COOP_CONNECTION_H
#define ARX_COOP_CONNECTION_H

// Internal header: only coop/*.cpp files should include this, it pulls in Boost.Asio.

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

#include "coop/Protocol.h"

namespace coop {

/*!
 * One framed TCP connection to a peer.
 *
 * All callbacks run from io_context::poll(), i.e. on the game thread.
 * The object keeps itself alive while asynchronous operations are pending.
 */
class Connection final : public std::enable_shared_from_this<Connection> {

public:

	typedef std::function<void(MessageType type, Reader & payload)> MessageHandler;
	typedef std::function<void(const std::string & reason)> CloseHandler;

	explicit Connection(boost::asio::ip::tcp::socket socket);

	boost::asio::ip::tcp::socket & socket() { return m_socket; }

	//! Starts reading frames. Handlers must be set before calling this.
	void start();

	void send(MessageType type, const Writer & payload);
	void send(MessageType type) { send(type, Writer()); }

	//! Closes the socket; the close handler is NOT invoked for local closes.
	void close();

	bool isOpen() const { return m_open; }

	std::string remoteAddress() const;

	MessageHandler onMessage;
	CloseHandler onClose;

private:

	void readHeader();
	void readPayload(FrameHeader header);
	void writeNext();
	void fail(const std::string & reason);

	boost::asio::ip::tcp::socket m_socket;
	bool m_open;
	u8 m_header[FrameHeaderSize];
	std::vector<u8> m_payload;
	std::deque<std::vector<u8>> m_outgoing;

};

} // namespace coop

#endif // ARX_COOP_CONNECTION_H
