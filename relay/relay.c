/*
 * RemoteDesk2K - Relay Server Implementation
 * Windows 2000 compatible relay server
 * With XOR encryption support for secure message passing
 */

#include "relay.h"
#include "crypto.h"

/* Logging callback for GUI console output */
typedef void (*RELAY_LOG_CALLBACK)(const char* message);
static RELAY_LOG_CALLBACK g_pfnLogCallback = NULL;
static CRITICAL_SECTION g_csLog;
static BOOL g_bLogInitialized = FALSE;

/* Set logging callback */
void Relay_SetLogCallback(RELAY_LOG_CALLBACK pfnCallback)
{
    if (!g_bLogInitialized) {
        InitializeCriticalSection(&g_csLog);
        g_bLogInitialized = TRUE;
    }
    EnterCriticalSection(&g_csLog);
    g_pfnLogCallback = pfnCallback;
    LeaveCriticalSection(&g_csLog);
}

/* Internal logging function */
static void RelayLog(const char* format, ...)
{
    char buffer[512];
    va_list args;
    
    if (!g_bLogInitialized) return;
    
    va_start(args, format);
    _vsnprintf(buffer, sizeof(buffer)-1, format, args);
    buffer[sizeof(buffer)-1] = '\0';
    va_end(args);
    
    EnterCriticalSection(&g_csLog);
    if (g_pfnLogCallback) {
        g_pfnLogCallback(buffer);
    }
    LeaveCriticalSection(&g_csLog);
}

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

/* Helper: Format client ID as friendly string "XXX XXX XXX XXX" */
static void FormatClientId(DWORD id, char *buffer)
{
    /* Convert DWORD to 4 bytes displayed as decimals with spaces */
    sprintf(buffer, "%03d %03d %03d %03d",
            (id >> 24) & 0xFF,
            (id >> 16) & 0xFF,
            (id >> 8) & 0xFF,
            id & 0xFF);
}

/* Helper: Send a relay packet to a socket with XOR encryption */
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
    header.flags = 0x01;  /* Flag: encrypted */
    header.reserved = 0;
    header.dataLength = dataLength;
    
    CopyMemory(packet, &header, sizeof(RELAY_HEADER));
    if (data && dataLength > 0) {
        CopyMemory(packet + sizeof(RELAY_HEADER), data, dataLength);
        /* XOR encrypt the data portion */
        Crypto_Encrypt(packet + sizeof(RELAY_HEADER), dataLength);
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
    
    /* IMPORTANT: Iterate ALL slots, not just activeConnections count
     * because the array can be sparse after disconnections */
    for (i = 0; i < pServer->maxConnections; i++) {
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

/* Check if a socket is still connected (not dead) 
 * Returns TRUE if socket appears alive, FALSE only if definitely dead.
 * Uses conservative checks to avoid false negatives on Wine/Linux. */
static BOOL IsSocketAlive(SOCKET sock)
{
    int optval;
    int optlen = sizeof(optval);
    
    if (sock == INVALID_SOCKET) return FALSE;
    
    /* Check socket error status - most reliable cross-platform method */
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen) == SOCKET_ERROR) {
        return FALSE;  /* Cannot query socket - probably dead */
    }
    
    if (optval != 0) {
        return FALSE;  /* Socket has error pending - dead */
    }
    
    /* Socket appears healthy */
    return TRUE;
}

/* Remove stale connections with same clientId (for reconnection handling) 
 * IMPORTANT: Protect PAIRED connections always, REGISTERED with timeout.
 * Dead sockets will be cleaned up naturally when recv() fails.
 * Returns TRUE if ID is available for registration, FALSE if already in use */
