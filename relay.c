/*
 * RemoteDesk2K - Relay Server Implementation
 * Windows 2000 compatible relay server
 */

#include "relay.h"
#include <stdio.h>

/* Relay server context */
typedef struct _RELAY_SERVER {
    SOCKET              listenSocket;
    SOCKET              *connectionSockets;
    PRELAY_CONNECTION   *connections;
    DWORD               maxConnections;
    DWORD               activeConnections;
    WORD                port;
    HANDLE              hServerThread;
    HANDLE              hStopEvent;
    CRITICAL_SECTION    csConnections;
    BOOL                bRunning;
} RELAY_SERVER_CONTEXT;

/* Helper: Send a relay packet to a socket */
static int SendRelayPacket(SOCKET sock, BYTE msgType, const BYTE *data, DWORD dataLength)
{
    RELAY_HEADER header;
    BYTE *packet;
    DWORD packetSize;
    int result;
    
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    packetSize = sizeof(RELAY_HEADER) + dataLength;
    packet = (BYTE*)malloc(packetSize);
    if (!packet) return RD2K_ERR_MEMORY;
    
    header.msgType = msgType;
    header.flags = 0;
    header.reserved = 0;
    header.dataLength = dataLength;
    
    CopyMemory(packet, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        CopyMemory(packet + sizeof(RELAY_HEADER), data, dataLength);
    }
    
    result = send(sock, (const char*)packet, packetSize, 0);
    free(packet);
    
    if (result == SOCKET_ERROR) {
        return RD2K_ERR_SEND;
    }
    
    return RD2K_SUCCESS;
}

static int WaitForSocketReady(SOCKET sock, BOOL bWrite, int timeoutMs)
{
    fd_set fds;
    struct timeval tv;
    int result;
    
    if (sock == INVALID_SOCKET) return -1;
    
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    
    if (bWrite) {
        result = select(0, NULL, &fds, NULL, &tv);
    } else {
        result = select(0, &fds, NULL, NULL, &tv);
    }
    
    if (result == SOCKET_ERROR) return -1;
    if (result == 0) return 0;
    return 1;
}

