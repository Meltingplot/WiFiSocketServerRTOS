/*
 * Socket.cpp
 *
 *  Created on: 11 Apr 2017
 *      Author: David
 */
#include <cstring> 			// memcpy
#include <algorithm>			// for std::min

#include "lwip/tcp.h"

#include "Connection.h"
#include "Misc.h"				// for millis
#include "Config.h"

static_assert(MaxConnections < CONFIG_LWIP_MAX_SOCKETS); // Limits the listen callback value notification

// Public interface
Connection::Connection(uint8_t num)
	: number(num), localPort(0), remotePort(0), remoteIp(0), conn(nullptr), state(ConnState::free),
	closeTimer(0), readBuf(nullptr), readIndex(0), alreadyRead(0), pendOtherEndClosed(false)
{
}

size_t Connection::Read(uint8_t *data, size_t length)
{
	size_t lengthRead = 0;
	if (readBuf != nullptr && length != 0 && (state == ConnState::connected || state == ConnState::otherEndClosed))
	{
		do
		{
			const size_t toRead = std::min<size_t>(readBuf->len - readIndex, length);
			memcpy(data + lengthRead, (uint8_t *)readBuf->payload + readIndex, toRead);
			lengthRead += toRead;
			readIndex += toRead;
			length -= toRead;
			if (readIndex != readBuf->len)
			{
				break;
			}
			pbuf * const currentPb = readBuf;
			readBuf = readBuf->next;
			currentPb->next = nullptr;
			pbuf_free(currentPb);
			readIndex = 0;
		} while (readBuf != nullptr && length != 0);

		alreadyRead += lengthRead;
		if (readBuf == nullptr || alreadyRead >= TCP_MSS)
		{
			netconn_tcp_recvd(conn, alreadyRead);
			alreadyRead = 0;
		}

		if (pendOtherEndClosed && !readBuf)
		{
			pendOtherEndClosed = false;
			SetState(ConnState::otherEndClosed);
		}
	}
	return lengthRead;
}

size_t Connection::CanRead() const
{
	return ((state == ConnState::connected || state == ConnState::otherEndClosed) && readBuf != nullptr)
			? readBuf->tot_len - readIndex : 0;
}

// Write data to the connection. The amount of data may be zero.
// A note about writing:
// - LWIP is compiled with option LWIP_NETIF_TX_SINGLE_PBUF set. A comment says this is mandatory for the ESP8266.
// - A side effect of this is that when we call tcp_write, the data is always copied even if we don't set the TCP_WRITE_FLAG_COPY flag.
// - The PBUFs used to copy the outgoing data into are always large enough to accommodate the MSS. The total allocation size per PBUF is 1560 bytes.
// - Sending a full 2K of data may require 2 of these PBUFs to be allocated.
// - Due to memory fragmentation and other pending packets, this allocation is sometimes fails if we are serving more than 2 files at a time.
// - The result returned by tcp_sndbuf doesn't take account of the possibility that this allocation may fail.
// - When it receives a write request from the Duet main processor, our socket server has to say how much data it can accept before accepting it.
// - So in version 1.21 it sometimes happened that we accept some data based on the amount that tcp_sndbuf say we can, but we can't actually send it.
// - We then terminate the connection, and the client request fails.
// To mitigate this we could:
// - Have one overflow write buffer, shared between all connections
// - Only accept write data from the Duet main processor if the overflow buffer is free
// - If after accepting data from the Duet main processor we find that we can't send it, we send some of it if we can and store the rest in the overflow buffer
// - Then we push any pending data that we already have, and in Poll() we try to send the data in overflow buffer
// - When the overflow buffer is empty again, we can start accepting write data from the Duet main processor again.
// A further mitigation would be to restrict the amount of data we accept so some amount that will fit in the MSS, then tcp_write will need to allocate at most one PBUF.
// However, another reason why tcp_write can fail is because MEMP_NUM_TCP_SEG is set too low in Lwip. It now appears that this is the maoin cause of files tcp_write
// call in version 1.21. So I have increased it from 10 to 16, which seems to have fixed the problem..
size_t Connection::Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending)
{
	if (!(state == ConnState::connected && !pendOtherEndClosed))
	{
		return 0;
	}

	// Try to send all the data
	const bool push = doPush || closeAfterSending;

	u8_t flag = NETCONN_COPY | (push ? 0 : NETCONN_MORE);

	size_t total = 0;
	size_t written = 0;
	err_t rc = ERR_OK;

	for( ; total < length; total += written) {
		written = 0;
		rc = netconn_write_partly(conn, data + total, length - total, flag, &written);

		if (rc != ERR_OK && rc != ERR_WOULDBLOCK && rc != ERR_MEM) {
			break;
		}
		if ((rc == ERR_WOULDBLOCK || rc == ERR_MEM) && written == 0) {
			break;		// send buffer full and no progress after timeout, avoid spinning
		}
	}

	if (rc != ERR_OK)
	{
		if (rc == ERR_RST || rc == ERR_CLSD || rc == ERR_ABRT || rc == ERR_CONN)
		{
			SetState(ConnState::otherEndClosed);
		}
		else if (rc != ERR_WOULDBLOCK && rc != ERR_MEM)
		{
			// We failed to write the data. See above for possible mitigations. For now we just terminate the connection.
			debugPrintfAlways("Write fail len=%u err=%d\n", total, (int)rc);
			Terminate(false);		// chrishamm: Not sure if this helps with LwIP v1.4.3 but it is mandatory for proper error handling with LwIP 2.0.3
			return 0;
		}
	}

	// Close the connection again when we're done
	if (closeAfterSending)
	{
		Close();
	}

	return total;
}

