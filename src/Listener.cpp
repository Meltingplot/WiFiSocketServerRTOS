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

#if SUPPORTS_TLS
// While TLS handshakes are in progress the Listener task wakes on this interval to step them
// rather than blocking on a listen notification
constexpr TickType_t HandshakeStepInterval = 1;
#endif

// Protects the listeners table, so that a listener cannot be deleted while the listener task is accepting a connection on it
static SemaphoreHandle_t listenersMutex = nullptr;

class ListenersLock
{
public:
	ListenersLock() { xSemaphoreTake(listenersMutex, portMAX_DELAY); }
	~ListenersLock() { xSemaphoreGive(listenersMutex); }
};


bool Listener::Start(uint16_t port, uint32_t ip, int protocol, int maxConns, bool tls)
{
	ListenersLock lock;

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
					listener->tls = tls;
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

// Stop listening and delete this listener. The caller must hold the listeners lock
void Listener::Stop()
{
	// Remove this listener from the table first, so that no further connection is accepted on it while it shuts down
	for (int i = 0; i < MaxConnections; i++)
	{
		if (listeners[i] == this)
		{
			listeners[i] = nullptr;
		}
	}

	netconn_close(conn);
	netconn_delete(conn);
	delete this;
}

/*static*/ void Listener::Init()
{
	for (int i  = 0; i < MaxConnections; i++)
	{
		listeners[i] = nullptr;
	}
	listenersMutex = xSemaphoreCreateMutex();
	xTaskCreatePinnedToCore(ListenerTask, "tcpListener", TCP_LISTENER_STACK, NULL,
		TCP_LISTENER_PRIO, &listenTaskHandle, NET_TASK_CPU);
}

/*static*/ void Listener::Stop(uint16_t port)
{
	ListenersLock lock;
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

// Wake the Listener task without a listen notification, so it runs PollHandshakes
/*static*/ void Listener::Wake()
{
	xTaskNotify(listenTaskHandle, 0, eNoAction);
}

/*static*/ void Listener::ListenerTask(void* p)
{
	TickType_t waitTime = portMAX_DELAY;

	for (;;)
	{
		// A finite waitTime means TLS handshakes are in progress and must be stepped again soon, so
		// a timeout here (xTaskNotifyWait returning pdFALSE, flags left at 0) is expected
		uint32_t flags = 0;
		xTaskNotifyWait(0, UINT_MAX, &flags, waitTime);

		for (int i = 0; i < MaxConnections; i++)
		{
			ListenersLock lock;
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
								c->listener = nullptr;	// clear before Stop() deletes the listener
								listener->Stop();	// don't listen for further connections
							}
						}
						else
						{
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

#if SUPPORTS_TLS
		// Step any deferred TLS handshakes, interleaved with the accepts above so a slow or stalled
		// client cannot block new connections. Keep waking on a short interval while a handshake is
		// still running, otherwise block until the next listen notification
		waitTime = Connection::PollHandshakes() ? HandshakeStepInterval : portMAX_DELAY;
#endif
	}
}

// Static member data
TaskHandle_t Listener::listenTaskHandle = nullptr;
Listener *Listener::listeners[MaxConnections];

// End