/* Find a connection by clientId */
static PRELAY_CONNECTION FindConnectionById(PRELAY_SERVER pServer, DWORD clientId)
{
    DWORD i;
    
    if (!pServer) return NULL;
    
    EnterCriticalSection(&pServer->csConnections);
    
    for (i = 0; i < pServer->activeConnections; i++) {
        if (pServer->connections[i] && 
            pServer->connections[i]->clientId == clientId &&
            pServer->connections[i]->state != RELAY_STATE_DISCONNECTED) {
            
            LeaveCriticalSection(&pServer->csConnections);
            return pServer->connections[i];
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    return NULL;
}

/* Add a new connection to the server */
static PRELAY_CONNECTION AddConnection(PRELAY_SERVER pServer, SOCKET sock)
{
    PRELAY_CONNECTION pConn;
    DWORD i;
    
    if (!pServer || pServer->activeConnections >= pServer->maxConnections) {
        return NULL;
    }
    
    pConn = (PRELAY_CONNECTION)calloc(1, sizeof(RELAY_CONNECTION));
    if (!pConn) return NULL;
    
    pConn->socket = sock;
    pConn->state = RELAY_STATE_CONNECTED;
    pConn->recvBufferSize = RELAY_BUFFER_SIZE;
    pConn->recvBuffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    pConn->recvPos = 0;
    pConn->hDisconnectEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    
    if (!pConn->recvBuffer || !pConn->hDisconnectEvent) {
        if (pConn->recvBuffer) free(pConn->recvBuffer);
        if (pConn->hDisconnectEvent) CloseHandle(pConn->hDisconnectEvent);
        free(pConn);
        return NULL;
    }
    
    EnterCriticalSection(&pServer->csConnections);
    
    /* Find empty slot */
    for (i = 0; i < pServer->maxConnections; i++) {
        if (!pServer->connections[i]) {
            pServer->connections[i] = pConn;
            pServer->activeConnections++;
            LeaveCriticalSection(&pServer->csConnections);
            return pConn;
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    
    /* No slot available */
    if (pConn->recvBuffer) free(pConn->recvBuffer);
    if (pConn->hDisconnectEvent) CloseHandle(pConn->hDisconnectEvent);
    free(pConn);
    return NULL;
}

/* Remove connection from server */
static void RemoveConnection(PRELAY_SERVER pServer, PRELAY_CONNECTION pConn)
{
    DWORD i;
    
    if (!pServer || !pConn) return;
    
    EnterCriticalSection(&pServer->csConnections);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i] == pConn) {
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0) {
                pServer->activeConnections--;
            }
            break;
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    
    /* Cleanup connection resources */
    if (pConn->socket != INVALID_SOCKET) {
        closesocket(pConn->socket);
    }
    if (pConn->recvBuffer) {
        free(pConn->recvBuffer);
    }
    if (pConn->hDisconnectEvent) {
        CloseHandle(pConn->hDisconnectEvent);
    }
    if (pConn->hThread) {
        CloseHandle(pConn->hThread);
    }
    
    free(pConn);
}

/* Handle relay protocol messages on a connection */
static int ProcessRelayMessage(PRELAY_SERVER pServer, PRELAY_CONNECTION pConn,
                              const BYTE *buffer, DWORD length)
{
    RELAY_HEADER header;
    PRELAY_CONNECTION pPartner;
    RELAY_CONNECT_RESPONSE response;
    
    if (length < sizeof(RELAY_HEADER)) {
        return -1;  /* Invalid message */
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    switch (header.msgType) {
        case RELAY_MSG_REGISTER: {
            RELAY_REGISTER_MSG reg;
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_MSG)) {
                return -1;
            }
            CopyMemory(&reg, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_MSG));
            pConn->clientId = reg.clientId;
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_CONNECT_REQUEST: {
            RELAY_CONNECT_REQUEST req;
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_REQUEST)) {
                return -1;
            }
            CopyMemory(&req, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_REQUEST));
            
            /* Look for partner */
            pPartner = FindConnectionById(pServer, req.partnerId);
            if (!pPartner || pPartner->state != RELAY_STATE_REGISTERED) {
                response.status = RD2K_ERR_CONNECT;  /* Partner not found */
            } else {
                /* Pair the connections */
                pConn->pPartner = pPartner;
                pPartner->pPartner = pConn;
                pConn->state = RELAY_STATE_PAIRED;
                pPartner->state = RELAY_STATE_PAIRED;
                response.status = RD2K_SUCCESS;
            }
            
            /* Send response to requesting client */
            SendRelayPacket(pConn->socket, RELAY_MSG_CONNECT_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            
            pConn->lastActivity = GetTickCount();
            return response.status;
        }
        
        case RELAY_MSG_DATA: {
            /* Relay data to partner if paired */
            if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
                SendRelayPacket(pConn->pPartner->socket, RELAY_MSG_DATA,
                               buffer + sizeof(RELAY_HEADER), header.dataLength);
            }
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        case RELAY_MSG_DISCONNECT: {
            pConn->state = RELAY_STATE_DISCONNECTED;
            /* Disconnect partner too */
            if (pConn->pPartner) {
                pConn->pPartner->state = RELAY_STATE_DISCONNECTED;
                pConn->pPartner->pPartner = NULL;
            }
            return 1;  /* Signal disconnection */
        }
        
        case RELAY_MSG_PING: {
            SendRelayPacket(pConn->socket, RELAY_MSG_PONG, NULL, 0);
            pConn->lastActivity = GetTickCount();
            return 0;
        }
        
        default:
            return -1;  /* Unknown message */
    }
}

/* Worker thread for client connection */
static DWORD WINAPI ClientWorkerThread(LPVOID lpParam)
{
    PRELAY_CONNECTION pConn = (PRELAY_CONNECTION)lpParam;
    PRELAY_SERVER pServer = NULL;  /* Set by caller context */
    BYTE buffer[RD2K_BUFFER_SIZE];
    int result;
    DWORD recvLen;
    DWORD lastKeepalive = GetTickCount();
    
    if (!pConn) return 1;
    
    /* Connection loop */
    while (pConn->state != RELAY_STATE_DISCONNECTED) {
        /* Wait for data with timeout */
        result = WaitForSocketReady(pConn->socket, FALSE, 1000);
        
        if (result < 0) {
            break;  /* Socket error */
        } else if (result == 0) {
            /* Timeout - check for keep-alive or disconnect event */
            if (WaitForSingleObject(pConn->hDisconnectEvent, 0) == WAIT_OBJECT_0) {
                break;  /* Disconnect signaled */
            }
            continue;  /* Timeout, try again */
        }
        
        /* Receive data */
        recvLen = recv(pConn->socket, (char*)buffer, sizeof(buffer), 0);
        if (recvLen == 0 || recvLen == SOCKET_ERROR) {
            break;  /* Connection closed or error */
        }
        
        /* Process relay message */
        result = ProcessRelayMessage(NULL, pConn, buffer, recvLen);
        if (result != 0 && result != RD2K_SUCCESS) {
            break;  /* Error or disconnect */
        }
    }
    
    /* Cleanup */
    pConn->state = RELAY_STATE_DISCONNECTED;
    if (pConn->pPartner) {
        pConn->pPartner->pPartner = NULL;
    }
    
    return 0;
}