static BOOL RemoveStaleConnections(PRELAY_SERVER pServer, DWORD clientId, PRELAY_CONNECTION pExclude)
{
    DWORD i;
    BOOL bIdAvailable = TRUE;
    DWORD currentTime = GetTickCount();
    
    /* Short timeout for registered connections (5 seconds).
     * This allows legitimate reconnections while preventing rapid loops.
     * Dead sockets are detected when recv() fails, which triggers cleanup. */
    #define REGISTERED_TIMEOUT_MS 5000
    
    if (!pServer) return FALSE;
    
    EnterCriticalSection(&pServer->csConnections);
    
    for (i = 0; i < pServer->maxConnections; i++) {
        PRELAY_CONNECTION pConn = pServer->connections[i];
        if (pConn && pConn != pExclude && pConn->clientId == clientId) {
            char idStr[20];
            DWORD idleTime;
            FormatClientId(clientId, idStr);
            
            /* Check how long since last activity */
            idleTime = currentTime - pConn->lastActivity;
            
            /* PAIRED connections - always protect, never kick out */
            if (pConn->state == RELAY_STATE_PAIRED) {
                RelayLog("[PROTECT] ID %s is in active session (PAIRED) - rejecting duplicate\r\n", idStr);
                bIdAvailable = FALSE;
                continue;
            }
            
            /* REGISTERED connections - protect unless timed out */
            if (pConn->state == RELAY_STATE_REGISTERED) {
                if (idleTime < REGISTERED_TIMEOUT_MS) {
                    RelayLog("[PROTECT] ID %s is recently registered (%lu ms ago) - rejecting duplicate\r\n", 
                            idStr, idleTime);
                    bIdAvailable = FALSE;
                    continue;
                }
                /* Timed out REGISTERED connection - allow removal */
                RelayLog("[TIMEOUT] ID %s was REGISTERED but idle for %lu ms - allowing reconnect\r\n", 
                        idStr, idleTime);
            }
            
            /* Remove: DISCONNECTED, CONNECTED(zombie), or timed-out REGISTERED */
            RelayLog("[CLEANUP] Removing connection for ID %s (state=%d, idle=%lu ms)\r\n", 
                    idStr, pConn->state, idleTime);
            
            /* Signal disconnect and close socket */
            if (pConn->hDisconnectEvent) {
                SetEvent(pConn->hDisconnectEvent);
            }
            if (pConn->socket != INVALID_SOCKET) {
                closesocket(pConn->socket);
                pConn->socket = INVALID_SOCKET;
            }
            pConn->state = RELAY_STATE_DISCONNECTED;
            
            /* Clear slot and update count */
            pServer->connections[i] = NULL;
            if (pServer->activeConnections > 0) {
                pServer->activeConnections--;
            }
        }
    }
    
    LeaveCriticalSection(&pServer->csConnections);
    return bIdAvailable;
    
    #undef REGISTERED_TIMEOUT_MS
}

/* Configure client socket for optimal relay performance */
static void ConfigureClientSocket(SOCKET sock)
{
    int opt;
    
    /* Disable Nagle's algorithm for lower latency */
    opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    
    /* Increase socket buffers for large transfers */
    opt = 512 * 1024;  /* 512KB */
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt));
    
    /* Enable keep-alive */
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&opt, sizeof(opt));
}

