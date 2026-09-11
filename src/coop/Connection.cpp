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

#include "coop/Connection.h"

#include <utility>

#include "io/log/Logger.h"

namespace coop {

Connection::Connection(boost::asio::ip::tcp::socket socket)
	: m_socket(std::move(socket))
	, m_open(true)
{
	boost::system::error_code ec;
	m_socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
}

void Connection::start() {
	readHeader();
}

std::string Connection::remoteAddress() const {
	boost::system::error_code ec;
	boost::asio::ip::tcp::endpoint endpoint = m_socket.remote_endpoint(ec);
	if(ec) {
		return "?";
	}
	return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

void Connection::send(MessageType type, const Writer & payload) {

	if(!m_open) {
		return;
	}

	std::vector<u8> frame(FrameHeaderSize + payload.size());
	encodeHeader(frame.data(), type, u32(payload.size()));
	if(payload.size()) {
		std::memcpy(frame.data() + FrameHeaderSize, payload.data().data(), payload.size());
	}

	bool idle = m_outgoing.empty();
	m_outgoing.push_back(std::move(frame));
	if(idle) {
		writeNext();
	}

}

void Connection::writeNext() {

	auto self = shared_from_this();
	boost::asio::async_write(m_socket, boost::asio::buffer(m_outgoing.front()),
		[self](const boost::system::error_code & ec, size_t /* transferred */) {
			if(!self->m_open) {
				return;
			}
			if(ec) {
				self->fail("write failed: " + ec.message());
				return;
			}
			self->m_outgoing.pop_front();
			if(!self->m_outgoing.empty()) {
				self->writeNext();
			}
		}
	);

}

void Connection::readHeader() {

	auto self = shared_from_this();
	boost::asio::async_read(m_socket, boost::asio::buffer(m_header, FrameHeaderSize),
		[self](const boost::system::error_code & ec, size_t /* transferred */) {
			if(!self->m_open) {
				return;
			}
			if(ec) {
				self->fail(ec == boost::asio::error::eof ? "connection closed" : "read failed: " + ec.message());
				return;
			}
			FrameHeader header = decodeHeader(self->m_header);
			if(header.size > MaxPayloadSize) {
				self->fail("oversized message");
				return;
			}
			self->readPayload(header);
		}
	);

}

void Connection::readPayload(FrameHeader header) {

	m_payload.resize(header.size);

	auto self = shared_from_this();
	auto dispatch = [self, header](const boost::system::error_code & ec, size_t /* transferred */) {
		if(!self->m_open) {
			return;
		}
		if(ec) {
			self->fail("read failed: " + ec.message());
			return;
		}
		if(self->onMessage) {
			Reader reader(self->m_payload.data(), self->m_payload.size());
			try {
				self->onMessage(MessageType(header.type), reader);
			} catch(const ReadError & e) {
				self->fail(std::string("malformed message: ") + e.what());
				return;
			}
		}
		if(self->m_open) {
			self->readHeader();
		}
	};

	if(header.size == 0) {
		// Nothing to read, but stay asynchronous so handlers never re-enter the caller.
		boost::asio::post(m_socket.get_executor(), [dispatch]() {
			dispatch(boost::system::error_code(), 0);
		});
	} else {
		boost::asio::async_read(m_socket, boost::asio::buffer(m_payload), dispatch);
	}

}

void Connection::fail(const std::string & reason) {
	if(!m_open) {
		return;
	}
	close();
	if(onClose) {
		onClose(reason);
	}
}

void Connection::close() {
	if(!m_open) {
		return;
	}
	m_open = false;
	boost::system::error_code ec;
	m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
	m_socket.close(ec);
	m_outgoing.clear();
}

} // namespace coop