/* Accept client connections */
static DWORD WINAPI AcceptThread(LPVOID lpParam)
{
    PRELAY_SERVER pServer = (PRELAY_SERVER)lpParam;
    struct sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);
    SOCKET clientSocket;
    PRELAY_CONNECTION pConn;
    HANDLE hThread;
    
    if (!pServer) return 1;
    
    while (pServer->bRunning) {
        /* Check stop event */
        if (WaitForSingleObject(pServer->hStopEvent, 10) == WAIT_OBJECT_0) {
            break;
        }
        
        /* Accept incoming connection */
        clientSocket = accept(pServer->listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            continue;
        }
        
        /* Create connection context */
        pConn = AddConnection(pServer, clientSocket);
        if (!pConn) {
            closesocket(clientSocket);
            continue;
        }
        
        /* Create worker thread for this connection */
        hThread = CreateThread(NULL, 0, ClientWorkerThread, (LPVOID)pConn, 0, NULL);
        if (hThread) {
            pConn->hThread = hThread;
        } else {
            RemoveConnection(pServer, pConn);
            closesocket(clientSocket);
        }
    }
    
    return 0;
}

/* ===== Public API Implementation ===== */

PRELAY_SERVER Relay_Create(WORD port)
{
    PRELAY_SERVER pServer;
    struct sockaddr_in addr;
    int result;
    
    pServer = (PRELAY_SERVER)calloc(1, sizeof(RELAY_SERVER_CONTEXT));
    if (!pServer) return NULL;
    
    pServer->port = port;
    pServer->maxConnections = RELAY_MAX_CONNECTIONS;
    pServer->activeConnections = 0;
    pServer->bRunning = FALSE;
    
    /* Allocate connection arrays */
    pServer->connectionSockets = (SOCKET*)calloc(RELAY_MAX_CONNECTIONS, sizeof(SOCKET));
    pServer->connections = (PRELAY_CONNECTION*)calloc(RELAY_MAX_CONNECTIONS, sizeof(PRELAY_CONNECTION*));
    
    if (!pServer->connectionSockets || !pServer->connections) {
        if (pServer->connectionSockets) free(pServer->connectionSockets);
        if (pServer->connections) free(pServer->connections);
        free(pServer);
        return NULL;
    }
    
    /* Initialize critical section */
    InitializeCriticalSection(&pServer->csConnections);
    
    /* Create stop event */
    pServer->hStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!pServer->hStopEvent) {
        free(pServer->connectionSockets);
        free(pServer->connections);
        DeleteCriticalSection(&pServer->csConnections);
        free(pServer);
        return NULL;
    }
    
    /* Create listening socket */
    pServer->listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pServer->listenSocket == INVALID_SOCKET) {
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        DeleteCriticalSection(&pServer->csConnections);
        free(pServer);
        return NULL;
    }
    
    /* Bind socket */
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    
    result = bind(pServer->listenSocket, (struct sockaddr*)&addr, sizeof(addr));
    if (result == SOCKET_ERROR) {
        closesocket(pServer->listenSocket);
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        DeleteCriticalSection(&pServer->csConnections);
        free(pServer);
        return NULL;
    }
    
    /* Listen for connections */
    result = listen(pServer->listenSocket, SOMAXCONN);
    if (result == SOCKET_ERROR) {
        closesocket(pServer->listenSocket);
        CloseHandle(pServer->hStopEvent);
        free(pServer->connectionSockets);
        free(pServer->connections);
        DeleteCriticalSection(&pServer->csConnections);
        free(pServer);
        return NULL;
    }
    
    return pServer;
}

int Relay_Start(PRELAY_SERVER pServer)
{
    HANDLE hThread;
    DWORD threadId;
    
    if (!pServer) return RD2K_ERR_SOCKET;
    
    pServer->bRunning = TRUE;
    
    /* Create accept thread */
    hThread = CreateThread(NULL, 0, AcceptThread, (LPVOID)pServer, 0, &threadId);
    if (!hThread) {
        pServer->bRunning = FALSE;
        return RD2K_ERR_SOCKET;
    }
    
    pServer->hServerThread = hThread;
    return RD2K_SUCCESS;
}

void Relay_Stop(PRELAY_SERVER pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    pServer->bRunning = FALSE;
    
    /* Signal stop event */
    if (pServer->hStopEvent) {
        SetEvent(pServer->hStopEvent);
    }
    
    /* Wait for server thread */
    if (pServer->hServerThread) {
        WaitForSingleObject(pServer->hServerThread, 5000);
        CloseHandle(pServer->hServerThread);
        pServer->hServerThread = NULL;
    }
    
    /* Disconnect all clients */
    EnterCriticalSection(&pServer->csConnections);
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i]) {
            SetEvent(pServer->connections[i]->hDisconnectEvent);
        }
    }
    LeaveCriticalSection(&pServer->csConnections);
}