/* Add a new connection to the server */
static PRELAY_CONNECTION AddConnection(PRELAY_SERVER pServer, SOCKET sock)
{
    PRELAY_CONNECTION pConn;
    DWORD i;
    
    if (!pServer || pServer->activeConnections >= pServer->maxConnections) {
        return NULL;
    }
    
    /* Configure socket options for optimal performance */
    ConfigureClientSocket(sock);
    
    pConn = (PRELAY_CONNECTION)calloc(1, sizeof(RELAY_CONNECTION));
    if (!pConn) return NULL;
    
    pConn->socket = sock;
    pConn->state = RELAY_STATE_CONNECTED;
    pConn->pServer = pServer;  /* Store server reference for worker thread */
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

/* Handle relay protocol messages on a connection with XOR decryption */
static int ProcessRelayMessage(PRELAY_SERVER pServer, PRELAY_CONNECTION pConn,
                              BYTE *buffer, DWORD length)
{
    RELAY_HEADER header;
    PRELAY_CONNECTION pPartner;
    RELAY_CONNECT_RESPONSE response;
    
    if (length < sizeof(RELAY_HEADER)) {
        RelayLog("[ERROR] Invalid message size: %lu < %d\r\n", 
                (unsigned long)length, (int)sizeof(RELAY_HEADER));
        return -1;  /* Invalid message */
    }
    
    CopyMemory(&header, buffer, sizeof(RELAY_HEADER));
    
    /* XOR decrypt if data is encrypted (flag 0x01) */
    if ((header.flags & 0x01) && header.dataLength > 0) {
        Crypto_Decrypt(buffer + sizeof(RELAY_HEADER), header.dataLength);
    }
    
    switch (header.msgType) {
        case RELAY_MSG_REGISTER: {
            RELAY_REGISTER_MSG reg;
            RELAY_REGISTER_RESPONSE response;
            char idStr[20];
            BOOL bIdAvailable;
            
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_REGISTER_MSG)) {
                return -1;
            }
            CopyMemory(&reg, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_REGISTER_MSG));
            
            /* Check for stale connections and verify ID is available.
             * If ID is already in an active PAIRED session, reject this registration. */
            bIdAvailable = RemoveStaleConnections(pServer, reg.clientId, pConn);
            
            FormatClientId(reg.clientId, idStr);
            
            if (!bIdAvailable) {
                /* ID is already in use - send duplicate ID error to client */
                RelayLog("[REJECT] Registration rejected for ID %s - already connected\r\n", idStr);
                response.status = RELAY_REGISTER_DUPLICATE;
                response.reserved = 0;
                SendRelayPacket(pConn->socket, RELAY_MSG_REGISTER_RESPONSE,
                               (const BYTE*)&response, sizeof(response));
                return -1;  /* Disconnect after sending response */
            }
            
            pConn->clientId = reg.clientId;
            pConn->state = RELAY_STATE_REGISTERED;
            pConn->lastActivity = GetTickCount();
            
            /* Send success response */
            response.status = RELAY_REGISTER_OK;
            response.reserved = 0;
            SendRelayPacket(pConn->socket, RELAY_MSG_REGISTER_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            
            /* Log client registration with friendly ID */
            RelayLog("[REGISTER] Client ID: %s registered\r\n", idStr);
            return 0;
        }
        
        case RELAY_MSG_CONNECT_REQUEST: {
            RELAY_CONNECT_REQUEST req;
            char clientIdStr[20], partnerIdStr[20];
            if (length < sizeof(RELAY_HEADER) + sizeof(RELAY_CONNECT_REQUEST)) {
                return -1;
            }
            CopyMemory(&req, buffer + sizeof(RELAY_HEADER), sizeof(RELAY_CONNECT_REQUEST));
            
            /* Format IDs for logging */
            FormatClientId(pConn->clientId, clientIdStr);
            FormatClientId(req.partnerId, partnerIdStr);
            
            /* Look for partner */
            pPartner = FindConnectionById(pServer, req.partnerId);
            if (!pPartner) {
                /* Partner not connected to relay server */
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] Client %s -> Partner %s: NOT ONLINE\r\n", 
                        clientIdStr, partnerIdStr);
            } else if (pPartner->state == RELAY_STATE_PAIRED) {
                /* Partner is already in a session with someone else */
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] Client %s -> Partner %s: BUSY (in another session)\r\n", 
                        clientIdStr, partnerIdStr);
            } else if (pPartner->state != RELAY_STATE_REGISTERED) {
                /* Partner in unexpected state */
                response.status = RD2K_ERR_CONNECT;
                RelayLog("[CONNECT] Client %s -> Partner %s: NOT READY (state=%d)\r\n", 
                        clientIdStr, partnerIdStr, pPartner->state);
            } else {
                /* Partner available - Pair the connections */
                pConn->pPartner = pPartner;
                pPartner->pPartner = pConn;
                pConn->state = RELAY_STATE_PAIRED;
                pPartner->state = RELAY_STATE_PAIRED;
                response.status = RD2K_SUCCESS;
                RelayLog("[CONNECT] Client %s <-> Partner %s: PAIRED\r\n", 
                        clientIdStr, partnerIdStr);
            }
            
            /* Send response to requesting client */
            SendRelayPacket(pConn->socket, RELAY_MSG_CONNECT_RESPONSE,
                           (const BYTE*)&response, sizeof(response));
            
            pConn->lastActivity = GetTickCount();
            
            /* Don't disconnect client on failed connect - let them try again */
            return 0;
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
            char idStr[20];
            FormatClientId(pConn->clientId, idStr);
            RelayLog("[DISCONNECT] Client %s disconnected\r\n", idStr);
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
            RelayLog("[ERROR] Unknown message type: 0x%02X (flags=0x%02X, dataLen=%lu)\r\n",
                    header.msgType, header.flags, (unsigned long)header.dataLength);
            return -1;  /* Unknown message */
    }
}

/* 
 * Helper function to receive exact number of bytes from socket.
 * Handles TCP fragmentation by looping until all data is received.
 * Returns: number of bytes received, 0 on connection close, -1 on error
 */
