/*
 * Listener.cpp
 *
 *  Created on: 12 Apr 2017
 *      Author: David
 */
#include <cstring>
#include <new>

#include "lwip/tcp.h"

#include "Listener.h"
#include "Connection.h"
#include "Config.h"

static_assert(MaxConnections < sizeof(uint32_t) * 8); // Limits the listen callback value notification



bool Listener::Start(uint16_t port, uint32_t ip, int protocol, int maxConns)
{
	// See if we are already listing for this
	for (Listener *listener : listeners)
	{
		if (listener && listener->port == port)
		{
			if (maxConns != 0 && (listener->ip == IPADDR_ANY || listener->ip == ip))
			{
				// already listening, so nothing to do
				debugPrintf("already listening on port %u\n", port);
				return true;
			}
			if (maxConns == 0 || ip == IPADDR_ANY)
			{
				listener->Stop();
				debugPrintf("stopped listening on port %u\n", port);
			}
		}
	}

	if (maxConns == 0)
	{
		return true;
	}

	uint8_t freeListener = MaxConnections + 1;

	for (int i = 0; i < MaxConnections; i++)
	{
		if (listeners[i] == nullptr)
		{
			freeListener = i;
			break;
		}
	}

	if (freeListener < MaxConnections)
	{
		Listener *listener = new (std::nothrow) Listener;

		if (listener)
		{
			// Setup LWIP listening connection.
			struct netconn * conn = netconn_new_with_callback(NETCONN_TCP, Listener::ListenCallback);

			if (conn)
			{
				netconn_set_nonblocking(conn, true);
				netconn_set_recvtimeout(conn, 1);
				netconn_set_sendtimeout(conn, 1);

				ip_addr_t tempIp;
				memset(&tempIp, 0, sizeof(tempIp));
				tempIp.u_addr.ip4.addr = ip;
				ip_set_option(conn->pcb.tcp, SOF_REUSEADDR); // seems to be needed for avoiding ERR_USE error when switching from client to AP

				err_t rc = netconn_bind(conn, &tempIp, port);

				if (rc == ERR_OK)
				{
					// Prepare this before listening
					listener->ip = ip;
					listener->port = port;
					listener->protocol = protocol;
					listener->maxConnections = maxConns;
					listener->conn = conn;
					listeners[freeListener] = listener;

					err_t rc = netconn_listen_with_backlog(conn, Backlog);
					if (rc == ERR_OK)
					{
						return true;
					}
					else
					{
						listeners[freeListener] = nullptr;
						debugPrintfAlways("Listen failed: %d\n", rc);
					}
				}
				else
				{
					debugPrintfAlways("can't bind PCB: %d\n", (int)rc);
				}

				netconn_close(conn);
				netconn_delete(conn);
			}
			else
			{
				debugPrintAlways("can't allocate PCB\n");
			}

			delete listener;
		}
		else
		{
			debugPrintAlways("failed to allocate memory for listener\n");
		}
	}
	else
	{
		debugPrintAlways("maximum number of listeners\n");
	}

	return false;
}

void Listener::Stop()
{
	netconn_close(conn);
	netconn_delete(conn);

	for (int i = 0; i < MaxConnections; i++)
	{
		Listener *listener = listeners[i];
		if (listener && listener->conn == conn)
		{
			delete listener;
			listeners[i] = nullptr;
		}
	}
}

/*static*/ void Listener::Init()
{
	for (int i  = 0; i < MaxConnections; i++)
	{
		listeners[i] = nullptr;
	}
	xTaskCreate(ListenerTask, "tcpListener", TCP_LISTENER_STACK, NULL, TCP_LISTENER_PRIO, &listenTaskHandle);
}

/*static*/ void Listener::Stop(uint16_t port)
{
	for (Listener *listener : listeners)
	{
		if (listener && (port == 0 || port == listener->port))
		{
			listener->Stop();
		}
	}
}

/*static*/ uint16_t Listener::GetPortByProtocol(uint8_t protocol)
{
	for (Listener *listener : listeners)
	{
		if (listener && listener->protocol == protocol)
		{
			return listener->port;
		}
	}
	return 0;
}

/*static*/ void Listener::ListenCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
	// debugPrintfAlways("netconn: %p, evt: %d len: %d\n", conn, evt, len);
	if (conn && conn->pcb.tcp && !len)
	{
		if (evt == NETCONN_EVT_RCVPLUS) // len == 0 && NETCONN_EVT_RCVPLUS can only be called for new connection
		{
			for (int i = 0; i < MaxConnections; i++)
			{
				Listener *listener = listeners[i];
				if (listener && listener->conn == conn)
				{
					xTaskNotify(listenTaskHandle, 0b1 << i, eSetBits);
				}
			}
		}
	}
}

void Listener::Notify()
{
	for (int i = 0; i < MaxConnections; i++)
	{
		Listener *listener = listeners[i];
		if (this == listener)
		{
			xTaskNotify(listenTaskHandle, 0b1 << i, eSetBits);
			break;
		}
	}
}

/*static*/ void Listener::ListenerTask(void* p)
{
	uint32_t flags = 0;

	while (xTaskNotifyWait(0, UINT_MAX, &flags, portMAX_DELAY) == pdTRUE) // should always be true
	{
		for (int i = 0; i < MaxConnections; i++)
		{
			Listener *listener = listeners[i];

			if (listener && (flags & (0b1 << i)))
			{
				const uint16_t numConns = Connection::CountConnectionsOnPort(listener->port);
				if (numConns < listener->maxConnections)
				{
					Connection * const c = Connection::Allocate();
					if (c != nullptr)
					{
						struct netconn *newConn;
						err_t rc = netconn_accept(listener->conn, &newConn);
						if (rc == ERR_OK)
						{
							netconn_set_nonblocking(newConn, true);
							netconn_set_recvtimeout(newConn, MaxReadWriteTime);
							netconn_set_sendtimeout(newConn, MaxReadWriteTime);
							c->Accept(listener, newConn, listener->protocol);
							if (listener->protocol == protocolFtpData)
							{
								debugPrintf("accept conn, stop listen on port %u\n", listener->port);
								listener->Stop();	// don't listen for further connections
							}
						}
						else
						{
							debugPrintf("accept failed on port %u: rc=%d\n", listener->port, (int)rc);
							c->Deallocate();
						}
					}
					else
					{
						debugPrintfAlways("pend connection on port %u no free conn\n", listener->port);
					}
				}
				else
				{
					debugPrintfAlways("pend connection on port %u already %u conns\n", listener->port, numConns);
				}
			}
		}
	}
}

// Static member data
TaskHandle_t Listener::listenTaskHandle = nullptr;
Listener *Listener::listeners[MaxConnections];

// End