size_t Connection::CanWrite() const
{
	// Return the amount of free space in the write buffer
	// Note: we cannot necessarily write this amount, because it depends on memory allocations being successful.
	return ((state == ConnState::connected && !pendOtherEndClosed) && conn && conn->pcb.tcp) ?
		std::min((size_t)tcp_sndbuf(conn->pcb.tcp), MaxDataLength) : 0;
}

void Connection::Poll()
{
	if ((state == ConnState::connected && !pendOtherEndClosed) || state == ConnState::otherEndClosed)
	{
		struct pbuf *data = nullptr;
		err_t rc = netconn_recv_tcp_pbuf_flags(conn, &data, NETCONN_NOAUTORCVD);

		while(rc == ERR_OK) {
			if (readBuf == nullptr) {
				readBuf = data;
				readIndex = alreadyRead = 0;
			} else {
				pbuf_cat(readBuf, data);
			}
			data = nullptr;
			rc = netconn_recv_tcp_pbuf_flags(conn, &data, NETCONN_NOAUTORCVD);
		}

		if (rc != ERR_WOULDBLOCK && rc != ERR_TIMEOUT)
		{
			debugPrintf("conn %u: poll recv rc=%d state=%d\n", number, (int)rc, (int)state);
			if (rc == ERR_RST || rc == ERR_CLSD || rc == ERR_CONN || rc == ERR_ABRT)
			{
				// Pend setting the state to other end closed if there is data to be read.
				// Otherwise, set it immediately. This is to avoid a case when a socket in RRF
				// gets stuck in the peer disconnecting state, when it recieves the change of
				// the connection state to other end closed first, then polls it when in fact it
				// had data. By that time, the responder might have been already closed, leaving
				// no one to consume this data and thus the socket unable to progress in state.
				if (readBuf)
				{
					pendOtherEndClosed = true;
				}
				else
				{
					SetState(ConnState::otherEndClosed);
				}
			}
			else
			{
				Terminate(false);
			}
		}
	}
	else if (state == ConnState::closePending)
	{
		const bool timeout = millis() - closeTimer >= MaxSendWaitTime;
		if (!conn || !conn->pcb.tcp || !conn->pcb.tcp->unsent || timeout)
		{
			// Unsent data drained, PCB lost, or timeout — complete the close.
			// Use a short sendtimeout: data is already on the wire, we just
			// need one WiFi RTT for FIN/ACK. Don't use the default 2000ms
			// as that blocks the main loop.
			if (conn)
			{
				netconn_set_sendtimeout(conn, timeout ? 1 : 100);
				netconn_close(conn);
				netconn_delete(conn);
				conn = nullptr;
			}
			FreePbuf();
			SetState(ConnState::free);
			if (listener)
			{
				listener->Notify();
			}
		}
	}
	else { }
}