static int RecvExact(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    int recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 100) + 10;  /* Calculate retries based on timeout */
    
    while (totalRecv < length) {
        /* Wait for data */
        waitResult = WaitForSocketReady(sock, FALSE, 100);  /* 100ms poll */
        
        if (waitResult < 0) {
            return -1;  /* Socket error */
        }
        
        if (waitResult == 0) {
            /* Timeout on this poll - check if we've exceeded total timeout */
            retries++;
            if (retries > maxRetries) {
                if (totalRecv > 0) {
                    /* Partial data received, return what we have */
                    return totalRecv;
                }
                return 0;  /* Full timeout, no data */
            }
            continue;
        }
        
        /* Receive available data */
        recvLen = recv(sock, (char*)(buffer + totalRecv), length - totalRecv, 0);
        
        if (recvLen == 0) {
            return 0;  /* Connection closed */
        }
        
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;  /* No data available, retry */
            }
            return -1;  /* Real error */
        }
        
        totalRecv += recvLen;
        retries = 0;  /* Reset timeout counter on successful recv */
    }
    
    return totalRecv;
}

/* Worker thread for client connection */
static DWORD WINAPI ClientWorkerThread(LPVOID lpParam)
{
    PRELAY_CONNECTION pConn = (PRELAY_CONNECTION)lpParam;
    PRELAY_SERVER pServer;
    BYTE* buffer;  /* Heap allocated - main packet buffer */
    int result;
    
    if (!pConn) return 1;
    
    /* Get server reference from connection */
    pServer = (PRELAY_SERVER)pConn->pServer;
    if (!pServer) {
        RelayLog("[ERROR] Worker thread: no server reference!\r\n");
        return 1;
    }
    
    /* Allocate receive buffer on heap (4MB is too large for stack!) */
    buffer = (BYTE*)malloc(RELAY_BUFFER_SIZE);
    if (!buffer) {
        RelayLog("[ERROR] Worker thread: failed to allocate buffer!\r\n");
        return 1;
    }
    
    RelayLog("[INFO] Client worker thread started for socket %d\r\n", (int)pConn->socket);
    
    /* Connection loop - proper TCP stream handling */
    while (pConn->state != RELAY_STATE_DISCONNECTED && pServer->bRunning) {
        RELAY_HEADER header;
        DWORD totalPacketSize;
        int recvLen;
        
        /* Check for disconnect event (non-blocking) */
        if (WaitForSingleObject(pConn->hDisconnectEvent, 0) == WAIT_OBJECT_0) {
            RelayLog("[INFO] Disconnect event signaled\r\n");
            break;
        }
        
        /* Step 1: Receive the relay header (8 bytes) - wait up to 1 second */
        recvLen = RecvExact(pConn->socket, (BYTE*)&header, sizeof(RELAY_HEADER), 1000);
        
        if (recvLen == 0) {
            /* Timeout with no data - just loop and check disconnect event */
            continue;
        }
        
        if (recvLen < 0) {
            RelayLog("[ERROR] Socket error receiving header\r\n");
            break;  /* Socket error */
        }
        
        if (recvLen < (int)sizeof(RELAY_HEADER)) {
            RelayLog("[WARN] Incomplete header: got %d bytes\r\n", recvLen);
            break;  /* Protocol error - incomplete header after timeout */
        }
        
        /* Step 2: Validate header and calculate total data needed */
        totalPacketSize = sizeof(RELAY_HEADER) + header.dataLength;
        
        if (header.dataLength > RELAY_BUFFER_SIZE - sizeof(RELAY_HEADER)) {
            RelayLog("[ERROR] Invalid data length: %lu bytes (msgType=0x%02X)\r\n", 
                    (unsigned long)header.dataLength, header.msgType);
            break;  /* Protocol error or corrupted stream */
        }
        
        /* Step 3: Copy header to buffer */
        CopyMemory(buffer, &header, sizeof(RELAY_HEADER));
        
        /* Step 4: Receive the data payload if any */
        if (header.dataLength > 0) {
            /* Wait up to 30 seconds for large data transfers */
            recvLen = RecvExact(pConn->socket, buffer + sizeof(RELAY_HEADER), 
                               header.dataLength, 30000);
            
            if (recvLen < 0) {
                RelayLog("[ERROR] Socket error receiving data\r\n");
                break;  /* Socket error */
            }
            
            if (recvLen < (int)header.dataLength) {
                RelayLog("[WARN] Incomplete data: got %d, expected %lu\r\n", 
                        recvLen, (unsigned long)header.dataLength);
                break;  /* Protocol error - incomplete data after timeout */
            }
        }
        
        /* Step 5: Process the complete message */
        result = ProcessRelayMessage(pServer, pConn, buffer, totalPacketSize);
        if (result != 0 && result != RD2K_SUCCESS) {
            /* result = 1 means graceful disconnect (RELAY_MSG_DISCONNECT)
             * result = -1 means protocol error */
            break;
        }
    }
    
    /* Cleanup - IMPORTANT: Notify partner before disconnecting */
    {
        char idStr[20];
        FormatClientId(pConn->clientId, idStr);
        
        /* Log proper disconnect message */
        if (pConn->clientId != 0) {
            RelayLog("[DISCONNECT] Client %s connection closed\r\n", idStr);
        } else {
            RelayLog("[DISCONNECT] Unregistered client connection closed\r\n");
        }
        
        /* If we have a partner, notify them that we disconnected */
        if (pConn->pPartner && pConn->pPartner->socket != INVALID_SOCKET) {
            RELAY_PARTNER_DISCONNECTED notification;
            char partnerIdStr[20];
            
            FormatClientId(pConn->pPartner->clientId, partnerIdStr);
            
            notification.reason = RELAY_DISCONNECT_PARTNER_LEFT;
            notification.partnerId = pConn->clientId;
            
            /* Send notification to partner - don't wait for success */
            SendRelayPacket(pConn->pPartner->socket, RELAY_MSG_PARTNER_DISCONNECTED,
                           (const BYTE*)&notification, sizeof(notification));
            
            RelayLog("[NOTIFY] Sent disconnect notification to partner %s\r\n", partnerIdStr);
            
            /* Signal partner's worker thread to check connection state */
            if (pConn->pPartner->hDisconnectEvent) {
                SetEvent(pConn->pPartner->hDisconnectEvent);
            }
            
            /* Clear the partner's reference to us */
            pConn->pPartner->pPartner = NULL;
            pConn->pPartner->state = RELAY_STATE_REGISTERED;  /* Back to registered state */
        }
    }
    
    pConn->state = RELAY_STATE_DISCONNECTED;
    pConn->pPartner = NULL;
    
    free(buffer);  /* Free heap buffer */
    
    /* IMPORTANT: Remove connection from server's array to allow slot reuse.
     * This prevents stale entries from piling up and causing lookup issues. */
    {
        DWORD i;
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
        
        /* Close socket and free resources */
        if (pConn->socket != INVALID_SOCKET) {
            closesocket(pConn->socket);
            pConn->socket = INVALID_SOCKET;
        }
        if (pConn->recvBuffer) {
            free(pConn->recvBuffer);
            pConn->recvBuffer = NULL;
        }
        if (pConn->hDisconnectEvent) {
            CloseHandle(pConn->hDisconnectEvent);
            pConn->hDisconnectEvent = NULL;
        }
        /* Note: Don't free pConn here - the thread handle is still in use.
         * The connection struct will be cleaned up when server stops. */
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
    fd_set readfds;
    struct timeval tv;
    int selectResult;
    
    if (!pServer) return 1;
    
    RelayLog("[INFO] Accept thread started, waiting for connections...\r\n");
    
    while (pServer->bRunning) {
        /* Check stop event */
        if (WaitForSingleObject(pServer->hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }
        
        /* Use select() to check for incoming connections with timeout */
        /* This prevents blocking forever in accept() */
        FD_ZERO(&readfds);
        FD_SET(pServer->listenSocket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms timeout */
        
        selectResult = select(0, &readfds, NULL, NULL, &tv);
        if (selectResult == SOCKET_ERROR) {
            RelayLog("[ERROR] select() failed in accept thread\r\n");
            break;
        }
        if (selectResult == 0) {
            /* Timeout - no connection pending, loop again */
            continue;
        }
        
        /* Connection is pending, accept it */
        addrLen = sizeof(clientAddr);
        clientSocket = accept(pServer->listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (pServer->bRunning) {
                RelayLog("[ERROR] accept() failed\r\n");
            }
            continue;
        }
        
        /* Privacy: Don't log client IP addresses */
        RelayLog("[INFO] New client connection accepted\r\n");
        
        /* Create connection context */
        pConn = AddConnection(pServer, clientSocket);
        if (!pConn) {
            RelayLog("[ERROR] Failed to add connection (max reached?)\r\n");
            closesocket(clientSocket);
            continue;
        }
        
        /* Create worker thread for this connection */
        hThread = CreateThread(NULL, 0, ClientWorkerThread, (LPVOID)pConn, 0, NULL);
        if (hThread) {
            pConn->hThread = hThread;
            RelayLog("[INFO] Worker thread created for new client\r\n");
        } else {
            RelayLog("[ERROR] Failed to create worker thread\r\n");
            RemoveConnection(pServer, pConn);
            closesocket(clientSocket);
        }
    }
    
    RelayLog("[INFO] Accept thread stopping\r\n");
    return 0;
}

/* ===== Public API Implementation ===== */

PRELAY_SERVER Relay_Create(WORD port, const char* ipAddr)
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
    if (ipAddr && ipAddr[0]) {
        unsigned long ip = inet_addr(ipAddr);
        if (ip == INADDR_NONE) {
            ip = htonl(INADDR_ANY);
        }
        addr.sin_addr.s_addr = ip;
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
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

/* 
 * Helper to receive exact number of bytes, handling TCP fragmentation.
 * Used by Relay_RecvData for receiving complete relay packets.
 */
static int RecvExactRelay(SOCKET sock, BYTE *buffer, int length, int timeoutMs)
{
    int totalRecv = 0;
    int recvLen;
    int waitResult;
    int retries = 0;
    int maxRetries = (timeoutMs / 50) + 5;
    
    while (totalRecv < length) {
        waitResult = WaitForSocketReady(sock, FALSE, 50);
        
        if (waitResult < 0) {
            return -1;
        }
        
        if (waitResult == 0) {
            retries++;
            if (retries > maxRetries) {
                return totalRecv;
            }
            continue;
        }
        
        recvLen = recv(sock, (char*)(buffer + totalRecv), length - totalRecv, 0);
        
        if (recvLen == 0) {
            return 0;
        }
        
        if (recvLen == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                continue;
            }
            return -1;
        }
        
        totalRecv += recvLen;
        retries = 0;
    }
    
    return totalRecv;
}

int Relay_RecvData(SOCKET relaySocket, BYTE *buffer, DWORD bufferSize, DWORD timeoutMs)
{
    RELAY_HEADER header;
    BYTE headerBuf[sizeof(RELAY_HEADER)];
    int recvLen;
    int result;
    
    if (relaySocket == INVALID_SOCKET) return RD2K_ERR_SOCKET;
    if (!buffer) return RD2K_ERR_SOCKET;
    
    /* Wait for data */
    result = WaitForSocketReady(relaySocket, FALSE, timeoutMs);
    if (result < 0) {
        return RD2K_ERR_RECV;
    }
    if (result == 0) {
        return 0;
    }
    
    /* Receive complete header */
    recvLen = RecvExactRelay(relaySocket, headerBuf, sizeof(RELAY_HEADER), timeoutMs);
    
    if (recvLen == 0) {
        return 0;
    }
    if (recvLen < 0) {
        return RD2K_ERR_RECV;
    }
    if (recvLen < (int)sizeof(RELAY_HEADER)) {
        return 0;
    }
    
    CopyMemory(&header, headerBuf, sizeof(RELAY_HEADER));
    
    /* Handle non-DATA messages */
    if (header.msgType != RELAY_MSG_DATA) {
        if (header.msgType == RELAY_MSG_DISCONNECT) {
            return RD2K_ERR_DISCONNECTED;
        }
        if (header.dataLength > 0 && header.dataLength < bufferSize) {
            RecvExactRelay(relaySocket, buffer, header.dataLength, 1000);
        }
        return 0;
    }
    
    /* Validate and receive data */
    if (header.dataLength == 0) {
        return 0;
    }
    if (header.dataLength > bufferSize) {
        DWORD remaining = header.dataLength;
        while (remaining > 0) {
            DWORD chunk = (remaining > bufferSize) ? bufferSize : remaining;
            recvLen = RecvExactRelay(relaySocket, buffer, chunk, 5000);
            if (recvLen <= 0) break;
            remaining -= recvLen;
        }
        return 0;
    }
    
    recvLen = RecvExactRelay(relaySocket, buffer, header.dataLength, 30000);
    
    if (recvLen <= 0) {
        return RD2K_ERR_RECV;
    }
    
    if ((DWORD)recvLen < header.dataLength) {
        return RD2K_ERR_RECV;
    }
    
    return recvLen;
}
