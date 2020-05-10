#define NOMINMAX
// API Abstraction
#include "PrimeEngine/APIAbstraction/APIAbstractionDefines.h"

#include "ServerNetworkManager.h"

// Outer-Engine includes

// Inter-Engine includes

#include "PrimeEngine/Lua/LuaEnvironment.h"

// additional lua includes needed
extern "C"
{
#include "PrimeEngine/../luasocket_dist/src/socket.h"
#include "PrimeEngine/../luasocket_dist/src/inet.h"
};

#include "PrimeEngine/../../GlobalConfig/GlobalConfig.h"

#include "PrimeEngine/GameObjectModel/GameObjectManager.h"
#include "PrimeEngine/Events/StandardEvents.h"
#include "PrimeEngine/Scene/DebugRenderer.h"

#include "PrimeEngine/Networking/StreamManager.h"
#include "PrimeEngine/Networking/EventManager.h"
#include "PrimeEngine/Networking/GhostManager.h"

// Sibling/Children includes
#include "ServerConnectionManager.h"

#include <string>
#include <sstream>

using namespace PE::Events;

namespace PE{
namespace Components{

PE_IMPLEMENT_CLASS1(ServerNetworkManager, NetworkManager);

ServerNetworkManager::ServerNetworkManager(PE::GameContext &context, PE::MemoryArena arena, Handle hMyself)
	: NetworkManager(context, arena, hMyself)
	, m_clientConnections(context, arena, PE_SERVER_MAX_CONNECTIONS)
{
	m_buffer = (char*)malloc(256 * sizeof(char));
	m_state = ServerState_Uninitialized;
}

ServerNetworkManager::~ServerNetworkManager()
{
	if (m_state != ServerState_Uninitialized)
		socket_destroy(&m_sock);
	free(m_buffer);
}

void ServerNetworkManager::addDefaultComponents()
{
	NetworkManager::addDefaultComponents();

	// no need to register handler as parent class already has this method registered
	// PE_REGISTER_EVENT_HANDLER(Events::Event_UPDATE, ServerConnectionManager::do_UPDATE);
}

void ServerNetworkManager::initNetwork()
{
	NetworkManager::initNetwork();
#if TCP_SERVER
	serverOpenTCPSocket();
#elif UDP_SERVER
	serverOpenUDPSocket();
#endif

}

void ServerNetworkManager::serverOpenTCPSocket()
{
	bool created = false;
	int numTries = 0;
	int port = PE_SERVER_PORT;

	while (!created)
	{
		const char *err = /*luasocket::*/inet_trycreate(&m_sock, SOCK_STREAM);
		if (err)
		{
			assert(!"error creating socket occurred");
			break;
		}

		err = inet_trybind(&m_sock, "0.0.0.0", (unsigned short)(port)); // leaves socket non-blocking
		numTries++;
		if (err)
		{
			if (numTries >= 10)
				break; // give up
			port++;
		}
		else
		{
			created = true;
			break;
		}
	}

	if (created)
		m_serverPort = port;
	else
	{
		assert(!"Could not create server");
		return;
	}

	const char *err = inet_trylisten(&m_sock, PE_SERVER_MAX_CONNECTIONS); // leaves socket non-blocking
	if (err)
	{
		PEINFO("Warning: Could not listen on socket. Err: %s\n", err);
		assert(!"Could not listen on socket");
		return;
	}

	m_state = ServerState_ConnectionListening;
}

void ServerNetworkManager::serverOpenUDPSocket()
{
	bool created = false;
	int numTries = 0;
	int port = PE_SERVER_PORT;

	//UDP socket
	while (!created)
	{
		const char *err = /*luasocket::*/inet_trycreate(&m_sock, SOCK_DGRAM);
		if (err)
		{
			assert(!"error creating socket occurred");
			break;
		}

		err = inet_trybind(&m_sock, "0.0.0.0", (unsigned short)(port)); // leaves socket non-blocking
		numTries++;
		if (err)
		{
			if (numTries >= 10)
				break; // give up
			port++;
		}
		else
		{
			created = true;
			break;
		}
	}

	if (created)
		m_serverPort = port;
	else
	{
		assert(!"Could not create server");
		return;
	}
	m_state = ServerState_ConnectionListening;
}

void ServerNetworkManager::createNetworkConnectionContext(t_socket sock, int clientId, PE::NetworkContext *pNetContext)
{
	pNetContext->m_clientId = clientId;

	NetworkManager::createNetworkConnectionContext(sock, pNetContext);

	{
		pNetContext->m_pConnectionManager = new (m_arena) ServerConnectionManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getConnectionManager()->addDefaultComponents();
	}

	{
		pNetContext->m_pStreamManager = new (m_arena) StreamManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getStreamManager()->addDefaultComponents();
	}

	{
		pNetContext->m_pEventManager = new (m_arena) EventManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getEventManager()->addDefaultComponents();
	}


	//ghost manager on server? 
	{
		pNetContext->m_pGhostManager = new (m_arena) GhostManager(*m_pContext, m_arena, *pNetContext, Handle());
		pNetContext->getGhostManager()->addDefaultComponents();
	}

	pNetContext->getConnectionManager()->initializeConnected(sock);

	addComponent(pNetContext->getConnectionManager()->getHandle());
	addComponent(pNetContext->getStreamManager()->getHandle());
}

t_socket ServerNetworkManager::GetClientSocketNumber(char* message, int& port)
{
	t_socket sock = 0;
	std::stringstream ss;
	ss << message;
	while (ss)
	{
		std::string s;
		ss >> s;
		if (s == "socket:")
		{
			std::string socket_num;
			ss >> socket_num;
			sock = atoi(socket_num.data());
		}
		if (s == "port:")
		{
			std::string port_num;
			ss >> port_num;
			port = atoi(port_num.data());
		}
	}
	return sock;
}

void ServerNetworkManager::GetAddressAndPort(char* message, char* addr, unsigned short &port)
{
	char* ptr = strtok(message, " ");
	while (ptr)
	{
		if (!strcmp(ptr, "addr:"))
		{
			ptr = strtok(NULL, " ");
			sprintf(addr, ptr);
		}
		else if (!strcmp(ptr, "port:"))
		{
			ptr = strtok(NULL, " ");
			port = (unsigned short)strtoul(ptr, NULL, 0);
		}
		ptr = strtok(NULL, " ");
	}
}

void ServerNetworkManager::do_UPDATE(Events::Event *pEvt)
{
	NetworkManager::do_UPDATE(pEvt);

	t_timeout timeout; // timeout supports managing timeouts of multiple blocking alls by using total.
	// but if total is < 0 it just uses block value for each blocking call
	timeout.block = 0;
	timeout.total = -1.0;
	timeout.start = 0;

	t_socket newlyCreatedSock;
#if TCP_SERVER
	int err = socket_accept(&m_sock, &newlyCreatedSock, NULL, NULL, &timeout);
	if (err != IO_DONE)
	{
		const char *s = socket_strerror(err);
		return;
	}
	//NOTE: needs to know if a client tries to connect first 
	if (err == IO_DONE)
	{
		m_connectionsMutex.lock();
		m_clientConnections.add(NetworkContext());
		int clientIndex = m_clientConnections.m_size - 1;
		NetworkContext &netContext = m_clientConnections[clientIndex];

		// create a tribes stack for this connection
		createNetworkConnectionContext(newlyCreatedSock, clientIndex, &netContext);
		m_connectionsMutex.unlock();

		PE::Events::Event_SERVER_CLIENT_CONNECTION_ACK evt(*m_pContext);
		evt.m_clientId = clientIndex;

		PE::Events::Event_SERVER_CLIENT_SYNC_LEVEL l_evt(*m_pContext);
		l_evt.m_LevelFile = "ccontrollvl0.x_level.levela";
		l_evt.m_LevelName = "CharacterControl";

		netContext.getEventManager()->scheduleEvent(&evt, m_pContext->getGameObjectManager(), true);
		netContext.getEventManager()->scheduleEvent(&l_evt, m_pContext->getGameObjectManager(), true);
	}
#elif UDP_SERVER 
	size_t bytesRecv;
	sockaddr_in messageOrigin;
 	socklen_t len = sizeof(messageOrigin);
	//TODO change the buffer used
	char* buff = (char*)malloc(512 * sizeof(char)); // this is a red flag change into member variable 
	int err = socket_recvfrom(&m_sock, buff, strlen(buff), &bytesRecv,
		(SA*)&messageOrigin, &len, &timeout);

	//TODO: change this so its the one from message sent
	char addrOrigin[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &(messageOrigin.sin_addr), addrOrigin, INET_ADDRSTRLEN);
	int portOrigin = ntohs(messageOrigin.sin_port);

	if (err == IO_DONE)
	{
		buff[(int)bytesRecv] = '\0';

		unsigned short portToConnect = 0;
		//int portToConnect = 0;
		char* addrToConnect = (char*)malloc(32 * sizeof(char));
		//t_socket ClientSock = GetClientSocketNumber(m_buffer, portToConnect);
		char* message = (char*)malloc(strlen(buff) * sizeof(char));
		strcpy(message, buff);
		GetAddressAndPort(buff, addrToConnect, portToConnect);

		if (strlen(buff) > 0)
		{
			PEINFO("Client connection message recieved, %s", buff);
			PEINFO("Length of message: %d \n", strlen(buff));

			t_timeout timeoutConnect;
			timeoutConnect.block = PE_CLIENT_TO_SERVER_CONNECT_TIMEOUT;
			timeoutConnect.total = -1.0;
			timeoutConnect.start = 0;

			int success = true;
			/*
				Create new socket for connection, and bind it to random port (0 trick)
			*/
			const char *sockErr = inet_trycreate(&newlyCreatedSock, SOCK_DGRAM);
			if (sockErr)
			{
				PEINFO("PE: Warning: Failed to create: reason: %s\n", sockErr);
				success = false;
			}

			/*
				get the address and port of the newly created sokcet
			*/
			sockErr = inet_trybind(&newlyCreatedSock, addrToConnect, portToConnect);

			if (sockErr)
			{
				PEINFO("PE: Warning: Failed to bind to new port: reason: %s\n", sockErr);
				success = false;
			}

			sockaddr_in newSocketConnectionInfo;
			int newSocketConnLen = sizeof(newSocketConnectionInfo);
			//change 0 to a specified port
			if (getsockname(newlyCreatedSock, (SA*)&newSocketConnectionInfo, &newSocketConnLen) < 0)
			{
				assert("Getting the new socket connection info fails");
			}
			int portNewlyCreatedSocket = ntohs(newSocketConnectionInfo.sin_port);

			//connect the newly created socket to original client port
			sockErr = inet_tryconnect(&newlyCreatedSock, addrOrigin, portOrigin, &timeoutConnect);
			if (sockErr)
			{
				PEINFO("PE: Warning: Failed to connect to %s: %d reason: %s\n", addrOrigin, portOrigin, sockErr);
				success = false;
			}

			if (success)
			{
				t_timeout timeoutSend;
				timeoutSend.block = PE_SOCKET_SEND_TIMEOUT;
				timeoutSend.total = -1.0;
				timeoutSend.start = 0;
				size_t sent;
				char* buff = (char*)malloc(1024 * sizeof(char));
				std::string clientConnectionMessage = "New connection, new port: ";
				//not supposed to use portOrigin, but the port of newly created socket
				clientConnectionMessage += std::to_string(portNewlyCreatedSocket);
				clientConnectionMessage += "\0";
				strcpy(buff, clientConnectionMessage.c_str());

				//use send packet function instead
				int errSend = socket_sendto(&newlyCreatedSock, (const char*)buff, strlen(buff), &sent,
					(SA*)&messageOrigin, len, &timeoutSend);
				//int errSend = socket_send(&newlyCreatedSock, (const char*)buff, strlen(buff), &sent, &timeoutSend);

				if (errSend == IO_DONE)
				{
					//not supposed to use portOrigin, but the port of newly created socket
					PEINFO("Send to client new port: %d", portNewlyCreatedSocket);
					PEINFO("%d bytes sent \n", sent);
				}
				else
				{
					PEINFO("Error sending message, %s", socket_strerror(errSend));
				}

				m_connectionsMutex.lock();
				m_clientConnections.add(NetworkContext());
				//this is what make client id unique
				int clientIndex = m_clientConnections.m_size - 1;
				NetworkContext &netContext = m_clientConnections[clientIndex];
				// create a tribes stack for this connection
				//this is where i try to access the ghost manager
				createNetworkConnectionContext(newlyCreatedSock, clientIndex, &netContext);
				m_connectionsMutex.unlock();

				PE::Events::Event_SERVER_CLIENT_CONNECTION_ACK evt(*m_pContext);
				evt.m_clientId = clientIndex;

				PE::Events::Event_SERVER_CLIENT_SYNC_LEVEL sEvt(*m_pContext);
				sEvt.m_LevelFile = "char_highlight.x_level.levela";
				sEvt.m_LevelName = "Basic";

				netContext.getEventManager()->scheduleEvent(&evt, m_pContext->getGameObjectManager(), true);
			}
		}
		free(addrToConnect);
	}
	else
	{
		const char* errMessage = socket_strerror(err);
		return;
	}
	free(buff);
#endif

}

void ServerNetworkManager::debugRender(int &threadOwnershipMask, float xoffset /* = 0*/, float yoffset /* = 0*/)
{
	sprintf(PEString::s_buf, "Server: Port %d %d Connections", m_serverPort, m_clientConnections.m_size);
	DebugRenderer::Instance()->createTextMesh(
		PEString::s_buf, true, false, false, false, 0,
		Vector3(xoffset, yoffset, 0), 1.0f, threadOwnershipMask);

	float dy = 0.025f;
	float dx = 0.01;
	float evtManagerDy = 0.15f;
	// debug render all networking contexts
	m_connectionsMutex.lock();
	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		sprintf(PEString::s_buf, "Connection[%d]:", i);

		DebugRenderer::Instance()->createTextMesh(
			PEString::s_buf, true, false, false, false, 0,
			Vector3(xoffset, yoffset + dy + evtManagerDy * i, 0), 1.0f, threadOwnershipMask);

		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->debugRender(threadOwnershipMask, xoffset + dx, yoffset + dy * 2.0f + evtManagerDy * i);
	}
	m_connectionsMutex.unlock();
}

void ServerNetworkManager::scheduleEventToAllExcept(PE::Networkable *pNetworkable, PE::Networkable *pNetworkableTarget, int exceptClient)
{
	for (unsigned int i = 0; i < m_clientConnections.m_size; ++i)
	{
		if ((int)(i) == exceptClient)
			continue;

		NetworkContext &netContext = m_clientConnections[i];
		netContext.getEventManager()->scheduleEvent(pNetworkable, pNetworkableTarget, true);
	}
}

#if 0 // template
		//////////////////////////////////////////////////////////////////////////
		// ConnectionManager Lua Interface
		//////////////////////////////////////////////////////////////////////////
		//
		void ConnectionManager::SetLuaFunctions(PE::Components::LuaEnvironment *pLuaEnv, lua_State *luaVM)
		{

			//static const struct luaL_Reg l_functions[] = {
			//	{"l_clientConnectToTCPServer", l_clientConnectToTCPServer},
			//	{NULL, NULL} // sentinel
			//};

			//luaL_register(luaVM, 0, l_functions);

			lua_register(luaVM, "l_clientConnectToTCPServer", l_clientConnectToTCPServer);


			// run a script to add additional functionality to Lua side of Skin
			// that is accessible from Lua
		// #if APIABSTRACTION_IOS
		// 	LuaEnvironment::Instance()->runScriptWorkspacePath("Code/PrimeEngine/Scene/Skin.lua");
		// #else
		// 	LuaEnvironment::Instance()->runScriptWorkspacePath("Code\\PrimeEngine\\Scene\\Skin.lua");
		// #endif

		}

		int ConnectionManager::l_clientConnectToTCPServer(lua_State *luaVM)
		{
			lua_Number lPort = lua_tonumber(luaVM, -1);
			int port = (int)(lPort);

			const char *strAddr = lua_tostring(luaVM, -2);

			GameContext *pContext = (GameContext *)(lua_touserdata(luaVM, -3));

			lua_pop(luaVM, 3);

			pContext->getConnectionManager()->clientConnectToTCPServer(strAddr, port);

			return 0; // no return values
		}

		//////////////////////////////////////////////////////////////////////////
#endif

	}; // namespace Components
}; // namespace PE