// Close the connection.
// If 'external' is true then the Duet main processor has requested termination, so we free up the connection.
// Otherwise it has failed because of an internal error, and we set the state to 'aborted'. The Duet main processor will see this and send a termination request,
// which will free it up.
void Connection::Close()
{
	debugPrintf("conn %u: close requested, state=%d\n", number, (int)state);
	switch(state)
	{
	case ConnState::connected:						// both ends are still connected
		// Defer the actual close to Poll() so we don't block the SPI handler.
		// Poll() will wait for unsent data to drain, then do the close.
		closeTimer = millis();
		SetState(ConnState::closePending);
		break;

	case ConnState::otherEndClosed:					// the other end has already closed the connection
	default:										// should not happen
		if (conn)
		{
			netconn_close(conn);
			netconn_delete(conn);
			conn = nullptr;
		}
		FreePbuf();
		SetState(ConnState::free);
		if (listener)
		{
			listener->Notify();
		}
		break;

	case ConnState::closePending:					// already pending, let Poll() handle it
		break;
	}
}

void Connection::Deallocate()
{
	if (state == ConnState::allocated)
	{
		SetState(ConnState::free);
	}
}

bool Connection::Connect(uint8_t protocol, uint32_t remoteIp, uint16_t remotePort)
{
	struct netconn * conn = netconn_new_with_callback(NETCONN_TCP, ConnectCallback);

	if (conn)
	{
		netconn_set_nonblocking(conn, true);
		netconn_set_recvtimeout(conn, MaxReadWriteTime);
		netconn_set_sendtimeout(conn, MaxReadWriteTime);

		ip_set_option(conn->pcb.tcp, SOF_REUSEADDR);

		this->conn = conn;
		this->protocol = protocol;
		SetState(ConnState::connecting);

		ip_addr_t tempIp;
		memset(&tempIp, 0, sizeof(tempIp));
		tempIp.u_addr.ip4.addr = remoteIp;
		err_t rc = netconn_connect(conn, &tempIp, remotePort);

		if (rc == ERR_OK || rc == ERR_INPROGRESS)
		{
			return true;
		}
		else
		{
			Terminate(true);
		}
	}
	else
	{
		debugPrintAlways("can't allocate connection\n");
	}

	return false;
}

void Connection::Terminate(bool external)
{
	debugPrintf("conn %u: terminate external=%d state=%d\n", number, (int)external, (int)state);
	if (conn) {
		// No need to pass to ConnectionTask and do a graceful close on the connection.
		// Delete it here.
		netconn_close(conn);
		netconn_delete(conn);
		conn = nullptr;
	}
	FreePbuf();
	SetState((external) ? ConnState::free : ConnState::aborted);
	// Only notify the listener when a slot actually becomes free.
	// When external=false the slot goes to 'aborted' (waiting for the
	// Duet to acknowledge), so there is no free slot to accept into.
	if (external && listener)
	{
		listener->Notify();
	}
}

void Connection::Accept(Listener *listener, struct netconn* conn, uint8_t protocol)
{
	this->protocol = protocol;
	Connected(listener, conn);
}

void Connection::Connected(Listener *listener, struct netconn* conn)
{
	this->conn = conn;
	this->listener = listener;
	localPort = conn->pcb.tcp->local_port;
	remotePort = conn->pcb.tcp->remote_port;
	remoteIp = conn->pcb.tcp->remote_ip.u_addr.ip4.addr;
	readIndex = alreadyRead = closeTimer = pendOtherEndClosed = 0;

	debugPrintf("conn %u: established %u.%u.%u.%u:%u -> :%u\n",
		number, remoteIp & 0xFF, (remoteIp >> 8) & 0xFF,
		(remoteIp >> 16) & 0xFF, (remoteIp >> 24) & 0xFF, remotePort, localPort);

	// This function is used in lower priority tasks than the main task.
	// Mark the connection ready last, so the main task does not use it when it's not ready.
	// This should also be free from being taken by Connection::Allocate, since the previous
	// state is not ConnState::free (Connection::Allocate sets the state to ConnState::allocated.).
	SetState(ConnState::connected);
}