void Relay_Destroy(PRELAY_SERVER pServer)
{
    DWORD i;
    
    if (!pServer) return;
    
    Relay_Stop(pServer);
    
    /* Clean up all connections */
    for (i = 0; i < pServer->maxConnections; i++) {
        if (pServer->connections[i]) {
            RemoveConnection(pServer, pServer->connections[i]);
        }
    }
    
    /* Close listening socket */
    if (pServer->listenSocket != INVALID_SOCKET) {
        closesocket(pServer->listenSocket);
    }
    
    /* Clean up resources */
    if (pServer->hStopEvent) {
        CloseHandle(pServer->hStopEvent);
    }
    
    DeleteCriticalSection(&pServer->csConnections);
    
    if (pServer->connectionSockets) free(pServer->connectionSockets);
    if (pServer->connections) free(pServer->connections);
    
    free(pServer);
}

void Relay_GetStats(PRELAY_SERVER pServer, PRELAY_STATS pStats)
{
    if (!pServer || !pStats) return;
    
    ZeroMemory(pStats, sizeof(RELAY_STATS));
    
    EnterCriticalSection(&pServer->csConnections);
    pStats->activeConnections = pServer->activeConnections;
    LeaveCriticalSection(&pServer->csConnections);
}

/* ===== Client-side relay functions ===== */

int Relay_ConnectToServer(const char *relayServerAddr, WORD relayPort,
                         DWORD clientId, SOCKET *pRelaySocket)
{
    struct sockaddr_in addr;
    struct hostent *pHost;
    unsigned long ipAddr;
    SOCKET sock;
    
    if (!relayServerAddr || !pRelaySocket) return RD2K_ERR_SOCKET;
    
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(relayPort);
    
    /* Resolve hostname/IP */
    ipAddr = inet_addr(relayServerAddr);
    if (ipAddr != INADDR_NONE) {
        addr.sin_addr.s_addr = ipAddr;
    } else {
        pHost = gethostbyname(relayServerAddr);
        if (!pHost) {
            closesocket(sock);
            return RD2K_ERR_CONNECT;
        }
        CopyMemory(&addr.sin_addr, pHost->h_addr, pHost->h_length);
    }
    
    /* Connect to relay server */
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return RD2K_ERR_CONNECT;
    }
    
    *pRelaySocket = sock;
    return RD2K_SUCCESS;
}

int Relay_Register(SOCKET relaySocket, DWORD clientId)
{
    RELAY_REGISTER_MSG msg;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    msg.clientId = clientId;
    msg.reserved = 0;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_REGISTER, (const BYTE*)&msg, sizeof(msg));
}

int Relay_RequestPartner(SOCKET relaySocket, DWORD partnerId, DWORD password)
{
    RELAY_CONNECT_REQUEST msg;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    msg.partnerId = partnerId;
    msg.password = password;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_CONNECT_REQUEST, (const BYTE*)&msg, sizeof(msg));
}

int Relay_WaitForConnection(SOCKET relaySocket, DWORD timeoutMs)
{
    BYTE buffer[512];
    DWORD recvLen;
    RELAY_HEADER header;
    RELAY_CONNECT_RESPONSE response;
    int result;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    
    /* Wait for response */
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result <= 0) {
        return RD2K_ERR_TIMEOUT;
    }
    
    recvLen = recv(relaySocket, (char*)buffer, sizeof(buffer), 0);
    if (recvLen == 0 || recvLen == SOCKET_ERROR) {
        return RD2K_ERR_RECV;
    }
    
    if (recvLen < sizeof(RELAY_HEADER)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    if (header.msgType != RELAY_MSG_CONNECT_RESPONSE) {
        return RD2K_ERR_PROTOCOL;
    }
    
    if (recvLen < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_RESPONSE)) {
        return RD2K_ERR_PROTOCOL;
    }
    
    CopyMemory(&response, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_RESPONSE));
    
    return response.status;
}

int Relay_SendData(SOCKET relaySocket, const BYTE *data, DWORD length)
{
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!data) return RD2K_ERR_SOCKET;
    
    return SendRelayPacket(relaySocket, RELAY_MSG_DATA, data, length);
}

int Relay_RecvData(SOCKET relaySocket, BYTE *buffer, DWORD bufferSize, DWORD timeoutMs)
{
    int result;
    DWORD recvLen;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!buffer) return RD2K_ERR_SOCKET;
    
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result < 0) {
        return RD2K_ERR_RECV;
    }
    if (result == 0) {
        return 0;  /* Timeout */
    }
    
    recvLen = recv(relaySocket, (char*)buffer, bufferSize, 0);
    if (recvLen == 0 || recvLen == SOCKET_ERROR) {
        return RD2K_ERR_RECV;
    }
    
    return (int)recvLen;
}
