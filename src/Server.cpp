/**
 * Server.cpp
 *
 * Copyright 2019 mikee47 <mike@sillyhouse.net>
 *
 * This file is part of the Sming SSDP Library
 *
 * This library is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, version 3 or later.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with FlashString.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 ****/

#include "debug.h"
#include "include/Network/SSDP/Server.h"
#include <SmingVersion.h>
#include <SystemClock.h>
#include <Timer.h>
#include <Platform/Station.h>

namespace SSDP
{
DEFINE_FSTR(SERVER_ID, "Sming/" SMING_VERSION " UPnP/1.0");

Server server;

void Server::UdpOut::onReceive(pbuf* buf, IpAddress remoteIP, uint16_t remotePort)
{
	server.onReceive(buf, remoteIP, remotePort);
}

void Server::onReceive(pbuf* buf, IpAddress remoteIP, uint16_t remotePort)
{
	// Block access from remote networks, or if connected via AP
	if(!WifiStation.isLocal(remoteIP)) {
		debug_w("[SSDP] Ignoring external message from %s", remoteIP.toString().c_str());
		return;
	}

	/*
	 * Content is text, so nul-terminate it.
	 * Queries from Echo Dot V3 look like this:
	 *
	 * 	 101 chars, 550 bytes, 1024 total (split over 2 packets)
	 *
	 * All except the first 101 characters are NUL, so determine the
	 * actual length before de-serialisation.
	 */
	size_t len = buf->len;
	auto p = memchr(buf->payload, '\0', len);
	if(p != nullptr) {
		len = static_cast<char*>(p) - static_cast<char*>(buf->payload);
	}

#if DEBUG_VERBOSE_LEVEL >= WARN
	String addr;
	addr += remoteIP.toString();
	addr += ':';
	addr += remotePort;
#endif

	if(len != buf->len || len != buf->tot_len) {
		debug_w("[SSDP] RX %s, %u chars, %u bytes, %u total", addr.c_str(), len, buf->len, buf->tot_len);
	}

	if(len == 0) {
		return;
	}

#if DEBUG_VERBOSE_LEVEL == DBG
	m_nputs(static_cast<const char*>(buf->payload), len);
	m_putc('\n');
#endif

	BasicMessage msg;
	HttpError err = msg.parse(static_cast<char*>(buf->payload), len);
	if(err != HPE_OK) {
		debug_e("[SSDP] errno: %u, %s (%u headers)", err, toString(err).c_str(), msg.count());
		return;
	}

	msg.remoteIP = remoteIP;
	msg.remotePort = remotePort;

	debug_d("[SSDP] RX %s %s: %u headers", addr.c_str(), toString(msg.type).c_str(), msg.count());

	receiveDelegate(msg);
}

/*
 * Called after device has filled in headers.
 */
static bool formatMessage(String& data, const Message& msg)
{
	DEFINE_FSTR_LOCAL(fstr_RESPONSE, "HTTP/1.1 200 OK\r\n");
	DEFINE_FSTR_LOCAL(fstr_NOTIFY, "NOTIFY");
	DEFINE_FSTR_LOCAL(fstr_MSEARCH, "M-SEARCH");
	DEFINE_FSTR_LOCAL(fstr_HTTP, " * HTTP/1.1\r\n");

	data.reserve(512);
	if(msg.type == MessageType::response) {
		data = fstr_RESPONSE;
	} else {
		if(msg.type == MessageType::notify) {
			// Check subtype has been set
			if(!msg.contains("NTS")) {
				debug_e("[SSDP] NTS field missing");
				return false;
			}
			data = fstr_NOTIFY;
		} else if(msg.type == MessageType::msearch) {
			data = fstr_MSEARCH;
		} else {
			debug_e("[SSDP] Bad message type");
			return false;
		}
		data += fstr_HTTP;
	}

	// Append message headers
	for(unsigned i = 0; i < msg.count(); ++i) {
		data += msg[i];
	}

	data += "\r\n";

#if DEBUG_VERBOSE_LEVEL == DBG
	debug_d("[SSDP] TX %s:%u", msg.remoteIP.toString().c_str(), msg.remotePort);
	m_nputs(data.c_str(), data.length());
#endif

	return true;
}

bool Server::sendMessage(const Message& msg)
{
	String data;
	if(!formatMessage(data, msg)) {
		return false;
	}

	/*
	 * If we don't do this, UDP goes pop with "udp_sendto: invalid pcb". Not entirely sure why
	 * but perhaps we need to bind to a new connection for each message...
	 */
	out.listen(0);

	if(!out.sendStringTo(msg.remoteIP, msg.remotePort, data)) {
		debug_e("[SSDP] sendStringTo (%s:%u) failed", toString(msg.remoteIP).c_str(), msg.remotePort);
		return false;
	}

	return true;
}

bool Server::begin(ReceiveDelegate onReceive, SendDelegate onSend)
{
	if(active) {
		debug_w("[SSDP] already started");
		return false;
	}

	if(!onReceive || !onSend) {
		debug_e("[SSDP] requires callbacks");
		return false;
	}

	this->receiveDelegate = onReceive;
	this->sendDelegate = onSend;

	if(!joinMulticastGroup(SSDP_MULTICAST_IP)) {
		debug_w("[SSDP] joinMulticastGroup() failed");
		return false;
	}

	if(!listen(SSDP_MULTICAST_PORT)) {
		debug_e("[SSDP] listen failed");
		return false;
	}

	debug_i("[SSDP] Started");
	active = true;
	return true;
}

void Server::onMessage(MessageSpec* ms)
{
	Message msg;
	if(buildMessage(msg, *ms)) {
		sendDelegate(msg, *ms);
	}

	if(ms->shouldRepeat()) {
		// Send again
		messageQueue.add(ms, 1000);
	} else {
		delete ms;
	}
}

void Server::end()
{
	if(!active) {
		return;
	}

	close();

	leaveMulticastGroup(SSDP_MULTICAST_IP);

	active = false;
}

bool Server::buildMessage(Message& msg, MessageSpec& ms)
{
	msg.type = ms.type();
	if(msg.type == MessageType::msearch) {
		msg["MAN"] = SSDP_MAN_DISCOVER;
		msg["MX"] = "10";
		msg.remoteIP = SSDP_MULTICAST_IP;
		msg.remotePort = SSDP_MULTICAST_PORT;

		switch(ms.target()) {
		case SearchTarget::root:
			msg["ST"] = UPNP_ROOTDEVICE;
			break;
		case SearchTarget::all:
			msg["ST"] = SSDP_ALL;
			break;
		default:
			/*
			 * TODO: This is all Control Point stuff.
			 *
			 * When that's implemented we'll have ControlPoint objects
			 * which will provide specific search target classes.
			 *
			 * For now though, we can issue general searches.
			 *
			 */
			debug_e("[SSDP] Invalid M-SEARCH target");
			return false;
		}
	} else {
		if(SystemClock.isSet()) {
			msg[HTTP_HEADER_DATE] = DateTime(SystemClock.now(eTZ_UTC)).toHTTPDate();
		}

		if(msg.type == MessageType::notify) {
			msg["NTS"] = toString(ms.notifySubtype());
		}

		if(msg.type == MessageType::response) {
			msg["EXT"] = "";
		}

		msg.remoteIP = ms.remoteIp();
		msg.remotePort = ms.remotePort();
		msg[HTTP_HEADER_CACHE_CONTROL] = _F("max-age=1800");
	}

	if(msg.type != MessageType::response) {
		msg[HTTP_HEADER_HOST] = msg.remoteIP.toString() + ':' + msg.remotePort;
	}
	//	msg[HTTP_HEADER_USER_AGENT] = SERVER_ID;

	// Recommended as this isn't a chunked message
	msg[HTTP_HEADER_CONTENT_LENGTH] = "0";

	// UPnP 2.0
	//	response["BOOTID.UPNP.ORG"] = bootId;
	//	response["CONFIGID.UPNP.ORG"] = configId;
	//	response["SEARCHPORT.UPNP.ORG"] = ...

	// These fields only required for IPv6
	//	response["01-NLS"] = bootId;
	//	response["OPT"] = _F("\"http://schemas.upnp.org/upnp/1/0/\"; ns=01");

	return true;
}

} // namespace SSDP