void Connection::GetStatus(ConnStatusResponse& resp) const
{
	resp.socketNumber = number;
	resp.protocol = protocol;
	resp.state = state;
	resp.bytesAvailable = CanRead();
	resp.writeBufferSpace = CanWrite();
	resp.localPort = localPort;
	resp.remotePort = remotePort;
	resp.remoteIp = remoteIp;
}

void Connection::FreePbuf()
{
	if (readBuf != nullptr)
	{
		pbuf_free(readBuf);
		readBuf = nullptr;
	}
}

void Connection::Report()
{
	// The following must be kept in the same order as the declarations in class ConnState
	static const char* const connStateText[] =
	{
		"free",
		"connecting",			// socket is trying to connect
		"connected",			// socket is connected
		"remoteClosed",			// the other end has closed the connection

		"aborted",				// an error has occurred
		"closePending",			// close this socket when sending is complete
		"closeReady",			// about to be closed
		"allocated"				// allocated but not yet connected
	};

	const unsigned int st = (int)state;
	ets_printf("%s", (st < ARRAY_SIZE(connStateText)) ? connStateText[st]: "unknown");
	if (state != ConnState::free)
	{
		ets_printf(" %u, %u, %u.%u.%u.%u", localPort, remotePort, remoteIp & 255, (remoteIp >> 8) & 255, (remoteIp >> 16) & 255, (remoteIp >> 24) & 255);
	}
}

// Static functions

/*static*/ void Connection::Init()
{
	allocateMutex = xSemaphoreCreateMutex();

	for (size_t i = 0; i < MaxConnections; ++i)
	{
		connectionList[i] = new Connection((uint8_t)i);
	}
}

/*static*/ void Connection::PollAll()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		Connection::Get(i).Poll();
	}
}

/*static*/ void Connection::TerminateAll()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		Connection::Get(i).Terminate(true);
	}
}


/*static*/ void Connection::ReportConnections()
{
	ets_printf("Conns");
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		ets_printf("%c %u:", (i == 0) ? ':' : ',', i);
		connectionList[i]->Report();
	}
	ets_printf("\n");
}

/*static*/ void Connection::GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets)
{
	connectedSockets = 0;
	otherEndClosedSockets = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (Connection::Get(i).GetState() == ConnState::connected)
		{
			connectedSockets |= (1 << i);
		}
		else if (Connection::Get(i).GetState() == ConnState::otherEndClosed)
		{
			otherEndClosedSockets |= (1 << i);
		}
		else { }
	}
}

/*static*/ Connection *Connection::Allocate()
{
	Connection *conn = nullptr;

	// This sequence must be protected with a mutex, since it happens on
	// both ConnectionTask and the main task, the latter having lower
	// priority. If for example, this is executing on main task
	// specifically after the state == free check, at which point is
	// pre-empted by the ConnectionTask executing the same code, the allocated
	// Connection will have been already spent.
	if (!allocateMutex) { return nullptr; }
	if (xSemaphoreTake(allocateMutex, pdMS_TO_TICKS(200)) != pdTRUE) { return nullptr; }
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->state == ConnState::free)
		{
			conn = connectionList[i];
			conn->SetState(ConnState::allocated);
			break;
		}
	}
	xSemaphoreGive(allocateMutex);
	return conn;
}

/*static*/ uint16_t Connection::CountConnectionsOnPort(uint16_t port)
{
	uint16_t count = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->localPort == port)
		{
			const ConnState state = connectionList[i]->state;
			if (state == ConnState::connected || state == ConnState::otherEndClosed || state == ConnState::closePending)
			{
				++count;
			}
		}
	}
	return count;
}

/*static*/ void Connection::ConnectCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
	for (Connection *connection : connectionList)
	{
		if ((connection && connection->conn == conn) && connection->state == ConnState::connecting)
		{
			switch (evt)
			{
			case NETCONN_EVT_SENDPLUS:
				connection->Connected(nullptr, conn);
				break;

			case NETCONN_EVT_ERROR:
				connection->SetState(ConnState::otherEndClosed);
				break;
			default:
				break;
			}
		}
	}
}

// Static data
SemaphoreHandle_t Connection::allocateMutex = nullptr;
Connection *Connection::connectionList[MaxConnections];

// End
